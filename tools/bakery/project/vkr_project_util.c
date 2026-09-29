#include "vkr_project_internal.h"

#include "../vkr_bakery_buffer.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_asset_path.h"
#include "platform/vkr_platform.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

vkr_internal const char *const vkr_project_document_blocks[] = {
    "transform", "mesh", "shape", "text3d"};

// =============================================================================
// Failure, cancellation and progress
// =============================================================================

bool8_t vkr_project_fail(VkrProjectJob *job, const char *format, ...) {
  if (!job->error[0]) {
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(job->error, sizeof(job->error), format, arguments);
    va_end(arguments);
  }
  job->failed = true_v;
  return false_v;
}

bool8_t vkr_project_check_cancel(VkrProjectJob *job) {
  if (job->cancelled || vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                                             VKR_MEMORY_ORDER_RELAXED)) {
    job->cancelled = true_v;
    return vkr_project_fail(job, "Project preparation cancelled");
  }
  return true_v;
}

bool8_t vkr_project_progress(VkrProjectJob *job, const char *stage,
                             float64_t fraction, const char *detail) {
  VKR_PROJECT_TRY(vkr_project_check_cancel(job));
  /* Rollback checks inject one failure at a named stage. */
  const char *fault = getenv("VKR_BAKERY_FAULT_STAGE");
  if (fault && strcmp(fault, stage) == 0) {
    return vkr_project_fail(job, "Injected failure at %s", stage);
  }
  Arena *arena = job->arena;
  VkrBakeryJson *value = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, value, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, value, "status",
                      vkr_bakery_json_cstr(arena, "running"));
  vkr_bakery_json_set(arena, value, "stage",
                      vkr_bakery_json_cstr(arena, stage));
  vkr_bakery_json_set(arena, value, "progress",
                      fraction < 0.0 ? vkr_bakery_json_null(arena)
                                     : vkr_bakery_json_float(arena, fraction));
  vkr_bakery_json_set(arena, value, "detail",
                      vkr_bakery_json_cstr(arena, detail ? detail : ""));
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, job->progress_path, value));
  printf("%s: %s\n", stage, detail ? detail : "");
  fflush(stdout);
  return true_v;
}

void vkr_project_warn(VkrProjectJob *job, const char *format, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  vkr_bakery_json_append(job->warnings, vkr_bakery_json_cstr(job->arena, text));
}

// =============================================================================
// Strings and collections
// =============================================================================

const char *vkr_project_strdup(VkrProjectJob *job, const char *text) {
  if (!text) {
    return NULL;
  }
  const uint64_t length = strlen(text);
  char *copy =
      (char *)arena_alloc(job->arena, length + 1u, ARENA_MEMORY_TAG_STRING);
  if (copy) {
    MemCopy(copy, text, length + 1u);
  }
  return copy;
}

const char *vkr_project_printf(VkrProjectJob *job, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  va_list measure;
  va_copy(measure, arguments);
  const int length = vsnprintf(NULL, 0, format, measure);
  va_end(measure);
  char *text = NULL;
  if (length >= 0) {
    text = (char *)arena_alloc(job->arena, (uint64_t)length + 1u,
                               ARENA_MEMORY_TAG_STRING);
    if (text) {
      (void)vsnprintf(text, (size_t)length + 1u, format, arguments);
    }
  }
  va_end(arguments);
  return text;
}

const char *vkr_project_json_text(const VkrBakeryJson *object,
                                  const char *key) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  return value && value->type == VKR_BAKERY_JSON_STRING
             ? (const char *)value->string.str
             : NULL;
}

bool8_t vkr_project_strings_push(VkrProjectJob *job, VkrProjectStrings *list,
                                 const char *text) {
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
    const char **items = (const char **)arena_alloc(
        job->arena, sizeof(char *) * capacity, ARENA_MEMORY_TAG_ARRAY);
    if (!items) {
      return vkr_project_fail(job, "Out of memory");
    }
    if (list->count) {
      MemCopy(items, list->items, sizeof(char *) * list->count);
    }
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count++] = vkr_project_strdup(job, text);
  return true_v;
}

bool8_t vkr_project_nodes_push(VkrProjectJob *job, VkrProjectNodes *list,
                               VkrBakeryJson *node) {
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
    VkrBakeryJson **items = (VkrBakeryJson **)arena_alloc(
        job->arena, capacity * sizeof(VkrBakeryJson *), ARENA_MEMORY_TAG_ARRAY);
    if (!items) {
      return vkr_project_fail(job, "Out of memory");
    }
    if (list->count) {
      MemCopy(items, list->items, list->count * sizeof(VkrBakeryJson *));
    }
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count++] = node;
  return true_v;
}

bool8_t vkr_project_truthy(const VkrBakeryJson *value) {
  if (!value) {
    return false_v;
  }
  switch (value->type) {
  case VKR_BAKERY_JSON_BOOL:
    return value->boolean;
  case VKR_BAKERY_JSON_INT:
    return value->integer != 0;
  case VKR_BAKERY_JSON_FLOAT:
    return value->number != 0.0;
  case VKR_BAKERY_JSON_STRING:
    return value->string.length != 0u;
  case VKR_BAKERY_JSON_ARRAY:
  case VKR_BAKERY_JSON_OBJECT:
    return value->count != 0u;
  default:
    return false_v;
  }
}

bool8_t vkr_project_number(const VkrBakeryJson *value, float64_t *out) {
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

bool8_t vkr_project_integer(const VkrBakeryJson *value, int64_t *out) {
  if (value && value->type == VKR_BAKERY_JSON_INT) {
    *out = value->integer;
    return true_v;
  }
  return false_v;
}

vkr_internal uint64_t vkr_project_hash_text(const char *text) {
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t *c = (const uint8_t *)text; *c; ++c) {
    hash = (hash ^ *c) * 1099511628211ull;
  }
  return hash;
}

vkr_internal uint32_t vkr_project_set_slot(const VkrProjectSet *set,
                                           const char *key) {
  uint32_t slot = (uint32_t)vkr_project_hash_text(key) & (set->capacity - 1u);
  while (set->keys[slot] && strcmp(set->keys[slot], key) != 0) {
    slot = (slot + 1u) & (set->capacity - 1u);
  }
  return slot;
}

bool8_t vkr_project_set_add(VkrProjectSet *set, const char *key,
                            const char *value) {
  if ((set->count + 1u) * 4u >= set->capacity * 3u) {
    VkrProjectSet grown = {.capacity =
                               set->capacity ? set->capacity * 2u : 256u};
    grown.keys = (char **)calloc(grown.capacity, sizeof(char *));
    grown.values = (const char **)calloc(grown.capacity, sizeof(char *));
    if (!grown.keys || !grown.values) {
      free(grown.keys);
      free((void *)grown.values);
      return false_v;
    }
    for (uint32_t i = 0u; i < set->capacity; ++i) {
      if (set->keys[i]) {
        const uint32_t slot = vkr_project_set_slot(&grown, set->keys[i]);
        grown.keys[slot] = set->keys[i];
        grown.values[slot] = set->values[i];
        grown.count += 1u;
      }
    }
    free(set->keys);
    free((void *)set->values);
    *set = grown;
  }
  const uint32_t slot = vkr_project_set_slot(set, key);
  if (!set->keys[slot]) {
    set->keys[slot] = strdup(key);
    if (!set->keys[slot]) {
      return false_v;
    }
    set->count += 1u;
  }
  set->values[slot] = value;
  return true_v;
}

const char *vkr_project_set_get(const VkrProjectSet *set, const char *key) {
  if (!set->capacity) {
    return NULL;
  }
  const uint32_t slot = vkr_project_set_slot(set, key);
  return set->keys[slot] ? set->values[slot] : NULL;
}

bool8_t vkr_project_set_has(const VkrProjectSet *set, const char *key) {
  return set->capacity && set->keys[vkr_project_set_slot(set, key)] != NULL;
}

void vkr_project_set_free(VkrProjectSet *set) {
  for (uint32_t i = 0u; i < set->capacity; ++i) {
    free(set->keys[i]);
  }
  free(set->keys);
  free((void *)set->values);
  MemZero(set, sizeof(*set));
}

typedef struct VkrProjectListContext {
  VkrProjectJob *job;
  VkrProjectStrings *names;
  bool8_t ok;
} VkrProjectListContext;

vkr_internal bool8_t vkr_project_list_visit(void *context, const char *name,
                                            bool8_t is_directory) {
  (void)is_directory;
  VkrProjectListContext *list = (VkrProjectListContext *)context;
  list->ok = vkr_project_strings_push(list->job, list->names, name) && list->ok;
  return true_v;
}

bool8_t vkr_project_list(VkrProjectJob *job, const char *directory,
                         VkrProjectStrings *out_names) {
  MemZero(out_names, sizeof(*out_names));
  if (!vkr_bakery_is_directory(directory)) {
    return true_v;
  }
  VkrProjectListContext context = {
      .job = job, .names = out_names, .ok = true_v};
  if (!vkr_bakery_list_directory(directory, vkr_project_list_visit, &context) ||
      !context.ok) {
    return vkr_project_fail(job, "Cannot list %s", directory);
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_walk(VkrProjectJob *job, const char *directory,
                                      VkrProjectStrings *out_paths,
                                      bool8_t reject_links,
                                      const char *link_error) {
  VkrProjectStrings names;
  VKR_PROJECT_TRY(vkr_project_list(job, directory, &names));
  for (uint32_t i = 0u; i < names.count; ++i) {
    char path[VKR_PROJECT_PATH];
    if (!vkr_bakery_path_join(path, sizeof(path), directory, names.items[i])) {
      return vkr_project_fail(job, "Path too long below %s", directory);
    }
    if (vkr_project_lstat_is_link(path)) {
      if (reject_links) {
        return vkr_project_fail(job, "%s", link_error);
      }
      continue;
    }
    if (vkr_bakery_is_directory(path)) {
      VKR_PROJECT_TRY(
          vkr_project_walk(job, path, out_paths, reject_links, link_error));
    } else if (vkr_bakery_is_file(path)) {
      VKR_PROJECT_TRY(vkr_project_strings_push(job, out_paths, path));
    }
  }
  return true_v;
}

vkr_internal int vkr_project_compare_strings(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

bool8_t vkr_project_walk_files(VkrProjectJob *job, const char *directory,
                               VkrProjectStrings *out_paths,
                               bool8_t reject_links, const char *link_error) {
  MemZero(out_paths, sizeof(*out_paths));
  VKR_PROJECT_TRY(
      vkr_project_walk(job, directory, out_paths, reject_links, link_error));
  if (out_paths->count > 1u) {
    qsort((void *)out_paths->items, out_paths->count, sizeof(char *),
          vkr_project_compare_strings);
  }
  return true_v;
}

// =============================================================================
// Identity and hashing
// =============================================================================

vkr_internal void vkr_project_random(uint8_t *bytes, uint32_t count) {
#if defined(__APPLE__)
  arc4random_buf(bytes, count);
#elif defined(_WIN32)
  (void)BCryptGenRandom(NULL, bytes, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
  FILE *source = file_fopen("/dev/urandom", "rb");
  if (!source || fread(bytes, 1u, count, source) != count) {
    for (uint32_t i = 0u; i < count; ++i) {
      bytes[i] = (uint8_t)rand();
    }
  }
  if (source) {
    fclose(source);
  }
#endif
}

void vkr_project_uuid4(char out[37]) {
  uint8_t bytes[16];
  vkr_project_random(bytes, sizeof(bytes));
  bytes[6] = (uint8_t)((bytes[6] & 0x0Fu) | 0x40u);
  bytes[8] = (uint8_t)((bytes[8] & 0x3Fu) | 0x80u);
  (void)snprintf(out, 37,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x"
                 "%02x%02x",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                 bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                 bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool8_t vkr_project_identifier_valid(const char *value) {
  if (!value || strlen(value) != 36u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < 36u; ++i) {
    const char c = value[i];
    if (i == 8u || i == 13u || i == 18u || i == 23u) {
      if (c != '-') {
        return false_v;
      }
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_project_identifier(VkrProjectJob *job, const VkrBakeryJson *value,
                               const char **out) {
  if (!value || value->type != VKR_BAKERY_JSON_STRING) {
    return vkr_project_fail(job, "Missing scene identifier");
  }
  const char *text = (const char *)value->string.str;
  if (!vkr_project_identifier_valid(text)) {
    /* uuid.UUID accepts other spellings; only the canonical one is kept. */
    bool8_t hex_like = strlen(text) >= 32u;
    for (const char *c = text; *c && hex_like; ++c) {
      hex_like = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
                 (*c >= 'A' && *c <= 'F') || *c == '-' || *c == '{' ||
                 *c == '}';
    }
    return vkr_project_fail(
        job, hex_like ? "Scene identifier must be a canonical UUID"
                      : "Invalid scene identifier");
  }
  *out = text;
  return true_v;
}

uint32_t vkr_project_tier_arguments(const VkrProjectJob *job,
                                    const char **arguments) {
  arguments[0] = "--texture-tier";
  arguments[1] = job->texture_deferred  ? "deferred"
                 : job->texture_preview ? "preview"
                                        : "final";
  arguments[2] = "--texture-encoding";
  arguments[3] = vkr_project_texture_encoding_name(job);
  return 4u;
}

const char *vkr_project_texture_encoding_name(const VkrProjectJob *job) {
  static const char *const names[5] = {"uastc", "astc", "astc-fast", "bc",
                                       "bc-fast"};
  return names[job->texture_encoding];
}

const char *vkr_project_texture_suffix(const VkrProjectJob *job) {
  static const char *const suffixes[10] = {
      "",           "-preview",           "-astc", "-astc-preview",
      "-astc-fast", "-astc-fast-preview", "-bc",   "-bc-preview",
      "-bc-fast",   "-bc-fast-preview"};
  return suffixes[job->texture_encoding * 2u +
                  (job->texture_preview ? 1u : 0u)];
}

void vkr_project_mark_tier(VkrProjectJob *job, VkrBakeryJson *record) {
  if (job->texture_preview || job->texture_deferred) {
    vkr_bakery_json_set(job->arena, record, "texture_tier",
                        vkr_bakery_json_cstr(job->arena, job->texture_deferred
                                                             ? "deferred"
                                                             : "preview"));
  } else {
    (void)vkr_bakery_json_remove(record, "texture_tier");
  }
}

int64_t vkr_project_count_preview(const VkrBakeryJson *records) {
  int64_t count = 0;
  for (const VkrBakeryJson *item = records ? records->first : NULL; item;
       item = item->next) {
    const VkrBakeryJson *tier = vkr_bakery_json_get(item, "texture_tier");
    count += vkr_bakery_json_is_string(tier, "preview") ||
                     vkr_bakery_json_is_string(tier, "deferred")
                 ? 1
                 : 0;
  }
  return count;
}

VkrBakeryIndex *vkr_project_index(VkrProjectJob *job) {
  if (!job->index) {
    job->index = vkr_bakery_index_open(job->config->cache_dir);
  }
  return job->index;
}

bool8_t vkr_project_digest(VkrProjectJob *job, const char *path,
                           char out[VKR_BAKERY_SHA256_HEX]) {
  VkrBakeryIndex *index = vkr_project_index(job);
  if (!(index ? vkr_bakery_index_hash(index, path, out, NULL)
              : vkr_bakery_hash_file(path, out, NULL))) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            path);
  }
  return true_v;
}

void vkr_project_fingerprint(const uint8_t *data, uint64_t length,
                             char out[17]) {
  uint64_t value = 14695981039346656037ull;
  for (uint64_t i = 0u; i < length; ++i) {
    value = (value ^ data[i]) * 1099511628211ull;
  }
  (void)snprintf(out, 17, "%016llx", (unsigned long long)value);
}

void vkr_project_mesh_identity(const char *seed, const char *mesh_hash,
                               char out[17]) {
  uint64_t value = strtoull(seed, NULL, 16);
  const uint64_t mesh = strtoull(mesh_hash, NULL, 16);
  for (uint32_t i = 0u; i < 8u; ++i) {
    value = (value ^ ((mesh >> (8u * i)) & 0xFFu)) * 1099511628211ull;
  }
  (void)snprintf(out, 17, "%016llx", (unsigned long long)value);
}

// =============================================================================
// Paths
// =============================================================================

bool8_t vkr_project_is_relative_to(const char *path, const char *root) {
  const uint64_t length = strlen(root);
  if (length == 1u && root[0] == '/') {
    return path[0] == '/';
  }
  return strncmp(path, root, length) == 0 &&
         (path[length] == 0 || path[length] == '/');
}

bool8_t vkr_project_relpath(const char *path, const char *start, char *out,
                            uint32_t capacity) {
  /* os.path.relpath over absolute, normalized paths. */
  uint64_t common = 0u;
  uint64_t index = 0u;
  for (;; ++index) {
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

bool8_t vkr_project_resolve(const char *path, bool8_t strict, char *out,
                            uint32_t capacity) {
  char absolute[VKR_PROJECT_PATH];
  if (!vkr_bakery_path_absolute(path, absolute, sizeof(absolute))) {
    return false_v;
  }
  /* Resolve the longest existing prefix, then append the remainder. */
  char prefix[VKR_PROJECT_PATH];
  (void)snprintf(prefix, sizeof(prefix), "%s", absolute);
  uint64_t split = strlen(prefix);
  for (;;) {
    FilePath value = {
        .path = {.str = (uint8_t *)prefix, .length = strlen(prefix)}};
    char resolved[VKR_PROJECT_PATH];
    if (file_path_resolve(&value, resolved, sizeof(resolved)) ==
        FILE_ERROR_NONE) {
      vkr_bakery_path_portable(resolved);
      const char *rest = absolute + split;
      if (*rest && strict) {
        return false_v;
      }
      const int written = snprintf(out, capacity, "%s%s", resolved, rest);
      return written > 0 && (uint32_t)written < capacity;
    }
    if (strict) {
      return false_v;
    }
    char *slash = strrchr(prefix, '/');
    if (!slash || slash == prefix) {
      return (uint32_t)snprintf(out, capacity, "%s", absolute) < capacity;
    }
    *slash = 0;
    split = (uint64_t)(slash - prefix);
  }
}

bool8_t vkr_project_validate_managed_path(VkrProjectJob *job,
                                          const char *value) {
  if (!value) {
    return vkr_project_fail(job, "Invalid managed path: None");
  }
  // A `..` segment gets its own message; the shared grammar owns the rest.
  bool8_t dotdot = false_v;
  const char *part = value;
  for (const char *c = value;; ++c) {
    if (*c == '/' || *c == 0) {
      const uint64_t length = (uint64_t)(c - part);
      dotdot = dotdot || (length == 2u && part[0] == '.' && part[1] == '.');
      if (*c == 0) {
        break;
      }
      part = c + 1;
    }
  }
  if (dotdot) {
    return vkr_project_fail(job, "Managed path escapes its owner: %s", value);
  }
  if (!vkr_asset_path_managed_valid(
          (String8){.str = (uint8_t *)value, .length = strlen(value)})) {
    return vkr_project_fail(job, "Invalid managed path: '%s'", value);
  }
  return true_v;
}

bool8_t vkr_project_managed_reference(VkrProjectJob *job, const char *path,
                                      const char *owner, const char **out) {
  char relative[VKR_PROJECT_PATH];
  char absolute_path[VKR_PROJECT_PATH];
  char absolute_owner[VKR_PROJECT_PATH];
  if (!vkr_bakery_path_absolute(path, absolute_path, sizeof(absolute_path)) ||
      !vkr_bakery_path_absolute(owner, absolute_owner,
                                sizeof(absolute_owner)) ||
      !vkr_project_is_relative_to(absolute_path, absolute_owner) ||
      strcmp(absolute_path, absolute_owner) == 0) {
    return vkr_project_fail(job, "Managed path %s is outside owner %s", path,
                            owner);
  }
  (void)snprintf(relative, sizeof(relative), "%s",
                 absolute_path + strlen(absolute_owner) + 1u);
  VKR_PROJECT_TRY(vkr_project_validate_managed_path(job, relative));
  *out = vkr_project_strdup(job, relative);
  return true_v;
}

bool8_t vkr_project_contained(VkrProjectJob *job, const char *root,
                              const char *value, bool8_t must_exist,
                              char *out) {
  char resolved_root[VKR_PROJECT_PATH];
  if (!vkr_project_resolve(root, false_v, resolved_root,
                           sizeof(resolved_root))) {
    return vkr_project_fail(job, "Invalid owner %s", root);
  }
  VKR_PROJECT_TRY(vkr_project_validate_managed_path(job, value));
  char joined[VKR_PROJECT_PATH];
  if (!vkr_bakery_path_join(joined, sizeof(joined), resolved_root, value) ||
      !vkr_project_resolve(joined, must_exist, out, VKR_PROJECT_PATH)) {
    return vkr_project_fail(
        job,
        "Managed path open failed: '%s', owner %s: [Errno 2] No such file or "
        "directory: '%s'",
        value, resolved_root, joined);
  }
  if (!vkr_project_is_relative_to(out, resolved_root)) {
    return vkr_project_fail(job, "Managed path escapes its owner: %s", value);
  }
  return true_v;
}

bool8_t vkr_project_portable_identity(VkrProjectJob *job, const char *path,
                                      char *out) {
  char editor[VKR_PROJECT_PATH];
  (void)snprintf(editor, sizeof(editor), "%s/editor/bundles/1", job->workspace);
  const struct {
    const char *owner;
    const char *prefix;
  } owners[] = {{job->project_root, "project"}, {editor, "editor"}};
  for (uint32_t i = 0u; i < ArrayCount(owners); ++i) {
    char root[VKR_PROJECT_PATH];
    if (!vkr_project_resolve(owners[i].owner, false_v, root, sizeof(root)) ||
        !vkr_project_is_relative_to(path, root) || strcmp(path, root) == 0) {
      continue;
    }
    if ((uint32_t)snprintf(out, VKR_PROJECT_PATH, "%s/%s", owners[i].prefix,
                           path + strlen(root) + 1u) >= VKR_PROJECT_PATH) {
      break;
    }
    return true_v;
  }
  return vkr_project_fail(
      job, "Cannot package %s: it is outside the project and editor bundle",
      path);
}

bool8_t vkr_project_source_file(VkrProjectJob *job, const char *value,
                                char *out) {
  char expanded[VKR_PROJECT_PATH];
  if (value[0] == '~' && (value[1] == '/' || value[1] == 0)) {
    const char *home = getenv("HOME");
    (void)snprintf(expanded, sizeof(expanded), "%s%s", home ? home : "",
                   value + 1);
  } else {
    (void)snprintf(expanded, sizeof(expanded), "%s", value);
  }
#if defined(_WIN32)
  /* Drive-relative ("C:foo") and device paths are unsupported. */
  if ((((expanded[0] >= 'A' && expanded[0] <= 'Z') ||
        (expanded[0] >= 'a' && expanded[0] <= 'z')) &&
       expanded[1] == ':' && expanded[2] != '/' && expanded[2] != '\\') ||
      strncmp(expanded, "\\\\.\\", 4) == 0) {
    return vkr_project_fail(
        job,
        "Source file is unavailable: %s: Drive-relative and device paths "
        "are unsupported",
        value);
  }
#endif
  if (!vkr_project_resolve(expanded, true_v, out, VKR_PROJECT_PATH)) {
    return vkr_project_fail(
        job,
        "Source file is unavailable: %s: [Errno 2] No such file or directory: "
        "'%s'",
        value, expanded);
  }
  if (!vkr_bakery_is_file(out)) {
    return vkr_project_fail(job, "Source is not a regular file: %s", out);
  }
  FILE *file = file_fopen(out, "rb");
  if (file) {
    char head[64] = {0};
    const size_t read = fread(head, 1u, sizeof(head) - 1u, file);
    fclose(file);
    static const char lfs[] = "version https://git-lfs.github.com/spec/v1\n";
    if (read >= sizeof(lfs) - 1u &&
        MemCompare(head, lfs, sizeof(lfs) - 1u) == 0) {
      return vkr_project_fail(job,
                              "Source is a Git LFS pointer, not asset data: "
                              "%s. Run git lfs pull in "
                              "the source repository, then import again.",
                              out);
    }
  }
  return true_v;
}

void vkr_project_suffix_lower(const char *path, char *out, uint32_t capacity) {
  const char *name = vkr_bakery_path_name(path);
  const char *dot = strrchr(name, '.');
  out[0] = 0;
  /* Path.suffix: a leading dot alone is part of the name. */
  if (!dot || dot == name || dot[1] == 0) {
    return;
  }
  uint32_t i = 0u;
  for (; dot[i] && i + 1u < capacity; ++i) {
    const char c = dot[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[i] = 0;
}

void vkr_project_stem(const char *path, char *out, uint32_t capacity) {
  const char *name = vkr_bakery_path_name(path);
  const char *dot = strrchr(name, '.');
  int length = (int)strlen(name);
  if (dot && dot != name && dot[1] != 0) {
    length = (int)(dot - name);
  }
  (void)snprintf(out, capacity, "%.*s", length, name);
}

bool8_t vkr_project_legacy_source(VkrProjectJob *job, const char *value,
                                  const char *origin, const char *legacy_root,
                                  char *out) {
  if (!value || !value[0]) {
    return vkr_project_fail(job, "Missing legacy asset path");
  }
  char raw[VKR_PROJECT_PATH];
  (void)snprintf(raw, sizeof(raw), "%s", value);
  char *query = strchr(raw, '?');
  if (query) {
    *query = 0;
  }
  char candidates[4][VKR_PROJECT_PATH];
  uint32_t count = 0u;
  if (vkr_bakery_path_is_absolute(raw)) {
    (void)snprintf(candidates[count++], VKR_PROJECT_PATH, "%s", raw);
  } else {
    (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH, origin,
                               raw);
    (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH,
                               legacy_root, raw);
  }
  char suffix[16];
  vkr_project_suffix_lower(raw, suffix, sizeof(suffix));
  if (!strcmp(suffix, ".png") || !strcmp(suffix, ".jpg") ||
      !strcmp(suffix, ".jpeg") || !strcmp(suffix, ".bmp") ||
      !strcmp(suffix, ".tga")) {
    const uint32_t base = count;
    for (uint32_t i = 0u; i < base; ++i) {
      if (!vkr_bakery_is_file(candidates[i])) {
        (void)snprintf(candidates[count++], VKR_PROJECT_PATH, "%s.vkt",
                       candidates[i]);
      }
    }
  }
  char matches[4][VKR_PROJECT_PATH];
  uint32_t match_count = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    char resolved[VKR_PROJECT_PATH];
    if (!vkr_project_resolve(candidates[i], true_v, resolved,
                             sizeof(resolved)) ||
        !vkr_bakery_is_file(resolved)) {
      continue;
    }
    bool8_t seen = false_v;
    for (uint32_t m = 0u; m < match_count; ++m) {
      seen = seen || strcmp(matches[m], resolved) == 0;
    }
    if (!seen) {
      (void)snprintf(matches[match_count++], VKR_PROJECT_PATH, "%s", resolved);
    }
  }
  if (match_count != 1u) {
    return vkr_project_fail(
        job, "Legacy dependency is missing or ambiguous: %s", value);
  }
  (void)snprintf(out, VKR_PROJECT_PATH, "%s", matches[0]);
  return true_v;
}

void vkr_project_gltf_texture_source(const char *value, const char *origin,
                                     const char *legacy_root, char *out) {
  char candidates[5][VKR_PROJECT_PATH];
  uint32_t count = 0u;
  (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH, origin,
                             value);
  char assets[VKR_PROJECT_PATH];
  (void)vkr_bakery_path_join(assets, sizeof(assets), legacy_root, "assets");
  char resolved_origin[VKR_PROJECT_PATH];
  char resolved_assets[VKR_PROJECT_PATH];
  const bool8_t repository_model =
      vkr_project_resolve(origin, false_v, resolved_origin,
                          sizeof(resolved_origin)) &&
      vkr_project_resolve(assets, false_v, resolved_assets,
                          sizeof(resolved_assets)) &&
      vkr_project_is_relative_to(resolved_origin, resolved_assets);
  if (repository_model) {
    char textures[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(textures, sizeof(textures), assets, "textures");
    (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH, assets,
                               value);
    (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH, textures,
                               value);
    if (strncmp(value, "objects/", 8u) == 0 && value[8]) {
      (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH,
                                 textures, value + 8);
    }
    (void)vkr_bakery_path_join(candidates[count++], VKR_PROJECT_PATH, textures,
                               vkr_bakery_path_name(value));
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (vkr_bakery_is_file(candidates[i])) {
      (void)snprintf(out, VKR_PROJECT_PATH, "%s", candidates[i]);
      return;
    }
    char cooked[VKR_PROJECT_PATH];
    (void)snprintf(cooked, sizeof(cooked), "%s.vkt", candidates[i]);
    if (vkr_bakery_is_file(cooked)) {
      (void)snprintf(out, VKR_PROJECT_PATH, "%s", cooked);
      return;
    }
  }
  (void)snprintf(out, VKR_PROJECT_PATH, "%s", candidates[0]);
}

vkr_internal int32_t vkr_project_hex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

vkr_internal bool8_t vkr_project_utf8_valid(const uint8_t *text,
                                            uint64_t length) {
  for (uint64_t i = 0u; i < length;) {
    const uint8_t c = text[i];
    uint32_t extra = c < 0x80u              ? 0u
                     : (c & 0xE0u) == 0xC0u ? 1u
                     : (c & 0xF0u) == 0xE0u ? 2u
                     : (c & 0xF8u) == 0xF0u ? 3u
                                            : 4u;
    if (extra == 4u || i + extra >= length + (extra ? 0u : 1u)) {
      if (extra == 4u || i + extra > length - 1u + (extra ? 0u : 1u)) {
        return false_v;
      }
    }
    for (uint32_t k = 1u; k <= extra; ++k) {
      if (i + k >= length || (text[i + k] & 0xC0u) != 0x80u) {
        return false_v;
      }
    }
    i += extra + 1u;
  }
  return true_v;
}

bool8_t vkr_project_gltf_uri_path(VkrProjectJob *job, const char *uri,
                                  char *out, uint32_t capacity) {
  /* urllib.parse.urlsplit: a scheme is [A-Za-z][A-Za-z0-9+.-]* before ':'. */
  const char *rest = uri;
  const char *colon = strchr(uri, ':');
  if (colon && colon > uri &&
      ((uri[0] >= 'A' && uri[0] <= 'Z') || (uri[0] >= 'a' && uri[0] <= 'z'))) {
    bool8_t scheme = true_v;
    for (const char *c = uri; c < colon; ++c) {
      scheme =
          scheme &&
          ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
           (*c >= '0' && *c <= '9') || *c == '+' || *c == '-' || *c == '.');
    }
    if (scheme) {
      return vkr_project_fail(job, "Remote or absolute URI dependencies are "
                                   "unsupported; download them before "
                                   "importing");
    }
  }
  if (rest[0] == '/' && rest[1] == '/') {
    return vkr_project_fail(job, "Remote or absolute URI dependencies are "
                                 "unsupported; download them before "
                                 "importing");
  }
  if (strchr(rest, '?') || strchr(rest, '#')) {
    return vkr_project_fail(
        job, "glTF dependency URI has unsupported query or fragment: %s", uri);
  }
  uint32_t length = 0u;
  for (const char *c = rest; *c; ++c) {
    char decoded = *c;
    if (c[0] == '%' && vkr_project_hex(c[1]) >= 0 &&
        vkr_project_hex(c[2]) >= 0) {
      decoded = (char)(vkr_project_hex(c[1]) * 16 + vkr_project_hex(c[2]));
      c += 2;
    }
    if (length + 1u >= capacity) {
      return vkr_project_fail(job, "glTF dependency URI is too long");
    }
    out[length++] = decoded;
  }
  out[length] = 0;
  if (!vkr_project_utf8_valid((const uint8_t *)out, length)) {
    return vkr_project_fail(job, "Invalid UTF-8 glTF dependency URI: %s", uri);
  }
  return true_v;
}

bool8_t vkr_project_model_tokens(VkrProjectJob *job, const char *line,
                                 VkrProjectStrings *out_tokens) {
  MemZero(out_tokens, sizeof(*out_tokens));
  char token[VKR_PROJECT_PATH];
  uint32_t length = 0u;
  char quote = 0;
  for (const char *c = line; *c; ++c) {
    if (quote) {
      if (*c == quote) {
        quote = 0;
      } else if (length + 1u < sizeof(token)) {
        token[length++] = *c;
      }
    } else if ((*c == '"' || *c == '\'') && length == 0u) {
      quote = *c;
    } else if (*c == '#') {
      break;
    } else if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n' ||
               *c == '\v' || *c == '\f') {
      if (length) {
        token[length] = 0;
        VKR_PROJECT_TRY(vkr_project_strings_push(job, out_tokens, token));
        length = 0u;
      }
    } else if (length + 1u < sizeof(token)) {
      token[length++] = *c;
    }
  }
  if (quote) {
    return vkr_project_fail(job, "Unterminated quote in OBJ/MTL filename");
  }
  if (length) {
    token[length] = 0;
    VKR_PROJECT_TRY(vkr_project_strings_push(job, out_tokens, token));
  }
  return true_v;
}

// =============================================================================
// Files
// =============================================================================

bool8_t vkr_project_lstat_is_link(const char *path) {
#if defined(_WIN32)
  wchar_t native[32768];
  FilePath value = {.path = {.str = (uint8_t *)path, .length = strlen(path)}};
  if (!file_windows_native_path(&value, native)) {
    return false_v;
  }
  const DWORD attributes = GetFileAttributesW(native);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  struct stat info;
  return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

bool8_t vkr_project_exists(const char *path) {
  VkrBakeryStat info;
  return vkr_project_lstat_is_link(path) ||
         (vkr_bakery_stat(path, &info) && info.exists);
}

VkrBakeryJson *vkr_project_load_json(VkrProjectJob *job, const char *path,
                                     uint64_t limit) {
  const char *name = vkr_bakery_path_name(path);
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists || info.is_directory ||
      info.size > limit) {
    vkr_project_fail(job, "Missing or oversized JSON: %s", name);
    return NULL;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, limit, &data, &length)) {
    vkr_project_fail(job, "Missing or oversized JSON: %s", name);
    return NULL;
  }
  VkrBakeryJsonError error;
  VkrBakeryJson *value =
      vkr_bakery_json_parse_ex(job->arena, data, length, 1024u,
                               VKR_BAKERY_JSON_REJECT_DUPLICATES, &error);
  free(data);
  if (!value) {
    if (strcmp(error.message, "duplicate JSON member") == 0) {
      vkr_project_fail(job, "Duplicate JSON member in %s", name);
    } else {
      vkr_project_fail(job, "Invalid JSON %s: %s: line %u column %u", name,
                       error.message, error.line, error.column);
    }
    return NULL;
  }
  if (value->type != VKR_BAKERY_JSON_OBJECT) {
    vkr_project_fail(job, "Expected a JSON object: %s", name);
    return NULL;
  }
  return value;
}

vkr_internal bool8_t vkr_project_document_depth(VkrProjectJob *job,
                                                const VkrBakeryJson *value,
                                                uint32_t depth) {
  if (value->type == VKR_BAKERY_JSON_OBJECT ||
      value->type == VKR_BAKERY_JSON_ARRAY) {
    depth += 1u;
    if (depth > 32u) {
      return vkr_project_fail(
          job,
          "Managed document exceeds the project-store nesting limit of 32");
    }
  }
  for (const VkrBakeryJson *child = value->first; child; child = child->next) {
    if (value->type == VKR_BAKERY_JSON_OBJECT && child->key.length > 255u) {
      return vkr_project_fail(job, "Managed document key exceeds the "
                                   "project-store limit of 255 UTF-8 bytes");
    }
    VKR_PROJECT_TRY(vkr_project_document_depth(job, child, depth));
  }
  return true_v;
}

bool8_t vkr_project_validate_document(VkrProjectJob *job,
                                      const VkrBakeryJson *value,
                                      uint64_t limit) {
  String8 text = {0};
  const uint64_t mark = arena_pos(job->arena);
  if (!vkr_bakery_json_write(job->arena, value, VKR_BAKERY_JSON_PRETTY,
                             &text)) {
    return vkr_project_fail(job, "Out of range float values are not JSON "
                                 "compliant");
  }
  const uint64_t size = text.length + 1u;
  arena_reset_to(job->arena, mark, ARENA_MEMORY_TAG_STRING);
  if (size > limit) {
    return vkr_project_fail(job,
                            "Managed document exceeds the %llu MiB "
                            "project-store limit; split the scene into smaller "
                            "scenes",
                            (unsigned long long)(limit / (1024u * 1024u)));
  }
  return vkr_project_document_depth(job, value, 0u);
}

bool8_t vkr_project_atomic_json(VkrProjectJob *job, const char *path,
                                const VkrBakeryJson *value) {
  if (strcmp(vkr_bakery_path_name(path), "scene.json") == 0) {
    int64_t version = 0;
    if (vkr_bakery_json_get_int(value, "version", &version) &&
        (version == 3 || version == 4 ||
         version == VKR_PROJECT_SCENE_VERSION)) {
      VKR_PROJECT_TRY(vkr_project_validate_document(
          job, value, VKR_PROJECT_MAX_DOCUMENT_BYTES));
    }
  }
  const uint64_t mark = arena_pos(job->arena);
  String8 text = {0};
  if (!vkr_bakery_json_write(job->arena, value, VKR_BAKERY_JSON_PRETTY,
                             &text)) {
    return vkr_project_fail(job, "Out of range float values are not JSON "
                                 "compliant");
  }
  char directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  VkrBakeryBuffer buffer = {0};
  vkr_bakery_buffer_append(&buffer, text.str, text.length);
  vkr_bakery_buffer_append(&buffer, "\n", 1u);
  const bool8_t ok =
      !buffer.failed &&
      (!directory[0] || vkr_bakery_make_directories(directory)) &&
      vkr_bakery_write_file_atomic(path, buffer.data, buffer.length);
  vkr_bakery_buffer_free(&buffer);
  arena_reset_to(job->arena, mark, ARENA_MEMORY_TAG_STRING);
  if (!ok) {
    return vkr_project_fail(job, "Cannot write %s", path);
  }
  /* Cleanup checks cancel a job right after one named document commits. */
  const char *fault = getenv("VKR_BAKERY_FAULT_CANCEL_AFTER");
  if (fault && strcmp(fault, path) == 0) {
    vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELAXED);
    return vkr_project_check_cancel(job);
  }
  return true_v;
}

bool8_t vkr_project_read_text(VkrProjectJob *job, const char *path,
                              uint64_t limit, const char **out,
                              uint64_t *out_length) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(path, limit, &data, &length)) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            path);
  }
  char *copy =
      (char *)arena_alloc(job->arena, length + 1u, ARENA_MEMORY_TAG_STRING);
  if (!copy) {
    free(data);
    return vkr_project_fail(job, "Out of memory");
  }
  MemCopy(copy, data, length + 1u);
  free(data);
  *out = copy;
  if (out_length) {
    *out_length = length;
  }
  return true_v;
}

bool8_t vkr_project_write_text(VkrProjectJob *job, const char *path,
                               const char *text, uint64_t length) {
  char directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  FILE *file = NULL;
  if ((directory[0] && !vkr_bakery_make_directories(directory)) ||
      !(file = file_fopen(path, "wb"))) {
    return vkr_project_fail(job, "Cannot write %s", path);
  }
  const bool8_t ok = fwrite(text, 1u, (size_t)length, file) == length;
  return fclose(file) == 0 && ok
             ? true_v
             : vkr_project_fail(job, "Cannot write %s", path);
}

bool8_t vkr_project_make_dirs(VkrProjectJob *job, const char *path) {
  return vkr_bakery_make_directories(path)
             ? true_v
             : vkr_project_fail(job, "Cannot create directory %s", path);
}

/* Copies `source` to `destination`; with `link`, a volume that cannot clone
   hard links it instead of copying the bytes. */
vkr_internal bool8_t vkr_project_place_file(VkrProjectJob *job,
                                            const char *source,
                                            const char *destination,
                                            bool8_t link) {
  char resolved[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_source_file(job, source, resolved));
  VkrBakeryStat info;
  (void)vkr_bakery_stat(resolved, &info);
  if (job->files_copied >= VKR_PROJECT_MAX_IMPORT_FILES ||
      job->bytes_copied + info.size > VKR_PROJECT_MAX_IMPORT_BYTES) {
    return vkr_project_fail(job,
                            "Import exceeds 16,384 files or 8 GiB; split the "
                            "import");
  }
  /* The copy is the hashed bytes when the source keeps its size,
     modification time and identity from before hashing until after the
     copy; the index already trusts that for every cached key. */
  char before[VKR_BAKERY_SHA256_HEX];
  char parent[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_digest(job, resolved, before));
  vkr_bakery_path_parent(parent, sizeof(parent), destination);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, parent));
  const bool8_t placed =
      link ? vkr_bakery_clone_link_or_copy(resolved, destination)
           : vkr_bakery_clone_or_copy(resolved, destination);
  if (!placed) {
    return vkr_project_fail(job, "Cannot copy %s", resolved);
  }
  VkrBakeryStat source_after;
  if (!vkr_bakery_stat(resolved, &source_after) || !source_after.exists ||
      source_after.size != info.size ||
      source_after.mtime_ns != info.mtime_ns ||
      source_after.file_id != info.file_id ||
      source_after.device != info.device) {
    (void)vkr_bakery_remove_file(destination);
    return vkr_project_fail(job, "Source changed while copying: %s",
                            vkr_bakery_path_name(resolved));
  }
  if (vkr_project_index(job)) {
    vkr_bakery_index_record(job->index, destination, before);
  }
  job->files_copied += 1u;
  job->bytes_copied += info.size;
  return true_v;
}

bool8_t vkr_project_copy_file(VkrProjectJob *job, const char *source,
                              const char *destination) {
  return vkr_project_place_file(job, source, destination, false_v);
}

/* The content name of a blob: the digest of `resolved` and its lowercase
   extension. Fails the job for an extension a managed name cannot hold. */
vkr_internal bool8_t vkr_project_blob_name(VkrProjectJob *job,
                                           const char *resolved,
                                           char hash[VKR_BAKERY_SHA256_HEX],
                                           char *name, uint32_t capacity) {
  char extension[32];
  vkr_project_suffix_lower(resolved, extension, sizeof(extension));
  bool8_t valid = strlen(extension) <= 16u;
  for (const char *c = extension; *c && valid; ++c) {
    valid = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            (*c >= 'A' && *c <= 'Z') || *c == '.';
  }
  if (!valid) {
    return vkr_project_fail(job, "Unsupported asset filename: %s",
                            vkr_bakery_path_name(resolved));
  }
  VKR_PROJECT_TRY(vkr_project_digest(job, resolved, hash));
  return (uint32_t)snprintf(name, capacity, "%s%s", hash, extension) < capacity
             ? true_v
             : vkr_project_fail(job, "Path too long");
}

bool8_t vkr_project_copy_blob(VkrProjectJob *job, const char *source,
                              const char *directory, char *out) {
  char resolved[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_source_file(job, source, resolved));
  char hash[VKR_BAKERY_SHA256_HEX];
  char name[128];
  VKR_PROJECT_TRY(
      vkr_project_blob_name(job, resolved, hash, name, sizeof(name)));
  char extension[32];
  vkr_project_suffix_lower(resolved, extension, sizeof(extension));
  if (!vkr_bakery_json_get(job->source_names, hash)) {
    char stem[512];
    vkr_project_stem(resolved, stem, sizeof(stem));
    vkr_bakery_json_set(job->arena, job->source_names, hash,
                        vkr_bakery_json_cstr(job->arena, stem));
  }
  char candidate[VKR_PROJECT_PATH];
  (void)snprintf(candidate, sizeof(candidate), "%s.vkt", resolved);
  if (strcmp(extension, ".vkt") != 0 && vkr_bakery_is_file(candidate)) {
    vkr_bakery_json_set(job->arena, job->texture_seeds, hash,
                        vkr_bakery_json_cstr(job->arena, candidate));
  }
  if (!vkr_bakery_path_join(out, VKR_PROJECT_PATH, directory, name)) {
    return vkr_project_fail(job, "Path too long");
  }
  /* A blob is named by its content and never rewritten. Where the volume
     cannot clone, one the workspace already owns (a snapshot, a cached or
     published build file, all published by rename) is hard linked rather
     than copied; user files are always copied, since their editors may
     rewrite them in place (ADR-077). */
  if (!vkr_project_exists(out)) {
    VKR_PROJECT_TRY(vkr_project_place_file(
        job, resolved, out,
        vkr_project_is_relative_to(resolved, job->workspace)));
  }
  return true_v;
}

typedef struct VkrProjectBlobPlacement {
  const char *source; /* Resolved source whose stat bounds the copy. */
  const char *origin; /* File placed: the source or a workspace blob. */
  char destination[VKR_PROJECT_PATH];
  char hash[VKR_BAKERY_SHA256_HEX];
  VkrBakeryStat before;
  bool8_t link;
  bool8_t placed;
} VkrProjectBlobPlacement;

typedef struct VkrProjectBlobPrefetch {
  VkrProjectBlobPlacement *items;
  uint32_t count;
  VkrAtomicUint32 next;
} VkrProjectBlobPrefetch;

vkr_internal void *vkr_project_prefetch_worker(void *argument) {
  VkrProjectBlobPrefetch *prefetch = argument;
  for (;;) {
    const uint32_t i = vkr_atomic_uint32_fetch_add(&prefetch->next, 1u,
                                                   VKR_MEMORY_ORDER_RELAXED);
    if (i >= prefetch->count) {
      return NULL;
    }
    VkrProjectBlobPlacement *item = &prefetch->items[i];
    item->placed =
        item->link
            ? vkr_bakery_clone_link_or_copy(item->origin, item->destination)
            : vkr_bakery_clone_or_copy(item->origin, item->destination);
  }
}

typedef struct VkrProjectSnapshotDirectories {
  VkrProjectJob *job;
  VkrProjectStrings *out;
  const char *parent;
  uint32_t depth;
} VkrProjectSnapshotDirectories;

/* Collects `<scene>/sources/<revision>/dependencies` below the project's
   scenes: blob directories whose files the workspace owns. */
vkr_internal bool8_t vkr_project_visit_snapshot_directory(void *context,
                                                          const char *name,
                                                          bool8_t directory) {
  VkrProjectSnapshotDirectories *walk = context;
  char path[VKR_PROJECT_PATH];
  if (!directory ||
      !vkr_bakery_path_join(path, sizeof(path), walk->parent, name)) {
    return true_v;
  }
  if (walk->depth == 0u) {
    char sources[VKR_PROJECT_PATH];
    VkrProjectSnapshotDirectories revisions = {walk->job, walk->out, sources,
                                               1u};
    if (vkr_bakery_path_join(sources, sizeof(sources), path, "sources")) {
      (void)vkr_bakery_list_directory(
          sources, vkr_project_visit_snapshot_directory, &revisions);
    }
    return true_v;
  }
  char dependencies[VKR_PROJECT_PATH];
  if (vkr_bakery_path_join(dependencies, sizeof(dependencies), path,
                           "dependencies") &&
      vkr_bakery_is_directory(dependencies)) {
    (void)vkr_project_strings_push(walk->job, walk->out, dependencies);
  }
  return true_v;
}

bool8_t vkr_project_prefetch_blobs(VkrProjectJob *job,
                                   const char *const *sources, uint32_t count,
                                   const char *directory) {
  VkrProjectBlobPlacement *items =
      count ? (VkrProjectBlobPlacement *)calloc(count, sizeof(*items)) : NULL;
  if (!items) {
    return count == 0u;
  }
  /* A user file another snapshot of this project already holds is linked
     from that workspace copy, so a re-import writes no bytes. */
  VkrProjectStrings snapshots = {0};
  char scenes[VKR_PROJECT_PATH];
  if (vkr_bakery_path_join(scenes, sizeof(scenes), job->project_root,
                           "scenes")) {
    VkrProjectSnapshotDirectories walk = {job, &snapshots, scenes, 0u};
    (void)vkr_bakery_list_directory(
        scenes, vkr_project_visit_snapshot_directory, &walk);
  }

  uint32_t item_count = 0u;
  uint64_t files = job->files_copied;
  uint64_t bytes = job->bytes_copied;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrProjectBlobPlacement *item = &items[item_count];
    char name[128];
    const bool8_t failed_before = job->failed;
    if (!vkr_bakery_stat(sources[i], &item->before) || !item->before.exists ||
        !vkr_project_blob_name(job, sources[i], item->hash, name,
                               sizeof(name)) ||
        !vkr_bakery_path_join(item->destination, sizeof(item->destination),
                              directory, name)) {
      /* vkr_project_copy_blob reports it; this pass only prefetches. */
      vkr_project_forgive(job, failed_before);
      continue;
    }
    bool8_t duplicate = vkr_project_exists(item->destination);
    for (uint32_t j = 0u; j < item_count && !duplicate; ++j) {
      duplicate = strcmp(items[j].destination, item->destination) == 0;
    }
    if (duplicate) {
      continue;
    }
    files += 1u;
    bytes += item->before.size;
    if (files > VKR_PROJECT_MAX_IMPORT_FILES ||
        bytes > VKR_PROJECT_MAX_IMPORT_BYTES) {
      break;
    }
    item->source = sources[i];
    item->origin = sources[i];
    item->link = vkr_project_is_relative_to(sources[i], job->workspace);
    for (uint32_t s = 0u; s < snapshots.count && !item->link; ++s) {
      char existing[VKR_PROJECT_PATH];
      if (vkr_bakery_path_join(existing, sizeof(existing), snapshots.items[s],
                               name) &&
          vkr_bakery_is_file(existing)) {
        item->origin = vkr_project_strdup(job, existing);
        item->link = item->origin != NULL;
        item->origin = item->link ? item->origin : sources[i];
      }
    }
    ++item_count;
  }

  bool8_t ok = item_count == 0u || vkr_project_make_dirs(job, directory);
  VkrProjectBlobPrefetch prefetch = {.items = items, .count = item_count};
  Arena *arena = ok && item_count > 1u ? arena_create(KB(64), KB(64)) : NULL;
  VkrAllocator allocator = {.ctx = arena};
  VkrThread workers[15] = {0};
  uint32_t started = 0u;
  if (arena && vkr_allocator_arena(&allocator)) {
    const uint32_t extra = Min(Min(vkr_bakery_logical_cores(), item_count),
                               (uint32_t)ArrayCount(workers) + 1u) -
                           1u;
    for (uint32_t i = 0u; i < extra; ++i) {
      if (vkr_thread_create(&allocator, &workers[started],
                            vkr_project_prefetch_worker, &prefetch)) {
        ++started;
      }
    }
  }
  if (ok) {
    (void)vkr_project_prefetch_worker(&prefetch);
  }
  for (uint32_t i = 0u; i < started; ++i) {
    (void)vkr_thread_join(workers[i]);
    (void)vkr_thread_destroy(&allocator, &workers[i]);
  }
  if (arena) {
    arena_destroy(arena);
  }

  /* As vkr_project_place_file: a copy is the hashed bytes only when its
     source kept its stat across the copy. */
  for (uint32_t i = 0u; ok && i < item_count; ++i) {
    const VkrProjectBlobPlacement *item = &items[i];
    if (!item->placed) {
      continue;
    }
    VkrBakeryStat after;
    if (!vkr_bakery_stat(item->source, &after) || !after.exists ||
        after.size != item->before.size ||
        after.mtime_ns != item->before.mtime_ns ||
        after.file_id != item->before.file_id ||
        after.device != item->before.device) {
      for (uint32_t j = i; j < item_count; ++j) {
        if (items[j].placed) {
          (void)vkr_bakery_remove_file(items[j].destination);
        }
      }
      ok = vkr_project_fail(job, "Source changed while copying: %s",
                            vkr_bakery_path_name(item->source));
      break;
    }
    if (vkr_project_index(job)) {
      vkr_bakery_index_record(job->index, item->destination, item->hash);
    }
    job->files_copied += 1u;
    job->bytes_copied += item->before.size;
  }
  free(items);
  return ok;
}

bool8_t vkr_project_mkdtemp(VkrProjectJob *job, const char *parent,
                            const char *prefix, char *out) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
  for (uint32_t attempt = 0u; attempt < 100u; ++attempt) {
    uint8_t random[8];
    vkr_project_random(random, sizeof(random));
    char suffix[9];
    for (uint32_t i = 0u; i < 8u; ++i) {
      suffix[i] = alphabet[random[i] % (sizeof(alphabet) - 1u)];
    }
    suffix[8] = 0;
    char name[512];
    (void)snprintf(name, sizeof(name), "%s%s", prefix, suffix);
    if (!vkr_bakery_path_join(out, VKR_PROJECT_PATH, parent, name)) {
      return vkr_project_fail(job, "Path too long");
    }
    FilePath path = {.path = {.str = (uint8_t *)out, .length = strlen(out)}};
    const FileError error = file_create_directory_exclusive(&path);
    if (error == FILE_ERROR_NONE) {
      return true_v;
    }
    if (error != FILE_ERROR_ALREADY_EXISTS) {
      break;
    }
  }
  return vkr_project_fail(job, "Cannot create a staging directory in %s",
                          parent);
}

bool8_t vkr_project_publish_directory(VkrProjectJob *job, const char *source,
                                      const char *destination) {
  /* The rename is exclusive (RENAME_EXCL, MoveFileEx without replace), so a
     revision another writer published, even an empty reservation, is never
     replaced. */
  if (!vkr_bakery_rename(source, destination, false_v)) {
    return vkr_project_fail(job, "[Errno 17] File exists: '%s'", destination);
  }
  /* The published files keep their identity; later digests reuse the
     staged hashes instead of reading the revision again. */
  vkr_bakery_index_move_tree(job->index, source, destination);
  return true_v;
}

typedef struct VkrProjectRemoveNames {
  char **names;
  uint32_t count;
  uint32_t capacity;
  bool8_t failed;
} VkrProjectRemoveNames;

vkr_internal bool8_t vkr_project_remove_visit(void *context, const char *name,
                                              bool8_t is_directory) {
  (void)is_directory;
  VkrProjectRemoveNames *list = (VkrProjectRemoveNames *)context;
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
    char **grown = (char **)realloc(list->names, capacity * sizeof(char *));
    if (!grown) {
      list->failed = true_v;
      return false_v;
    }
    list->names = grown;
    list->capacity = capacity;
  }
  const uint64_t length = strlen(name) + 1u;
  char *copy = (char *)malloc(length);
  if (!copy) {
    list->failed = true_v;
    return false_v;
  }
  MemCopy(copy, name, length);
  list->names[list->count++] = copy;
  return true_v;
}

uint64_t vkr_project_remove_tree(const char *path) {
  if (vkr_project_lstat_is_link(path)) {
    (void)vkr_bakery_remove_file(path);
    return 0u;
  }
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return 0u;
  }
  if (!info.is_directory) {
    (void)vkr_bakery_remove_file(path);
    return info.size;
  }
  /* Collect names first: removing entries while listing is not portable. */
  uint64_t freed = 0u;
  VkrProjectRemoveNames list = {0};
  (void)vkr_bakery_list_directory(path, vkr_project_remove_visit, &list);
  for (uint32_t i = 0u; i < list.count; ++i) {
    char child[VKR_PROJECT_PATH];
    if (vkr_bakery_path_join(child, sizeof(child), path, list.names[i])) {
      freed += vkr_project_remove_tree(child);
    }
    free(list.names[i]);
  }
  free(list.names);
#if defined(_WIN32)
  (void)vkr_bakery_remove_tree(path);
#else
  (void)rmdir(path);
#endif
  return freed;
}

bool8_t vkr_project_older_than(const char *path, int64_t seconds, int64_t now) {
#if defined(_WIN32)
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return false_v;
  }
  return now - info.mtime_ns / 1000000000LL > seconds;
#else
  struct stat info;
  if (lstat(path, &info) != 0) {
    return false_v;
  }
#if defined(__APPLE__)
  const float64_t mtime = (float64_t)info.st_mtimespec.tv_sec +
                          (float64_t)info.st_mtimespec.tv_nsec * 1e-9;
#else
  const float64_t mtime =
      (float64_t)info.st_mtim.tv_sec + (float64_t)info.st_mtim.tv_nsec * 1e-9;
#endif
  return (float64_t)now - mtime > (float64_t)seconds;
#endif
}

// =============================================================================
// Managed documents
// =============================================================================

VkrBakeryJson *vkr_project_record_by_id(const VkrBakeryJson *records,
                                        const char *id) {
  for (VkrBakeryJson *record = records ? records->first : NULL; record;
       record = record->next) {
    const char *value = vkr_project_json_text(record, "id");
    if (value && id && strcmp(value, id) == 0) {
      return record;
    }
  }
  return NULL;
}

void vkr_project_remove_record(VkrBakeryJson *records,
                               const VkrBakeryJson *record) {
  VkrBakeryJson *previous = NULL;
  for (VkrBakeryJson *item = records ? records->first : NULL; item;
       item = item->next) {
    if (item == record) {
      if (previous) {
        previous->next = item->next;
      } else {
        records->first = item->next;
      }
      if (records->last == item) {
        records->last = previous;
      }
      records->count -= 1u;
      return;
    }
    previous = item;
  }
}

VkrBakeryJson *vkr_project_reference(VkrProjectJob *job, const char *scope,
                                     const char *id, const char *role) {
  Arena *arena = job->arena;
  VkrBakeryJson *reference = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, reference, "scope",
                      vkr_bakery_json_cstr(arena, scope));
  vkr_bakery_json_set(arena, reference, "id", vkr_bakery_json_cstr(arena, id));
  if (role) {
    vkr_bakery_json_set(arena, reference, "role",
                        vkr_bakery_json_cstr(arena, role));
  }
  return reference;
}

VkrBakeryJson *vkr_project_identity_transform(VkrProjectJob *job) {
  static const char text[] =
      "{\"pos\": [0, 0, 0], \"rot\": [0, 0, 0, 1], \"scale\": [1, 1, 1]}";
  return vkr_bakery_json_parse(job->arena, (const uint8_t *)text,
                               sizeof(text) - 1u, 8u, NULL);
}

bool8_t vkr_project_document_entity_ids(VkrProjectJob *job,
                                        const VkrBakeryJson *entities,
                                        VkrBakeryJson **out_ids) {
  VkrBakeryJson *ids = vkr_bakery_json_object(job->arena);
  int64_t index = 0;
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next, ++index) {
    const char *value = entity->type == VKR_BAKERY_JSON_OBJECT
                            ? vkr_project_json_text(entity, "id")
                            : NULL;
    if (!value || !vkr_project_identifier_valid(value) ||
        vkr_bakery_json_get(ids, value)) {
      return vkr_project_fail(job,
                              "Every entity needs a unique canonical UUID id");
    }
    vkr_bakery_json_set(job->arena, ids, value,
                        vkr_bakery_json_int(job->arena, index));
  }
  if (out_ids) {
    *out_ids = ids;
  }
  return true_v;
}

bool8_t vkr_project_document_to_internal(VkrProjectJob *job,
                                         VkrBakeryJson *scene) {
  VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  if (entities && entities->type != VKR_BAKERY_JSON_ARRAY) {
    return vkr_project_fail(job, "Invalid entity array");
  }
  VkrBakeryJson *ids = NULL;
  VKR_PROJECT_TRY(vkr_project_document_entity_ids(job, entities, &ids));
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    VkrBakeryJson *components = vkr_bakery_json_get(entity, "components");
    if (components && components->type == VKR_BAKERY_JSON_OBJECT) {
      for (uint32_t b = 0u; b < ArrayCount(vkr_project_document_blocks); ++b) {
        const char *block = vkr_project_document_blocks[b];
        VkrBakeryJson *value = vkr_bakery_json_get(components, block);
        if (!value) {
          continue;
        }
        if (vkr_bakery_json_get(entity, block)) {
          return vkr_project_fail(
              job, "An entity has both a %s block and component", block);
        }
        vkr_bakery_json_remove(components, block);
        vkr_bakery_json_set(job->arena, entity, block, value);
      }
      if (!components->count) {
        vkr_bakery_json_remove(entity, "components");
      }
    }
    VkrBakeryJson *parent = vkr_bakery_json_get(entity, "parent");
    if (parent && parent->type != VKR_BAKERY_JSON_NULL) {
      const VkrBakeryJson *index =
          parent->type == VKR_BAKERY_JSON_STRING
              ? vkr_bakery_json_get(ids, (const char *)parent->string.str)
              : NULL;
      if (!index) {
        return vkr_project_fail(job,
                                "An entity parent names a missing entity id");
      }
      vkr_bakery_json_set(job->arena, entity, "parent",
                          vkr_bakery_json_int(job->arena, index->integer));
    }
  }
  return true_v;
}

bool8_t vkr_project_document_from_internal(VkrProjectJob *job,
                                           VkrBakeryJson *document) {
  Arena *arena = job->arena;
  VkrBakeryJson *entities = vkr_bakery_json_get(document, "entities");
  uint32_t count = entities ? entities->count : 0u;
  const char **ids =
      count ? (const char **)arena_alloc(arena, sizeof(char *) * count,
                                         ARENA_MEMORY_TAG_ARRAY)
            : NULL;
  uint32_t index = 0u;
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next, ++index) {
    const char *id = vkr_project_json_text(entity, "id");
    if (!id) {
      char fresh[37];
      vkr_project_uuid4(fresh);
      vkr_bakery_json_set(arena, entity, "id",
                          vkr_bakery_json_cstr(arena, fresh));
      id = vkr_project_json_text(entity, "id");
    }
    ids[index] = id;
  }
  VKR_PROJECT_TRY(vkr_project_document_entity_ids(job, entities, NULL));
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    VkrBakeryJson *parent = vkr_bakery_json_get(entity, "parent");
    if (parent && parent->type != VKR_BAKERY_JSON_NULL) {
      if (parent->type != VKR_BAKERY_JSON_INT || parent->integer < 0 ||
          parent->integer >= (int64_t)count) {
        return vkr_project_fail(job, "Invalid entity parent index");
      }
      vkr_bakery_json_set(arena, entity, "parent",
                          vkr_bakery_json_cstr(arena, ids[parent->integer]));
    }
    VkrBakeryJson *moved[ArrayCount(vkr_project_document_blocks)] = {0};
    bool8_t any = false_v;
    for (uint32_t b = 0u; b < ArrayCount(vkr_project_document_blocks); ++b) {
      moved[b] = vkr_bakery_json_get(entity, vkr_project_document_blocks[b]);
      if (moved[b]) {
        vkr_bakery_json_remove(entity, vkr_project_document_blocks[b]);
        any = true_v;
      }
    }
    if (!any) {
      continue;
    }
    VkrBakeryJson *components = vkr_bakery_json_get(entity, "components");
    if (!components) {
      components = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, entity, "components", components);
    }
    for (uint32_t b = 0u; b < ArrayCount(vkr_project_document_blocks); ++b) {
      if (moved[b] &&
          vkr_bakery_json_get(components, vkr_project_document_blocks[b])) {
        return vkr_project_fail(
            job, "An entity has both a block and a component of one kind");
      }
    }
    for (uint32_t b = 0u; b < ArrayCount(vkr_project_document_blocks); ++b) {
      if (moved[b]) {
        vkr_bakery_json_set(arena, components, vkr_project_document_blocks[b],
                            moved[b]);
      }
    }
  }
  return true_v;
}

bool8_t vkr_project_overlay_references(VkrProjectJob *job,
                                       VkrBakeryJson *record,
                                       VkrProjectOverlayReference *out,
                                       uint32_t capacity, uint32_t *out_count) {
  uint32_t count = 0u;
  out[count++] =
      (VkrProjectOverlayReference){record, "source_fingerprint", true_v};
  VkrBakeryJson *physics = vkr_bakery_json_get(record, "physics");
  if (physics && physics->type != VKR_BAKERY_JSON_NULL) {
    if (physics->type != VKR_BAKERY_JSON_OBJECT) {
      return vkr_project_fail(job, "Invalid physics override");
    }
    VkrBakeryJson *attachment = vkr_bakery_json_get(physics, "attachment");
    if (attachment && attachment->type != VKR_BAKERY_JSON_NULL) {
      VkrBakeryJson *source = vkr_bakery_json_get(attachment, "source");
      if (attachment->type != VKR_BAKERY_JSON_OBJECT || !source ||
          source->type != VKR_BAKERY_JSON_OBJECT) {
        return vkr_project_fail(job, "Invalid physics attachment source");
      }
      out[count++] = (VkrProjectOverlayReference){
          source, "fingerprint",
          vkr_bakery_recipe_bool(attachment, "enabled", false_v)};
    }
    VkrBakeryJson *joints = vkr_bakery_json_get(physics, "joints");
    if (joints &&
        (joints->type != VKR_BAKERY_JSON_ARRAY || joints->count > 16u)) {
      return vkr_project_fail(job, "Invalid physics joint list");
    }
    for (VkrBakeryJson *joint = joints ? joints->first : NULL; joint;
         joint = joint->next) {
      VkrBakeryJson *target = vkr_bakery_json_get(joint, "target");
      if (joint->type != VKR_BAKERY_JSON_OBJECT || !target ||
          target->type != VKR_BAKERY_JSON_OBJECT) {
        return vkr_project_fail(job, "Invalid physics joint target");
      }
      if (count < capacity) {
        out[count++] = (VkrProjectOverlayReference){
            target, "fingerprint",
            vkr_bakery_recipe_bool(joint, "enabled", false_v)};
      }
    }
  }
  *out_count = count;
  return true_v;
}

vkr_internal void vkr_project_rebind(VkrProjectJob *job,
                                     VkrBakeryJson *reference,
                                     const VkrBakeryJson *saved,
                                     const VkrBakeryJson *current) {
  VkrBakeryJson *index = vkr_bakery_json_get(reference, "scene_entity");
  if (!index || index->type != VKR_BAKERY_JSON_INT) {
    return;
  }
  int64_t rebound = -1;
  if (index->integer >= 0 && index->integer < (int64_t)saved->count) {
    const VkrBakeryJson *id =
        vkr_bakery_json_at(saved, (uint32_t)index->integer);
    const VkrBakeryJson *found =
        id ? vkr_bakery_json_get(current, (const char *)id->string.str) : NULL;
    rebound = found ? found->integer : -1;
  }
  vkr_bakery_json_set(job->arena, reference, "scene_entity",
                      vkr_bakery_json_int(job->arena, rebound));
}

bool8_t vkr_project_remap_overlay_indices(VkrProjectJob *job,
                                          VkrBakeryJson *overlay,
                                          const VkrBakeryJson *entities) {
  const VkrBakeryJson *saved = vkr_bakery_json_get(overlay, "document_ids");
  if (!saved) {
    return true_v;
  }
  bool8_t strings = saved->type == VKR_BAKERY_JSON_ARRAY;
  for (const VkrBakeryJson *value = strings ? saved->first : NULL; value;
       value = value->next) {
    strings = strings && value->type == VKR_BAKERY_JSON_STRING;
  }
  if (!strings) {
    return vkr_project_fail(job, "Invalid authored override document ids");
  }
  VkrBakeryJson *current = NULL;
  const char saved_error = job->error[0];
  const bool8_t saved_failed = job->failed;
  if (!vkr_project_document_entity_ids(job, entities, &current)) {
    /* Documents without ids bind saved indices directly. */
    if (!saved_error) {
      job->error[0] = 0;
    }
    job->failed = saved_failed;
    return true_v;
  }
  const char *lists[] = {"overrides", "created"};
  for (uint32_t l = 0u; l < ArrayCount(lists); ++l) {
    VkrBakeryJson *records = vkr_bakery_json_get(overlay, lists[l]);
    for (VkrBakeryJson *record = records ? records->first : NULL; record;
         record = record->next) {
      if (record->type != VKR_BAKERY_JSON_OBJECT) {
        return vkr_project_fail(job, "Invalid authored override record");
      }
      vkr_project_rebind(job, record, saved, current);
      VkrBakeryJson *parent = vkr_bakery_json_get(record, "parent");
      if (parent && parent->type == VKR_BAKERY_JSON_OBJECT) {
        vkr_project_rebind(job, parent, saved, current);
      }
      VkrProjectOverlayReference references[20];
      uint32_t reference_count = 0u;
      VKR_PROJECT_TRY(vkr_project_overlay_references(
          job, record, references, ArrayCount(references), &reference_count));
      for (uint32_t r = 0u; r < reference_count; ++r) {
        if (references[r].reference != record) {
          vkr_project_rebind(job, references[r].reference, saved, current);
        }
      }
    }
  }
  VkrBakeryJson *document_ids = vkr_bakery_json_array(job->arena);
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    vkr_bakery_json_append(
        document_ids,
        vkr_bakery_json_cstr(job->arena, vkr_project_json_text(entity, "id")));
  }
  vkr_bakery_json_set(job->arena, overlay, "document_ids", document_ids);
  return true_v;
}

VkrBakeryJson *vkr_project_read_managed_scene(VkrProjectJob *job,
                                              const char *path) {
  VkrBakeryJson *scene =
      vkr_project_load_json(job, path, VKR_PROJECT_MAX_DOCUMENT_BYTES);
  if (!scene) {
    return NULL;
  }
  int64_t version = 0;
  if (!vkr_bakery_json_get_int(scene, "version", &version) ||
      (version != 4 && version != VKR_PROJECT_SCENE_VERSION)) {
    return scene;
  }
  VkrBakeryJson *reference = vkr_bakery_json_get(scene, "inventory");
  vkr_bakery_json_remove(scene, "inventory");
  if (!reference || reference->type != VKR_BAKERY_JSON_STRING ||
      vkr_bakery_json_get(scene, "assets")) {
    vkr_project_fail(job,
                     "Managed scene needs exactly one inventory reference");
    return NULL;
  }
  char directory[VKR_PROJECT_PATH];
  char inventory_path[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (!vkr_project_contained(job, directory,
                             (const char *)reference->string.str, true_v,
                             inventory_path)) {
    return NULL;
  }
  VkrBakeryJson *inventory = vkr_project_load_json(
      job, inventory_path, VKR_PROJECT_MAX_INVENTORY_BYTES);
  if (!inventory) {
    return NULL;
  }
  int64_t inventory_version = 0;
  VkrBakeryJson *assets = vkr_bakery_json_get(inventory, "assets");
  if (!vkr_bakery_json_get_int(inventory, "version", &inventory_version) ||
      inventory_version != 1 || !assets ||
      assets->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_project_fail(job, "Managed scene inventory is invalid");
    return NULL;
  }
  vkr_bakery_json_set(job->arena, scene, "assets", assets);
  if (version == VKR_PROJECT_SCENE_VERSION &&
      !vkr_project_document_to_internal(job, scene)) {
    return NULL;
  }
  return scene;
}

bool8_t vkr_project_validate_managed_scene(VkrProjectJob *job,
                                           const VkrBakeryJson *scene,
                                           const char *reference,
                                           VkrBakeryJson **out_document,
                                           VkrBakeryJson **out_inventory) {
  Arena *arena = job->arena;
  VkrBakeryJson *document = vkr_bakery_json_object(arena);
  for (const VkrBakeryJson *field = scene->first; field; field = field->next) {
    if (field->key.length == 6u &&
        MemCompare(field->key.str, "assets", 6u) == 0) {
      continue;
    }
    vkr_bakery_json_set(arena, document, (const char *)field->key.str,
                        vkr_bakery_json_clone(arena, field));
  }
  vkr_bakery_json_set(arena, document, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_SCENE_VERSION));
  vkr_bakery_json_set(
      arena, document, "inventory",
      vkr_bakery_json_cstr(
          arena, reference ? reference
                           : "inventory/000000000000000000000000000000000000"
                             ".json"));
  VKR_PROJECT_TRY(vkr_project_document_from_internal(job, document));
  VkrBakeryJson *inventory = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, inventory, "version",
                      vkr_bakery_json_int(arena, 1));
  const VkrBakeryJson *assets = vkr_bakery_json_get(scene, "assets");
  vkr_bakery_json_set(arena, inventory, "assets",
                      assets ? vkr_bakery_json_clone(arena, assets)
                             : vkr_bakery_json_array(arena));
  VKR_PROJECT_TRY(vkr_project_validate_document(
      job, document, VKR_PROJECT_MAX_DOCUMENT_BYTES));
  VKR_PROJECT_TRY(vkr_project_validate_document(
      job, inventory, VKR_PROJECT_MAX_INVENTORY_BYTES));
  if (out_document) {
    *out_document = document;
  }
  if (out_inventory) {
    *out_inventory = inventory;
  }
  return true_v;
}

bool8_t vkr_project_write_managed_scene(VkrProjectJob *job, const char *path,
                                        VkrBakeryJson *scene) {
  char id[37];
  vkr_project_uuid4(id);
  const char *reference = vkr_project_printf(job, "inventory/%s.json", id);
  VkrBakeryJson *document = NULL;
  VkrBakeryJson *inventory = NULL;
  VKR_PROJECT_TRY(vkr_project_validate_managed_scene(job, scene, reference,
                                                     &document, &inventory));
  char directory[VKR_PROJECT_PATH];
  char inventory_path[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  (void)vkr_bakery_path_join(inventory_path, sizeof(inventory_path), directory,
                             reference);
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, inventory_path, inventory));
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, path, document));
  vkr_bakery_json_set(
      job->arena, scene, "version",
      vkr_bakery_json_int(job->arena, VKR_PROJECT_SCENE_VERSION));
  return true_v;
}

void vkr_project_document_references(const VkrBakeryJson *value,
                                     VkrProjectSet *digests,
                                     VkrProjectSet *revisions) {
  if (!value) {
    return;
  }
  if (value->type == VKR_BAKERY_JSON_OBJECT ||
      value->type == VKR_BAKERY_JSON_ARRAY) {
    for (const VkrBakeryJson *child = value->first; child;
         child = child->next) {
      vkr_project_document_references(child, digests, revisions);
    }
    return;
  }
  if (value->type != VKR_BAKERY_JSON_STRING) {
    return;
  }
  const char *text = (const char *)value->string.str;
  const char *digest = strncmp(text, "sha256:", 7u) == 0 ? text + 7 : text;
  if (strlen(digest) == 64u) {
    bool8_t hex = true_v;
    for (const char *c = digest; *c && hex; ++c) {
      hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f');
    }
    if (hex && digests) {
      (void)vkr_project_set_add(digests, digest, NULL);
    }
  }
  const char *path = strncmp(text, "./", 2u) == 0 ? text + 2 : text;
  if (strncmp(path, "builds/", 7u) == 0 && revisions) {
    const char *revision = path + 7;
    const char *end = strchr(revision, '/');
    char name[256];
    (void)snprintf(name, sizeof(name), "%.*s",
                   end ? (int)(end - revision) : (int)strlen(revision),
                   revision);
    (void)vkr_project_set_add(revisions, name, NULL);
  }
}

vkr_internal bool8_t vkr_project_migrate_source(VkrProjectJob *job,
                                                VkrBakeryJson *record,
                                                const char *label,
                                                const char *field) {
  VkrBakeryJson *value = vkr_bakery_json_get(record, "source");
  if (!value || value->type == VKR_BAKERY_JSON_NULL) {
    return true_v;
  }
  if (value->type != VKR_BAKERY_JSON_STRING) {
    return vkr_project_fail(job, "%s: %s.source: Invalid managed path", label,
                            field);
  }
  char normalized[VKR_PROJECT_PATH];
  (void)snprintf(normalized, sizeof(normalized), "%s",
                 (const char *)value->string.str);
  for (char *c = normalized; *c; ++c) {
    if (*c == '\\') {
      *c = '/';
    }
  }
  if (!vkr_project_validate_managed_path(job, normalized)) {
    char message[2048];
    (void)snprintf(message, sizeof(message), "%s", job->error);
    job->error[0] = 0;
    return vkr_project_fail(job, "%s: %s.source: %s", label, field, message);
  }
  vkr_bakery_json_set(job->arena, record, "source",
                      vkr_bakery_json_cstr(job->arena, normalized));
  return true_v;
}

bool8_t vkr_project_migrate_source_references(VkrProjectJob *job,
                                              VkrBakeryJson *document,
                                              const char *label) {
  VkrBakeryJson *assets = vkr_bakery_json_get(document, "assets");
  uint32_t index = 0u;
  for (VkrBakeryJson *record = assets ? assets->first : NULL; record;
       record = record->next, ++index) {
    char field[64];
    (void)snprintf(field, sizeof(field), "assets[%u]", index);
    VKR_PROJECT_TRY(vkr_project_migrate_source(job, record, label, field));
  }
  int64_t version = 0;
  if (vkr_bakery_json_get_int(document, "version", &version) && version == 1 &&
      vkr_bakery_json_get(document, "id") &&
      (vkr_bakery_json_get(document, "dependencies") ||
       vkr_bakery_json_get(document, "material_remaps"))) {
    VKR_PROJECT_TRY(vkr_project_migrate_source(job, document, label, "import"));
    VkrBakeryJson *asset = vkr_bakery_json_get(document, "asset");
    if (asset && asset->type == VKR_BAKERY_JSON_OBJECT) {
      VKR_PROJECT_TRY(vkr_project_migrate_source(job, asset, label, "asset"));
    }
    VkrBakeryJson *artifacts = vkr_bakery_json_get(document, "artifacts");
    index = 0u;
    for (VkrBakeryJson *record = artifacts ? artifacts->first : NULL; record;
         record = record->next, ++index) {
      char field[64];
      (void)snprintf(field, sizeof(field), "artifacts[%u]", index);
      VKR_PROJECT_TRY(vkr_project_migrate_source(job, record, label, field));
    }
  }
  return true_v;
}

// =============================================================================
// Tools
// =============================================================================

vkr_internal bool8_t vkr_project_is_cancelled(void *context) {
  (void)context;
  return vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                              VKR_MEMORY_ORDER_RELAXED)
             ? true_v
             : false_v;
}

bool8_t vkr_project_run_program(VkrProjectJob *job, const char *executable,
                                const char *const *arguments, uint32_t count,
                                const char *label, const char *stdout_path,
                                int32_t *out_code) {
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .working_directory = job->config->root[0] ? job->config->root : NULL,
      .stdout_path = stdout_path,
      .stderr_path = stdout_path,
      .append_output = stdout_path ? true_v : false_v,
      .termination_grace_ms = 5000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = vkr_project_is_cancelled,
  };
  bool8_t timed_out = false_v;
  const float64_t started = vkr_bakery_monotonic_seconds();
  uint64_t cpu_before = 0u;
  uint64_t peak = 0u;
  (void)vkr_bakery_children_usage(&cpu_before, &peak);
  *out_code = -1;
  const bool8_t ran = vkr_platform_process_run(&config, out_code, &timed_out);
  uint64_t cpu_after = 0u;
  (void)vkr_bakery_children_usage(&cpu_after, &peak);
  printf(
      "timing: stage='%s' detail='%s' exit=%d wall_ms=%llu cpu_ms=%llu "
      "peak_rss_mib=%llu\n",
      label, vkr_bakery_path_name(executable), *out_code,
      (unsigned long long)((vkr_bakery_monotonic_seconds() - started) * 1000.0),
      (unsigned long long)(cpu_after - cpu_before),
      (unsigned long long)(peak / (1024u * 1024u)));
  fflush(stdout);
  if (vkr_project_is_cancelled(NULL)) {
    job->cancelled = true_v;
    return vkr_project_fail(job, "Project preparation cancelled");
  }
  if (!ran) {
    return vkr_project_fail(job, "%s requires the installed %s tool", label,
                            vkr_bakery_path_name(executable));
  }
  return true_v;
}

bool8_t vkr_project_run_bakery(VkrProjectJob *job, const char *const *arguments,
                               uint32_t count, const char *label,
                               const char *fallback_detail, int32_t accepted,
                               int32_t *out_code) {
  const char *detail = fallback_detail;
  static const char *const flags[] = {"--input", "--layer", "--config",
                                      "--scene"};
  for (uint32_t f = 0u; f < ArrayCount(flags) && detail == fallback_detail;
       ++f) {
    for (uint32_t i = 0u; i + 1u < count; ++i) {
      if (strcmp(arguments[i], flags[f]) == 0) {
        detail = vkr_bakery_path_name(arguments[i + 1u]);
        break;
      }
    }
  }
  VKR_PROJECT_TRY(vkr_project_progress(job, label, -1.0, detail));
  fflush(stdout);
  int32_t code = -1;
  VKR_PROJECT_TRY(vkr_project_run_program(
      job, job->config->self_path, arguments, count, label, NULL, &code));
  if (out_code) {
    *out_code = code;
  }
  if (code != 0 && code != accepted) {
    return vkr_project_fail(job, "%s failed (exit %d); see the job log", label,
                            code);
  }
  return true_v;
}

bool8_t vkr_project_run_tool(VkrProjectJob *job, const char *tool,
                             const char *const *arguments, uint32_t count,
                             const char *label, int32_t accepted,
                             int32_t *out_code) {
  const char *argv[64];
  if (count + 2u > ArrayCount(argv)) {
    return vkr_project_fail(job, "Too many tool arguments");
  }
  argv[0] = "tool";
  argv[1] = tool;
  for (uint32_t i = 0u; i < count; ++i) {
    argv[i + 2u] = arguments[i];
  }
  return vkr_project_run_bakery(job, argv, count + 2u, label, tool, accepted,
                                out_code);
}

VkrBakeryJson *vkr_project_inspect_mesh(VkrProjectJob *job, const char *mesh) {
  char resolved[VKR_PROJECT_PATH];
  if (!vkr_project_resolve(mesh, true_v, resolved, sizeof(resolved))) {
    vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'", mesh);
    return NULL;
  }
  /* A report describes the artifact's bytes, so it is keyed by their
     digest: a revision renamed into place keeps its report. */
  char digest[VKR_BAKERY_SHA256_HEX];
  if (!vkr_project_digest(job, resolved, digest)) {
    return NULL;
  }
  VkrBakeryJson *cached = vkr_bakery_json_get(job->inspections, digest);
  if (cached) {
    return cached;
  }
  char directory[VKR_PROJECT_PATH];
  if (job->read_only) {
    (void)vkr_bakery_path_join(directory, sizeof(directory),
                               job->runtime_directory, "inspection");
  } else {
    (void)snprintf(directory, sizeof(directory), "%s/jobs/inspection",
                   job->workspace);
  }
  if (!vkr_project_make_dirs(job, directory)) {
    return NULL;
  }
  char id[37];
  vkr_project_uuid4(id);
  const char *report = vkr_project_printf(job, "%s/%s.json", directory, id);
  const char *arguments[] = {"--inspect", "--input", resolved, "--output",
                             report};
  if (!vkr_project_run_tool(job, "mesh", arguments, ArrayCount(arguments),
                            "Validating cooked model", 0, NULL)) {
    return NULL;
  }
  VkrBakeryJson *info =
      vkr_project_load_json(job, report, VKR_PROJECT_MAX_JSON_BYTES);
  if (info) {
    vkr_bakery_json_set(job->arena, job->inspections,
                        vkr_project_strdup(job, digest), info);
  }
  return info;
}

bool8_t vkr_project_adopt_inspection(VkrProjectJob *job, const char *mesh,
                                     const char *report) {
  char digest[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_digest(job, mesh, digest));
  VkrBakeryJson *info =
      vkr_project_load_json(job, report, VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(info);
  vkr_bakery_json_set(job->arena, job->inspections,
                      vkr_project_strdup(job, digest), info);
  return true_v;
}

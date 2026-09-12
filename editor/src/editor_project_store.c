#include "editor_project_store.h"

#include "core/vkr_json_writer.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_edit.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#if defined(PLATFORM_WINDOWS)
#include <objbase.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define PROJECT_JSON_DEPTH 32u

typedef struct s_ProjectJsonToken {
  uint32_t start;
  uint32_t end;
  uint32_t next;
  uint32_t child;
  char kind;
} s_ProjectJsonToken;

typedef struct s_ProjectJson {
  String8 bytes;
  uint32_t pos;
  uint32_t count;
  s_ProjectJsonToken *tokens;
  uint32_t capacity;
  Arena *arena;
} s_ProjectJson;

static bool8_t project_error(VkrEditorProjectError *error, const char *format,
                             ...) {
  if (error) {
    va_list args;
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
  }
  return false_v;
}

static String8 project_string(const char *text) {
  return (String8){.str = (uint8_t *)text, .length = strlen(text)};
}

static FilePath project_path(const char *text) {
  return (FilePath){.path = project_string(text),
                    .type = FILE_PATH_TYPE_ABSOLUTE};
}

static bool8_t project_join(char *out, const char *root, const char *relative) {
  int32_t length =
      snprintf(out, VKR_EDITOR_PROJECT_PATH_CAPACITY, "%s/%s", root, relative);
  return length > 0 && length < VKR_EDITOR_PROJECT_PATH_CAPACITY;
}

static bool8_t project_id_valid(const char *id) {
  if (!id || strlen(id) != 36) {
    return false_v;
  }
  for (uint32_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') {
        return false_v;
      }
    } else if (!((id[i] >= '0' && id[i] <= '9') ||
                 (id[i] >= 'a' && id[i] <= 'f'))) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_editor_project_id_generate(char id[37],
                                       VkrEditorProjectError *error) {
  uint8_t bytes[16];
#if defined(PLATFORM_WINDOWS)
  GUID guid;
  if (FAILED(CoCreateGuid(&guid))) {
    return project_error(error, "Unable to generate project identity");
  }
  MemCopy(bytes, &guid, sizeof(bytes));
#else
  arc4random_buf(bytes, sizeof(bytes));
#endif
  bytes[6] = (bytes[6] & 15u) | 64u;
  bytes[8] = (bytes[8] & 63u) | 128u;
  snprintf(
      id, 37,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return true_v;
}

// Decode one scalar, rejecting overlong UTF-8, surrogate values and truncation.
static bool8_t project_utf8(const uint8_t *bytes, uint32_t length,
                            uint32_t *position, uint32_t *scalar) {
  if (*position >= length) {
    return false_v;
  }
  uint32_t value = bytes[(*position)++];
  uint32_t extra = 0;
  uint32_t minimum = 0;
  if (value >= 0xc2 && value <= 0xdf) {
    extra = 1;
    minimum = 0x80;
    value &= 31;
  } else if (value >= 0xe0 && value <= 0xef) {
    extra = 2;
    minimum = 0x800;
    value &= 15;
  } else if (value >= 0xf0 && value <= 0xf4) {
    extra = 3;
    minimum = 0x10000;
    value &= 7;
  } else if (value >= 0x80) {
    return false_v;
  }
  for (uint32_t i = 0; i < extra; ++i) {
    if (*position >= length || (bytes[*position] & 0xc0) != 0x80) {
      return false_v;
    }
    value = (value << 6) | (bytes[(*position)++] & 63);
  }
  if (value < minimum || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return false_v;
  }
  *scalar = value;
  return true_v;
}

bool8_t vkr_editor_project_name_valid(const char *name,
                                      VkrEditorProjectError *error) {
  if (!name || !name[0] || strlen(name) >= VKR_EDITOR_PROJECT_NAME_CAPACITY) {
    return project_error(error,
                         "Name must contain 1 to 128 Unicode characters");
  }
  uint32_t length = (uint32_t)strlen(name);
  uint32_t position = 0;
  uint32_t count = 0;
  bool8_t visible = false_v;
  while (position < length) {
    uint32_t scalar;
    if (!project_utf8((const uint8_t *)name, length, &position, &scalar) ||
        scalar < 32 || scalar == 127 || ++count > 128) {
      return project_error(error, "Name contains invalid Unicode, control "
                                  "characters, or exceeds 128 characters");
    }
    visible |= scalar != ' ';
  }
  return visible ? true_v
                 : project_error(error, "Name cannot consist only of spaces");
}

static void project_json_space(s_ProjectJson *json) {
  while (json->pos < json->bytes.length &&
         (json->bytes.str[json->pos] == ' ' ||
          json->bytes.str[json->pos] == '\t' ||
          json->bytes.str[json->pos] == '\r' ||
          json->bytes.str[json->pos] == '\n')) {
    ++json->pos;
  }
}

static bool8_t project_json_hex(s_ProjectJson *json, uint32_t *value) {
  *value = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (json->pos >= json->bytes.length) {
      return false_v;
    }
    uint8_t c = json->bytes.str[json->pos++];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false_v;
    }
    *value = *value * 16 + digit;
  }
  return true_v;
}

// Optional decoded output is used for keys/names. Other string values remain
// byte-for-byte JSON views, avoiding allocations and lossy number conversion.
static bool8_t project_json_string(s_ProjectJson *json, char *out,
                                   uint32_t capacity) {
  if (json->pos >= json->bytes.length || json->bytes.str[json->pos++] != '"') {
    return false_v;
  }
  uint32_t written = 0;
  while (json->pos < json->bytes.length) {
    uint32_t scalar;
    if (json->bytes.str[json->pos] == '"') {
      ++json->pos;
      if (out) {
        out[written] = '\0';
      }
      return true_v;
    }
    if (json->bytes.str[json->pos] == '\\') {
      ++json->pos;
      if (json->pos >= json->bytes.length) {
        return false_v;
      }
      uint8_t escaped = json->bytes.str[json->pos++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        scalar = escaped;
        break;
      case 'b':
        scalar = 8;
        break;
      case 'f':
        scalar = 12;
        break;
      case 'n':
        scalar = 10;
        break;
      case 'r':
        scalar = 13;
        break;
      case 't':
        scalar = 9;
        break;
      case 'u':
        if (!project_json_hex(json, &scalar)) {
          return false_v;
        }
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
          if (json->pos + 2 > json->bytes.length ||
              json->bytes.str[json->pos++] != '\\' ||
              json->bytes.str[json->pos++] != 'u') {
            return false_v;
          }
          uint32_t low;
          if (!project_json_hex(json, &low) || low < 0xdc00 || low > 0xdfff) {
            return false_v;
          }
          scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
        } else if (scalar >= 0xdc00 && scalar <= 0xdfff) {
          return false_v;
        }
        break;
      default:
        return false_v;
      }
    } else {
      if (!project_utf8(json->bytes.str, (uint32_t)json->bytes.length,
                        &json->pos, &scalar) ||
          scalar < 32) {
        return false_v;
      }
    }
    if (out) {
      // Embedded NUL cannot be represented by the typed C string fields/keys.
      if (!scalar) {
        return false_v;
      }
      uint8_t encoded[4];
      uint32_t count;
      if (scalar < 0x80) {
        encoded[0] = (uint8_t)scalar;
        count = 1;
      } else if (scalar < 0x800) {
        encoded[0] = 0xc0 | (scalar >> 6);
        encoded[1] = 0x80 | (scalar & 63);
        count = 2;
      } else if (scalar < 0x10000) {
        encoded[0] = 0xe0 | (scalar >> 12);
        encoded[1] = 0x80 | ((scalar >> 6) & 63);
        encoded[2] = 0x80 | (scalar & 63);
        count = 3;
      } else {
        encoded[0] = 0xf0 | (scalar >> 18);
        encoded[1] = 0x80 | ((scalar >> 12) & 63);
        encoded[2] = 0x80 | ((scalar >> 6) & 63);
        encoded[3] = 0x80 | (scalar & 63);
        count = 4;
      }
      if (written + count >= capacity) {
        return false_v;
      }
      MemCopy(out + written, encoded, count);
      written += count;
    }
  }
  return false_v;
}

static bool8_t project_json_decode(s_ProjectJson *json, uint32_t token,
                                   char *out, uint32_t capacity) {
  uint32_t saved = json->pos;
  json->pos = json->tokens[token].start;
  bool8_t success = project_json_string(json, out, capacity);
  json->pos = saved;
  return success;
}

static bool8_t project_json_value(s_ProjectJson *json, uint32_t depth) {
  project_json_space(json);
  if (depth >= PROJECT_JSON_DEPTH || json->pos >= json->bytes.length ||
      json->count >= json->capacity) {
    return false_v;
  }
  uint32_t index = ++json->count;
  s_ProjectJsonToken *token = &json->tokens[index - 1];
  *token = (s_ProjectJsonToken){.start = json->pos,
                                .kind = (char)json->bytes.str[json->pos]};
  if (token->kind == '{' || token->kind == '[') {
    char close = token->kind == '{' ? '}' : ']';
    ++json->pos;
    project_json_space(json);
    token->child = json->count;
    bool8_t first = true_v;
    while (json->pos < json->bytes.length &&
           json->bytes.str[json->pos] != close) {
      if (!first) {
        if (json->bytes.str[json->pos++] != ',') {
          return false_v;
        }
        project_json_space(json);
      }
      if (json->pos >= json->bytes.length) {
        return false_v;
      }
      if (token->kind == '{') {
        uint32_t key = json->count;
        char name[256];
        if (json->bytes.str[json->pos] != '"' ||
            !project_json_value(json, depth + 1) ||
            !project_json_decode(json, key, name, sizeof(name))) {
          return false_v;
        }
        for (uint32_t previous = token->child; previous < key;) {
          char prior[256];
          if (!project_json_decode(json, previous, prior, sizeof(prior)) ||
              strcmp(name, prior) == 0) {
            return false_v;
          }
          previous = json->tokens[previous + 1].next;
        }
        project_json_space(json);
        if (json->pos >= json->bytes.length ||
            json->bytes.str[json->pos++] != ':') {
          return false_v;
        }
      }
      if (!project_json_value(json, depth + 1)) {
        return false_v;
      }
      first = false_v;
      project_json_space(json);
    }
    if (json->pos >= json->bytes.length ||
        json->bytes.str[json->pos++] != close) {
      return false_v;
    }
  } else if (token->kind == '"') {
    if (!project_json_string(json, NULL, 0)) {
      return false_v;
    }
  } else {
    uint32_t start = json->pos;
    while (json->pos < json->bytes.length &&
           !strchr(" \t\r\n,}]", json->bytes.str[json->pos])) {
      ++json->pos;
    }
    uint32_t length = json->pos - start;
    char value[128];
    if (!length || length >= sizeof(value)) {
      return false_v;
    }
    MemCopy(value, json->bytes.str + start, length);
    value[length] = '\0';
    if (strcmp(value, "true") && strcmp(value, "false") &&
        strcmp(value, "null")) {
      uint32_t p = value[0] == '-' ? 1 : 0;
      if (value[p] == '0') {
        ++p;
      } else {
        if (value[p] < '1' || value[p] > '9') {
          return false_v;
        }
        while (isdigit((unsigned char)value[p])) {
          ++p;
        }
      }
      if (value[p] == '.') {
        ++p;
        if (!isdigit((unsigned char)value[p])) {
          return false_v;
        }
        while (isdigit((unsigned char)value[p])) {
          ++p;
        }
      }
      if (value[p] == 'e' || value[p] == 'E') {
        ++p;
        if (value[p] == '+' || value[p] == '-') {
          ++p;
        }
        if (!isdigit((unsigned char)value[p])) {
          return false_v;
        }
        while (isdigit((unsigned char)value[p])) {
          ++p;
        }
      }
      if (p != length || !isfinite(strtod(value, NULL))) {
        return false_v;
      }
    }
  }
  token->end = json->pos;
  token->next = json->count;
  return true_v;
}

static bool8_t project_json_finish(s_ProjectJson *json, bool8_t result) {
  if (json->arena) {
    arena_destroy(json->arena);
  }
  *json = (s_ProjectJson){0};
  return result;
}

static bool8_t project_json_parse(s_ProjectJson *json, String8 bytes) {
  (void)project_json_finish(json, true_v);
  if (!bytes.str || !bytes.length ||
      bytes.length > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    return false_v;
  }
  /* Every token consumes at least one byte plus a delimiter, apart from the
   * root. This bound supports any document within the byte limit and avoids
   * fixed token ceilings and multi-megabyte stack frames. */
  json->capacity = (uint32_t)(bytes.length / 2 + 2);
  uint64_t storage = (uint64_t)json->capacity * sizeof(*json->tokens);
  json->arena = arena_create(storage + KB(64), KB(64));
  if (!json->arena) {
    return false_v;
  }
  json->tokens = arena_alloc(json->arena, storage, ARENA_MEMORY_TAG_ARRAY);
  if (!json->tokens) {
    return false_v;
  }
  json->bytes = bytes;
  json->count = 0;
  json->pos = 0;
  if (!project_json_value(json, 0)) {
    return false_v;
  }
  project_json_space(json);
  return json->pos == bytes.length;
}

static uint32_t project_json_field(s_ProjectJson *json, uint32_t object,
                                   const char *name) {
  if (json->tokens[object].kind != '{') {
    return UINT32_MAX;
  }
  for (uint32_t key = json->tokens[object].child;
       key < json->tokens[object].next; key = json->tokens[key + 1].next) {
    char text[256];
    if (project_json_decode(json, key, text, sizeof(text)) &&
        !strcmp(text, name)) {
      return key + 1;
    }
  }
  return UINT32_MAX;
}

static String8 project_json_view(s_ProjectJson *json, uint32_t token) {
  s_ProjectJsonToken *value = &json->tokens[token];
  return (String8){.str = json->bytes.str + value->start,
                   .length = value->end - value->start};
}

static bool8_t project_json_text(s_ProjectJson *json, uint32_t object,
                                 const char *name, char *out,
                                 uint32_t capacity) {
  uint32_t field = project_json_field(json, object, name);
  return field != UINT32_MAX && json->tokens[field].kind == '"' &&
         project_json_decode(json, field, out, capacity);
}

static bool8_t project_json_section(s_ProjectJson *json, const char *name,
                                    char kind, String8 *out) {
  uint32_t field = project_json_field(json, 0, name);
  if (field == UINT32_MAX || json->tokens[field].kind != kind) {
    return false_v;
  }
  *out = project_json_view(json, field);
  return true_v;
}

static uint64_t project_fingerprint(String8 bytes) {
  uint64_t value = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < bytes.length; ++i) {
    value ^= bytes.str[i];
    value *= UINT64_C(1099511628211);
  }
  return value;
}

bool8_t vkr_editor_project_parse(String8 bytes, VkrEditorProject *project,
                                 VkrEditorProjectError *error) {
  s_ProjectJson json = {0};
  if (!project || !project_json_parse(&json, bytes) ||
      json.tokens[0].kind != '{') {
    return project_json_finish(
        &json,
        project_error(error,
                      "Malformed project JSON (1 MiB, 16384 tokens, 32 levels, "
                      "255-byte keys maximum; duplicate keys are invalid)"));
  }
  uint32_t version = project_json_field(&json, 0, "version");
  if (version == UINT32_MAX ||
      json.tokens[version].end - json.tokens[version].start != 1 ||
      bytes.str[json.tokens[version].start] != '1') {
    return project_json_finish(
        &json,
        project_error(
            error, "Unsupported project version; expected integer version 1"));
  }
  MemZero(project, sizeof(*project));
  if (!project_json_text(&json, 0, "id", project->id, sizeof(project->id)) ||
      !project_id_valid(project->id) ||
      !project_json_text(&json, 0, "name", project->name,
                         sizeof(project->name)) ||
      !vkr_editor_project_name_valid(project->name, error)) {
    return project_json_finish(
        &json, project_error(error, "Invalid project identity or name"));
  }
  if (!project_json_section(&json, "default_font", '{',
                            &project->default_font) ||
      !project_json_section(&json, "editor_settings", '{',
                            &project->editor_settings) ||
      !project_json_section(&json, "scene_editor_state", '{',
                            &project->scene_editor_state) ||
      !project_json_section(&json, "assets", '[', &project->assets)) {
    return project_json_finish(
        &json,
        project_error(error, "Project requires default_font, editor_settings, "
                             "scene_editor_state and assets"));
  }
  uint32_t scenes = project_json_field(&json, 0, "scenes");
  if (scenes == UINT32_MAX || json.tokens[scenes].kind != '[') {
    return project_json_finish(
        &json, project_error(error, "Project scenes must be an array"));
  }
  for (uint32_t item = json.tokens[scenes].child;
       item < json.tokens[scenes].next; item = json.tokens[item].next) {
    if (project->scene_count == VKR_EDITOR_PROJECT_MAX_SCENES) {
      return project_json_finish(
          &json, project_error(error, "Project exceeds 128 scenes"));
    }
    VkrEditorProjectScene *scene = &project->scenes[project->scene_count];
    if (!project_json_text(&json, item, "id", scene->id, sizeof(scene->id)) ||
        !project_id_valid(scene->id) ||
        !project_json_text(&json, item, "name", scene->name,
                           sizeof(scene->name)) ||
        !vkr_editor_project_name_valid(scene->name, error) ||
        !project_json_text(&json, item, "path", scene->path,
                           sizeof(scene->path))) {
      return project_json_finish(&json,
                                 project_error(error, "Invalid scene entry %u",
                                               project->scene_count + 1));
    }
    char expected[128];
    snprintf(expected, sizeof(expected), "scenes/%s/scene.json", scene->id);
    if (strcmp(scene->path, expected)) {
      return project_json_finish(
          &json,
          project_error(error,
                        "Scene path must match its project-owned identity: %s",
                        expected));
    }
    for (uint32_t i = 0; i < project->scene_count; ++i) {
      if (!strcmp(scene->id, project->scenes[i].id)) {
        return project_json_finish(
            &json,
            project_error(error, "Duplicate scene identity: %s", scene->id));
      }
    }
    ++project->scene_count;
  }
  project->document = bytes;
  project->fingerprint = project_fingerprint(bytes);
  return project_json_finish(&json, true_v);
}

static bool8_t project_relative_valid(const char *relative) {
  if (!relative || !relative[0] || relative[0] == '/' ||
      strchr(relative, '\\') || strchr(relative, ':')) {
    return false_v;
  }
  const char *segment = relative;
  for (const char *p = relative;; ++p) {
    if (*p == '/' || !*p) {
      size_t length = (size_t)(p - segment);
      if (!length || (length == 1 && segment[0] == '.') ||
          (length == 2 && segment[0] == '.' && segment[1] == '.')) {
        return false_v;
      }
      if (!*p) {
        return true_v;
      }
      segment = p + 1;
    }
  }
}

bool8_t
vkr_editor_project_resolve(const char *owner_root, const char *relative,
                           char out_path[VKR_EDITOR_PROJECT_PATH_CAPACITY],
                           VkrEditorProjectError *error) {
  char root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char combined[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  FilePath owner = project_path(owner_root);
  if (!project_relative_valid(relative) ||
      file_path_resolve(&owner, root, sizeof(root)) != FILE_ERROR_NONE ||
      !project_join(combined, root, relative)) {
    return project_error(error, "Invalid managed path: %s",
                         relative ? relative : "(null)");
  }
  FilePath path = project_path(combined);
  if (file_path_resolve(&path, out_path, VKR_EDITOR_PROJECT_PATH_CAPACITY) !=
      FILE_ERROR_NONE) {
    return project_error(error, "Managed path is missing or inaccessible: %s",
                         combined);
  }
  size_t root_length = strlen(root);
  if (!file_path_starts_with(out_path, root) ||
      (out_path[root_length] != '/' && out_path[root_length] != '\\')) {
    return project_error(error, "Managed path escapes its owner: %s", relative);
  }
  return true_v;
}

static bool8_t project_directory(const char *root, const char *relative,
                                 char *out, VkrEditorProjectError *error) {
  char combined[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_relative_valid(relative) ||
      !project_join(combined, root, relative)) {
    return project_error(error, "Workspace path exceeds supported length");
  }
  FilePath path = project_path(combined);
  if (!file_create_directory(&path)) {
    return project_error(error, "Cannot create directory: %s", combined);
  }
  return vkr_editor_project_resolve(root, relative, out, error);
}

static bool8_t project_read(const char *path, VkrAllocator *allocator,
                            String8 *bytes, VkrEditorProjectError *error) {
  FilePath file = project_path(path);
  FileStats stats;
  FileHandle handle = {0};
  FileError result = file_stats(&file, &stats);
  if (result != FILE_ERROR_NONE || !stats.size ||
      stats.size > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    return project_error(
        error, "Cannot read manifest (missing, empty, or exceeds 1 MiB): %s",
        path);
  }
  result = file_open(
      &file, (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY}, &handle);
  uint8_t *buffer = NULL;
  uint64_t count = 0;
  if (result == FILE_ERROR_NONE) {
    buffer = vkr_allocator_alloc(allocator, stats.size + 1,
                                 VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!buffer) {
      result = FILE_ERROR_IO_ERROR;
    } else {
      result = file_read_into(&handle, buffer, stats.size + 1, &count);
      if (count != stats.size) {
        result = FILE_ERROR_IO_ERROR;
      }
    }
  }
  file_close(&handle);
  if (result != FILE_ERROR_NONE) {
    if (buffer) {
      vkr_allocator_free(allocator, buffer, stats.size + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    return project_error(
        error, "Cannot read manifest or it changed during read: %s", path);
  }
  buffer[count] = '\0';
  *bytes = (String8){.str = buffer, .length = count};
  return true_v;
}

// Streaming fingerprint avoids a second retained manifest allocation on save.
static bool8_t project_file_fingerprint(const char *path,
                                        uint64_t *fingerprint) {
  FilePath file = project_path(path);
  FileHandle handle = {0};
  if (file_open(&file, (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY},
                &handle) != FILE_ERROR_NONE) {
    return false_v;
  }
  uint64_t hash = UINT64_C(14695981039346656037);
  uint8_t buffer[4096];
  uint64_t count;
  bool8_t success = true_v;
  do {
    if (file_read_into(&handle, buffer, sizeof(buffer), &count) !=
        FILE_ERROR_NONE) {
      success = false_v;
      break;
    }
    for (uint64_t i = 0; i < count; ++i) {
      hash ^= buffer[i];
      hash *= UINT64_C(1099511628211);
    }
  } while (count);
  file_close(&handle);
  *fingerprint = hash;
  return success;
}

bool8_t vkr_editor_project_load(const VkrEditorWorkspace *workspace,
                                const char *id, VkrAllocator *allocator,
                                VkrEditorProject *project,
                                VkrEditorProjectError *error) {
  if (!workspace || !workspace->initialized || !project_id_valid(id) ||
      !allocator || !project) {
    return project_error(
        error, "Select an initialized workspace and valid project identity");
  }
  char relative[128];
  char absolute[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  snprintf(relative, sizeof(relative), "projects/%s/project.json", id);
  if (!vkr_editor_project_resolve(workspace->root, relative, absolute, error)) {
    return false_v;
  }
  String8 bytes = {0};
  if (!project_read(absolute, allocator, &bytes, error)) {
    return false_v;
  }
  bool8_t parsed = vkr_editor_project_parse(bytes, project, error);
  if (!parsed || strcmp(project->id, id)) {
    if (parsed) {
      project_error(error, "Project directory identity mismatch: %s", absolute);
    }
    vkr_allocator_free(allocator, bytes.str, bytes.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(project, sizeof(*project));
    return false_v;
  }
  strcpy(project->manifest_path, absolute);
  return true_v;
}

static bool8_t project_write_sink(void *context, const uint8_t *bytes,
                                  uint64_t length) {
  uint64_t written;
  return file_write(context, length, bytes, &written) == FILE_ERROR_NONE &&
         written == length;
}

static bool8_t project_write_text(FileHandle *file, const char *text) {
  return project_write_sink(file, (const uint8_t *)text, strlen(text));
}

static bool8_t project_write_quote(FileHandle *file, const char *text) {
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_write_sink, file);
  return vkr_json_writer_string(&writer, project_string(text)) &&
         vkr_json_writer_complete(&writer);
}

static bool8_t project_write_section(FileHandle *file, const char *prefix,
                                     String8 bytes, char kind,
                                     VkrEditorProjectError *error) {
  s_ProjectJson json = {0};
  if (!project_json_parse(&json, bytes) || json.tokens[0].kind != kind) {
    return project_json_finish(
        &json,
        project_error(error, "Invalid project JSON section: %s", prefix));
  }
  return project_json_finish(
      &json, project_write_text(file, prefix) &&
                 project_write_sink(file, bytes.str, bytes.length));
}

static bool8_t project_write_extensions(FileHandle *file, String8 document,
                                        VkrEditorProjectError *error) {
  if (!document.length) {
    return true_v;
  }
  s_ProjectJson json = {0};
  if (!project_json_parse(&json, document) || json.tokens[0].kind != '{') {
    return project_json_finish(
        &json,
        project_error(error, "Original project document is no longer valid"));
  }
  static const char *known[] = {"version",
                                "id",
                                "name",
                                "default_font",
                                "editor_settings",
                                "scene_editor_state",
                                "assets",
                                "scenes"};
  for (uint32_t key = json.tokens[0].child; key < json.tokens[0].next;
       key = json.tokens[key + 1].next) {
    char name[256];
    if (!project_json_decode(&json, key, name, sizeof(name))) {
      return project_json_finish(&json, false_v);
    }
    bool8_t recognized = false_v;
    for (uint32_t i = 0; i < ArrayCount(known); ++i) {
      recognized |= strcmp(name, known[i]) == 0;
    }
    if (!recognized) {
      uint32_t start = json.tokens[key].start;
      uint32_t end = json.tokens[key + 1].end;
      if (!project_write_text(file, ",") ||
          !project_write_sink(file, document.str + start, end - start)) {
        return project_json_finish(&json, false_v);
      }
    }
  }
  return project_json_finish(&json, true_v);
}

static bool8_t project_write_manifest(FileHandle *file,
                                      VkrEditorProject *project,
                                      VkrEditorProjectError *error) {
  if (!project_write_text(file, "{\"version\":1,\"id\":") ||
      !project_write_quote(file, project->id) ||
      !project_write_text(file, ",\"name\":") ||
      !project_write_quote(file, project->name) ||
      !project_write_section(file, ",\"default_font\":", project->default_font,
                             '{', error) ||
      !project_write_section(file, ",\"editor_settings\":",
                             project->editor_settings, '{', error) ||
      !project_write_section(file, ",\"scene_editor_state\":",
                             project->scene_editor_state, '{', error) ||
      !project_write_section(file, ",\"assets\":", project->assets, '[',
                             error) ||
      !project_write_text(file, ",\"scenes\":[")) {
    return false_v;
  }
  for (uint32_t i = 0; i < project->scene_count; ++i) {
    VkrEditorProjectScene *scene = &project->scenes[i];
    char expected[128];
    snprintf(expected, sizeof(expected), "scenes/%s/scene.json", scene->id);
    if (!project_id_valid(scene->id) ||
        !vkr_editor_project_name_valid(scene->name, error) ||
        strcmp(scene->path, expected)) {
      return project_error(error, "Invalid scene membership at entry %u",
                           i + 1);
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (!strcmp(project->scenes[j].id, scene->id)) {
        return project_error(error, "Duplicate scene identity: %s", scene->id);
      }
    }
    if ((i && !project_write_text(file, ",")) ||
        !project_write_text(file, "{\"id\":") ||
        !project_write_quote(file, scene->id) ||
        !project_write_text(file, ",\"name\":") ||
        !project_write_quote(file, scene->name) ||
        !project_write_text(file, ",\"path\":") ||
        !project_write_quote(file, scene->path) ||
        !project_write_text(file, "}")) {
      return false_v;
    }
  }
  return project_write_text(file, "]") &&
         project_write_extensions(file, project->document, error) &&
         project_write_text(file, "}\n");
}

bool8_t vkr_editor_project_save(VkrEditorProject *project,
                                VkrEditorProjectError *error) {
  if (error) {
    error->message[0] = '\0';
  }
  if (!project || !project_id_valid(project->id) ||
      !vkr_editor_project_name_valid(project->name, error) ||
      project->scene_count > VKR_EDITOR_PROJECT_MAX_SCENES ||
      !project->manifest_path[0]) {
    return project_error(error, "Invalid project save request");
  }
  char temporary[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char lock_directory[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char lock_name[64];
  char transaction[37];
  if (!vkr_editor_project_id_generate(transaction, error)) {
    return false_v;
  }
  int32_t temp_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%s",
                                 project->manifest_path, transaction);
  if (temp_length < 0 || temp_length >= sizeof(temporary)) {
    return project_error(error,
                         "Project manifest path exceeds supported length");
  }
  strcpy(lock_directory, project->manifest_path);
  char *separator = strrchr(lock_directory, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(lock_directory, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
#endif
  if (!separator) {
    return project_error(error,
                         "Project manifest needs an absolute owner path");
  }
  *separator = '\0';
  snprintf(lock_name, sizeof(lock_name), "project-%s", project->id);
  VkrPlatformProcessLock lock = {0};
  if (!vkr_platform_process_lock_acquire(lock_name, lock_directory, &lock)) {
    return project_error(error,
                         "Project is currently being saved by another editor");
  }
  FilePath destination = project_path(project->manifest_path);
  FilePath temp = project_path(temporary);
  FileHandle output = {0};
  FileMode exclusive = {.set = FILE_MODE_WRITE | FILE_MODE_BINARY |
                               FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE};
  bool8_t success = false_v;
  bool8_t temp_created = false_v;
  uint64_t current = 0;
  bool8_t exists = file_exists(&destination);
  if ((project->fingerprint &&
       (!project_file_fingerprint(project->manifest_path, &current) ||
        current != project->fingerprint)) ||
      (!project->fingerprint && exists)) {
    project_error(error,
                  "Project changed outside this editor; reload before saving");
    goto cleanup;
  }
  if (file_open(&temp, exclusive, &output) != FILE_ERROR_NONE) {
    project_error(error, "Cannot create staged project manifest");
    goto cleanup;
  }
  temp_created = true_v;
  if (!project_write_manifest(&output, project, error) ||
      file_sync(&output) != FILE_ERROR_NONE) {
    if (!error || !error->message[0]) {
      project_error(
          error, "Cannot write project manifest; previous version preserved");
    }
    goto cleanup;
  }
  file_close(&output);
  FileStats staged_stats;
  if (file_stats(&temp, &staged_stats) != FILE_ERROR_NONE ||
      staged_stats.size > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    project_error(error,
                  "Project manifest exceeds 1 MiB; previous version preserved");
    goto cleanup;
  }
  if (!project_file_fingerprint(temporary, &current)) {
    project_error(error, "Cannot verify staged project manifest");
    goto cleanup;
  }
  // Recheck after writing, so a non-cooperating external editor is detected.
  uint64_t previous;
  if (project->fingerprint &&
      (!project_file_fingerprint(project->manifest_path, &previous) ||
       previous != project->fingerprint)) {
    project_error(error,
                  "Project changed during save; previous version preserved");
    goto cleanup;
  }
  if (file_rename(&temp, &destination, project->fingerprint != 0) !=
      FILE_ERROR_NONE) {
    project_error(
        error, "Cannot publish project manifest; previous version preserved");
    goto cleanup;
  }
  project->fingerprint = current;
  success = true_v;
cleanup:
  file_close(&output);
  if (temp_created && !success) {
    file_remove(&temp);
  }
  vkr_platform_process_lock_release(&lock);
  return success;
}

bool8_t vkr_editor_project_json_member(String8 object, const char *name,
                                       String8 *value,
                                       VkrEditorProjectError *error) {
  s_ProjectJson json = {0};
  if (!value || !name || !project_json_parse(&json, object) ||
      json.tokens[0].kind != '{') {
    return project_json_finish(&json,
                               project_error(error, "Invalid JSON object"));
  }
  uint32_t field = project_json_field(&json, 0, name);
  if (field == UINT32_MAX) {
    *value = (String8){0};
    return project_json_finish(
        &json, project_error(error, "JSON member is missing: %s", name));
  }
  *value = project_json_view(&json, field);
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_workspace_open(const char *directory, bool8_t create,
                                  VkrEditorWorkspace *workspace,
                                  VkrEditorProjectError *error) {
  if (!directory || !directory[0] || !workspace) {
    return project_error(error, "Choose an existing workspace directory");
  }
  char chosen[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  FilePath input = project_path(directory);
  if (file_path_resolve(&input, chosen, sizeof(chosen)) != FILE_ERROR_NONE) {
    return project_error(
        error, "Workspace folder does not exist or is inaccessible: %s",
        directory);
  }
  FilePath chosen_path = project_path(chosen);
  if (!file_create_directory(&chosen_path)) {
    return project_error(error, "Selected workspace is not a directory");
  }
  MemZero(workspace, sizeof(*workspace));
  if (!project_join(workspace->root, chosen, ".vkreditor")) {
    return project_error(error, "Workspace path exceeds supported length");
  }
  FilePath root = project_path(workspace->root);
  if (!file_exists(&root)) {
    if (!create) {
      return true_v;
    }
    if (!project_directory(chosen, ".vkreditor", workspace->root, error)) {
      return false_v;
    }
  } else {
    char resolved[VKR_EDITOR_PROJECT_PATH_CAPACITY];
    if (!vkr_editor_project_resolve(chosen, ".vkreditor", resolved, error)) {
      return false_v;
    }
    strcpy(workspace->root, resolved);
  }
  char manifest[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_join(manifest, workspace->root, "workspace.json")) {
    return project_error(error, "Workspace path exceeds supported length");
  }
  FilePath manifest_path = project_path(manifest);
  if (!file_exists(&manifest_path)) {
    if (!create) {
      return true_v;
    }
    char projects[VKR_EDITOR_PROJECT_PATH_CAPACITY];
    if (!project_directory(workspace->root, "projects", projects, error) ||
        !vkr_editor_project_id_generate(workspace->id, error)) {
      return false_v;
    }
    char staging[VKR_EDITOR_PROJECT_PATH_CAPACITY];
    char filename[80];
    snprintf(filename, sizeof(filename), "workspace.%s.tmp", workspace->id);
    if (!project_join(staging, workspace->root, filename)) {
      return project_error(error, "Workspace path exceeds supported length");
    }
    FilePath staged_path = project_path(staging);
    FileHandle output = {0};
    FileMode exclusive = {.set = FILE_MODE_WRITE | FILE_MODE_BINARY |
                                 FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE};
    if (file_open(&staged_path, exclusive, &output) != FILE_ERROR_NONE) {
      return project_error(error, "Cannot stage workspace manifest");
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"version\":1,\"id\":\"%s\"}\n",
             workspace->id);
    bool8_t written = project_write_text(&output, json) &&
                      file_sync(&output) == FILE_ERROR_NONE;
    file_close(&output);
    FileError published =
        written ? file_rename(&staged_path, &manifest_path, false_v)
                : FILE_ERROR_IO_ERROR;
    if (published != FILE_ERROR_NONE) {
      file_remove(&staged_path);
      if (published != FILE_ERROR_ALREADY_EXISTS) {
        return project_error(error, "Cannot publish workspace manifest");
      }
    }
  }
  char resolved_manifest[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!vkr_editor_project_resolve(workspace->root, "workspace.json",
                                  resolved_manifest, error)) {
    return false_v;
  }
  FilePath resolved_path = project_path(resolved_manifest);
  FileHandle input_file = {0};
  uint8_t bytes[4096];
  uint64_t read_count = 0;
  if (file_open(&resolved_path,
                (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY},
                &input_file) != FILE_ERROR_NONE) {
    return project_error(error, "Cannot read workspace manifest");
  }
  FileError read_error =
      file_read_into(&input_file, bytes, sizeof(bytes), &read_count);
  file_close(&input_file);
  s_ProjectJson json = {0};
  if (read_error != FILE_ERROR_NONE || read_count == sizeof(bytes) ||
      !project_json_parse(&json,
                          (String8){.str = bytes, .length = read_count}) ||
      json.tokens[0].kind != '{') {
    return project_json_finish(
        &json, project_error(
                   error, "Malformed workspace manifest (maximum 4095 bytes)"));
  }
  uint32_t version = project_json_field(&json, 0, "version");
  if (version == UINT32_MAX ||
      json.tokens[version].end - json.tokens[version].start != 1 ||
      bytes[json.tokens[version].start] != '1' ||
      !project_json_text(&json, 0, "id", workspace->id,
                         sizeof(workspace->id)) ||
      !project_id_valid(workspace->id)) {
    return project_json_finish(
        &json,
        project_error(
            error,
            "Unsupported workspace version or invalid workspace identity"));
  }
  workspace->initialized = true_v;
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_project_begin(VkrEditorWorkspace *workspace,
                                 const char *name, VkrEditorProject *project,
                                 VkrEditorProjectError *error) {
  if (!workspace || !project || !vkr_editor_project_name_valid(name, error)) {
    return false_v;
  }
  if (!workspace->initialized) {
    char chosen[VKR_EDITOR_PROJECT_PATH_CAPACITY];
    strcpy(chosen, workspace->root);
    char *separator = strrchr(chosen, '/');
#if defined(PLATFORM_WINDOWS)
    char *backslash = strrchr(chosen, '\\');
    if (backslash && (!separator || backslash > separator)) {
      separator = backslash;
    }
#endif
    if (!separator) {
      return project_error(error,
                           "Workspace must be opened before project creation");
    }
    if (separator == chosen) {
      separator[1] = '\0';
    } else {
      *separator = '\0';
    }
    if (!vkr_editor_workspace_open(chosen, true_v, workspace, error)) {
      return false_v;
    }
  }
  MemZero(project, sizeof(*project));
  if (!vkr_editor_project_id_generate(project->id, error)) {
    return false_v;
  }
  strcpy(project->name, name);
  char projects[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_directory(workspace->root, "projects", projects, error)) {
    return false_v;
  }
  char project_root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_join(project_root, projects, project->id)) {
    return project_error(error, "Project path exceeds supported length");
  }
  FilePath root_path = project_path(project_root);
  if (file_create_directory_exclusive(&root_path) != FILE_ERROR_NONE) {
    return project_error(error, "Cannot reserve new project directory");
  }
  char scenes[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_directory(project_root, "scenes", scenes, error) ||
      !project_join(project->manifest_path, project_root, "project.json")) {
    return false_v;
  }
  project->default_font =
      string8_lit("{\"scope\":\"editor\",\"id\":\"default-scene-font\"}");
  project->editor_settings =
      string8_lit("{\"version\":1,\"graphics\":{},\"layout\":{},\"viewport\":{}"
                  ",\"input\":{},\"panels\":{},\"bakery\":{}}");
  project->scene_editor_state = string8_lit("{}");
  project->assets = string8_lit("[]");
  return true_v;
}

bool8_t vkr_editor_project_create(VkrEditorWorkspace *workspace,
                                  const char *name, VkrEditorProject *project,
                                  VkrEditorProjectError *error) {
  return vkr_editor_project_begin(workspace, name, project, error) &&
         vkr_editor_project_save(project, error);
}

bool8_t vkr_editor_workspace_visit(const VkrEditorWorkspace *workspace,
                                   VkrEditorProjectVisitor visitor,
                                   void *context,
                                   VkrEditorProjectError *error) {
  if (!workspace || !visitor) {
    return project_error(error, "Invalid workspace discovery request");
  }
  if (!workspace->initialized) {
    return true_v;
  }
  char projects[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!vkr_editor_project_resolve(workspace->root, "projects", projects,
                                  error)) {
    return false_v;
  }
#if defined(PLATFORM_WINDOWS)
  char pattern[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  wchar_t wide[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_join(pattern, projects, "*") ||
      !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wide,
                           ArrayCount(wide))) {
    return project_error(error, "Invalid workspace discovery path");
  }
  WIN32_FIND_DATAW entry;
  HANDLE search = FindFirstFileW(wide, &entry);
  if (search == INVALID_HANDLE_VALUE) {
    return project_error(error, "Cannot list workspace projects");
  }
  do {
    char id[37];
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1,
                            id, sizeof(id), NULL, NULL) &&
        project_id_valid(id)) {
      char relative[128];
      char manifest[VKR_EDITOR_PROJECT_PATH_CAPACITY];
      snprintf(relative, sizeof(relative), "%s/project.json", id);
      if (vkr_editor_project_resolve(projects, relative, manifest, NULL) &&
          !visitor(id, context)) {
        break;
      }
    }
  } while (FindNextFileW(search, &entry));
  FindClose(search);
#else
  DIR *directory = opendir(projects);
  if (!directory) {
    return project_error(error, "Cannot list workspace projects: %s", projects);
  }
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (project_id_valid(entry->d_name)) {
      char relative[128];
      char manifest[VKR_EDITOR_PROJECT_PATH_CAPACITY];
      snprintf(relative, sizeof(relative), "%s/project.json", entry->d_name);
      if (vkr_editor_project_resolve(projects, relative, manifest, NULL) &&
          !visitor(entry->d_name, context)) {
        break;
      }
    }
  }
  closedir(directory);
#endif
  return true_v;
}

bool8_t vkr_editor_project_json_string(String8 object, const char *name,
                                       char *out, uint32_t capacity,
                                       VkrEditorProjectError *error) {
  s_ProjectJson json = {0};
  if (!out || !capacity || !name || !project_json_parse(&json, object) ||
      !project_json_text(&json, 0, name, out, capacity)) {
    return project_json_finish(
        &json,
        project_error(error, "Invalid or oversized JSON string member: %s",
                      name ? name : "(null)"));
  }
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_workspace_lease_acquire(const VkrEditorWorkspace *workspace,
                                           VkrEditorWorkspaceLease *lease,
                                           VkrEditorProjectError *error) {
  if (!workspace || !workspace->initialized || !lease || lease->lock.acquired) {
    return project_error(
        error, "Initialize the workspace before acquiring its write lease");
  }
  char name[64];
  snprintf(name, sizeof(name), "workspace-%s", workspace->id);
  if (!vkr_platform_process_lock_acquire(name, workspace->root, &lease->lock)) {
    return project_error(
        error, "Workspace is open in another editor; using read-only access");
  }
  return true_v;
}

void vkr_editor_workspace_lease_release(VkrEditorWorkspaceLease *lease) {
  if (lease) {
    vkr_platform_process_lock_release(&lease->lock);
  }
}

static bool8_t project_locator_path(const char *override,
                                    char path[VKR_EDITOR_PROJECT_PATH_CAPACITY],
                                    VkrEditorProjectError *error) {
  if (override) {
    if (!override[0] || strlen(override) >= VKR_EDITOR_PROJECT_PATH_CAPACITY) {
      return project_error(error, "Invalid local workspace locator path");
    }
    strcpy(path, override);
    return true_v;
  }
#if defined(PLATFORM_WINDOWS)
  wchar_t wide[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char local[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  DWORD count =
      GetEnvironmentVariableW(L"LOCALAPPDATA", wide, ArrayCount(wide));
  if (!count || count >= ArrayCount(wide) ||
      !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, local,
                           sizeof(local), NULL, NULL) ||
      !project_join(path, local, "VKR/editor.json")) {
    return project_error(
        error, "LOCALAPPDATA is unavailable for the workspace locator");
  }
#else
  const char *home_directory = getenv("HOME");
  if (!home_directory || !home_directory[0] ||
      !project_join(path, home_directory,
                    "Library/Application Support/VKR/editor.json")) {
    return project_error(
        error, "Home directory is unavailable for the workspace locator");
  }
#endif
  return true_v;
}

bool8_t vkr_editor_workspace_locator_load(
    const char *locator_path, char directory[VKR_EDITOR_PROJECT_PATH_CAPACITY],
    VkrEditorProjectError *error) {
  if (!directory) {
    return project_error(error, "Workspace locator needs output storage");
  }
  directory[0] = '\0';
  char path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!project_locator_path(locator_path, path, error)) {
    return false_v;
  }
  FilePath file = project_path(path);
  FileHandle input = {0};
  FileError result = file_open(
      &file, (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY}, &input);
  if (result == FILE_ERROR_NOT_FOUND) {
    return true_v;
  }
  if (result != FILE_ERROR_NONE) {
    return project_error(error, "Cannot read local workspace locator: %s",
                         path);
  }
  uint8_t bytes[4096];
  uint64_t count = 0;
  result = file_read_into(&input, bytes, sizeof(bytes), &count);
  file_close(&input);
  s_ProjectJson json = {0};
  if (result != FILE_ERROR_NONE || count == sizeof(bytes) ||
      !project_json_parse(&json, (String8){.str = bytes, .length = count})) {
    return project_json_finish(
        &json,
        project_error(error, "Malformed local workspace locator: %s", path));
  }
  uint32_t version = project_json_field(&json, 0, "version");
  char selected[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (version == UINT32_MAX ||
      json.tokens[version].end - json.tokens[version].start != 1 ||
      bytes[json.tokens[version].start] != '1' ||
      !project_json_text(&json, 0, "directory", selected, sizeof(selected))) {
    return project_json_finish(
        &json,
        project_error(error,
                      "Unsupported local workspace locator version or path"));
  }
  FilePath chosen = project_path(selected);
  if (file_path_resolve(&chosen, directory, VKR_EDITOR_PROJECT_PATH_CAPACITY) !=
      FILE_ERROR_NONE) {
    directory[0] = '\0';
    return project_json_finish(
        &json,
        project_error(error, "Previous workspace folder is unavailable: %s",
                      selected));
  }
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_workspace_locator_save(const char *locator_path,
                                          const char *directory,
                                          VkrEditorProjectError *error) {
  if (!directory || !directory[0]) {
    return project_error(error,
                         "Select a workspace folder before saving its locator");
  }
  char path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char selected[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  FilePath chosen = project_path(directory);
  if (!project_locator_path(locator_path, path, error) ||
      file_path_resolve(&chosen, selected, sizeof(selected)) !=
          FILE_ERROR_NONE) {
    return project_error(error, "Cannot resolve workspace locator paths");
  }
  char parent[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  strcpy(parent, path);
  char *separator = strrchr(parent, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(parent, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
#endif
  if (!separator) {
    return project_error(
        error, "Local workspace locator needs an absolute parent directory");
  }
  *separator = '\0';
  Arena *arena = arena_create(KB(16), KB(4));
  if (!arena) {
    return project_error(error, "Unable to allocate workspace locator scratch");
  }
  VkrAllocator allocator = {.ctx = arena};
  bool8_t allocator_ready = vkr_allocator_arena(&allocator);
  String8 parent_view = project_string(parent);
  bool8_t directory_ready =
      allocator_ready && file_ensure_directory(&allocator, &parent_view);
  if (allocator_ready) {
    vkr_allocator_release_global_accounting(&allocator);
  }
  arena_destroy(arena);
  if (!directory_ready) {
    return project_error(error,
                         "Cannot create local workspace locator directory");
  }
  char id[37];
  char temporary[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!vkr_editor_project_id_generate(id, error)) {
    return false_v;
  }
  int32_t length =
      snprintf(temporary, sizeof(temporary), "%s.%s.tmp", path, id);
  if (length <= 0 || length >= sizeof(temporary)) {
    return project_error(
        error, "Local workspace locator path exceeds supported length");
  }
  FilePath staged = project_path(temporary);
  FilePath destination = project_path(path);
  FileHandle output = {0};
  if (file_open(&staged,
                (FileMode){.set = FILE_MODE_WRITE | FILE_MODE_BINARY |
                                  FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE},
                &output) != FILE_ERROR_NONE) {
    return project_error(error, "Cannot stage local workspace locator");
  }
  bool8_t written =
      project_write_text(&output, "{\"version\":1,\"directory\":") &&
      project_write_quote(&output, selected) &&
      project_write_text(&output, "}\n") &&
      file_sync(&output) == FILE_ERROR_NONE;
  file_close(&output);
  if (!written ||
      file_rename(&staged, &destination, true_v) != FILE_ERROR_NONE) {
    file_remove(&staged);
    return project_error(error, "Cannot publish local workspace locator");
  }
  return true_v;
}

typedef struct s_ProjectMemorySink {
  uint8_t *bytes;
  uint64_t capacity;
  uint64_t length;
} s_ProjectMemorySink;

static bool8_t project_memory_sink(void *context, const uint8_t *bytes,
                                   uint64_t length) {
  s_ProjectMemorySink *sink = context;
  if (length > sink->capacity - sink->length) {
    return false_v;
  }
  MemCopy(sink->bytes + sink->length, bytes, length);
  sink->length += length;
  return true_v;
}

bool8_t vkr_editor_project_json_replace_member(VkrAllocator *allocator,
                                               String8 object, const char *key,
                                               String8 value, String8 *out,
                                               VkrEditorProjectError *error) {
  if (!allocator || !out || !key || strlen(key) > 255) {
    return project_error(error, "Invalid JSON member replacement");
  }
  *out = (String8){0};
  uint32_t position = 0;
  while (position < strlen(key)) {
    uint32_t scalar;
    if (!project_utf8((const uint8_t *)key, (uint32_t)strlen(key), &position,
                      &scalar)) {
      return project_error(error, "JSON member name contains invalid Unicode");
    }
  }
  s_ProjectJson json = {0};
  if (!project_json_parse(&json, object) || json.tokens[0].kind != '{') {
    return project_json_finish(
        &json,
        project_error(error, "JSON member replacement requires an object"));
  }
  uint32_t field = project_json_field(&json, 0, key);
  bool8_t present = field != UINT32_MAX;
  uint64_t start = present ? json.tokens[field].start : json.tokens[0].end - 1u;
  uint64_t end = present ? json.tokens[field].end : start;
  bool8_t comma = json.tokens[0].child != json.tokens[0].next;
  if (!project_json_parse(&json, value)) {
    return project_json_finish(
        &json, project_error(error, "JSON replacement value is malformed"));
  }
  uint8_t name[1538];
  s_ProjectMemorySink quoted = {.bytes = name, .capacity = sizeof(name)};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_memory_sink, &quoted);
  if (!vkr_json_writer_string(&writer, project_string(key)) ||
      !vkr_json_writer_complete(&writer)) {
    return project_json_finish(
        &json, project_error(error, "Cannot encode JSON member name"));
  }
  uint64_t prefix_length = present ? 0 : quoted.length + 1u + (comma ? 1u : 0u);
  uint64_t length =
      object.length - (end - start) + prefix_length + value.length;
  if (length > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    return project_json_finish(
        &json, project_error(error, "Updated JSON object exceeds 1 MiB"));
  }
  uint8_t *bytes = vkr_allocator_alloc(allocator, length + 1u,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!bytes) {
    return project_json_finish(
        &json, project_error(error, "Unable to allocate updated JSON object"));
  }
  MemCopy(bytes, object.str, start);
  uint64_t cursor = start;
  if (!present) {
    if (comma) {
      bytes[cursor++] = ',';
    }
    MemCopy(bytes + cursor, name, quoted.length);
    cursor += quoted.length;
    bytes[cursor++] = ':';
  }
  MemCopy(bytes + cursor, value.str, value.length);
  cursor += value.length;
  MemCopy(bytes + cursor, object.str + end, object.length - end);
  bytes[length] = '\0';
  *out = (String8){.str = bytes, .length = length};
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_project_json_read_file(const char *path,
                                          VkrAllocator *allocator,
                                          String8 *bytes, uint64_t *fingerprint,
                                          VkrEditorProjectError *error) {
  if (!path || !allocator || !bytes || !fingerprint ||
      !project_read(path, allocator, bytes, error)) {
    return false_v;
  }
  s_ProjectJson json = {0};
  if (!project_json_parse(&json, *bytes) || json.tokens[0].kind != '{') {
    vkr_allocator_free(allocator, bytes->str, bytes->length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *bytes = (String8){0};
    return project_json_finish(
        &json,
        project_error(error, "Malformed managed JSON document: %s", path));
  }
  *fingerprint = project_fingerprint(*bytes);
  return project_json_finish(&json, true_v);
}

bool8_t vkr_editor_project_save_scene_overlay(const char *manifest_path,
                                              uint64_t *expected_fingerprint,
                                              VkrSceneEditState *edits,
                                              const VkrScene *scene,
                                              VkrAllocator *scratch_allocator,
                                              VkrEditorProjectError *error) {
  if (!manifest_path || !expected_fingerprint || !*expected_fingerprint ||
      !edits || !scene || !scratch_allocator) {
    return project_error(
        error, "Open a managed scene before saving its authored edits");
  }
  char root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (strlen(manifest_path) >= sizeof(root)) {
    return project_error(error, "Managed scene path exceeds supported length");
  }
  strcpy(root, manifest_path);
  char *separator = strrchr(root, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(root, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
#endif
  if (!separator) {
    return project_error(error,
                         "Managed scene needs an absolute manifest path");
  }
  *separator = '\0';
  String8 document = {0};
  String8 updated = {0};
  uint64_t original_hash;
  bool8_t success = false_v;
  bool8_t overlay_published = false_v;
  bool8_t staged_created = false_v;
  uint64_t saved_revision = edits->saved_revision;
  FileHandle output = {0};
  VkrPlatformProcessLock lock = {0};
  char overlay_path[VKR_EDITOR_PROJECT_PATH_CAPACITY] = {0};
  char staged_path[VKR_EDITOR_PROJECT_PATH_CAPACITY] = {0};
  FilePath staged = {0};
  if (!vkr_editor_project_json_read_file(manifest_path, scratch_allocator,
                                         &document, &original_hash, error)) {
    goto cleanup;
  }
  if (original_hash != *expected_fingerprint) {
    project_error(
        error, "Scene changed outside this editor; reload before saving edits");
    goto cleanup;
  }
  char scene_id[37];
  String8 version;
  if (!vkr_editor_project_json_member(document, "version", &version, error) ||
      version.length != 1 || version.str[0] != '3' ||
      !vkr_editor_project_json_string(document, "id", scene_id,
                                      sizeof(scene_id), error) ||
      !project_id_valid(scene_id)) {
    project_error(
        error,
        "Authored managed saves require scene version 3 and its identity");
    goto cleanup;
  }
  char lock_name[64];
  snprintf(lock_name, sizeof(lock_name), "scene-%s", scene_id);
  if (!vkr_platform_process_lock_acquire(lock_name, root, &lock)) {
    project_error(error, "Scene is currently being saved by another editor");
    goto cleanup;
  }
  char edits_root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char revision_id[37];
  char relative[80];
  if (!project_directory(root, "edits", edits_root, error) ||
      !vkr_editor_project_id_generate(revision_id, error)) {
    goto cleanup;
  }
  snprintf(relative, sizeof(relative), "edits/%s.json", revision_id);
  if (!project_join(overlay_path, root, relative)) {
    project_error(error, "Authored overlay path exceeds supported length");
    goto cleanup;
  }
  if (!vkr_scene_edit_save(edits, scene, project_string(overlay_path))) {
    project_error(error, "%s", edits->status);
    goto cleanup;
  }
  overlay_published = true_v;
  uint8_t quoted_bytes[128];
  s_ProjectMemorySink quote = {.bytes = quoted_bytes,
                               .capacity = sizeof(quoted_bytes)};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_memory_sink, &quote);
  if (!vkr_json_writer_string(&writer, project_string(relative)) ||
      !vkr_json_writer_complete(&writer) ||
      !vkr_editor_project_json_replace_member(
          scratch_allocator, document, "edit_overlay",
          (String8){.str = quoted_bytes, .length = quote.length}, &updated,
          error)) {
    goto cleanup;
  }
  int32_t length = snprintf(staged_path, sizeof(staged_path), "%s.tmp.%s",
                            manifest_path, revision_id);
  if (length <= 0 || length >= sizeof(staged_path)) {
    project_error(error, "Staged scene manifest path exceeds supported length");
    goto cleanup;
  }
  staged = project_path(staged_path);
  if (file_open(&staged,
                (FileMode){.set = FILE_MODE_WRITE | FILE_MODE_BINARY |
                                  FILE_MODE_CREATE | FILE_MODE_EXCLUSIVE},
                &output) != FILE_ERROR_NONE) {
    project_error(error,
                  "Cannot stage scene manifest; previous edits preserved");
    goto cleanup;
  }
  staged_created = true_v;
  if (!project_write_sink(&output, updated.str, updated.length) ||
      file_sync(&output) != FILE_ERROR_NONE) {
    project_error(error,
                  "Cannot write scene manifest; previous edits preserved");
    goto cleanup;
  }
  file_close(&output);
  uint64_t current_hash;
  FilePath destination = project_path(manifest_path);
  if (!project_file_fingerprint(manifest_path, &current_hash) ||
      current_hash != original_hash) {
    project_error(error, "Scene changed during save; previous edits preserved");
    goto cleanup;
  }
  if (file_rename(&staged, &destination, true_v) != FILE_ERROR_NONE) {
    project_error(error,
                  "Cannot publish scene manifest; previous edits preserved");
    goto cleanup;
  }
  *expected_fingerprint = project_fingerprint(updated);
  success = true_v;
cleanup:
  file_close(&output);
  if (!success) {
    edits->saved_revision = saved_revision;
    if (error && error->message[0]) {
      snprintf(edits->status, sizeof(edits->status), "%s", error->message);
    }
    if (overlay_published) {
      FilePath overlay = project_path(overlay_path);
      file_remove(&overlay);
    }
    if (staged_created) {
      file_remove(&staged);
    }
  }
  vkr_platform_process_lock_release(&lock);
  if (updated.str) {
    vkr_allocator_free(scratch_allocator, updated.str, updated.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (document.str) {
    vkr_allocator_free(scratch_allocator, document.str, document.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  return success;
}

uint64_t vkr_editor_project_document_fingerprint(String8 bytes) {
  return project_fingerprint(bytes);
}

static bool8_t project_json_merge_emit(s_ProjectJson *old, uint32_t old_index,
                                       s_ProjectJson *fresh, uint32_t new_index,
                                       uint8_t *bytes, uint64_t capacity,
                                       uint64_t *length) {
  if (old->tokens[old_index].kind != '{' ||
      fresh->tokens[new_index].kind != '{') {
    String8 value = project_json_view(fresh, new_index);
    if (value.length > capacity - *length) {
      return false_v;
    }
    MemCopy(bytes + *length, value.str, value.length);
    *length += value.length;
    return true_v;
  }
  if (*length >= capacity) {
    return false_v;
  }
  bytes[(*length)++] = '{';
  bool8_t comma = false_v;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    s_ProjectJson *source = pass ? old : fresh;
    uint32_t object = pass ? old_index : new_index;
    for (uint32_t key = source->tokens[object].child;
         key < source->tokens[object].next;
         key = source->tokens[key + 1].next) {
      char name[256];
      if (!project_json_decode(source, key, name, sizeof(name))) {
        return false_v;
      }
      uint32_t previous = project_json_field(old, old_index, name);
      if (pass && project_json_field(fresh, new_index, name) != UINT32_MAX) {
        continue;
      }
      String8 key_bytes = project_json_view(source, key);
      if (key_bytes.length + 2 > capacity - *length) {
        return false_v;
      }
      if (comma) {
        bytes[(*length)++] = ',';
      }
      MemCopy(bytes + *length, key_bytes.str, key_bytes.length);
      *length += key_bytes.length;
      bytes[(*length)++] = ':';
      if (!pass && previous != UINT32_MAX) {
        if (!project_json_merge_emit(old, previous, fresh, key + 1, bytes,
                                     capacity, length)) {
          return false_v;
        }
      } else {
        String8 value = project_json_view(source, key + 1);
        if (value.length > capacity - *length) {
          return false_v;
        }
        MemCopy(bytes + *length, value.str, value.length);
        *length += value.length;
      }
      comma = true_v;
    }
  }
  if (*length >= capacity) {
    return false_v;
  }
  bytes[(*length)++] = '}';
  return true_v;
}

bool8_t vkr_editor_project_json_merge_objects(VkrAllocator *allocator,
                                              String8 previous, String8 owned,
                                              String8 *out,
                                              VkrEditorProjectError *error) {
  if (!allocator || !out ||
      previous.length + owned.length > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    return project_error(error, "Merged settings exceed the JSON limit");
  }
  *out = (String8){0};
  s_ProjectJson *documents = vkr_allocator_alloc(
      allocator, 2 * sizeof(*documents), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!documents) {
    return project_error(error, "Unable to allocate settings parser");
  }
  MemZero(documents, 2 * sizeof(*documents));
  bool8_t valid = project_json_parse(&documents[0], previous) &&
                  project_json_parse(&documents[1], owned) &&
                  documents[0].tokens[0].kind == '{' &&
                  documents[1].tokens[0].kind == '{';
  uint64_t capacity = previous.length + owned.length;
  uint8_t *bytes = valid ? vkr_allocator_alloc(allocator, capacity + 1,
                                               VKR_ALLOCATOR_MEMORY_TAG_STRING)
                         : NULL;
  uint64_t length = 0;
  valid = bytes && project_json_merge_emit(&documents[0], 0, &documents[1], 0,
                                           bytes, capacity, &length);
  (void)project_json_finish(&documents[0], true_v);
  (void)project_json_finish(&documents[1], true_v);
  vkr_allocator_free(allocator, documents, 2 * sizeof(*documents),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!valid) {
    if (bytes) {
      vkr_allocator_free(allocator, bytes, capacity + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    return project_error(error, "Invalid or oversized settings objects");
  }
  bytes[length] = 0;
  *out = (String8){.str = bytes, .length = length};
  return true_v;
}

bool8_t vkr_editor_project_local_jobs_directory(
    char out[VKR_EDITOR_PROJECT_PATH_CAPACITY], VkrEditorProjectError *error) {
  char locator[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!out || !project_locator_path(NULL, locator, error)) {
    return false_v;
  }
  char *separator = strrchr(locator, '/');
#if defined(PLATFORM_WINDOWS)
  char *backslash = strrchr(locator, '\\');
  if (backslash && (!separator || backslash > separator)) {
    separator = backslash;
  }
#endif
  if (!separator) {
    return project_error(error, "Local settings path has no parent");
  }
  *separator = 0;
  if (!project_join(out, locator, "jobs")) {
    return project_error(error, "Local jobs path exceeds supported capacity");
  }
  Arena *arena = arena_create(KB(16), KB(4));
  if (!arena) {
    return project_error(error, "Unable to allocate local path scratch");
  }
  VkrAllocator allocator = {.ctx = arena};
  bool8_t initialized = vkr_allocator_arena(&allocator);
  String8 directory = project_string(out);
  bool8_t created =
      initialized && file_ensure_directory(&allocator, &directory);
  if (initialized) {
    vkr_allocator_release_global_accounting(&allocator);
  }
  arena_destroy(arena);
  if (!created) {
    return project_error(error, "Cannot create local job directory");
  }
  FilePath path = project_path(out);
  char resolved[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (file_path_resolve(&path, resolved, sizeof(resolved)) != FILE_ERROR_NONE) {
    return project_error(error, "Cannot resolve local job directory");
  }
  strcpy(out, resolved);
  return true_v;
}

#include "editor_bakery.h"
#include "editor_bakery_service.h"
#include "editor_install.h"
#include "editor_internal.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_json.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "platform/vkr_platform.h"
#include "vkr_shader_catalog.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDITOR_BAKERY_JOB_CAPACITY 16u
#define EDITOR_BAKERY_PATH_CAPACITY 1024u
#define EDITOR_BAKERY_LOG_CAPACITY KB(16)
#define EDITOR_BAKERY_NONE UINT32_MAX

typedef enum EditorBakeKind {
  EDITOR_BAKE_MESH,
  EDITOR_BAKE_FONT,
  EDITOR_BAKE_TEXTURE,
  EDITOR_BAKE_TEXTURES,
  EDITOR_BAKE_DFG_TABLE,
  EDITOR_BAKE_SHEEN_TABLE,
  EDITOR_BAKE_ANISOTROPY_TABLE,
  EDITOR_BAKE_DIFFUSE_VOLUME,
  EDITOR_BAKE_REFLECTION_PROBE,
  EDITOR_BAKE_COLLISION_HULL,
  EDITOR_BAKE_COLLISION_MESH,
  EDITOR_BAKE_SHADERS,
  EDITOR_BAKE_KIND_COUNT,
  EDITOR_BAKE_PROJECT = EDITOR_BAKE_KIND_COUNT,
  /* A project package (docs/proposals/project-packaging.md). */
  EDITOR_BAKE_PACKAGE,
} EditorBakeKind;

static const char *const editor_bakery_kind_names[] = {
    "Mesh",           "Font",       "Texture", "Texture folder", "GGX DFG",
    "Charlie",        "Anisotropy", "Diffuse", "Reflection",     "Hull",
    "Collision mesh", "Shaders",    "Project", "Package"};
_Static_assert(ArrayCount(editor_bakery_kind_names) == EDITOR_BAKE_PACKAGE + 1,
               "Every bake kind and each project job need a display name");

typedef enum EditorBakeryView {
  EDITOR_BAKERY_SETUP,
  EDITOR_BAKERY_JOBS,
  EDITOR_BAKERY_OUTPUT,
} EditorBakeryView;

typedef enum EditorBakeStatus {
  EDITOR_BAKE_QUEUED,
  EDITOR_BAKE_RUNNING,
  EDITOR_BAKE_SUCCEEDED,
  EDITOR_BAKE_FAILED,
  EDITOR_BAKE_CANCELLED,
} EditorBakeStatus;

typedef struct EditorBakeJob {
  uint64_t id;
  EditorBakeKind kind;
  EditorBakeStatus status;
  char input[EDITOR_BAKERY_PATH_CAPACITY];
  char output[EDITOR_BAKERY_PATH_CAPACITY + 5u];
  char stdout_path[EDITOR_BAKERY_PATH_CAPACITY];
  char stderr_path[EDITOR_BAKERY_PATH_CAPACITY];
  int32_t exit_code;
  bool8_t force;
  bool8_t process_ok;
  bool8_t timed_out;
  /* Bytes of the event stream already forwarded to the Console, and the
     action counters seen so far. */
  uint64_t console_offset;
  uint32_t actions_total;
  uint32_t actions_finished;
  uint32_t actions_failed;
} EditorBakeJob;

struct VkrEditorBakery {
  VkrAllocator *allocator;
  VkrThread worker;
  VkrPlatformProcessLock process_lock;
  VkrAtomicBool cancel_requested;
  VkrAtomicBool complete;
  int32_t worker_exit_code;
  bool8_t worker_process_ok;
  bool8_t worker_timed_out;
  EditorBakeJob jobs[EDITOR_BAKERY_JOB_CAPACITY];
  uint32_t job_count;
  uint64_t next_job_id;
  uint32_t selected;
  uint32_t running;
  EditorBakeKind kind;
  EditorBakeryView view;
  bool8_t force;
  bool8_t managed;
  bool8_t writable_scene;
  bool8_t scene_bake_requested;
  bool8_t reflection;
  bool8_t diffuse;
  bool8_t lightmap;
  bool8_t lightmap_uvs;
  uint8_t source_paths[EDITOR_BAKE_KIND_COUNT][EDITOR_BAKERY_PATH_CAPACITY];
  uint32_t source_lengths[EDITOR_BAKE_KIND_COUNT];
  uint8_t output_paths[EDITOR_BAKE_KIND_COUNT][EDITOR_BAKERY_PATH_CAPACITY];
  uint32_t output_lengths[EDITOR_BAKE_KIND_COUNT];
  uint8_t log[EDITOR_BAKERY_LOG_CAPACITY];
  uint32_t log_length;
  float64_t next_log_read;
  float64_t next_console_read;
  char log_directory[EDITOR_BAKERY_PATH_CAPACITY];
  char lock_directory[EDITOR_BAKERY_PATH_CAPACITY];
  char message[192];
  /* The `vkr_bakery serve` daemon; NULL where it is unavailable. The shader
     watch recompiles this editor's catalog when a shader source changes. */
  EditorBakeryService *service;
  uint32_t shader_watch;
  bool8_t shader_watch_requested;
  /* The materials watch rebuilds the project's Custom material library
     when an asset changes; a compile asks the renderer to reload it. */
  uint32_t materials_watch;
  bool8_t materials_watch_requested;
  bool8_t materials_compiled;
};

/* Bakery text is UI labels and path fragments, and both are legitimately
   empty: a recipe without an explicit output has no path, and a path without a
   dot has no extension. Both call sites below pass "" for exactly that case,
   so this needs the empty-tolerant constructor. */
static String8 editor_bakery_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

static const char *editor_bakery_status(EditorBakeStatus status) {
  static const char *const names[] = {"Queued", "Running", "Done", "Failed",
                                      "Cancelled"};
  return names[status];
}

static bool8_t editor_bakery_uses_explicit_output(EditorBakeKind kind) {
  return kind == EDITOR_BAKE_DIFFUSE_VOLUME ||
         kind == EDITOR_BAKE_REFLECTION_PROBE ||
         kind == EDITOR_BAKE_COLLISION_HULL ||
         kind == EDITOR_BAKE_COLLISION_MESH;
}

static bool8_t editor_bakery_is_static_table(EditorBakeKind kind) {
  return kind == EDITOR_BAKE_DFG_TABLE || kind == EDITOR_BAKE_SHEEN_TABLE ||
         kind == EDITOR_BAKE_ANISOTROPY_TABLE;
}

/* Cook, table and shader recipes and project packages stream vkr_bakery
 * JSON events; recipes are cached by action key, so a rebuild of unchanged
 * inputs needs an explicit Force. Project jobs and scene bakes print a plain
 * log and publish a result. */
static bool8_t editor_bakery_streams_events(EditorBakeKind kind) {
  return kind != EDITOR_BAKE_PROJECT && kind != EDITOR_BAKE_DIFFUSE_VOLUME &&
         kind != EDITOR_BAKE_REFLECTION_PROBE;
}

static bool8_t editor_bakery_supports_force(EditorBakeKind kind) {
  return editor_bakery_streams_events(kind) && kind != EDITOR_BAKE_PACKAGE;
}

/* Recipes whose source is fixed: renderer tables and the shader catalog. */
static bool8_t editor_bakery_has_fixed_source(EditorBakeKind kind) {
  return editor_bakery_is_static_table(kind) || kind == EDITOR_BAKE_SHADERS;
}

static const char *editor_bakery_table_output(EditorBakeKind kind) {
  static const char *const outputs[] = {
      "renderer/src/vkr_dfg_lut_data.inc",
      "renderer/src/vkr_sheen_lut_data.inc",
      "renderer/src/vkr_anisotropy_lut_data.inc",
  };
  return outputs[kind - EDITOR_BAKE_DFG_TABLE];
}

/* The worker borrows one immutable job and fixed path buffers until complete
 * is released. The UI never edits that job while the worker owns it. */
static bool8_t editor_bakery_is_cancelled(void *context) {
  const VkrEditorBakery *bakery = context;
  return vkr_atomic_bool_load(&bakery->cancel_requested,
                              VKR_MEMORY_ORDER_ACQUIRE);
}

static void *editor_bakery_worker(void *argument) {
  VkrEditorBakery *bakery = argument;
  EditorBakeJob *job = &bakery->jobs[bakery->running];
  const char *arguments[32];
  char manifest[EDITOR_BAKERY_PATH_CAPACITY + 20u];
  uint32_t count = 0u;
  if (job->kind == EDITOR_BAKE_PACKAGE) {
    /* A package stage per event, read by the Build panel. */
    arguments[count++] = "bundle";
    arguments[count++] = job->input;
    arguments[count++] = "--profile";
    arguments[count++] = job->output;
    arguments[count++] = "--json";
  } else if (editor_bakery_streams_events(job->kind)) {
    /* One vkr_bakery process per recipe; its JSON event stream on stdout
       drives the job's progress, action list, output and Console lines. */
    switch (job->kind) {
    case EDITOR_BAKE_SHADERS:
      arguments[count++] = "shaders";
      arguments[count++] = "--out";
      arguments[count++] = job->input;
      break;
    case EDITOR_BAKE_DFG_TABLE:
    case EDITOR_BAKE_SHEEN_TABLE:
    case EDITOR_BAKE_ANISOTROPY_TABLE:
      arguments[count++] = "cook";
      arguments[count++] = "--producer";
      arguments[count++] = "table";
      arguments[count++] = "--recipe";
      arguments[count++] = job->kind == EDITOR_BAKE_DFG_TABLE ? "table=dfg"
                           : job->kind == EDITOR_BAKE_SHEEN_TABLE
                               ? "table=sheen"
                               : "table=anisotropy";
      break;
    case EDITOR_BAKE_COLLISION_HULL:
    case EDITOR_BAKE_COLLISION_MESH:
      arguments[count++] = "cook";
      arguments[count++] = job->input;
      arguments[count++] = "--producer";
      arguments[count++] = "collision";
      arguments[count++] = "--recipe";
      arguments[count++] =
          job->kind == EDITOR_BAKE_COLLISION_HULL ? "kind=hull" : "kind=mesh";
      arguments[count++] = "--out";
      arguments[count++] = job->output;
      break;
    default:
      arguments[count++] = "cook";
      arguments[count++] = job->input;
      if (job->kind != EDITOR_BAKE_TEXTURES) {
        arguments[count++] = "--producer";
        arguments[count++] = job->kind == EDITOR_BAKE_MESH   ? "mesh"
                             : job->kind == EDITOR_BAKE_FONT ? "font"
                                                             : "texture";
      }
      break;
    }
    arguments[count++] = "--root";
    arguments[count++] = vkr_content_root();
    arguments[count++] = "--json";
    if (job->force) {
      arguments[count++] = "--force";
    }
  } else if (job->kind == EDITOR_BAKE_PROJECT) {
    arguments[count++] = "project";
    arguments[count++] = "--request";
    arguments[count++] = job->input;
    arguments[count++] = "--result";
    arguments[count++] = job->output;
  } else if (job->kind == EDITOR_BAKE_DIFFUSE_VOLUME) {
    (void)snprintf(manifest, sizeof(manifest), "%s.manifest.json", job->output);
    arguments[count++] = "bake";
    arguments[count++] = "diffuse";
    arguments[count++] = "--scene";
    arguments[count++] = job->input;
    arguments[count++] = "--output";
    arguments[count++] = job->output;
    arguments[count++] = "--manifest";
    arguments[count++] = manifest;
    /* The baker places probe bricks over the scene's bounds at its default
       1 m spacing. */
    arguments[count++] = "--face-size";
    arguments[count++] = "8";
    arguments[count++] = "--samples";
    arguments[count++] = "4";
    arguments[count++] = "--max-depth";
    arguments[count++] = "12";
    arguments[count++] = "--seed";
    arguments[count++] = "1";
    arguments[count++] = "--photons";
    arguments[count++] = "1000000";
  } else {
    arguments[count++] = "bake";
    arguments[count++] = "probe";
    arguments[count++] = "--scene";
    arguments[count++] = job->input;
    arguments[count++] = "--position";
    arguments[count++] = "-7.5";
    arguments[count++] = "2.2";
    arguments[count++] = "9";
    arguments[count++] = "--size";
    arguments[count++] = "256";
    arguments[count++] = "--output";
    arguments[count++] = job->output;
    arguments[count++] = "--harness";
    arguments[count++] = vkr_editor_tool_path(VKR_EDITOR_TOOL_HARNESS);
  }
  const VkrPlatformProcessConfig config = {
      .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
      .arguments = arguments,
      .argument_count = count,
      .working_directory = vkr_content_root(),
      .stdout_path = job->stdout_path,
      .stderr_path = job->stderr_path,
      .timeout_ms = job->kind == EDITOR_BAKE_REFLECTION_PROBE
                        ? 35u * 60u * 1000u
                    : job->kind == EDITOR_BAKE_PACKAGE ? 0u
                                                       : 30u * 60u * 1000u,
      /* A cancelled package removes its staging folder before it exits. */
      .termination_grace_ms = job->kind == EDITOR_BAKE_PACKAGE ? 5000u : 250u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = editor_bakery_is_cancelled,
      .cancel_context = bakery,
  };
  bakery->worker_process_ok = vkr_platform_process_run(
      &config, &bakery->worker_exit_code, &bakery->worker_timed_out);
  vkr_atomic_bool_store(&bakery->complete, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

VkrEditorBakery *vkr_editor_bakery_create(VkrAllocator *allocator) {
  if (!allocator || allocator->type != VKR_ALLOCATOR_TYPE_DMEMORY)
    return NULL;
  VkrEditorBakery *bakery = vkr_allocator_alloc(
      allocator, sizeof(*bakery), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!bakery)
    return NULL;
  MemZero(bakery, sizeof(*bakery));
  bakery->allocator = allocator;
  bakery->selected = EDITOR_BAKERY_NONE;
  bakery->running = EDITOR_BAKERY_NONE;
  static const char *const sources[] = {
      "assets/models/bistro.gltf",
      "assets/fonts/UbuntuMono-cooked.fontcfg",
      "assets/textures/UbuntuMono21px_0.png",
      "assets/textures",
      "renderer/src/vkr_dfg_lut_data.inc",
      "renderer/src/vkr_sheen_lut_data.inc",
      "renderer/src/vkr_anisotropy_lut_data.inc",
      "assets/scenes/fixtures/bakery_diffuse_room.scene.json",
      "assets/scenes/bistro.scene.json"};
  static const char *const outputs[] = {
      "",
      "",
      "",
      "",
      "",
      "",
      "",
      "assets/textures/bakery_diffuse_room.vkdv",
      "assets/textures/bistro_bakery_probe.vkt"};
  for (uint32_t i = 0u; i < ArrayCount(sources); ++i)
    bakery->source_lengths[i] =
        (uint32_t)snprintf((char *)bakery->source_paths[i],
                           EDITOR_BAKERY_PATH_CAPACITY, "%s", sources[i]);
  for (uint32_t i = 0u; i < ArrayCount(outputs); ++i)
    bakery->output_lengths[i] =
        (uint32_t)snprintf((char *)bakery->output_paths[i],
                           EDITOR_BAKERY_PATH_CAPACITY, "%s", outputs[i]);
  /* Until a workspace opens, job logs go below the repository's build tree,
     or below a per-user directory when the editor is installed. */
  char artifacts[EDITOR_BAKERY_PATH_CAPACITY];
  bool8_t artifacts_ok = false_v;
  if (vkr_content_root_is_repository()) {
    const int written =
        snprintf(artifacts, sizeof(artifacts), "%sbuild/_artifacts/bakery",
                 vkr_content_root());
    artifacts_ok = written > 0 && (uint32_t)written < sizeof(artifacts);
  } else {
    artifacts_ok = vkr_editor_user_path(VKR_PLATFORM_USER_CACHE, "bakery",
                                        artifacts, sizeof(artifacts));
  }
  const int directory_length =
      artifacts_ok
          ? snprintf(bakery->log_directory, sizeof(bakery->log_directory),
                     "%s/%u", artifacts, vkr_platform_get_process_id())
          : -1;
  /* Keep room for each job's /<slot>.stdout.log or .stderr.log suffix. */
  if (directory_length < 0 ||
      directory_length + 20 >= EDITOR_BAKERY_PATH_CAPACITY) {
    vkr_allocator_free(allocator, bakery, sizeof(*bakery),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
  snprintf(bakery->lock_directory, sizeof(bakery->lock_directory), "%s",
           artifacts);
  bakery->service = editor_bakery_service_create(allocator);
  return bakery;
}

void vkr_editor_bakery_destroy(VkrEditorBakery *bakery) {
  if (!bakery)
    return;
  if (bakery->worker) {
    vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
    (void)vkr_thread_join(bakery->worker);
    (void)vkr_thread_destroy(bakery->allocator, &bakery->worker);
    vkr_platform_process_lock_release(&bakery->process_lock);
  }
  editor_bakery_service_destroy(bakery->service);
  vkr_allocator_free(bakery->allocator, bakery, sizeof(*bakery),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

static void editor_bakery_remove_log(const char *path) {
  FilePath file = {.path = editor_bakery_string(path),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  (void)file_remove(&file);
}

static uint32_t editor_bakery_tail(const char *path, uint8_t *bytes,
                                   uint32_t capacity) {
  FILE *file = file_fopen(path, "rb");
  if (!file)
    return 0u;
  uint32_t length = 0u;
  if (FSEEK64(file, 0, SEEK_END) == 0) {
    const int64_t size = FTELL64(file);
    if (size > 0 &&
        FSEEK64(file, size > capacity ? size - capacity : 0, SEEK_SET) == 0)
      length = (uint32_t)fread(bytes, 1u, capacity, file);
  }
  fclose(file);
  /* A tail may begin inside a multibyte codepoint. */
  uint32_t skip = 0u;
  while (skip < length && (bytes[skip] & 0xc0u) == 0x80u)
    ++skip;
  if (skip > 0u) {
    memmove(bytes, bytes + skip, length - skip);
    length -= skip;
  }
  return length;
}

/* vkr_bakery event lines are compact JSON objects. String values are copied
 * with their escapes decoded so messages read naturally in the panel. */
static uint32_t editor_bakery_event_text(const char *line, uint64_t length,
                                         const char *field, char *out,
                                         uint32_t capacity) {
  out[0] = 0;
  VkrJsonReader reader = vkr_json_reader_create((const uint8_t *)line, length);
  String8 raw = {0};
  if (!vkr_json_get_string(&reader, field, &raw) || capacity == 0u)
    return 0u;
  uint32_t written = 0u;
  for (uint64_t i = 0u; i < raw.length && written + 4u < capacity; ++i) {
    uint8_t c = raw.str[i];
    if (c == '\\' && i + 1u < raw.length) {
      const uint8_t escape = raw.str[++i];
      if (escape == 'n')
        c = '\n';
      else if (escape == 't')
        c = '\t';
      else if (escape == 'r')
        continue;
      else if (escape == 'u' && i + 4u < raw.length) {
        uint32_t codepoint = 0u;
        for (uint32_t d = 1u; d <= 4u; ++d) {
          const uint8_t h = raw.str[i + d];
          codepoint = codepoint * 16u +
                      (uint32_t)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
        }
        i += 4u;
        if (codepoint < 0x80u) {
          out[written++] = (char)codepoint;
        } else if (codepoint < 0x800u) {
          out[written++] = (char)(0xC0u | (codepoint >> 6));
          out[written++] = (char)(0x80u | (codepoint & 0x3Fu));
        } else {
          out[written++] = (char)(0xE0u | (codepoint >> 12));
          out[written++] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
          out[written++] = (char)(0x80u | (codepoint & 0x3Fu));
        }
        continue;
      } else
        c = escape;
    }
    out[written++] = (char)c;
  }
  out[written] = 0;
  return written;
}

static int64_t editor_bakery_event_int(const char *line, uint64_t length,
                                       const char *field) {
  VkrJsonReader reader = vkr_json_reader_create((const uint8_t *)line, length);
  float64_t value = 0.0;
  return vkr_json_find_field(&reader, field) &&
                 vkr_json_parse_double(&reader, &value)
             ? (int64_t)value
             : 0;
}

typedef struct EditorBakeRunning {
  int64_t id;
  float64_t fraction;
  char label[96];
} EditorBakeRunning;

typedef struct EditorBakeDisplay {
  uint32_t total;
  uint32_t finished;
  uint32_t cached;
  uint32_t failed;
  bool8_t summary;
  EditorBakeRunning running[6];
  uint8_t *lines;
  uint32_t lines_length;
  uint32_t lines_capacity;
} EditorBakeDisplay;

static void editor_bakery_display_line(EditorBakeDisplay *display,
                                       const char *format, ...) {
  char text[768];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (length <= 0)
    return;
  if ((uint32_t)length >= sizeof(text))
    length = (int)sizeof(text) - 1;
  text[length++] = '\n';
  /* Keep the newest lines: drop whole old lines when the buffer is full. */
  while (display->lines_length + (uint32_t)length > display->lines_capacity &&
         display->lines_length) {
    uint32_t cut = display->lines_length / 4u;
    while (cut < display->lines_length && display->lines[cut - 1u] != '\n')
      ++cut;
    memmove(display->lines, display->lines + cut, display->lines_length - cut);
    display->lines_length -= cut;
  }
  if ((uint32_t)length <= display->lines_capacity) {
    MemCopy(display->lines + display->lines_length, text, (uint32_t)length);
    display->lines_length += (uint32_t)length;
  }
}

/* Applies one event to the display and, when `console` is set, reports its
 * diagnostics and failures through the editor log (the Console). */
static void editor_bakery_apply_event(EditorBakeDisplay *display,
                                      const char *line, uint64_t length,
                                      bool8_t console) {
  char event[16];
  if (!editor_bakery_event_text(line, length, "ev", event, sizeof(event)))
    return;
  char text[512];
  char source[256];
  if (!strcmp(event, "graph")) {
    display->total = (uint32_t)editor_bakery_event_int(line, length, "actions");
    display->cached = (uint32_t)editor_bakery_event_int(line, length, "cached");
    display->finished = display->cached;
  } else if (!strcmp(event, "start")) {
    for (uint32_t i = 0u; i < ArrayCount(display->running); ++i) {
      EditorBakeRunning *slot = &display->running[i];
      if (slot->id)
        continue;
      slot->id = editor_bakery_event_int(line, length, "id");
      slot->fraction = -1.0;
      (void)editor_bakery_event_text(line, length, "source", slot->label,
                                     sizeof(slot->label));
      break;
    }
  } else if (!strcmp(event, "progress")) {
    const int64_t id = editor_bakery_event_int(line, length, "id");
    for (uint32_t i = 0u; i < ArrayCount(display->running); ++i) {
      if (display->running[i].id == id) {
        VkrJsonReader reader =
            vkr_json_reader_create((const uint8_t *)line, length);
        float64_t fraction = -1.0;
        if (vkr_json_find_field(&reader, "fraction") &&
            vkr_json_parse_double(&reader, &fraction))
          display->running[i].fraction = fraction;
      }
    }
  } else if (!strcmp(event, "done")) {
    const int64_t id = editor_bakery_event_int(line, length, "id");
    (void)snprintf(source, sizeof(source), "action %lld", (long long)id);
    for (uint32_t i = 0u; i < ArrayCount(display->running); ++i) {
      if (display->running[i].id == id) {
        (void)snprintf(source, sizeof(source), "%s", display->running[i].label);
        display->running[i] = (EditorBakeRunning){0};
      }
    }
    char status[16];
    (void)editor_bakery_event_text(line, length, "status", status,
                                   sizeof(status));
    display->finished += 1u;
    if (strcmp(status, "ok")) {
      display->failed += 1u;
      editor_bakery_display_line(display, "%s  %s", status, source);
      if (console)
        log_error("Bakery: %s %s", source, status);
    } else {
      editor_bakery_display_line(
          display, "done  %s  %.2fs  %lld MiB", source,
          (float64_t)editor_bakery_event_int(line, length, "wall_ms") / 1000.0,
          (long long)editor_bakery_event_int(line, length, "peak_rss_mib"));
    }
  } else if (!strcmp(event, "diag")) {
    char code[32];
    char severity[16];
    char hint[256];
    (void)editor_bakery_event_text(line, length, "code", code, sizeof(code));
    (void)editor_bakery_event_text(line, length, "severity", severity,
                                   sizeof(severity));
    (void)editor_bakery_event_text(line, length, "source", source,
                                   sizeof(source));
    (void)editor_bakery_event_text(line, length, "message", text, sizeof(text));
    (void)editor_bakery_event_text(line, length, "hint", hint, sizeof(hint));
    const int64_t line_number = editor_bakery_event_int(line, length, "line");
    char location[300];
    if (line_number > 0)
      (void)snprintf(location, sizeof(location), "%s:%lld", source,
                     (long long)line_number);
    else
      (void)snprintf(location, sizeof(location), "%s", source);
    editor_bakery_display_line(display, "%s %s %s%s%s%s%s", severity, code,
                               location, location[0] ? ": " : "", text,
                               hint[0] ? "\n  hint: " : "", hint);
    if (console) {
      if (!strcmp(severity, "error"))
        log_error("Bakery %s %s: %s", code, location, text);
      else
        log_warn("Bakery %s %s: %s", code, location, text);
    }
  } else if (!strcmp(event, "log")) {
    char level[16];
    (void)editor_bakery_event_text(line, length, "level", level, sizeof(level));
    (void)editor_bakery_event_text(line, length, "text", text, sizeof(text));
    editor_bakery_display_line(display, "%s%s",
                               strcmp(level, "warn") ? "" : "warn ", text);
  } else if (!strcmp(event, "summary")) {
    display->summary = true_v;
    editor_bakery_display_line(
        display, "%lld ok, %lld failed, %lld cancelled, %lld cached in %.2fs",
        (long long)editor_bakery_event_int(line, length, "ok"),
        (long long)editor_bakery_event_int(line, length, "failed"),
        (long long)editor_bakery_event_int(line, length, "cancelled"),
        (long long)editor_bakery_event_int(line, length, "cached"),
        (float64_t)editor_bakery_event_int(line, length, "wall_ms") / 1000.0);
  }
}

/* Reads up to the last 1 MiB of a job's event file; returns malloc storage. */
static uint8_t *editor_bakery_read_events(const char *path, uint64_t offset,
                                          uint64_t *out_length,
                                          uint64_t *out_start) {
  *out_length = 0u;
  *out_start = offset;
  FILE *file = file_fopen(path, "rb");
  if (!file)
    return NULL;
  uint8_t *data = NULL;
  if (FSEEK64(file, 0, SEEK_END) == 0) {
    const int64_t size = FTELL64(file);
    uint64_t start = offset;
    if (size > 0 && (uint64_t)size > start + MB(1))
      start = (uint64_t)size - MB(1);
    if (size > 0 && (uint64_t)size > start &&
        FSEEK64(file, (int64_t)start, SEEK_SET) == 0) {
      data = malloc((size_t)((uint64_t)size - start) + 1u);
      if (data) {
        *out_length = fread(data, 1u, (size_t)((uint64_t)size - start), file);
        data[*out_length] = 0u;
        *out_start = start;
      }
    }
  }
  fclose(file);
  return data;
}

/* Calls `apply` for each complete line; returns bytes consumed. */
static uint64_t editor_bakery_for_lines(const uint8_t *data, uint64_t length,
                                        bool8_t skip_partial_first,
                                        EditorBakeDisplay *display,
                                        bool8_t console) {
  uint64_t start = 0u;
  if (skip_partial_first) {
    while (start < length && data[start] != '\n')
      ++start;
    start = start < length ? start + 1u : length;
  }
  uint64_t consumed = start;
  for (uint64_t i = start; i < length; ++i) {
    if (data[i] != '\n')
      continue;
    if (i > start && data[start] == '{')
      editor_bakery_apply_event(display, (const char *)data + start, i - start,
                                console);
    start = i + 1u;
    consumed = start;
  }
  return consumed;
}

/* Forwards new diagnostics of the running job to the Console. */
static void editor_bakery_forward_console(EditorBakeJob *job) {
  if (!editor_bakery_streams_events(job->kind))
    return;
  uint64_t length = 0u;
  uint64_t start = 0u;
  uint8_t *data = editor_bakery_read_events(
      job->stdout_path, job->console_offset, &length, &start);
  if (!data)
    return;
  uint8_t scratch[4096];
  EditorBakeDisplay display = {.lines = scratch,
                               .lines_capacity = sizeof(scratch),
                               .total = job->actions_total,
                               .finished = job->actions_finished,
                               .failed = job->actions_failed};
  job->console_offset =
      start + editor_bakery_for_lines(
                  data, length, start != job->console_offset, &display, true_v);
  job->actions_total = display.total;
  job->actions_finished = display.finished;
  job->actions_failed = display.failed;
  free(data);
}

static void editor_bakery_read_log(VkrEditorBakery *bakery) {
  if (bakery->selected == EDITOR_BAKERY_NONE)
    return;
  const EditorBakeJob *job = &bakery->jobs[bakery->selected];
  uint32_t length = (uint32_t)snprintf(
      (char *)bakery->log, sizeof(bakery->log),
      "%s | %s | exit %d%s\nSource: %s\nOutput: %s\n",
      editor_bakery_kind_names[job->kind], editor_bakery_status(job->status),
      job->exit_code, job->timed_out ? " | timed out" : "", job->input,
      job->output[0] ? job->output : "Declared by the recipe");
  if (!editor_bakery_streams_events(job->kind)) {
    static const char output[] = "\nOutput (tail):\n";
    MemCopy(bakery->log + length, output, sizeof(output) - 1u);
    length += sizeof(output) - 1u;
    length += editor_bakery_tail(job->stdout_path, bakery->log + length, KB(8));
    static const char errors[] = "\nErrors (tail):\n";
    MemCopy(bakery->log + length, errors, sizeof(errors) - 1u);
    length += sizeof(errors) - 1u;
    length += editor_bakery_tail(job->stderr_path, bakery->log + length,
                                 (uint32_t)sizeof(bakery->log) - length - 1u);
    bakery->log[length] = 0u;
    bakery->log_length = length;
    return;
  }
  uint8_t lines[KB(12)];
  EditorBakeDisplay display = {.lines = lines, .lines_capacity = sizeof(lines)};
  uint64_t events_length = 0u;
  uint64_t start = 0u;
  uint8_t *data =
      editor_bakery_read_events(job->stdout_path, 0u, &events_length, &start);
  if (data) {
    (void)editor_bakery_for_lines(data, events_length, start != 0u, &display,
                                  false_v);
    free(data);
  }
  length += (uint32_t)snprintf(
      (char *)bakery->log + length, sizeof(bakery->log) - length,
      "Progress: %u/%u actions, %u cached, %u failed\n", display.finished,
      display.total, display.cached, display.failed);
  for (uint32_t i = 0u; i < ArrayCount(display.running); ++i) {
    const EditorBakeRunning *running = &display.running[i];
    if (!running->id || length + 160u >= sizeof(bakery->log))
      continue;
    length += (uint32_t)snprintf(
        (char *)bakery->log + length, sizeof(bakery->log) - length,
        running->fraction >= 0.0 ? "Running: %s %.0f%%\n" : "Running: %s\n",
        running->label, running->fraction * 100.0);
  }
  static const char separator[] = "\n";
  MemCopy(bakery->log + length, separator, sizeof(separator) - 1u);
  length += sizeof(separator) - 1u;
  const uint32_t copy =
      Min(display.lines_length, (uint32_t)sizeof(bakery->log) - length - 1u);
  MemCopy(bakery->log + length, display.lines + display.lines_length - copy,
          copy);
  length += copy;
  /* With no events the process never ran; show why. */
  if (!display.total && !display.summary &&
      length + 64u < sizeof(bakery->log)) {
    const uint32_t tail =
        editor_bakery_tail(job->stderr_path, bakery->log + length,
                           (uint32_t)sizeof(bakery->log) - length - 1u);
    length += tail;
  }
  bakery->log[length] = 0u;
  bakery->log_length = length;
}

/* Recompiles the catalog this editor loads whenever a shader source changes.
   The renderer has no pipeline hot reload (ARCHITECTURE.md), so the Console
   asks for a restart once the compile succeeds. */
static void editor_bakery_watch_shaders(VkrEditorBakery *bakery) {
  if (!bakery->shader_watch_requested) {
    bakery->shader_watch_requested = true_v;
    /* An installed editor has a compiled catalog and no shader sources. */
    char catalog[EDITOR_BAKERY_PATH_CAPACITY];
    char sources[EDITOR_BAKERY_PATH_CAPACITY];
    if (!vkr_content_root_is_repository() ||
        !vkr_shader_catalog_root(catalog, sizeof(catalog))) {
      return;
    }
    (void)snprintf(sources, sizeof(sources), "%srenderer/src/shaders",
                   vkr_content_root());
    const char *paths[] = {sources};
    const char *arguments[] = {"shaders", "--out", catalog, "--root",
                               vkr_content_root()};
    bakery->shader_watch =
        editor_bakery_service_watch(bakery->service, paths, ArrayCount(paths),
                                    arguments, ArrayCount(arguments));
  }
  char changed[4][EDITOR_BAKERY_SERVICE_PATH];
  while (editor_bakery_service_take_changes(
      bakery->service, bakery->shader_watch, changed, ArrayCount(changed))) {
  }
  EditorBakeryRebuild rebuild = {0};
  if (!editor_bakery_service_take_rebuild(bakery->service, bakery->shader_watch,
                                          &rebuild)) {
    return;
  }
  if (rebuild.exit_code == 3) {
    log_warn("Shaders: background compile cancelled");
  } else if (rebuild.exit_code != 0) {
    log_error("Shaders: background compile failed (exit %d, %u of %u "
              "entries failed); the diagnostics are above",
              rebuild.exit_code, rebuild.failed, rebuild.actions);
  } else if (rebuild.actions > rebuild.cached) {
    log_warn("Shaders: recompiled %u entries; restart the editor to "
             "load them",
             rebuild.actions - rebuild.cached);
  }
}

/* Rebuilds the project's Custom material library (`vkr_bakery materials`)
   when a file under the content root's assets changes, or the engine's
   tiled shader source the library repeats; Bakery compiles only when the
   generated source changed (ADR-096). Metal only. */
static void editor_bakery_watch_materials(VkrEditorBakery *bakery) {
#if defined(PLATFORM_APPLE)
  if (!bakery->materials_watch_requested) {
    bakery->materials_watch_requested = true_v;
    char catalog[EDITOR_BAKERY_PATH_CAPACITY];
    char assets[EDITOR_BAKERY_PATH_CAPACITY];
    char engine[EDITOR_BAKERY_PATH_CAPACITY];
    if (!vkr_shader_catalog_root(catalog, sizeof(catalog))) {
      return;
    }
    (void)snprintf(assets, sizeof(assets), "%sassets", vkr_content_root());
    (void)snprintf(engine, sizeof(engine), "%s/metal/library.metal", catalog);
    const char *paths[] = {assets, engine};
    const char *arguments[] = {"materials", "--shaders", catalog, "--root",
                               vkr_content_root()};
    bakery->materials_watch =
        editor_bakery_service_watch(bakery->service, paths, ArrayCount(paths),
                                    arguments, ArrayCount(arguments));
  }
  char changed[4][EDITOR_BAKERY_SERVICE_PATH];
  while (editor_bakery_service_take_changes(
      bakery->service, bakery->materials_watch, changed, ArrayCount(changed))) {
  }
  EditorBakeryRebuild rebuild = {0};
  if (!editor_bakery_service_take_rebuild(bakery->service,
                                          bakery->materials_watch, &rebuild)) {
    return;
  }
  if (rebuild.exit_code != 0 && rebuild.exit_code != 3) {
    log_error("Materials: the Custom material library did not compile "
              "(exit %d); the diagnostics are above",
              rebuild.exit_code);
  } else if (rebuild.exit_code == 0 && rebuild.actions > rebuild.cached) {
    log_info("Materials: Custom material library rebuilt; reloading");
    bakery->materials_compiled = true_v;
  }
#else
  (void)bakery;
#endif
}

bool8_t vkr_editor_bakery_take_materials_compiled(VkrEditorBakery *bakery) {
  if (!bakery || !bakery->materials_compiled)
    return false_v;
  bakery->materials_compiled = false_v;
  return true_v;
}

void vkr_editor_bakery_update(VkrEditorBakery *bakery) {
  if (!bakery)
    return;
  if (bakery->service) {
    editor_bakery_watch_shaders(bakery);
    editor_bakery_watch_materials(bakery);
    editor_bakery_service_update(bakery->service);
  }
  if (bakery->worker &&
      vkr_atomic_bool_load(&bakery->complete, VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)vkr_thread_join(bakery->worker);
    (void)vkr_thread_destroy(bakery->allocator, &bakery->worker);
    vkr_platform_process_lock_release(&bakery->process_lock);
    EditorBakeJob *job = &bakery->jobs[bakery->running];
    job->exit_code = bakery->worker_exit_code;
    job->process_ok = bakery->worker_process_ok;
    job->timed_out = bakery->worker_timed_out;
    /* Completion can race a late Cancel click. A successfully reaped child
       published its output; the request must not relabel that success. */
    job->status = job->process_ok && !job->timed_out && job->exit_code == 0
                      ? EDITOR_BAKE_SUCCEEDED
                  : vkr_atomic_bool_load(&bakery->cancel_requested,
                                         VKR_MEMORY_ORDER_ACQUIRE)
                      ? EDITOR_BAKE_CANCELLED
                      : EDITOR_BAKE_FAILED;
    if (job->status == EDITOR_BAKE_FAILED) {
      log_error("Bakery: %s (exit %d); output: %s", job->input, job->exit_code,
                job->stderr_path);
    } else {
      log_info("Bakery: %s: %s", editor_bakery_status(job->status), job->input);
    }
    editor_bakery_forward_console(job);
    bakery->running = EDITOR_BAKERY_NONE;
    bakery->next_log_read = 0.0;
  }
  if (!bakery->worker) {
    for (uint32_t i = 0u; i < bakery->job_count; ++i) {
      EditorBakeJob *job = &bakery->jobs[i];
      if (job->status != EDITOR_BAKE_QUEUED)
        continue;
      bakery->running = i;
      job->status = EDITOR_BAKE_RUNNING;
      bakery->worker_exit_code = -1;
      bakery->worker_process_ok = false_v;
      bakery->worker_timed_out = false_v;
      vkr_atomic_bool_store(&bakery->cancel_requested, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      vkr_atomic_bool_store(&bakery->complete, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      if (!vkr_platform_process_lock_acquire("VkrEditorBakery",
                                             bakery->lock_directory,
                                             &bakery->process_lock)) {
        bakery->running = EDITOR_BAKERY_NONE;
        job->status = EDITOR_BAKE_QUEUED;
        snprintf(bakery->message, sizeof(bakery->message),
                 "Waiting for the current asset worker to finish or cancel.");
        break;
      }
      if (!vkr_thread_create(bakery->allocator, &bakery->worker,
                             editor_bakery_worker, bakery)) {
        vkr_platform_process_lock_release(&bakery->process_lock);
        bakery->running = EDITOR_BAKERY_NONE;
        job->status = EDITOR_BAKE_FAILED;
        snprintf(bakery->message, sizeof(bakery->message),
                 "Unable to start baker; another editor may be baking.");
        log_error("Bakery: unable to launch %s", job->input);
      } else {
        log_info("Bakery: started %s", job->input);
      }
      bakery->next_log_read = 0.0;
      break;
    }
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (bakery->running != EDITOR_BAKERY_NONE &&
      now >= bakery->next_console_read) {
    editor_bakery_forward_console(&bakery->jobs[bakery->running]);
    bakery->next_console_read = now + 0.25;
  }
  const bool8_t selected_running =
      bakery->selected < bakery->job_count &&
      bakery->jobs[bakery->selected].status == EDITOR_BAKE_RUNNING;
  if (now >= bakery->next_log_read &&
      (selected_running || bakery->next_log_read == 0.0)) {
    editor_bakery_read_log(bakery);
    bakery->next_log_read = now + 0.25;
  }
}

static void editor_bakery_enqueue(VkrEditorBakery *bakery, EditorBakeKind kind,
                                  const char *input, const char *output) {
  char catalog[EDITOR_BAKERY_PATH_CAPACITY];
  if (kind == EDITOR_BAKE_SHADERS) {
    /* Compile into the catalog this editor's renderer loads. */
    if (!vkr_shader_catalog_root(catalog, sizeof(catalog))) {
      snprintf(bakery->message, sizeof(bakery->message),
               "The shader catalog path is too long.");
      return;
    }
    input = catalog;
  }
  if (!input[0]) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Enter a source path or font configuration first.");
    return;
  }
  const char *extension = strrchr(input, '.');
  const String8 source_extension =
      editor_bakery_string(extension ? extension : "");
  const String8 gltf = string8_lit(".gltf"), glb = string8_lit(".glb"),
                obj = string8_lit(".obj");
  if (kind == EDITOR_BAKE_MESH && !string8_equalsi(&source_extension, &gltf) &&
      !string8_equalsi(&source_extension, &glb) &&
      !string8_equalsi(&source_extension, &obj)) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Mesh source must end in .gltf, .glb or .obj.");
    return;
  }
  if ((kind == EDITOR_BAKE_COLLISION_HULL ||
       kind == EDITOR_BAKE_COLLISION_MESH) &&
      !string8_equalsi(&source_extension, &gltf) &&
      !string8_equalsi(&source_extension, &glb)) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Collision source must end in .gltf or .glb.");
    return;
  }
  if (editor_bakery_uses_explicit_output(kind) && !output[0]) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Enter an output path before adding this bake.");
    return;
  }
  const char *output_dot = output ? strrchr(output, '.') : NULL;
  const String8 output_extension =
      editor_bakery_string(output_dot ? output_dot : "");
  const String8 vkdv = string8_lit(".vkdv"), vkt = string8_lit(".vkt");
  if ((kind == EDITOR_BAKE_DIFFUSE_VOLUME &&
       !string8_equalsi(&output_extension, &vkdv)) ||
      (kind == EDITOR_BAKE_REFLECTION_PROBE &&
       !string8_equalsi(&output_extension, &vkt))) {
    snprintf(
        bakery->message, sizeof(bakery->message), "%s output must end in %s.",
        kind == EDITOR_BAKE_DIFFUSE_VOLUME ? "Diffuse volume" : "Reflection",
        kind == EDITOR_BAKE_DIFFUSE_VOLUME ? ".vkdv" : ".vkt");
    return;
  }
  if ((kind == EDITOR_BAKE_COLLISION_HULL ||
       kind == EDITOR_BAKE_COLLISION_MESH) &&
      (!output_dot || strcmp(output_dot, ".vkc"))) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Collision output must end in .vkc.");
    return;
  }
  if (bakery->job_count == EDITOR_BAKERY_JOB_CAPACITY) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Queue full; clear finished jobs first.");
    return;
  }
  String8 log_directory = editor_bakery_string(bakery->log_directory);
  if (!file_ensure_directory(bakery->allocator, &log_directory)) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Cannot create bake log directory.");
    return;
  }
  const uint32_t index = bakery->job_count++;
  EditorBakeJob *job = &bakery->jobs[index];
  *job = (EditorBakeJob){
      .id = ++bakery->next_job_id,
      .kind = kind,
      .status = EDITOR_BAKE_QUEUED,
      .force = editor_bakery_supports_force(kind) ? bakery->force : false_v,
      .exit_code = -1};
  snprintf(job->input, sizeof(job->input), "%s", input);
  if (kind == EDITOR_BAKE_SHADERS) {
    snprintf(job->output, sizeof(job->output), "%s", input);
  } else if (editor_bakery_is_static_table(kind)) {
    snprintf(job->input, sizeof(job->input), "%s",
             editor_bakery_table_output(kind));
    snprintf(job->output, sizeof(job->output), "%s",
             editor_bakery_table_output(kind));
  } else if (kind == EDITOR_BAKE_MESH)
    snprintf(job->output, sizeof(job->output), "%.*s.vkb",
             (int)(extension - input), input);
  else if (kind == EDITOR_BAKE_TEXTURE)
    snprintf(job->output, sizeof(job->output), "%s.vkt", input);
  else if (editor_bakery_uses_explicit_output(kind))
    snprintf(job->output, sizeof(job->output), "%s", output);
  snprintf(job->stdout_path, sizeof(job->stdout_path),
           editor_bakery_streams_events(kind) ? "%s/%u.events.jsonl"
                                              : "%s/%u.stdout.log",
           bakery->log_directory, index);
  snprintf(job->stderr_path, sizeof(job->stderr_path), "%s/%u.stderr.log",
           bakery->log_directory, index);
  editor_bakery_remove_log(job->stdout_path);
  editor_bakery_remove_log(job->stderr_path);
  bakery->selected = index;
  bakery->view = EDITOR_BAKERY_JOBS;
  bakery->message[0] = '\0';
  bakery->next_log_read = 0.0;
}

uint64_t vkr_editor_bakery_project_start(VkrEditorBakery *bakery,
                                         const char *request_path,
                                         const char *result_path) {
  if (!bakery || !request_path || !result_path || !request_path[0] ||
      !result_path[0] || strlen(request_path) >= EDITOR_BAKERY_PATH_CAPACITY ||
      strlen(result_path) + 12u >= EDITOR_BAKERY_PATH_CAPACITY) {
    return 0;
  }
  uint32_t slot = bakery->job_count;
  if (slot == EDITOR_BAKERY_JOB_CAPACITY) {
    for (slot = 0u; slot < bakery->job_count; ++slot) {
      if (bakery->jobs[slot].status >= EDITOR_BAKE_SUCCEEDED) {
        break;
      }
    }
    if (slot == bakery->job_count) {
      return 0;
    }
  } else {
    ++bakery->job_count;
  }
  EditorBakeJob *job = &bakery->jobs[slot];
  *job = (EditorBakeJob){.id = ++bakery->next_job_id,
                         .kind = EDITOR_BAKE_PROJECT,
                         .status = EDITOR_BAKE_QUEUED,
                         .exit_code = -1};
  snprintf(job->input, sizeof(job->input), "%s", request_path);
  snprintf(job->output, sizeof(job->output), "%s", result_path);
  snprintf(job->stdout_path, sizeof(job->stdout_path), "%s.stdout.log",
           result_path);
  snprintf(job->stderr_path, sizeof(job->stderr_path), "%s.stderr.log",
           result_path);
  bakery->selected = slot;
  bakery->next_log_read = 0;
  return job->id;
}

uint64_t vkr_editor_bakery_package_start(VkrEditorBakery *bakery,
                                         const char *project_directory,
                                         const char *profile,
                                         const char *events_path) {
  if (!bakery || !project_directory || !profile || !events_path ||
      !project_directory[0] || !profile[0] ||
      strlen(project_directory) >= EDITOR_BAKERY_PATH_CAPACITY ||
      strlen(profile) >= EDITOR_BAKERY_PATH_CAPACITY ||
      strlen(events_path) + 12u >= EDITOR_BAKERY_PATH_CAPACITY) {
    return 0;
  }
  const uint64_t id =
      vkr_editor_bakery_project_start(bakery, project_directory, events_path);
  for (uint32_t i = 0; id && i < bakery->job_count; ++i) {
    EditorBakeJob *job = &bakery->jobs[i];
    if (job->id != id) {
      continue;
    }
    job->kind = EDITOR_BAKE_PACKAGE;
    snprintf(job->output, sizeof(job->output), "%s", profile);
    snprintf(job->stdout_path, sizeof(job->stdout_path), "%s", events_path);
    snprintf(job->stderr_path, sizeof(job->stderr_path), "%s.stderr.log",
             events_path);
    editor_bakery_remove_log(job->stdout_path);
    editor_bakery_remove_log(job->stderr_path);
  }
  return id;
}

VkrEditorProjectJobStatus
vkr_editor_bakery_project_status(VkrEditorBakery *bakery, uint64_t job_id,
                                 String8 *log) {
  if (log) {
    *log = (String8){0};
  }
  if (!bakery || !job_id) {
    return VKR_EDITOR_PROJECT_JOB_UNKNOWN;
  }
  for (uint32_t i = 0; i < bakery->job_count; ++i) {
    if (bakery->jobs[i].id == job_id) {
      if (log && bakery->selected == i) {
        *log = string8_create(bakery->log, bakery->log_length);
      }
      return (VkrEditorProjectJobStatus)bakery->jobs[i].status;
    }
  }
  return VKR_EDITOR_PROJECT_JOB_UNKNOWN;
}

void vkr_editor_bakery_project_cancel(VkrEditorBakery *bakery,
                                      uint64_t job_id) {
  if (!bakery || !job_id) {
    return;
  }
  for (uint32_t i = 0; i < bakery->job_count; ++i) {
    EditorBakeJob *job = &bakery->jobs[i];
    if (job->id != job_id) {
      continue;
    }
    if (job->status == EDITOR_BAKE_QUEUED) {
      job->status = EDITOR_BAKE_CANCELLED;
    } else if (job->status == EDITOR_BAKE_RUNNING) {
      vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
    }
  }
}

static VkrUiWidgetConfig editor_bakery_widget(uint32_t row, uint32_t column) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.row = row;
  config.placement.column = column;
  config.style.padding_pt = (VkrUiEdges){3.0f, 8.0f, 3.0f, 8.0f};
  config.style.font_size_pt = vkr_ui_theme()->font_body;
  config.style.text_color = vkr_ui_theme()->text;
  return config;
}

static const VkrUiIcon editor_bakery_kind_icons[] = {
    VKR_UI_ICON_MESH,     VKR_UI_ICON_FONT,         VKR_UI_ICON_TEXTURE,
    VKR_UI_ICON_FOLDER,   VKR_UI_ICON_GRAPH,        VKR_UI_ICON_SPARKLE,
    VKR_UI_ICON_WAVES,    VKR_UI_ICON_SUN_DIM,      VKR_UI_ICON_PROBE,
    VKR_UI_ICON_COLLIDER, VKR_UI_ICON_BOUNDING_BOX, VKR_UI_ICON_CODE,
    VKR_UI_ICON_PROJECT,  VKR_UI_ICON_EXPORT};
_Static_assert(ArrayCount(editor_bakery_kind_icons) ==
                   ArrayCount(editor_bakery_kind_names),
               "Every bake kind needs an icon");

static void editor_bakery_tab_style(VkrUiWidgetConfig *config,
                                    VkrFontHandle heading, bool8_t selected) {
  vkr_editor_action_style(config, heading);
  config->fill = true_v;
  config->text.font = selected ? heading : VKR_FONT_HANDLE_INVALID;
  config->style.text_color =
      selected ? vkr_ui_theme()->text : vkr_ui_theme()->text_secondary;
  config->icon_color =
      selected ? vkr_ui_theme()->accent_hover : vkr_ui_theme()->text_secondary;
  if (selected) {
    config->style.background_color =
        vkr_ui_color_alpha(vkr_ui_theme()->accent, 0.2f);
    config->style.hover_background_color =
        vkr_ui_color_alpha(vkr_ui_theme()->accent, 0.28f);
    config->style.border_color = vkr_ui_theme()->accent;
  }
}

static void editor_bakery_setup(VkrEditorBakery *bakery, VkrUiSystem *ui,
                                VkrFontHandle heading, float32_t width) {
  const bool8_t wide = width >= 628.0f;
  const uint32_t recipe_columns = wide ? 4u : width >= 224.0f ? 2u : 1u;
  const uint32_t recipe_row_count =
      (EDITOR_BAKE_KIND_COUNT + recipe_columns - 1u) / recipe_columns;
  const VkrUiTrack column = {.unit = VKR_UI_TRACK_FR, .value = 1.0f};
  VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_PX,
       .value = (28.0f + 4.0f) * recipe_row_count - 4.0f},
      {.unit = VKR_UI_TRACK_PX, .value = wide ? 30.0f : 82.0f},
      {.unit = VKR_UI_TRACK_PX,
       .value = wide || (bakery->kind != EDITOR_BAKE_FONT &&
                         !editor_bakery_uses_explicit_output(bakery->kind))
                    ? 28.0f
                : width < 224.0f ? 92.0f
                                 : 60.0f},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  VkrUiPanelConfig setup = vkr_ui_panel_config_default();
  setup.placement.row = 1u;
  setup.columns = &column;
  setup.column_count = 1u;
  setup.rows = rows;
  setup.row_count = ArrayCount(rows);
  setup.style.gap_pt = 4.0f;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("setup"), &setup))
    return;

  const VkrUiTrack equal[] = {column, column, column, column};
  const VkrUiTrack recipe_rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
  };
  VkrUiPanelConfig recipes = vkr_ui_panel_config_default();
  recipes.placement.row = 0u;
  recipes.columns = equal;
  recipes.column_count = recipe_columns;
  recipes.rows = recipe_rows;
  recipes.row_count = recipe_row_count;
  recipes.style.gap_pt = 4.0f;
  if (vkr_ui_panel_begin(ui, string8_lit("types"), &recipes)) {
    for (uint32_t i = 0u; i < EDITOR_BAKE_KIND_COUNT; ++i) {
      VkrUiWidgetConfig config = editor_bakery_widget(i / recipes.column_count,
                                                      i % recipes.column_count);
      editor_bakery_tab_style(&config, heading,
                              bakery->kind == (EditorBakeKind)i);
      config.icon = editor_bakery_kind_icons[i];
      config.icon_size_pt = 14.0f;
      config.tooltip = editor_bakery_string(editor_bakery_kind_names[i]);
      if (vkr_ui_button(
              ui, editor_bakery_string(editor_bakery_kind_names[i]),
              editor_bakery_string(i == EDITOR_BAKE_TEXTURES && width < 124.0f
                                       ? "Folder"
                                       : editor_bakery_kind_names[i]),
              &config)) {
        bakery->kind = (EditorBakeKind)i;
        bakery->message[0] = '\0';
      }
    }
    (void)vkr_ui_panel_end(ui);
  }

  const VkrUiTrack source_columns[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 58.0f},
      column,
      {.unit = VKR_UI_TRACK_PX, .value = 110.0f},
  };
  const VkrUiTrack source_rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 20.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
  };
  VkrUiPanelConfig source = vkr_ui_panel_config_default();
  source.placement.row = 1u;
  source.columns = wide ? source_columns : &column;
  source.column_count = wide ? 3u : 1u;
  source.rows = wide ? NULL : source_rows;
  source.row_count = wide ? 0u : ArrayCount(source_rows);
  source.style.gap_pt = 3.0f;
  if (vkr_ui_panel_begin(ui, string8_lit("source"), &source)) {
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    vkr_ui_label(ui, string8_lit("label"),
                 bakery->kind == EDITOR_BAKE_SHADERS ? string8_lit("Catalog")
                 : editor_bakery_is_static_table(bakery->kind)
                     ? string8_lit("Table")
                     : string8_lit("Source"),
                 &config);
    config = editor_bakery_widget(wide ? 0u : 1u, wide ? 1u : 0u);
    vkr_editor_field_style(&config);
    static const char *const path_hints[] = {
        "Mesh source: .gltf, .glb or .obj. Project-relative or absolute path.",
        "Font configuration: .fontcfg. Project-relative or absolute path.",
        "Single 2D texture source. Project-relative or absolute path.",
        "Texture directory. Project-relative or absolute path.",
        "Fixed output for the shared GGX DFG static table.",
        "Fixed output for the shared Charlie sheen static table.",
        "Fixed output for the shared anisotropic GGX static table.",
        "Scene JSON used to bake a diffuse volume.",
        "Scene JSON captured into a six-face reflection probe.",
        "Static glTF/GLB geometry; cooks one convex hull. Split detailed "
        "sources into subtrees with the CLI.",
        "Static glTF/GLB geometry; cooks triangle collision for static or "
        "kinematic bodies.",
        "Shader catalog this editor's renderer loads.",
    };
    _Static_assert(ArrayCount(path_hints) == EDITOR_BAKE_KIND_COUNT,
                   "Every bake recipe needs a source hint");
    config.tooltip = editor_bakery_string(path_hints[bakery->kind]);
    config.disabled = editor_bakery_has_fixed_source(bakery->kind);
    VkrUiTextEditBuffer buffer = {.data = bakery->source_paths[bakery->kind],
                                  .length =
                                      bakery->source_lengths[bakery->kind],
                                  .capacity = EDITOR_BAKERY_PATH_CAPACITY};
    if (vkr_ui_text_field(ui, string8_lit("path"), &buffer, &config))
      bakery->message[0] = '\0';
    bakery->source_lengths[bakery->kind] = buffer.length;
    config = editor_bakery_widget(wide ? 0u : 2u, wide ? 2u : 0u);
    vkr_editor_action_style(&config, heading);
    config.disabled = (!buffer.length && bakery->kind != EDITOR_BAKE_SHADERS) ||
                      (editor_bakery_uses_explicit_output(bakery->kind) &&
                       !bakery->output_lengths[bakery->kind]) ||
                      bakery->job_count == EDITOR_BAKERY_JOB_CAPACITY;
    config.tooltip = string8_lit("Add this source to the bake queue");
    if (vkr_ui_button(ui, string8_lit("bake"),
                      width < 124.0f ? string8_lit("Bake")
                                     : string8_lit("Queue bake"),
                      &config))
      editor_bakery_enqueue(bakery, bakery->kind, (char *)buffer.data,
                            (char *)bakery->output_paths[bakery->kind]);
    (void)vkr_ui_panel_end(ui);
  }

  VkrUiPanelConfig options = vkr_ui_panel_config_default();
  options.placement.row = 2u;
  options.columns = equal;
  options.column_count = wide ? 3u : 1u;
  options.rows = recipe_rows;
  options.row_count =
      wide || (bakery->kind != EDITOR_BAKE_FONT &&
               !editor_bakery_uses_explicit_output(bakery->kind))
          ? 1u
      : width < 224.0f ? 3u
                       : 2u;
  options.style.gap_pt = 4.0f;
  if (vkr_ui_panel_begin(ui, string8_lit("options"), &options)) {
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    if (editor_bakery_uses_explicit_output(bakery->kind)) {
      vkr_ui_label(ui, string8_lit("output-label"), string8_lit("Output"),
                   &config);
      config = editor_bakery_widget(wide ? 0u : 1u, wide ? 1u : 0u);
      config.placement.column_span = wide ? 2u : 1u;
      vkr_editor_field_style(&config);
      config.tooltip = (bakery->kind == EDITOR_BAKE_COLLISION_HULL ||
                        bakery->kind == EDITOR_BAKE_COLLISION_MESH)
                           ? string8_lit("Collision asset output: .vkc")
                       : bakery->kind == EDITOR_BAKE_DIFFUSE_VOLUME
                           ? string8_lit("Volume output: .vkdv")
                           : string8_lit("Cubemap output: .vkt");
      VkrUiTextEditBuffer output = {.data = bakery->output_paths[bakery->kind],
                                    .length =
                                        bakery->output_lengths[bakery->kind],
                                    .capacity = EDITOR_BAKERY_PATH_CAPACITY};
      if (vkr_ui_text_field(ui, string8_lit("output"), &output, &config))
        bakery->message[0] = '\0';
      bakery->output_lengths[bakery->kind] = output.length;
    } else if (editor_bakery_supports_force(bakery->kind)) {
      config.tooltip = string8_lit(
          "Bake again even when the cache already holds this result");
      (void)vkr_ui_checkbox(ui, string8_lit("force"),
                            width < 180.0f ? string8_lit("Rebuild")
                                           : string8_lit("Rebuild existing"),
                            &bakery->force, &config);
    }
    if (bakery->kind == EDITOR_BAKE_FONT) {
      const VkrUiTrack presets_columns[] = {column, column};
      VkrUiPanelConfig presets = vkr_ui_panel_config_default();
      presets.placement.row = wide ? 0u : 1u;
      presets.placement.column = wide ? 1u : 0u;
      presets.placement.column_span = wide ? 2u : 1u;
      presets.columns = presets_columns;
      presets.column_count = width < 224.0f ? 1u : 2u;
      presets.placement.row_span = width < 224.0f ? 2u : 1u;
      presets.rows = recipe_rows;
      presets.row_count = width < 224.0f ? 2u : 1u;
      presets.style.gap_pt = 4.0f;
      if (vkr_ui_panel_begin(ui, string8_lit("presets"), &presets)) {
        static const char *const names[] = {"Regular", "Bold"};
        static const char *const paths[] = {
            "assets/fonts/UbuntuMono-cooked.fontcfg",
            "assets/fonts/UbuntuMono-Bold-cooked.fontcfg"};
        for (uint32_t i = 0u; i < ArrayCount(names); ++i) {
          config = editor_bakery_widget(i / presets.column_count,
                                        i % presets.column_count);
          vkr_editor_action_style(&config, heading);
          config.tooltip =
              string8_lit("Fill the source with an editor font configuration");
          if (vkr_ui_button(ui, editor_bakery_string(names[i]),
                            editor_bakery_string(names[i]), &config)) {
            bakery->source_lengths[EDITOR_BAKE_FONT] = (uint32_t)snprintf(
                (char *)bakery->source_paths[EDITOR_BAKE_FONT],
                EDITOR_BAKERY_PATH_CAPACITY, "%s", paths[i]);
            bakery->message[0] = '\0';
          }
        }
        (void)vkr_ui_panel_end(ui);
      }
    }
    (void)vkr_ui_panel_end(ui);
  }
  static const char *const help[] = {
      "Accepts .gltf, .glb, .obj. Writes <source stem>.vkb beside the "
      "source.\nReload the scene to use the baked mesh.",
      "Accepts .fontcfg. Writes the atlas and metrics specified by the "
      "configuration.\nRestart the editor to use a rebaked editor font.",
      "Writes <source>.vkt beside the 2D image.\nReload the scene to use the "
      "baked texture.",
      "Bakes all supported textures in this directory.\nReload the scene to "
      "use the baked textures.",
      "Regenerates the checked-in GGX DFG static table. Rebuild the renderer "
      "after this recipe succeeds.",
      "Regenerates the checked-in Charlie sheen static table. Rebuild the "
      "renderer after this recipe succeeds.",
      "Regenerates the checked-in anisotropic GGX static table. Rebuild the "
      "renderer after this recipe succeeds.",
      "Writes a checked diffuse volume and dependency metadata. Uses the "
      "existing bounded 4x4x4, 1M-photon recipe.",
      "Captures six 256px HDR faces at Bistro's selected probe center and "
      "writes a reflection cubemap. Uses the existing 330-second-per-face "
      "recipe.",
      "Cooks one convex hull from static glTF/GLB geometry into the .vkc "
      "output.\nSplit detailed sources into subtrees with the CLI.",
      "Cooks triangle collision from static glTF/GLB geometry into the .vkc "
      "output.\nUse it for static or kinematic bodies.",
      "Compiles changed Metal and Vulkan shaders into the renderer's catalog. "
      "Unchanged shaders come from the cache.\nRestart the editor to load "
      "them.",
  };
  _Static_assert(ArrayCount(help) == EDITOR_BAKE_KIND_COUNT,
                 "Every bake recipe needs help text");
  VkrUiWidgetConfig hint = editor_bakery_widget(3u, 0u);
  hint.text.layout.word_wrap = true_v;
  hint.text.layout.max_width = Max(1.0f, width - 12.0f);
  hint.style.text_color = bakery->message[0] ? vkr_ui_theme()->warning
                                             : vkr_ui_theme()->text_secondary;
  vkr_ui_label(ui, string8_lit("help"),
               editor_bakery_string(bakery->message[0] ? bakery->message
                                                       : help[bakery->kind]),
               &hint);
  (void)vkr_ui_scroll_area_end(ui);
}

static void editor_bakery_jobs(VkrEditorBakery *bakery, VkrUiSystem *ui,
                               VkrFontHandle heading, float32_t width,
                               float32_t height) {
  const uint32_t action_columns = width >= 628.0f   ? 4u
                                  : width >= 224.0f ? 2u
                                                    : 1u;
  const float32_t actions_height = 32.0f * (4u / action_columns) - 4.0f;
  const bool8_t scroll_pane = height < actions_height + 60.0f;
  const VkrUiTrack rows[] = {
      {.unit = scroll_pane ? VKR_UI_TRACK_PX : VKR_UI_TRACK_FR,
       .value = scroll_pane ? 56.0f : 1.0f},
      {.unit = VKR_UI_TRACK_PX, .value = actions_height},
  };
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement.row = 1u;
  pane.rows = rows;
  pane.row_count = ArrayCount(rows);
  pane.style.gap_pt = 4.0f;
  if (!(scroll_pane ? vkr_ui_scroll_area_begin(ui, string8_lit("queue"), &pane)
                    : vkr_ui_panel_begin(ui, string8_lit("queue"), &pane)))
    return;
  VkrUiPanelConfig jobs = vkr_ui_panel_config_default();
  jobs.placement.row = 0u;
  VkrUiTrack job_rows[EDITOR_BAKERY_JOB_CAPACITY];
  const uint32_t count = bakery->job_count ? bakery->job_count : 1u;
  for (uint32_t i = 0u; i < count; ++i)
    job_rows[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_PX, .value = 28.0f};
  jobs.rows = job_rows;
  jobs.row_count = count;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("jobs"), &jobs)) {
    if (!bakery->job_count) {
      VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
      config.text.layout.word_wrap = true_v;
      config.text.layout.max_width = Max(1.0f, width - 12.0f);
      vkr_ui_label(ui, string8_lit("empty"),
                   string8_lit("No jobs yet. Start in New bake."), &config);
    }
    for (uint32_t i = 0u; i < bakery->job_count; ++i) {
      const EditorBakeJob *job = &bakery->jobs[i];
      const char *name = job->input;
      for (const char *p = name; *p; ++p)
        if (*p == '/' || *p == '\\')
          name = p + 1;
      const uint32_t name_length = (uint32_t)strlen(name);
      uint32_t shown_length = Min(name_length, 72u);
      while (shown_length < name_length &&
             ((uint8_t)name[shown_length] & 0xc0u) == 0x80u)
        --shown_length;
      char progress[48] = "";
      if (job->actions_total) {
        snprintf(progress, sizeof(progress), "  \xc2\xb7  %u/%u%s",
                 job->actions_finished, job->actions_total,
                 job->actions_failed ? " (failures)" : "");
      }
      const String8 text = string8_create_formatted(
          ui->frame_allocator, "%s  \xc2\xb7  %s%s  \xc2\xb7  %.*s%s",
          editor_bakery_status(job->status),
          editor_bakery_kind_names[job->kind], progress, (int)shown_length,
          name, shown_length < name_length ? "\xe2\x80\xa6" : "");
      VkrUiWidgetConfig config = editor_bakery_widget(i, 0u);
      editor_bakery_tab_style(&config, heading, i == bakery->selected);
      config.placement.justify = VKR_UI_ALIGN_STRETCH;
      config.fill = true_v;
      config.tooltip = editor_bakery_string(job->input);
      const VkrUiTheme *theme = vkr_ui_theme();
      config.icon_size_pt = 14.0f;
      switch (job->status) {
      case EDITOR_BAKE_SUCCEEDED:
        config.icon = VKR_UI_ICON_CHECK_CIRCLE;
        config.icon_color = theme->success;
        break;
      case EDITOR_BAKE_FAILED:
        config.icon = VKR_UI_ICON_LOG_ERROR;
        config.icon_color = theme->error;
        config.style.text_color = theme->error;
        break;
      case EDITOR_BAKE_RUNNING:
        config.icon = VKR_UI_ICON_SPINNER;
        config.icon_color = theme->accent_hover;
        break;
      case EDITOR_BAKE_QUEUED:
        config.icon = VKR_UI_ICON_CLOCK;
        config.icon_color = theme->text_secondary;
        break;
      default:
        config.icon = VKR_UI_ICON_CLOSE;
        config.icon_color = theme->text_disabled;
        break;
      }
      if (vkr_ui_push_id_u64(ui, i)) {
        if (vkr_ui_button(ui, string8_lit("job"), text, &config)) {
          bakery->selected = i;
          editor_bakery_read_log(bakery);
        }
        (void)vkr_ui_pop_id(ui);
      }
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  const VkrUiTrack equal[] = {
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
  };
  const VkrUiTrack action_rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
  };
  VkrUiPanelConfig actions = vkr_ui_panel_config_default();
  actions.placement.row = 1u;
  actions.columns = equal;
  actions.column_count = action_columns;
  actions.rows = action_rows;
  actions.row_count = 4u / action_columns;
  actions.style.gap_pt = 4.0f;
  if (vkr_ui_panel_begin(ui, string8_lit("actions"), &actions)) {
    EditorBakeJob *selected = bakery->selected < bakery->job_count
                                  ? &bakery->jobs[bakery->selected]
                                  : NULL;
    VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
    vkr_editor_action_style(&config, heading);
    config.disabled = !selected;
    config.tooltip = string8_lit("View the selected job's status and output");
    if (vkr_ui_button(ui, string8_lit("output"),
                      width < 124.0f ? string8_lit("Output")
                                     : string8_lit("View output"),
                      &config))
      bakery->view = EDITOR_BAKERY_OUTPUT;
    config.placement.row = 1u / action_columns;
    config.placement.column = 1u % action_columns;
    config.disabled = !selected || (selected->status != EDITOR_BAKE_RUNNING &&
                                    selected->status != EDITOR_BAKE_QUEUED);
    config.tooltip = string8_lit("Cancel the selected running or queued job");
    if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel job"),
                      &config) &&
        selected) {
      if (selected->status == EDITOR_BAKE_RUNNING) {
        vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                              VKR_MEMORY_ORDER_RELEASE);
      } else {
        selected->status = EDITOR_BAKE_CANCELLED;
      }
      bakery->next_log_read = 0.0;
    }
    config.placement.row = 2u / action_columns;
    config.placement.column = 2u % action_columns;
    config.disabled = !selected || selected->status < EDITOR_BAKE_SUCCEEDED;
    config.tooltip =
        string8_lit("Requeue the selected job with its original settings");
    if (vkr_ui_button(ui, string8_lit("retry"), string8_lit("Retry job"),
                      &config) &&
        selected) {
      selected->status = EDITOR_BAKE_QUEUED;
      selected->exit_code = -1;
      selected->timed_out = false_v;
      selected->process_ok = false_v;
      selected->console_offset = 0u;
      selected->actions_total = 0u;
      selected->actions_finished = 0u;
      selected->actions_failed = 0u;
      bakery->message[0] = 0;
      editor_bakery_remove_log(selected->stdout_path);
      editor_bakery_remove_log(selected->stderr_path);
      bakery->next_log_read = 0.0;
    }
    config.placement.row = 3u / action_columns;
    config.placement.column = 3u % action_columns;
    config.disabled = !bakery->job_count || bakery->worker != NULL;
    for (uint32_t i = 0u; i < bakery->job_count; ++i)
      config.disabled |= bakery->jobs[i].status == EDITOR_BAKE_QUEUED;
    config.tooltip = string8_lit("Clear finished jobs once the queue is idle");
    if (vkr_ui_button(ui, string8_lit("clear"),
                      width < 124.0f ? string8_lit("Clear")
                                     : string8_lit("Clear history"),
                      &config)) {
      bakery->job_count = 0u;
      bakery->selected = EDITOR_BAKERY_NONE;
      bakery->log_length = 0u;
      bakery->next_log_read = 0.0;
    }
    (void)vkr_ui_panel_end(ui);
  }
  if (scroll_pane)
    (void)vkr_ui_scroll_area_end(ui);
  else
    (void)vkr_ui_panel_end(ui);
}

static void editor_bakery_output(VkrEditorBakery *bakery, VkrUiSystem *ui,
                                 VkrFontHandle heading, float32_t width) {
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      {.unit = VKR_UI_TRACK_FR, .value = 1.0f},
  };
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement.row = 1u;
  pane.rows = rows;
  pane.row_count = ArrayCount(rows);
  pane.style.gap_pt = 4.0f;
  if (!vkr_ui_panel_begin(ui, string8_lit("output"), &pane))
    return;
  VkrUiWidgetConfig config = editor_bakery_widget(0u, 0u);
  vkr_editor_action_style(&config, heading);
  config.disabled = bakery->selected == EDITOR_BAKERY_NONE;
  config.tooltip =
      string8_lit("Copy status, source and the captured output/error tails");
  if (vkr_ui_button(ui, string8_lit("copy"),
                    width < 124.0f ? string8_lit("Copy output")
                                   : string8_lit("Copy job output"),
                    &config))
    (void)vkr_platform_clipboard_write_text(bakery->log, bakery->log_length);
  VkrUiPanelConfig logs = vkr_ui_panel_config_default();
  logs.placement.row = 1u;
  const VkrUiTrack log_row = {.unit = VKR_UI_TRACK_AUTO};
  logs.rows = &log_row;
  logs.row_count = 1u;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("logs"), &logs)) {
    config = editor_bakery_widget(0u, 0u);
    config.text.layout.word_wrap = true_v;
    config.text.layout.max_width = Max(1.0f, width - 12.0f);
    /* Offscreen glyphs still consume the whole UI's command budget. Copy keeps
       the full captured tail while the visible text has a bounded budget. */
    uint32_t offset =
        bakery->log_length > KB(4) ? bakery->log_length - KB(4) : 0u;
    while (offset < bakery->log_length &&
           (bakery->log[offset] & 0xc0u) == 0x80u)
      ++offset;
    const String8 output =
        offset ? string8_create_formatted(ui->frame_allocator,
                                          "Showing last 4 KiB. Copy includes "
                                          "the full captured tail.\n%.*s",
                                          (int)(bakery->log_length - offset),
                                          bakery->log + offset)
               : (String8){.str = bakery->log, .length = bakery->log_length};
    vkr_ui_label(
        ui, string8_lit("text"),
        bakery->log_length
            ? output
            : string8_lit("Select a job in Jobs to inspect its output."),
        &config);
    (void)vkr_ui_scroll_area_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static void editor_bakery_managed_setup(VkrEditorBakery *bakery,
                                        VkrUiSystem *ui,
                                        VkrFontHandle heading) {
  const VkrUiTrack column = {.unit = VKR_UI_TRACK_FR, .value = 1};
  VkrUiTrack rows[9];
  for (uint32_t i = 0; i < ArrayCount(rows); ++i) {
    rows[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_PX, .value = 30};
  }
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 1;
  panel.columns = &column;
  panel.column_count = 1;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style.gap_pt = 4;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("managed.bakes"), &panel)) {
    return;
  }
  VkrUiWidgetConfig widget = editor_bakery_widget(0, 0);
  vkr_ui_label(ui, string8_lit("editor.scope"),
               string8_lit("Editor bundle / reused after validation"), &widget);
  widget.placement.row = 1;
  vkr_ui_label(ui, string8_lit("project.scope"),
               string8_lit("Project font / prepared once per source"), &widget);
  widget.placement.row = 2;
  (void)vkr_ui_checkbox(ui, string8_lit("reflection"),
                        string8_lit("Scene reflection probes"),
                        &bakery->reflection, &widget);
  widget.placement.row = 3;
  (void)vkr_ui_checkbox(ui, string8_lit("diffuse"),
                        string8_lit("Scene diffuse volume"), &bakery->diffuse,
                        &widget);
  widget.placement.row = 4;
  (void)vkr_ui_checkbox(ui, string8_lit("lightmap"),
                        string8_lit("Scene lightmaps (Metal ray tracing)"),
                        &bakery->lightmap, &widget);
  widget.placement.row = 5;
  (void)vkr_ui_checkbox(ui, string8_lit("lightmap_uvs"),
                        string8_lit("Lightmap UVs on model import and rebuild"),
                        &bakery->lightmap_uvs, &widget);
  widget.placement.row = 6;
  widget.disabled = !bakery->writable_scene || vkr_editor_bakery_busy(bakery);
  editor_bakery_tab_style(&widget, heading, true_v);
  if (vkr_ui_button(ui, string8_lit("prepare"),
                    string8_lit("Prepare selected scene outputs"), &widget)) {
    bakery->scene_bake_requested = true_v;
  }
  widget = editor_bakery_widget(7, 0);
  vkr_ui_label(ui, string8_lit("compiled"),
               string8_lit("Renderer source tables stay with the renderer."),
               &widget);
  widget.placement.row = 8;
  vkr_ui_label(
      ui, string8_lit("content.hint"),
      string8_lit("Use Content to import, reimport or rebuild assets."),
      &widget);
  (void)vkr_ui_scroll_area_end(ui);
}

void vkr_editor_bakery_set_managed(VkrEditorBakery *bakery, bool8_t enabled,
                                   bool8_t writable_scene,
                                   const char *workspace) {
  if (!bakery) {
    return;
  }
  bakery->managed = enabled;
  bakery->writable_scene = writable_scene;
  if (enabled && workspace && workspace[0]) {
    snprintf(bakery->log_directory, sizeof(bakery->log_directory), "%s/jobs",
             workspace);
    snprintf(bakery->lock_directory, sizeof(bakery->lock_directory), "%s",
             workspace);
  }
}

bool8_t vkr_editor_bakery_request_scene_bake(VkrEditorBakery *bakery,
                                             bool8_t reflection,
                                             bool8_t diffuse) {
  if (!vkr_editor_bakery_scene_bake_available(bakery)) {
    return false_v;
  }
  bakery->reflection = reflection;
  bakery->diffuse = diffuse;
  bakery->scene_bake_requested = true_v;
  return true_v;
}

bool8_t vkr_editor_bakery_scene_bake_available(const VkrEditorBakery *bakery) {
  return bakery && bakery->managed && bakery->writable_scene &&
         !vkr_editor_bakery_busy(bakery);
}

bool8_t vkr_editor_bakery_take_scene_bake(VkrEditorBakery *bakery,
                                          bool8_t *reflection, bool8_t *diffuse,
                                          bool8_t *lightmap) {
  if (!bakery || !bakery->scene_bake_requested) {
    return false_v;
  }
  bakery->scene_bake_requested = false_v;
  *reflection = bakery->reflection;
  *diffuse = bakery->diffuse;
  *lightmap = bakery->lightmap;
  return true_v;
}

bool8_t vkr_editor_bakery_lightmap(const VkrEditorBakery *bakery) {
  return bakery && bakery->lightmap;
}

bool8_t vkr_editor_bakery_lightmap_uvs(const VkrEditorBakery *bakery) {
  return bakery && bakery->lightmap_uvs;
}

EditorBakeryService *vkr_editor_bakery_service(VkrEditorBakery *bakery) {
  return bakery ? bakery->service : NULL;
}

bool8_t vkr_editor_bakery_busy(const VkrEditorBakery *bakery) {
  if (!bakery) {
    return false_v;
  }
  for (uint32_t i = 0; i < bakery->job_count; ++i) {
    if (bakery->jobs[i].status == EDITOR_BAKE_RUNNING ||
        bakery->jobs[i].status == EDITOR_BAKE_QUEUED) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_bakery_write_settings(const VkrEditorBakery *bakery,
                                         VkrJsonWriter *writer) {
  return vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("reflection")) &&
         vkr_json_writer_bool(writer, bakery->reflection) &&
         vkr_json_writer_name(writer, string8_lit("diffuse")) &&
         vkr_json_writer_bool(writer, bakery->diffuse) &&
         vkr_json_writer_name(writer, string8_lit("lightmap")) &&
         vkr_json_writer_bool(writer, bakery->lightmap) &&
         vkr_json_writer_name(writer, string8_lit("lightmap_uvs")) &&
         vkr_json_writer_bool(writer, bakery->lightmap_uvs) &&
         vkr_json_writer_end_object(writer);
}

void vkr_editor_bakery_read_settings(VkrEditorBakery *bakery,
                                     String8 settings) {
  if (!bakery) {
    return;
  }
  bakery->reflection = false_v;
  bakery->diffuse = false_v;
  bakery->lightmap = false_v;
  bakery->lightmap_uvs = false_v;
  VkrJsonReader reader = vkr_json_reader_from_string(settings);
  (void)vkr_json_get_bool(&reader, "reflection", &bakery->reflection);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "diffuse", &bakery->diffuse);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "lightmap", &bakery->lightmap);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "lightmap_uvs", &bakery->lightmap_uvs);
}

void vkr_editor_bakery_build(VkrEditorBakery *bakery, VkrUiSystem *ui,
                             VkrUiRect rect, VkrFontHandle heading) {
  if (!bakery)
    return;
  const float32_t width = Max(1.0f, rect.width / ui->content_scale - 12.0f);
  const float32_t height = Max(1.0f, rect.height / ui->content_scale - 43.0f);
  const VkrUiTrack column = {.unit = VKR_UI_TRACK_FR, .value = 1.0f};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_PX, .value = 28.0f},
      column,
  };
  VkrUiPanelConfig root = vkr_ui_panel_config_default();
  root.columns = &column;
  root.column_count = 1u;
  root.rows = rows;
  root.row_count = ArrayCount(rows);
  root.style.padding_pt = (VkrUiEdges){8.0f, 8.0f, 6.0f, 8.0f};
  root.style.gap_pt = 8.0f;
  if (!vkr_ui_panel_begin(ui, string8_lit("bakery"), &root))
    return;
  const VkrUiTrack tabs[] = {column, column, column};
  VkrUiPanelConfig nav = vkr_ui_panel_config_default();
  nav.placement.row = 0u;
  nav.columns = tabs;
  nav.column_count = ArrayCount(tabs);
  nav.style.gap_pt = 4.0f;
  if (vkr_ui_panel_begin(ui, string8_lit("views"), &nav)) {
    const String8 names[] = {string8_lit("New bake"),
                             string8_create_formatted(ui->frame_allocator,
                                                      "Jobs (%u)",
                                                      bakery->job_count),
                             string8_lit("Output")};
    static const char *const ids[] = {"new", "jobs", "output"};
    for (uint32_t i = 0u; i < ArrayCount(names); ++i) {
      VkrUiWidgetConfig config = editor_bakery_widget(0u, i);
      config.tooltip = names[i];
      const String8 compact[] = {string8_lit("New"), string8_lit("Jobs"),
                                 string8_lit("Log")};
      if (width < 224.0f)
        config.style.padding_pt.left = config.style.padding_pt.right = 0.0f;
      editor_bakery_tab_style(&config, heading,
                              bakery->view == (EditorBakeryView)i);
      static const VkrUiIcon view_icons[] = {
          VKR_UI_ICON_PLUS_CIRCLE, VKR_UI_ICON_LIST, VKR_UI_ICON_TERMINAL};
      config.icon = view_icons[i];
      config.icon_size_pt = 14.0f;
      if (vkr_ui_button(ui, editor_bakery_string(ids[i]),
                        width < 224.0f ? compact[i] : names[i], &config))
        bakery->view = (EditorBakeryView)i;
    }
    (void)vkr_ui_panel_end(ui);
  }
  switch (bakery->view) {
  case EDITOR_BAKERY_SETUP:
    if (bakery->managed) {
      editor_bakery_managed_setup(bakery, ui, heading);
    } else {
      editor_bakery_setup(bakery, ui, heading, width);
    }
    break;
  case EDITOR_BAKERY_JOBS:
    editor_bakery_jobs(bakery, ui, heading, width, height);
    break;
  case EDITOR_BAKERY_OUTPUT:
    editor_bakery_output(bakery, ui, heading, width);
    break;
  }
  (void)vkr_ui_panel_end(ui);
}

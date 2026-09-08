#include "editor_bakery.h"
#include "editor_internal.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <stdio.h>
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
  EDITOR_BAKE_KIND_COUNT,
} EditorBakeKind;

static const char *const editor_bakery_kind_names[] = {
    "Mesh",    "Font",       "Texture", "Texture folder", "GGX DFG",
    "Charlie", "Anisotropy", "Diffuse", "Reflection"};

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
  uint32_t selected;
  uint32_t running;
  EditorBakeKind kind;
  EditorBakeryView view;
  bool8_t force;
  uint8_t source_paths[EDITOR_BAKE_KIND_COUNT][EDITOR_BAKERY_PATH_CAPACITY];
  uint32_t source_lengths[EDITOR_BAKE_KIND_COUNT];
  uint8_t output_paths[EDITOR_BAKE_KIND_COUNT][EDITOR_BAKERY_PATH_CAPACITY];
  uint32_t output_lengths[EDITOR_BAKE_KIND_COUNT];
  uint8_t log[EDITOR_BAKERY_LOG_CAPACITY];
  uint32_t log_length;
  float64_t next_log_read;
  char log_directory[EDITOR_BAKERY_PATH_CAPACITY];
  char message[192];
};

static String8 editor_bakery_string(const char *text) {
  return string8_create((uint8_t *)text, strlen(text));
}

static const char *editor_bakery_status(EditorBakeStatus status) {
  static const char *const names[] = {"Queued", "Running", "Done", "Failed",
                                      "Cancelled"};
  return names[status];
}

static bool8_t editor_bakery_uses_explicit_output(EditorBakeKind kind) {
  return kind == EDITOR_BAKE_DIFFUSE_VOLUME ||
         kind == EDITOR_BAKE_REFLECTION_PROBE;
}

static bool8_t editor_bakery_is_static_table(EditorBakeKind kind) {
  return kind == EDITOR_BAKE_DFG_TABLE || kind == EDITOR_BAKE_SHEEN_TABLE ||
         kind == EDITOR_BAKE_ANISOTROPY_TABLE;
}

static bool8_t editor_bakery_supports_force(EditorBakeKind kind) {
  return kind == EDITOR_BAKE_FONT || kind == EDITOR_BAKE_TEXTURE ||
         kind == EDITOR_BAKE_TEXTURES;
}

static const char *editor_bakery_table_output(EditorBakeKind kind) {
  static const char *const outputs[] = {
      "renderer/src/vkr_dfg_lut_data.inc",
      "renderer/src/vkr_sheen_lut_data.inc",
      "renderer/src/vkr_anisotropy_lut_data.inc",
  };
  return outputs[kind - EDITOR_BAKE_DFG_TABLE];
}

static bool8_t editor_bakery_directory(const char *name) {
  const FilePath path = {.path = editor_bakery_string(name),
                         .type = FILE_PATH_TYPE_ABSOLUTE};
  return file_create_directory(&path);
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
  const char *arguments[24];
  char manifest[EDITOR_BAKERY_PATH_CAPACITY + 20u];
  uint32_t count = 0u;
  const char *executable;
  if (job->kind == EDITOR_BAKE_MESH) {
    executable = VKR_EDITOR_MESH_COOKER_PATH;
    arguments[count++] = "--input";
    arguments[count++] = job->input;
    arguments[count++] = "--output";
    arguments[count++] = job->output;
  } else if (job->kind == EDITOR_BAKE_FONT) {
    executable = VKR_EDITOR_FONT_COOKER_PATH;
    arguments[count++] = "--config";
    arguments[count++] = job->input;
  } else if (job->kind == EDITOR_BAKE_TEXTURE ||
             job->kind == EDITOR_BAKE_TEXTURES) {
    executable = VKR_EDITOR_TEXTURE_COOKER_PATH;
    if (job->kind == EDITOR_BAKE_TEXTURES) {
      arguments[count++] = "--input-dir";
      arguments[count++] = job->input;
      arguments[count++] = "--strict";
    } else {
      arguments[count++] = "--output";
      arguments[count++] = job->output;
      arguments[count++] = "--type";
      arguments[count++] = "2d";
      arguments[count++] = "--layer";
      arguments[count++] = job->input;
    }
    arguments[count++] = "--basis-threads";
    arguments[count++] = "2";
  } else if (job->kind == EDITOR_BAKE_DFG_TABLE) {
    executable = VKR_EDITOR_DFG_COOKER_PATH;
    arguments[count++] = job->output;
  } else if (job->kind == EDITOR_BAKE_SHEEN_TABLE) {
    executable = VKR_EDITOR_SHEEN_COOKER_PATH;
    arguments[count++] = job->output;
  } else if (job->kind == EDITOR_BAKE_ANISOTROPY_TABLE) {
    executable = VKR_EDITOR_ANISOTROPY_COOKER_PATH;
    arguments[count++] = job->output;
  } else if (job->kind == EDITOR_BAKE_DIFFUSE_VOLUME) {
    executable = VKR_EDITOR_PYTHON_PATH;
    (void)snprintf(manifest, sizeof(manifest), "%s.manifest.json", job->output);
    arguments[count++] = "tools/bake_diffuse_volume.py";
    arguments[count++] = "--scene";
    arguments[count++] = job->input;
    arguments[count++] = "--output";
    arguments[count++] = job->output;
    arguments[count++] = "--manifest";
    arguments[count++] = manifest;
    arguments[count++] = "--baker";
    arguments[count++] = VKR_EDITOR_DIFFUSE_BAKER_PATH;
    arguments[count++] = "--grid";
    arguments[count++] = "4";
    arguments[count++] = "4";
    arguments[count++] = "4";
    arguments[count++] = "--face-size";
    arguments[count++] = "16";
    arguments[count++] = "--samples";
    arguments[count++] = "64";
    arguments[count++] = "--max-depth";
    arguments[count++] = "12";
    arguments[count++] = "--seed";
    arguments[count++] = "1";
    arguments[count++] = "--photons";
    arguments[count++] = "1000000";
  } else {
    executable = VKR_EDITOR_PYTHON_PATH;
    arguments[count++] = "tools/bake_reflection_probe.py";
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
    arguments[count++] = VKR_EDITOR_HARNESS_PATH;
    arguments[count++] = "--packer";
    arguments[count++] = VKR_EDITOR_HDR_CUBE_PACKER_PATH;
  }
  if (job->force && editor_bakery_supports_force(job->kind))
    arguments[count++] = "--force";
  const VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .working_directory = PROJECT_SOURCE_DIR,
      .stdout_path = job->stdout_path,
      .stderr_path = job->stderr_path,
      .timeout_ms = job->kind == EDITOR_BAKE_REFLECTION_PROBE
                        ? 35u * 60u * 1000u
                        : 30u * 60u * 1000u,
      .termination_grace_ms = 250u,
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
  const int directory_length =
      snprintf(bakery->log_directory, sizeof(bakery->log_directory),
               PROJECT_SOURCE_DIR "build/_artifacts/bakery/%u",
               vkr_platform_get_process_id());
  /* Keep room for each job's /<slot>.stdout.log or .stderr.log suffix. */
  if (directory_length < 0 ||
      directory_length + 20 >= EDITOR_BAKERY_PATH_CAPACITY ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build") ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build/_artifacts") ||
      !editor_bakery_directory(PROJECT_SOURCE_DIR "build/_artifacts/bakery") ||
      !editor_bakery_directory(bakery->log_directory)) {
    vkr_allocator_free(allocator, bakery, sizeof(*bakery),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
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
  vkr_allocator_free(bakery->allocator, bakery, sizeof(*bakery),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

static uint32_t editor_bakery_tail(const char *path, uint8_t *bytes,
                                   uint32_t capacity) {
  FILE *file = fopen(path, "rb");
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

static void editor_bakery_read_log(VkrEditorBakery *bakery) {
  if (bakery->selected == EDITOR_BAKERY_NONE)
    return;
  const EditorBakeJob *job = &bakery->jobs[bakery->selected];
  uint32_t length = (uint32_t)snprintf(
      (char *)bakery->log, sizeof(bakery->log),
      "%s | %s | exit %d%s\nSource: %s\nOutput: %s\n\nOutput (tail):\n",
      editor_bakery_kind_names[job->kind], editor_bakery_status(job->status),
      job->exit_code, job->timed_out ? " | timed out" : "", job->input,
      job->output[0] ? job->output : "See cooker output below");
  length += editor_bakery_tail(job->stdout_path, bakery->log + length, KB(8));
  static const char errors[] = "\nErrors (tail):\n";
  MemCopy(bakery->log + length, errors, sizeof(errors) - 1u);
  length += sizeof(errors) - 1u;
  length += editor_bakery_tail(job->stderr_path, bakery->log + length,
                               (uint32_t)sizeof(bakery->log) - length - 1u);
  bakery->log[length] = 0u;
  bakery->log_length = length;
}

void vkr_editor_bakery_update(VkrEditorBakery *bakery) {
  if (!bakery)
    return;
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
      if (!vkr_platform_process_lock_acquire(
              "VkrEditorBakery", PROJECT_SOURCE_DIR "build/_artifacts/bakery",
              &bakery->process_lock) ||
          !vkr_thread_create(bakery->allocator, &bakery->worker,
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
  if (bakery->job_count == EDITOR_BAKERY_JOB_CAPACITY) {
    snprintf(bakery->message, sizeof(bakery->message),
             "Queue full; clear finished jobs first.");
    return;
  }
  const uint32_t index = bakery->job_count++;
  EditorBakeJob *job = &bakery->jobs[index];
  *job = (EditorBakeJob){
      .kind = kind,
      .status = EDITOR_BAKE_QUEUED,
      .force = editor_bakery_supports_force(kind) ? bakery->force : false_v,
      .exit_code = -1};
  snprintf(job->input, sizeof(job->input), "%s", input);
  if (editor_bakery_is_static_table(kind)) {
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
  snprintf(job->stdout_path, sizeof(job->stdout_path), "%s/%u.stdout.log",
           bakery->log_directory, index);
  snprintf(job->stderr_path, sizeof(job->stderr_path), "%s/%u.stderr.log",
           bakery->log_directory, index);
  (void)remove(job->stdout_path);
  (void)remove(job->stderr_path);
  bakery->selected = index;
  bakery->view = EDITOR_BAKERY_JOBS;
  bakery->message[0] = '\0';
  bakery->next_log_read = 0.0;
}

static VkrUiWidgetConfig editor_bakery_widget(uint32_t row, uint32_t column) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.row = row;
  config.placement.column = column;
  config.style.padding_pt = (VkrUiEdges){3.0f, 6.0f, 3.0f, 6.0f};
  config.style.font_size_pt = 13.0f;
  return config;
}

static void editor_bakery_tab_style(VkrUiWidgetConfig *config,
                                    VkrFontHandle heading, bool8_t selected) {
  vkr_editor_action_style(config, heading);
  if (selected) {
    config->style.background_color = (Vec4){0.30f, 0.23f, 0.15f, 1.0f};
    config->style.border_color = (Vec4){0.82f, 0.62f, 0.34f, 1.0f};
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
                 editor_bakery_is_static_table(bakery->kind)
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
    };
    config.tooltip = editor_bakery_string(path_hints[bakery->kind]);
    config.disabled = editor_bakery_is_static_table(bakery->kind);
    VkrUiTextEditBuffer buffer = {.data = bakery->source_paths[bakery->kind],
                                  .length =
                                      bakery->source_lengths[bakery->kind],
                                  .capacity = EDITOR_BAKERY_PATH_CAPACITY};
    if (vkr_ui_text_field(ui, string8_lit("path"), &buffer, &config))
      bakery->message[0] = '\0';
    bakery->source_lengths[bakery->kind] = buffer.length;
    config = editor_bakery_widget(wide ? 0u : 2u, wide ? 2u : 0u);
    vkr_editor_action_style(&config, heading);
    config.disabled = !buffer.length ||
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
      config.tooltip = bakery->kind == EDITOR_BAKE_DIFFUSE_VOLUME
                           ? string8_lit("Volume output: .vkdv")
                           : string8_lit("Cubemap output: .vkt");
      VkrUiTextEditBuffer output = {.data = bakery->output_paths[bakery->kind],
                                    .length =
                                        bakery->output_lengths[bakery->kind],
                                    .capacity = EDITOR_BAKERY_PATH_CAPACITY};
      if (vkr_ui_text_field(ui, string8_lit("output"), &output, &config))
        bakery->message[0] = '\0';
      bakery->output_lengths[bakery->kind] = output.length;
    } else if (bakery->kind == EDITOR_BAKE_MESH) {
      config.tooltip = string8_lit("Meshes always rebuild their output");
      vkr_ui_label(ui, string8_lit("rebuild"),
                   width < 180.0f ? string8_lit("Rebuild")
                                  : string8_lit("Meshes always rebuild"),
                   &config);
    } else if (editor_bakery_supports_force(bakery->kind)) {
      config.tooltip =
          string8_lit("Bake again even when the output is current");
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
  };
  VkrUiWidgetConfig hint = editor_bakery_widget(3u, 0u);
  hint.text.layout.word_wrap = true_v;
  hint.text.layout.max_width = Max(1.0f, width - 12.0f);
  hint.style.text_color = bakery->message[0]
                              ? (Vec4){1.0f, 0.65f, 0.48f, 1.0f}
                              : (Vec4){0.72f, 0.77f, 0.83f, 1.0f};
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
      const String8 text = string8_create_formatted(
          ui->frame_allocator, "%s | %s | %.*s%s",
          editor_bakery_status(job->status),
          editor_bakery_kind_names[job->kind], (int)shown_length, name,
          shown_length < name_length ? "..." : "");
      VkrUiWidgetConfig config = editor_bakery_widget(i, 0u);
      editor_bakery_tab_style(&config, heading, i == bakery->selected);
      config.tooltip = editor_bakery_string(job->input);
      if (job->status == EDITOR_BAKE_FAILED)
        config.style.text_color = (Vec4){1.0f, 0.57f, 0.49f, 1.0f};
      else if (job->status == EDITOR_BAKE_SUCCEEDED)
        config.style.text_color = (Vec4){0.60f, 0.91f, 0.70f, 1.0f};
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
                      &config)) {
      if (selected->status == EDITOR_BAKE_RUNNING)
        vkr_atomic_bool_store(&bakery->cancel_requested, true_v,
                              VKR_MEMORY_ORDER_RELEASE);
      else
        selected->status = EDITOR_BAKE_CANCELLED;
      bakery->next_log_read = 0.0;
    }
    config.placement.row = 2u / action_columns;
    config.placement.column = 2u % action_columns;
    config.disabled = !selected || selected->status < EDITOR_BAKE_SUCCEEDED;
    config.tooltip =
        string8_lit("Requeue the selected job with its original settings");
    if (vkr_ui_button(ui, string8_lit("retry"), string8_lit("Retry job"),
                      &config)) {
      selected->status = EDITOR_BAKE_QUEUED;
      selected->exit_code = -1;
      selected->timed_out = false_v;
      selected->process_ok = false_v;
      bakery->message[0] = 0;
      (void)remove(selected->stdout_path);
      (void)remove(selected->stderr_path);
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
  root.style.padding_pt = (VkrUiEdges){5.0f, 6.0f, 5.0f, 6.0f};
  root.style.gap_pt = 5.0f;
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
      if (vkr_ui_button(ui, editor_bakery_string(ids[i]),
                        width < 224.0f ? compact[i] : names[i], &config))
        bakery->view = (EditorBakeryView)i;
    }
    (void)vkr_ui_panel_end(ui);
  }
  switch (bakery->view) {
  case EDITOR_BAKERY_SETUP:
    editor_bakery_setup(bakery, ui, heading, width);
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

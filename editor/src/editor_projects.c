#include "editor_projects.h"
#include "editor_content.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include <math.h>

#include "core/vkr_atomic.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_file_dialog.h"
#include "platform/vkr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_CARD_COUNT 128u
#define PROJECT_MODEL_COUNT 16u
#define PROJECT_LIGHT_COUNT 32u
#define PROJECT_SETTINGS_CAPACITY KB(64)
#define PROJECT_MODAL_LAYER 1000u
/* Launcher heading row, aligned with the native window controls. */
#define PROJECT_LAUNCHER_TITLE_PT 40.0f

typedef enum ProjectView {
  PROJECT_VIEW_CHOOSER,
  PROJECT_VIEW_CREATE,
  PROJECT_VIEW_ADD_SCENE,
  PROJECT_VIEW_ADD_ENTITY,
  PROJECT_VIEW_SCENES,
  PROJECT_VIEW_PROGRESS,
  PROJECT_VIEW_CONFIRM,
  PROJECT_VIEW_RENAME,
  PROJECT_VIEW_DELETE,
  PROJECT_VIEW_DELETE_PROJECT,
  PROJECT_VIEW_EDITOR,
} ProjectView;

typedef struct ProjectCard {
  char id[37];
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  char error[128];
  uint32_t scenes;
} ProjectCard;

typedef struct ProjectLightDraft {
  char name[96];
  uint32_t kind;
  Vec3 position;
  Vec3 color;
  float32_t intensity;
  float32_t range;
  Vec3 direction;
  Vec2 size;
  float32_t inner_angle;
  float32_t outer_angle;
  bool8_t casts_shadow;
} ProjectLightDraft;

typedef struct ProjectProbeDraft {
  Vec3 center;
  Vec3 extents;
  float32_t blend;
  float32_t intensity;
  float32_t diffuse;
  float32_t specular;
} ProjectProbeDraft;

typedef struct ProjectJsonBuffer {
  uint8_t *bytes;
  uint32_t length;
  uint32_t capacity;
} ProjectJsonBuffer;

typedef struct ProjectSettingsSave {
  VkrEditorProject project;
  VkrEditorProjectError error;
  VkrThread worker;
  VkrAtomicBool complete;
  bool8_t succeeded;
  uint64_t capacity;
  uint8_t bytes[];
} ProjectSettingsSave;

struct VkrEditorProjects {
  VkrAllocator *allocator;
  Arena *project_arena;
  VkrAllocator project_allocator;
  VkrEditorProject *project;
  ProjectSettingsSave *settings_save;
  VkrEditorWorkspace workspace;
  VkrEditorWorkspaceLease lease;
  bool8_t read_only;
  VkrFontHandle fonts[48];
  uint32_t font_count;
  VkrFontSystem *font_system;
  char content_scene[37];
  ProjectView view;
  ProjectView resume_view;
  uint32_t dropdown;
  bool8_t dropdown_opened;
  ProjectCard cards[PROJECT_CARD_COUNT];
  uint32_t card_count;
  uint32_t scan_index;
  char workspace_directory[1024];
  char locator_path[1024];
  char initial_project[1024];
  char initial_scene[37];
  char search[128];
  char project_name[513];
  char scene_name[513];
  char source_scene[1024];
  char models[PROJECT_MODEL_COUNT][1024];
  uint32_t model_count;
  char font_source[1024];
  char project_font_source[1024];
  char bootstrap_directory[2048];
  float32_t environment_intensity;
  float32_t environment_diffuse;
  float32_t environment_specular;
  bool8_t environment_enabled;
  /** Physical sky, sun and global IBL from the scene atmosphere (ADR-058). */
  bool8_t sky_enabled;
  bool8_t reflection_enabled;
  bool8_t bake_reflection;
  bool8_t bake_diffuse;
  bool8_t prepare_assets;
  ProjectProbeDraft probes[VKR_SCENE_REFLECTION_PROBE_MAX];
  uint32_t probe_count;
  uint32_t probe_selected;
  bool8_t include_scene;
  bool8_t import_scene;
  /* Scene form height measured on the previous build; sizes its scroll. */
  float32_t form_height;
  /* Pixel rectangle of the navigation button that opened `dropdown`. */
  VkrUiRect dropdown_anchor_px;
  ProjectLightDraft lights[PROJECT_LIGHT_COUNT];
  uint32_t light_count;
  uint32_t light_selected;
  bool8_t adding_model;
  uint32_t added_scene_entity;
  bool8_t select_added_entity;
  uint32_t active_scene;
  uint32_t pending_scene;
  char scene_id[37];
  char request_path[1024];
  char local_jobs_directory[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char result_path[1024];
  char operation[32];
  char action_asset[37];
  char action_source[1024];
  char progress_stage[128];
  char progress_detail[512];
  float64_t next_progress_check;
  char runtime_path[1024];
  char scene_manifest_path[1024];
  uint64_t scene_manifest_fingerprint;
  char edit_path[1024];
  uint64_t job_id;
  bool8_t job_creates_scene;
  bool8_t job_creates_project;
  /** Published with defaults; its first job imports the project font and
   * first scene. Project settings stay unrestored until that job completes. */
  bool8_t creating_project;
  bool8_t waiting_activation;
  bool8_t dialog_closed;
  bool8_t settings_dirty;
  bool8_t settings_restored;
  bool8_t discard_edits;
  bool8_t closing;
  bool8_t awaiting_save;
  bool8_t rename_project;
  bool8_t delete_waiting_unload;
  /** Project chosen for deletion; its erase job runs after unpublishing. */
  char delete_project_id[37];
  char delete_project_name[513];
  bool8_t delete_project_waiting_unload;
  uint64_t delete_project_job;
  char rename_name[513];
  char action_name[513];
  char message[512];
  VkrGraphicsSettings graphics;
  VkrGraphicsSettings default_graphics;
  bool8_t defaults_captured;
  VkrSampleRuntimePreferences runtime_preferences;
  VkrSampleRuntimePreferences default_preferences;
  uint8_t *scene_states;
  uint32_t scene_states_length;
  String8 owned_assets;
  String8 owned_default_font;
  uint8_t saved_settings[PROJECT_SETTINGS_CAPACITY];
  uint32_t saved_settings_length;
  float64_t next_settings_check;
};

static String8 project_string(const char *value) {
  return string8_create_from_cstr((const uint8_t *)value, strlen(value));
}

static bool8_t project_buffer_write(void *context, const uint8_t *bytes,
                                    uint64_t length) {
  ProjectJsonBuffer *buffer = context;
  if (length > buffer->capacity - buffer->length) {
    return false_v;
  }
  MemCopy(buffer->bytes + buffer->length, bytes, length);
  buffer->length += (uint32_t)length;
  return true_v;
}

static bool8_t project_json_text(VkrJsonWriter *writer, const char *name,
                                 const char *value) {
  return vkr_json_writer_name(writer, project_string(name)) &&
         vkr_json_writer_string(writer, project_string(value));
}

static bool8_t project_json_bool(VkrJsonWriter *writer, const char *name,
                                 bool8_t value) {
  return vkr_json_writer_name(writer, project_string(name)) &&
         vkr_json_writer_bool(writer, value);
}

static bool8_t project_json_number(VkrJsonWriter *writer, const char *name,
                                   float64_t value) {
  return vkr_json_writer_name(writer, project_string(name)) &&
         vkr_json_writer_f64(writer, value);
}

static bool8_t project_json_vec3(VkrJsonWriter *writer, const char *name,
                                 Vec3 value) {
  return vkr_json_writer_name(writer, project_string(name)) &&
         vkr_json_writer_begin_array(writer) &&
         vkr_json_writer_f64(writer, value.x) &&
         vkr_json_writer_f64(writer, value.y) &&
         vkr_json_writer_f64(writer, value.z) &&
         vkr_json_writer_end_array(writer);
}

static String8 project_member(String8 object, const char *key) {
  String8 result = {0};
  (void)vkr_editor_project_json_member(object, key, &result, NULL);
  return result;
}

static bool8_t project_read_file(const char *path, VkrAllocator *allocator,
                                 String8 *bytes) {
  FilePath file_path = {.path = project_string(path),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  FileHandle file = {0};
  FileStats stats = {0};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  if (file_stats(&file_path, &stats) != FILE_ERROR_NONE ||
      stats.size > VKR_EDITOR_PROJECT_JSON_LIMIT ||
      file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  const bool8_t ok =
      file_read_string(&file, allocator, bytes) == FILE_ERROR_NONE;
  file_close(&file);
  return ok;
}

static void project_error(VkrEditorProjects *projects,
                          const VkrEditorProjectError *error) {
  snprintf(projects->message, sizeof(projects->message), "%s", error->message);
}

static bool8_t project_add_card(const char *id, void *context) {
  VkrEditorProjects *projects = context;
  if (projects->card_count == PROJECT_CARD_COUNT) {
    snprintf(
        projects->message, sizeof(projects->message),
        "Showing the first %u projects. Choose another workspace to see more.",
        PROJECT_CARD_COUNT);
    return false_v;
  }
  ProjectCard *card = &projects->cards[projects->card_count++];
  *card = (ProjectCard){0};
  snprintf(card->id, sizeof(card->id), "%s", id);
  snprintf(card->name, sizeof(card->name), "Loading project...");
  return true_v;
}

static void project_refresh(VkrEditorProjects *projects) {
  projects->card_count = 0;
  projects->scan_index = 0;
  VkrEditorProjectError error = {0};
  if (!vkr_editor_workspace_visit(&projects->workspace, project_add_card,
                                  projects, &error)) {
    project_error(projects, &error);
  }
}

static bool8_t project_collect_settings(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrUiDockTree *dock,
                                        uint8_t *bytes, uint32_t *length) {
  ProjectJsonBuffer buffer = {.bytes = bytes,
                              .capacity = PROJECT_SETTINGS_CAPACITY};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_buffer_write, &buffer);
  bool8_t ok =
      vkr_json_writer_begin_object(&writer) &&
      project_json_number(&writer, "version", 1) &&
      vkr_json_writer_name(&writer, string8_lit("graphics")) &&
      vkr_graphics_settings_write_json(&writer, &projects->graphics) &&
      vkr_json_writer_name(&writer, string8_lit("runtime")) &&
      vkr_sample_runtime_preferences_write_json(&projects->runtime_preferences,
                                                &writer) &&
      vkr_json_writer_name(&writer, string8_lit("bakery")) &&
      vkr_editor_bakery_write_settings(editor->bakery, &writer) &&
      vkr_json_writer_name(&writer, string8_lit("scene_panels")) &&
      vkr_editor_scene_panels_write_json(editor->scene_panels, &writer) &&
      vkr_json_writer_name(&writer, string8_lit("layout")) &&
      vkr_ui_dock_write_json(&writer, dock) &&
      vkr_json_writer_name(&writer, string8_lit("panels")) &&
      vkr_json_writer_begin_object(&writer) &&
      project_json_bool(&writer, "labels_enabled", editor->labels_enabled) &&
      project_json_bool(&writer, "labels_directional",
                        editor->labels_directional) &&
      project_json_bool(&writer, "labels_spot", editor->labels_spot) &&
      project_json_bool(&writer, "labels_point", editor->labels_point) &&
      project_json_bool(&writer, "console_follow",
                        editor->console.follow_tail) &&
      project_json_bool(&writer, "console_verbose",
                        log_max_level_get() >= LOG_LEVEL_DEBUG) &&
      project_json_text(&writer, "console_search",
                        (char *)editor->console.search) &&
      project_json_number(&writer, "graphics_tab", editor->graphics_tab) &&
      project_json_number(&writer, "ui_scale", editor->ui_scale) &&
      project_json_bool(&writer, "reduce_motion", editor->reduce_motion) &&
      vkr_json_writer_name(&writer, string8_lit("console_levels")) &&
      vkr_json_writer_begin_array(&writer);
  for (uint32_t i = 0; ok && i < ArrayCount(editor->console.levels); ++i) {
    ok = vkr_json_writer_bool(&writer, editor->console.levels[i]);
  }
  ok = ok && vkr_json_writer_end_array(&writer) &&
       vkr_json_writer_name(&writer, string8_lit("windows")) &&
       vkr_json_writer_begin_array(&writer);
  for (uint32_t i = 0; ok && i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    const VkrEditorWindowState *window = &editor->windows[i];
    ok = vkr_json_writer_begin_object(&writer) &&
         project_json_bool(&writer, "visible", window->visible) &&
         project_json_number(&writer, "x", window->position_pt.x) &&
         project_json_number(&writer, "y", window->position_pt.y) &&
         project_json_number(&writer, "width", window->size_pt.x) &&
         project_json_number(&writer, "height", window->size_pt.y) &&
         project_json_number(&writer, "z", window->z_order) &&
         vkr_json_writer_end_object(&writer);
  }
  ok = ok && vkr_json_writer_end_array(&writer) &&
       vkr_json_writer_end_object(&writer) &&
       vkr_json_writer_name(&writer, string8_lit("animation")) &&
       vkr_editor_animation_write_settings(&editor->animation, &writer) &&
       vkr_json_writer_name(&writer, string8_lit("content")) &&
       vkr_editor_content_write_settings(editor->content, &writer) &&
       vkr_json_writer_end_object(&writer) && vkr_json_writer_complete(&writer);
  if (!ok) {
    snprintf(projects->message, sizeof(projects->message),
             "Project settings exceed their storage limit or contain invalid "
             "values.");
    return false_v;
  }
  VkrEditorProjectError error = {0};
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&projects->project_allocator);
  String8 merged = {0};
  ok = vkr_editor_project_json_merge_objects(
      &projects->project_allocator, projects->project->editor_settings,
      string8_create(bytes, buffer.length), &merged, &error);
  if (ok && merged.length <= PROJECT_SETTINGS_CAPACITY) {
    MemCopy(bytes, merged.str, merged.length);
    buffer.length = merged.length;
  } else {
    ok = false_v;
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!ok) {
    snprintf(
        projects->message, sizeof(projects->message),
        "Project settings cannot be preserved within their storage limit.");
    return false_v;
  }
  *length = buffer.length;
  return true_v;
}

static void *project_settings_save_worker(void *context) {
  ProjectSettingsSave *save = context;
  save->succeeded = vkr_editor_project_save(&save->project, &save->error);
  vkr_atomic_bool_store(&save->complete, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

// Explicit transitions wait here; periodic updates call only after completion.
// The worker accesses only its snapshot, never UI storage or its allocator.
static bool8_t project_finish_settings_save(VkrEditorProjects *projects) {
  ProjectSettingsSave *save = projects->settings_save;
  if (!save || !save->worker) {
    return true_v;
  }
  if (!vkr_thread_join(save->worker) ||
      !vkr_thread_destroy(projects->allocator, &save->worker)) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot finish the project settings writer. Retry saving.");
    return false_v;
  }
  if (!save->succeeded) {
    projects->settings_dirty = true_v;
    project_error(projects, &save->error);
    return false_v;
  }
  projects->project->fingerprint = save->project.fingerprint;
  const String8 settings = save->project.editor_settings;
  MemCopy(projects->saved_settings, settings.str, settings.length);
  projects->saved_settings_length = (uint32_t)settings.length;
  projects->project->editor_settings =
      string8_create(projects->saved_settings, settings.length);
  // Dispatch consumed the previous dirty state. Recall may have changed while
  // the immutable snapshot was being written; completion must retain that.
  return true_v;
}

static bool8_t project_save_settings(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrUiDockTree *dock) {
  if (!project_finish_settings_save(projects)) {
    return false_v;
  }
  if (!projects->project || projects->creating_project ||
      !projects->settings_restored || projects->read_only) {
    return true_v;
  }
  uint8_t bytes[PROJECT_SETTINGS_CAPACITY];
  uint32_t length = 0;
  if (!project_collect_settings(projects, editor, dock, bytes, &length)) {
    return false_v;
  }
  if (!projects->settings_dirty && length == projects->saved_settings_length &&
      MemCompare(bytes, projects->saved_settings, length) == 0) {
    return true_v;
  }
  VkrEditorProjectError error = {0};
  const String8 previous = projects->project->editor_settings;
  projects->project->editor_settings = string8_create(bytes, length);
  if (!vkr_editor_project_save(projects->project, &error)) {
    projects->project->editor_settings = previous;
    projects->settings_dirty = true_v;
    project_error(projects, &error);
    return false_v;
  }
  MemCopy(projects->saved_settings, bytes, length);
  projects->saved_settings_length = length;
  projects->project->editor_settings =
      string8_create(projects->saved_settings, projects->saved_settings_length);
  projects->settings_dirty = false_v;
  return true_v;
}

static void project_queue_settings_save(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrUiDockTree *dock) {
  if (!projects->project || projects->creating_project ||
      !projects->settings_restored || projects->read_only ||
      (projects->settings_save && projects->settings_save->worker)) {
    return;
  }
  uint8_t bytes[PROJECT_SETTINGS_CAPACITY];
  uint32_t length = 0;
  if (!project_collect_settings(projects, editor, dock, bytes, &length)) {
    return;
  }
  if (!projects->settings_dirty && length == projects->saved_settings_length &&
      MemCompare(bytes, projects->saved_settings, length) == 0) {
    return;
  }
  const String8 views[] = {
      projects->project->default_font, string8_create(bytes, length),
      projects->project->scene_editor_state, projects->project->assets,
      projects->project->document};
  uint64_t capacity = 0;
  for (uint32_t i = 0; i < ArrayCount(views); ++i) {
    capacity += views[i].length;
  }
  // Each view is bounded by the managed document limit. Retain rounded
  // capacity so small camera-JSON length changes do not churn allocations.
  capacity = AlignPow2(capacity, KB(64));
  ProjectSettingsSave *save = projects->settings_save;
  if (!save || capacity > save->capacity) {
    ProjectSettingsSave *replacement = vkr_allocator_alloc(
        projects->allocator, sizeof(*replacement) + capacity,
        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!replacement) {
      snprintf(projects->message, sizeof(projects->message),
               "Cannot retain the project settings snapshot. Retry saving.");
      return;
    }
    MemZero(replacement, sizeof(*replacement));
    replacement->capacity = capacity;
    if (save) {
      vkr_allocator_free(projects->allocator, save,
                         sizeof(*save) + save->capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    }
    projects->settings_save = replacement;
    save = replacement;
  }
  save->project = *projects->project;
  String8 *destinations[] = {&save->project.default_font,
                             &save->project.editor_settings,
                             &save->project.scene_editor_state,
                             &save->project.assets, &save->project.document};
  uint64_t offset = 0;
  for (uint32_t i = 0; i < ArrayCount(views); ++i) {
    if (views[i].length) {
      MemCopy(save->bytes + offset, views[i].str, views[i].length);
    }
    *destinations[i] = string8_create(save->bytes + offset, views[i].length);
    offset += views[i].length;
  }
  save->error = (VkrEditorProjectError){0};
  save->succeeded = false_v;
  vkr_atomic_bool_store(&save->complete, false_v, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_thread_create(projects->allocator, &save->worker,
                         project_settings_save_worker, save)) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot start the project settings writer. Retry saving.");
    return;
  }
  projects->settings_dirty = false_v;
}

static void project_restore_settings(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  const String8 settings = projects->project->editor_settings;
  projects->scene_states_length =
      (uint32_t)projects->project->scene_editor_state.length;
  MemCopy(projects->scene_states, projects->project->scene_editor_state.str,
          projects->scene_states_length);
  projects->project->scene_editor_state =
      string8_create(projects->scene_states, projects->scene_states_length);
  projects->runtime_preferences = projects->default_preferences;
  (void)vkr_sample_runtime_preferences_read_json(
      project_member(settings, "runtime"), &projects->runtime_preferences);
  frame->editor_state_request->apply_preferences = true_v;
  frame->editor_state_request->preferences = projects->runtime_preferences;
  (void)vkr_editor_scene_panels_read_json(
      editor->scene_panels, project_member(settings, "scene_panels"));
  VkrEditorUi defaults;
  vkr_editor_ui_init(&defaults);
  MemCopy(editor->windows, defaults.windows, sizeof(editor->windows));
  editor->labels_enabled = true_v;
  editor->labels_directional = true_v;
  editor->labels_spot = true_v;
  editor->labels_point = true_v;
  editor->graphics_tab = 0;
  editor->console.follow_tail = true_v;
  editor->console.search[0] = '\0';
  for (uint32_t i = 0; i < ArrayCount(editor->console.levels); ++i) {
    editor->console.levels[i] = true_v;
  }
  projects->graphics = projects->default_graphics;
  const String8 graphics = project_member(settings, "graphics");
  if (graphics.length) {
    (void)vkr_graphics_settings_read_json(graphics, &projects->graphics);
  }
  *frame->graphics_request = (VkrGraphicsSettingsRequest){
      .apply = true_v, .settings = projects->graphics};
  const String8 layout = project_member(settings, "layout");
  if (!layout.length || !vkr_ui_dock_read_json(layout, frame->dock)) {
    vkr_ui_dock_default_editor_layout(frame->dock);
  }
  VkrJsonReader panels =
      vkr_json_reader_from_string(project_member(settings, "panels"));
  (void)vkr_json_get_bool(&panels, "labels_enabled", &editor->labels_enabled);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "labels_directional",
                          &editor->labels_directional);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "labels_spot", &editor->labels_spot);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "labels_point", &editor->labels_point);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "console_follow",
                          &editor->console.follow_tail);
  panels.pos = 0;
  int32_t tab = 0;
  panels.pos = 0;
  float32_t ui_scale = 1.0f;
  (void)vkr_json_get_float(&panels, "ui_scale", &ui_scale);
  vkr_ui_system_set_user_scale(frame->ui, ui_scale);
  panels.pos = 0;
  bool8_t reduce_motion = false_v;
  (void)vkr_json_get_bool(&panels, "reduce_motion", &reduce_motion);
  frame->ui->reduce_motion = reduce_motion;
  panels.pos = 0;
  if (vkr_json_get_int(&panels, "graphics_tab", &tab) && tab >= 0 &&
      tab < VKR_EDITOR_GRAPHICS_TAB_COUNT) {
    editor->graphics_tab = (VkrEditorGraphicsTab)tab;
  }
  const String8 panel_json = project_member(settings, "panels");
  (void)vkr_editor_project_json_string(panel_json, "console_search",
                                       (char *)editor->console.search,
                                       sizeof(editor->console.search), NULL);
  editor->console.search_length =
      (uint32_t)strlen((char *)editor->console.search);
  editor->console.filter_dirty = true_v;
  panels = vkr_json_reader_from_string(panel_json);
  bool8_t verbose = false_v;
  (void)vkr_json_get_bool(&panels, "console_verbose", &verbose);
  log_max_level_set(verbose ? LOG_LEVEL_TRACE : LOG_LEVEL_INFO);
  panels.pos = 0;
  if (vkr_json_find_array(&panels, "console_levels")) {
    for (uint32_t i = 0; i < ArrayCount(editor->console.levels) &&
                         vkr_json_next_array_element(&panels);
         ++i) {
      (void)vkr_json_parse_bool(&panels, &editor->console.levels[i]);
    }
  }
  panels = vkr_json_reader_from_string(panel_json);
  if (vkr_json_find_array(&panels, "windows")) {
    for (uint32_t i = 0;
         i < VKR_EDITOR_WINDOW_COUNT && vkr_json_next_array_element(&panels);
         ++i) {
      VkrJsonReader entry;
      if (!vkr_json_enter_object(&panels, &entry)) {
        break;
      }
      VkrEditorWindowState candidate = editor->windows[i];
      (void)vkr_json_get_bool(&entry, "visible", &candidate.visible);
      entry.pos = 0;
      (void)vkr_json_get_float(&entry, "x", &candidate.position_pt.x);
      entry.pos = 0;
      (void)vkr_json_get_float(&entry, "y", &candidate.position_pt.y);
      entry.pos = 0;
      (void)vkr_json_get_float(&entry, "width", &candidate.size_pt.x);
      entry.pos = 0;
      (void)vkr_json_get_float(&entry, "height", &candidate.size_pt.y);
      entry.pos = 0;
      int32_t value = 0;
      if (vkr_json_get_int(&entry, "z", &value) && value >= 0) {
        candidate.z_order = (uint32_t)value;
      }
      if (isfinite(candidate.position_pt.x) &&
          isfinite(candidate.position_pt.y) && isfinite(candidate.size_pt.x) &&
          isfinite(candidate.size_pt.y) && candidate.size_pt.x >= 100 &&
          candidate.size_pt.y >= 80) {
        editor->windows[i] = candidate;
      }
    }
  }
  vkr_editor_animation_read_settings(&editor->animation,
                                     project_member(settings, "animation"));
  vkr_editor_content_restore_settings(editor->content,
                                      project_member(settings, "content"));
  vkr_editor_bakery_read_settings(editor->bakery,
                                  project_member(settings, "bakery"));
  projects->settings_restored = true_v;
  projects->saved_settings_length = 0;
  projects->next_settings_check = vkr_platform_get_absolute_time() + 1;
}

static void project_remember_scene(VkrEditorProjects *projects,
                                   VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  if (!projects->project || !frame->scene || projects->read_only ||
      projects->active_scene >= projects->project->scene_count ||
      frame->editor_state_request->apply_recall ||
      !string8_equals(&frame->scene_path,
                      &(String8){.str = (uint8_t *)projects->runtime_path,
                                 .length = strlen(projects->runtime_path)})) {
    return;
  }
  uint8_t bytes[PROJECT_SETTINGS_CAPACITY];
  ProjectJsonBuffer buffer = {.bytes = bytes, .capacity = sizeof(bytes)};
  VkrJsonWriter writer;
  vkr_json_writer_init(&writer, project_buffer_write, &buffer);
  if (!vkr_json_writer_begin_object(&writer) ||
      !vkr_json_writer_name(&writer, string8_lit("viewport")) ||
      !vkr_sample_scene_recall_write_json(&frame->scene_recall, &writer) ||
      !vkr_json_writer_name(&writer, string8_lit("hierarchy")) ||
      !vkr_editor_scene_panels_write_scene_json(editor->scene_panels, frame,
                                                &writer) ||
      !vkr_json_writer_end_object(&writer) ||
      !vkr_json_writer_complete(&writer)) {
    return;
  }
  const char *id = projects->project->scenes[projects->active_scene].id;
  const String8 previous =
      project_member(projects->project->scene_editor_state, id);
  if (previous.length == buffer.length &&
      MemCompare(previous.str, bytes, buffer.length) == 0) {
    return;
  }
  String8 updated = {0};
  VkrEditorProjectError error = {0};
  if (!vkr_editor_project_json_replace_member(
          frame->ui->frame_allocator, projects->project->scene_editor_state, id,
          string8_create(bytes, buffer.length), &updated, &error) ||
      updated.length > VKR_EDITOR_PROJECT_JSON_LIMIT) {
    project_error(projects, &error);
    return;
  }
  MemCopy(projects->scene_states, updated.str, updated.length);
  projects->scene_states_length = (uint32_t)updated.length;
  projects->project->scene_editor_state =
      string8_create(projects->scene_states, updated.length);
  projects->settings_dirty = true_v;
}

static bool8_t project_load(VkrEditorProjects *projects, const char *id,
                            VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame) {
  if (!project_save_settings(projects, editor, frame->dock)) {
    return false_v;
  }
  Arena *arena = arena_create(MB(4), KB(256));
  if (!arena) {
    return false_v;
  }
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  VkrEditorProject *candidate = vkr_allocator_alloc(
      &allocator, sizeof(*candidate), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  VkrEditorProjectError error = {0};
  if (!candidate || !vkr_editor_project_load(&projects->workspace, id,
                                             &allocator, candidate, &error)) {
    project_error(projects, &error);
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
    return false_v;
  }
  if (projects->project_arena) {
    vkr_allocator_release_global_accounting(&projects->project_allocator);
    arena_destroy(projects->project_arena);
  }
  projects->project_arena = arena;
  projects->project_allocator = allocator;
  projects->project = candidate;
  projects->active_scene = UINT32_MAX;
  projects->creating_project = false_v;
  project_restore_settings(projects, editor, frame);
  *frame->scene_request = (VkrSampleSceneRequest){
      .unload = true_v, .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  projects->view =
      candidate->scene_count ? PROJECT_VIEW_SCENES : PROJECT_VIEW_EDITOR;
  projects->content_scene[0] = '\0';
  vkr_editor_content_set_project(editor->content, projects->workspace.root,
                                 candidate->id, "");
  projects->message[0] = '\0';
  return true_v;
}

static VkrUiWidgetConfig project_widget(float32_t x, float32_t y,
                                        float32_t width, float32_t height) {
  VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
  widget.placement = VKR_UI_PLACEMENT_DEFAULT;
  widget.placement.column = 0;
  widget.placement.row = 0;
  widget.placement.justify = VKR_UI_ALIGN_START;
  widget.placement.align = VKR_UI_ALIGN_START;
  widget.placement.margin_pt.left = x;
  widget.placement.margin_pt.top = y;
  widget.style.min_size_pt = (Vec2){width, height};
  widget.style.max_size_pt = widget.style.min_size_pt;
  widget.style.font_size_pt = vkr_ui_theme()->font_body;
  widget.style.padding_pt = (VkrUiEdges){5, 10, 5, 10};
  widget.style.text_color = vkr_ui_theme()->text;
  return widget;
}

static void project_label(VkrUiSystem *ui, const char *id, const char *text,
                          float32_t x, float32_t y, float32_t width) {
  VkrUiWidgetConfig widget = project_widget(x, y, width, 28);
  widget.style.text_color = vkr_ui_theme()->text_secondary;
  vkr_ui_label(ui, project_string(id), project_string(text), &widget);
}

static bool8_t project_button(VkrUiSystem *ui, const char *id, const char *text,
                              float32_t x, float32_t y, float32_t width,
                              bool8_t disabled) {
  VkrUiWidgetConfig widget = project_widget(x, y, width, 30);
  widget.disabled = disabled;
  if (strstr(id, "browse") || strstr(id, "workspace.")) {
    widget.icon = VKR_UI_ICON_FOLDER;
  } else if (strstr(id, "refresh") || strstr(id, "retry")) {
    widget.icon = VKR_UI_ICON_REFRESH;
  } else if (strstr(id, "submit")) {
    widget.icon = VKR_UI_ICON_BAKERY;
  } else if (strstr(id, "light.add")) {
    widget.icon = VKR_UI_ICON_LIGHT;
  } else if (strstr(id, "model.add")) {
    widget.icon = VKR_UI_ICON_MESH;
  } else if (strstr(id, "font")) {
    widget.icon = VKR_UI_ICON_FONT;
  } else if (strstr(id, "project.new")) {
    widget.icon = VKR_UI_ICON_ADD;
  } else if (strstr(id, "project.open")) {
    widget.icon = VKR_UI_ICON_PROJECT;
  } else if (strstr(id, "scene.open") || strstr(id, "scene.add")) {
    widget.icon = VKR_UI_ICON_SCENE;
  }
  /* Creation and submission are primary; deletion is destructive. */
  const bool8_t primary = strstr(id, "submit") || strstr(id, "project.new") ||
                          strstr(id, "scene.add") ||
                          strstr(id, "workspace.choose");
  if (primary)
    vkr_editor_primary_style(&widget, VKR_FONT_HANDLE_INVALID);
  else
    vkr_editor_action_style(&widget, VKR_FONT_HANDLE_INVALID);
  if (strstr(id, "delete")) {
    widget.style.background_color =
        vkr_ui_color_alpha(vkr_ui_theme()->error, 0.18f);
    widget.style.hover_background_color =
        vkr_ui_color_alpha(vkr_ui_theme()->error, 0.32f);
    widget.style.border_color = vkr_ui_color_alpha(vkr_ui_theme()->error, 0.5f);
    widget.icon = VKR_UI_ICON_TRASH;
  }
  widget.icon_size_pt = 14.0f;
  return vkr_ui_button(ui, project_string(id), project_string(text), &widget);
}

static void project_field(VkrUiSystem *ui, const char *id, char *text,
                          uint32_t capacity, float32_t x, float32_t y,
                          float32_t width) {
  VkrUiWidgetConfig widget = project_widget(x, y, width, 30);
  vkr_editor_field_style(&widget);
  VkrUiTextEditBuffer buffer = {.data = (uint8_t *)text,
                                .length = (uint32_t)strlen(text),
                                .capacity = capacity};
  (void)vkr_ui_text_field(ui, project_string(id), &buffer, &widget);
}

static void project_check(VkrUiSystem *ui, const char *id, const char *text,
                          bool8_t *value, float32_t x, float32_t y,
                          float32_t width) {
  VkrUiWidgetConfig widget = project_widget(x, y, width, 28);
  (void)vkr_ui_checkbox(ui, project_string(id), project_string(text), value,
                        &widget);
}

static void project_slider(VkrUiSystem *ui, const char *id, const char *name,
                           float32_t *value, float32_t min, float32_t max,
                           float32_t x, float32_t y, float32_t width) {
  /* Name, track and value share one row, so no gap opens on wide forms. */
  const float32_t label_width = Min(150.0f, width * .38f);
  const float32_t value_width = 54.0f;
  project_label(ui, id, name, x, y, label_width);
  VkrUiWidgetConfig widget =
      project_widget(x + label_width + 6.0f, y,
                     Max(40.0f, width - label_width - value_width - 12.0f), 27);
  (void)vkr_ui_push_id_label(ui, project_string(id));
  (void)vkr_ui_slider_f32(ui, string8_lit("value"), value, min, max, &widget);
  char text[32];
  snprintf(text, sizeof(text), "%.2f", (double)*value);
  VkrUiWidgetConfig readout =
      project_widget(x + width - value_width, y, value_width, 27);
  readout.style.text_color = vkr_ui_theme()->text;
  readout.center = true_v;
  vkr_ui_label(ui, string8_lit("readout"), project_string(text), &readout);
  (void)vkr_ui_pop_id(ui);
}

static void project_browse(VkrEditorProjects *projects,
                           const VkrSampleUiFrame *frame, const char *title,
                           const char *const *extensions, uint32_t count,
                           bool8_t directory, char *out, uint32_t capacity) {
  VkrFileDialogRequest request = {.kind = directory
                                              ? VKR_FILE_DIALOG_OPEN_FOLDER
                                              : VKR_FILE_DIALOG_OPEN_FILE,
                                  .title = title,
                                  .extensions = extensions,
                                  .extension_count = count};
  VkrFileDialogResult result = {0};
  vkr_file_dialog_show(frame->window, &request, projects->allocator, &result);
  projects->dialog_closed = true_v;
  if (result.status == VKR_FILE_DIALOG_SELECTED && result.path_count == 1u) {
    if (strlen(result.paths[0]) >= capacity) {
      snprintf(projects->message, sizeof(projects->message),
               "Selected path is too long.");
    } else {
      snprintf(out, capacity, "%s", result.paths[0]);
    }
  } else if (result.status == VKR_FILE_DIALOG_ERROR) {
    snprintf(projects->message, sizeof(projects->message), "%s",
             result.diagnostic);
  }
  vkr_file_dialog_result_destroy(projects->allocator, &result);
}

static void project_reset_scene_draft(VkrEditorProjects *projects) {
  snprintf(projects->scene_name, sizeof(projects->scene_name),
           "Untitled scene");
  projects->source_scene[0] = '\0';
  projects->font_source[0] = '\0';
  projects->model_count = 0;
  projects->light_count = 0;
  projects->environment_enabled = false_v;
  projects->sky_enabled = false_v;
  projects->environment_intensity = 1;
  projects->environment_diffuse = 1;
  projects->environment_specular = 1;
  projects->reflection_enabled = false_v;
  projects->probe_count = 1;
  projects->probe_selected = 0;
  projects->probes[0] = (ProjectProbeDraft){.center = {0, 2, 0},
                                            .extents = {5, 3, 5},
                                            .blend = .5f,
                                            .intensity = 1,
                                            .diffuse = 1,
                                            .specular = 1};
  projects->bake_reflection = false_v;
  projects->bake_diffuse = false_v;
  projects->prepare_assets = true_v;
  projects->include_scene = true_v;
  projects->operation[0] = '\0';
  projects->action_asset[0] = '\0';
  projects->action_source[0] = '\0';
  projects->import_scene = false_v;
  projects->message[0] = '\0';
}

static bool8_t project_write_lights(VkrEditorProjects *projects,
                                    VkrJsonWriter *writer) {
  bool8_t ok = vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < projects->light_count; ++i) {
    const ProjectLightDraft *light = &projects->lights[i];
    ok = vkr_json_writer_begin_object(writer) &&
         project_json_text(writer, "name", light->name) &&
         vkr_json_writer_name(writer, string8_lit("transform")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_vec3(writer, "pos", light->position) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_name(
             writer, project_string(light->kind == 0   ? "directional_light"
                                    : light->kind == 3 ? "rectangle_light"
                                                       : "point_light")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_bool(writer, "enabled", true_v) &&
         project_json_vec3(writer, "color", light->color);
    if (light->kind == 3) {
      ok = ok && project_json_number(writer, "radiance", light->intensity) &&
           vkr_json_writer_name(writer, string8_lit("size")) &&
           vkr_json_writer_begin_array(writer) &&
           vkr_json_writer_f64(writer, light->size.x) &&
           vkr_json_writer_f64(writer, light->size.y) &&
           vkr_json_writer_end_array(writer);
    } else {
      ok = ok && project_json_number(writer, "intensity", light->intensity);
    }
    if (light->kind == 0 || light->kind == 2) {
      ok = ok && project_json_vec3(writer, "direction_local", light->direction);
    }
    if (light->kind == 1 || light->kind == 2) {
      ok = ok && project_json_number(writer, "range", light->range) &&
           project_json_bool(writer, "casts_shadow", light->casts_shadow);
    }
    if (light->kind == 2) {
      ok =
          ok && project_json_number(writer, "kind", 2) &&
          project_json_number(writer, "inner_cone_angle", light->inner_angle) &&
          project_json_number(writer, "outer_cone_angle", light->outer_angle);
    }
    ok = ok && vkr_json_writer_end_object(writer) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer);
  return ok;
}
static bool8_t project_write_job(VkrEditorProjects *projects,
                                 const VkrSampleUiFrame *frame,
                                 bool8_t create_scene) {
  VkrEditorProjectError error = {0};
  char job_id[37];
  if (!vkr_editor_project_id_generate(job_id, &error)) {
    project_error(projects, &error);
    return false_v;
  }
  char job_directory[1024];
  if (projects->read_only && !vkr_editor_project_local_jobs_directory(
                                 projects->local_jobs_directory, &error)) {
    project_error(projects, &error);
    return false_v;
  }
  const int length =
      projects->read_only
          ? snprintf(job_directory, sizeof(job_directory), "%s/%s",
                     projects->local_jobs_directory, job_id)
          : snprintf(job_directory, sizeof(job_directory), "%s/jobs/%s",
                     projects->workspace.root, job_id);
  if (length < 0 || length + 32 >= (int)sizeof(job_directory)) {
    snprintf(projects->message, sizeof(projects->message),
             "Workspace path is too long.");
    return false_v;
  }
  String8 directory = project_string(job_directory);
  if (!file_ensure_directory(frame->ui->frame_allocator, &directory)) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot create job directory.");
    return false_v;
  }
  snprintf(projects->request_path, sizeof(projects->request_path),
           "%s/request.json", job_directory);
  snprintf(projects->result_path, sizeof(projects->result_path),
           "%s/result.json", job_directory);
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file,
                                  project_string(projects->request_path))) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot open Bakery request for writing: %s",
             projects->request_path);
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  projects->job_creates_project = create_scene && !projects->include_scene;
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      project_json_number(writer, "version", 1) &&
      project_json_bool(writer, "read_only", projects->read_only) &&
      project_json_text(writer, "runtime_directory", job_directory) &&
      project_json_text(writer, "operation",
                        projects->operation[0]          ? projects->operation
                        : projects->job_creates_project ? "create_project"
                        : create_scene                  ? "create_scene"
                                                        : "prepare_scene") &&
      project_json_text(writer, "workspace_root", projects->workspace.root) &&
      project_json_text(writer, "project_path",
                        projects->project->manifest_path) &&
      project_json_text(writer, "legacy_root", PROJECT_SOURCE_DIR) &&
      project_json_text(writer, "bootstrap_directory",
                        projects->bootstrap_directory) &&
      project_json_text(
          writer, "project_font_source",
          projects->creating_project ? projects->project_font_source : "");
  if (create_scene) {
    ok = ok && project_json_text(writer, "scene_id", projects->scene_id) &&
         project_json_text(writer, "scene_name", projects->scene_name) &&
         project_json_text(writer, "source_scene",
                           projects->import_scene ? projects->source_scene
                                                  : "") &&
         project_json_text(writer, "font_source", projects->font_source) &&
         vkr_json_writer_name(writer, string8_lit("models")) &&
         vkr_json_writer_begin_array(writer);
    for (uint32_t i = 0; ok && i < projects->model_count; ++i) {
      ok = vkr_json_writer_string(writer, project_string(projects->models[i]));
    }
    ok = ok && vkr_json_writer_end_array(writer) &&
         vkr_json_writer_name(writer, string8_lit("atmosphere")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_bool(writer, "enabled", projects->sky_enabled) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("environment")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_bool(writer, "enabled", projects->environment_enabled) &&
         project_json_number(writer, "intensity",
                             projects->environment_intensity) &&
         project_json_number(writer, "diffuse_intensity",
                             projects->environment_diffuse) &&
         project_json_number(writer, "specular_intensity",
                             projects->environment_specular) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("lights")) &&
         project_write_lights(projects, writer);
    if (projects->reflection_enabled && !projects->import_scene) {
      ok = ok &&
           vkr_json_writer_name(writer, string8_lit("reflection_probes")) &&
           vkr_json_writer_begin_array(writer);
      for (uint32_t i = 0; ok && i < projects->probe_count; ++i) {
        const ProjectProbeDraft *probe = &projects->probes[i];
        ok = vkr_json_writer_begin_object(writer) &&
             project_json_bool(writer, "enabled", projects->bake_reflection) &&
             project_json_vec3(writer, "center", probe->center) &&
             project_json_vec3(writer, "extents", probe->extents) &&
             project_json_number(writer, "blend_distance", probe->blend) &&
             project_json_number(writer, "intensity", probe->intensity) &&
             project_json_number(writer, "diffuse_intensity", probe->diffuse) &&
             project_json_number(writer, "specular_intensity",
                                 probe->specular) &&
             vkr_json_writer_end_object(writer);
      }
      ok = ok && vkr_json_writer_end_array(writer);
    }
    ok =
        ok && vkr_json_writer_name(writer, string8_lit("bakes")) &&
        vkr_json_writer_begin_object(writer) &&
        project_json_bool(writer, "prepare_assets", projects->prepare_assets) &&
        project_json_bool(writer, "reflection",
                          projects->reflection_enabled &&
                              projects->bake_reflection) &&
        project_json_bool(writer, "diffuse", projects->bake_diffuse) &&
        vkr_json_writer_end_object(writer);
  } else {
    char scene_path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
    if (!vkr_editor_project_scene_path(
            projects->project, projects->pending_scene,
            strcmp(projects->operation, "delete_scene") != 0, scene_path,
            &error)) {
      project_error(projects, &error);
      ok = false_v;
    } else {
      ok = ok &&
           project_json_text(
               writer, "scene_id",
               projects->project->scenes[projects->pending_scene].id) &&
           project_json_text(writer, "scene_path", scene_path);
    }
  }
  if (!strcmp(projects->operation, "add_entities")) {
    ok = ok && vkr_json_writer_name(writer, string8_lit("models")) &&
         vkr_json_writer_begin_array(writer);
    if (projects->adding_model) {
      ok = ok &&
           vkr_json_writer_string(writer, project_string(projects->models[0]));
    }
    ok = ok && vkr_json_writer_end_array(writer) &&
         vkr_json_writer_name(writer, string8_lit("lights")) &&
         project_write_lights(projects, writer);
  }
  if (strcmp(projects->operation, "bake_scene") == 0) {
    ok = ok && vkr_json_writer_name(writer, string8_lit("bakes")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_bool(writer, "prepare_assets", true_v) &&
         project_json_bool(writer, "reflection", projects->bake_reflection) &&
         project_json_bool(writer, "diffuse", projects->bake_diffuse) &&
         vkr_json_writer_end_object(writer);
  }
  if (projects->operation[0]) {
    ok = ok && project_json_text(writer, "name", projects->action_name) &&
         project_json_text(writer, "asset_id", projects->action_asset) &&
         project_json_text(writer, "source", projects->action_source) &&
         project_json_bool(writer, "add_instances", false_v) &&
         vkr_json_writer_name(writer, string8_lit("sources")) &&
         vkr_json_writer_begin_array(writer);
    if (projects->action_source[0]) {
      ok = ok && vkr_json_writer_string(
                     writer, project_string(projects->action_source));
    }
    ok = ok && vkr_json_writer_end_array(writer);
  }
  ok = ok && vkr_json_writer_name(writer, string8_lit("tools")) &&
       vkr_json_writer_begin_object(writer) &&
       project_json_text(writer, "mesh", VKR_EDITOR_MESH_COOKER_PATH) &&
       project_json_text(writer, "animation",
                         VKR_EDITOR_ANIMATION_COOKER_PATH) &&
       project_json_text(writer, "collision",
                         VKR_EDITOR_COLLISION_COOKER_PATH) &&
       project_json_text(writer, "font", VKR_EDITOR_FONT_COOKER_PATH) &&
       project_json_text(writer, "texture", VKR_EDITOR_TEXTURE_COOKER_PATH) &&
       project_json_text(writer, "harness", VKR_EDITOR_HARNESS_PATH) &&
       project_json_text(writer, "hdr_packer",
                         VKR_EDITOR_HDR_CUBE_PACKER_PATH) &&
       project_json_text(writer, "diffuse", VKR_EDITOR_DIFFUSE_BAKER_PATH) &&
       vkr_json_writer_end_object(writer) &&
       vkr_json_writer_end_object(writer) && vkr_json_file_writer_commit(&file);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    if (!projects->message[0]) {
      snprintf(projects->message, sizeof(projects->message),
               "Cannot serialize or publish Bakery request: %s",
               projects->request_path);
    }
    return false_v;
  }
  projects->job_creates_scene = create_scene && !projects->job_creates_project;
  return true_v;
}

static void project_start_job(VkrEditorProjects *projects, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, bool8_t create) {
  if (projects->read_only && (create || projects->operation[0])) {
    snprintf(projects->message, sizeof(projects->message),
             "This workspace is read-only. Existing prepared scenes can be "
             "opened; asset changes need its write lease.");
    return;
  }
  project_remember_scene(projects, editor, frame);
  if (!project_save_settings(projects, editor, frame->dock)) {
    return;
  }
  projects->message[0] = '\0';
  if (!project_write_job(projects, frame, create)) {
    return;
  }
  projects->job_id = vkr_editor_bakery_project_start(
      editor->bakery, projects->request_path, projects->result_path);
  if (!projects->job_id) {
    snprintf(projects->message, sizeof(projects->message),
             "Bakery queue is full. Finish or cancel pending jobs.");
    return;
  }
  *frame->scene_request = (VkrSampleSceneRequest){
      .unload = true_v, .discard_edits = projects->discard_edits};
  if (!projects->job_creates_project && frame->scene_backdrop_blur) {
    *frame->scene_backdrop_blur = true_v;
  }
  projects->view = PROJECT_VIEW_PROGRESS;
  projects->dropdown = 0;
  (void)vkr_ui_keyboard_layer_set(frame->ui, 0);
  projects->progress_stage[0] = '\0';
  projects->progress_detail[0] = '\0';
  projects->waiting_activation = false_v;
  projects->message[0] = '\0';
}

static void project_delete_scene(VkrEditorProjects *projects,
                                 VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  if (projects->read_only || !projects->project || projects->job_id ||
      projects->waiting_activation || frame->scene_loading ||
      vkr_editor_bakery_busy(editor->bakery) ||
      projects->pending_scene >= projects->project->scene_count) {
    snprintf(projects->message, sizeof(projects->message),
             "Scene deletion requires a writable, idle project.");
    return;
  }
  if (!vkr_editor_content_stop_previews(editor->content)) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot stop asset previews. Retry deleting the scene.");
    return;
  }
  if (!projects->delete_waiting_unload) {
    project_remember_scene(projects, editor, frame);
    snprintf(projects->operation, sizeof(projects->operation), "delete_scene");
    if (!project_save_settings(projects, editor, frame->dock) ||
        !project_write_job(projects, frame, false_v)) {
      return;
    }
    if (projects->active_scene == projects->pending_scene && frame->scene) {
      *frame->scene_request =
          (VkrSampleSceneRequest){.unload = true_v, .discard_edits = true_v};
      projects->delete_waiting_unload = true_v;
      return;
    }
  }
  if (projects->delete_waiting_unload && frame->scene) {
    return;
  }
  projects->delete_waiting_unload = false_v;
  if (!project_finish_settings_save(projects)) {
    return;
  }
  VkrEditorProjectError error = {0};
  const uint32_t index = projects->pending_scene;
  if (!vkr_editor_project_remove_scene(projects->project, index,
                                       frame->ui->frame_allocator, &error)) {
    project_error(projects, &error);
    return;
  }
  const String8 recall = projects->project->scene_editor_state;
  MemCopy(projects->scene_states, recall.str, recall.length);
  projects->scene_states_length = (uint32_t)recall.length;
  projects->project->scene_editor_state =
      string8_create(projects->scene_states, recall.length);
  if (projects->active_scene == index) {
    projects->active_scene = UINT32_MAX;
    projects->content_scene[0] = '\0';
    projects->runtime_path[0] = '\0';
    projects->scene_manifest_path[0] = '\0';
    projects->scene_manifest_fingerprint = 0;
    projects->edit_path[0] = '\0';
    vkr_editor_content_set_project(editor->content, projects->workspace.root,
                                   projects->project->id, "");
  } else if (projects->active_scene != UINT32_MAX &&
             projects->active_scene > index) {
    --projects->active_scene;
  }
  projects->pending_scene = UINT32_MAX;
  projects->discard_edits = false_v;
  projects->job_id = vkr_editor_bakery_project_start(
      editor->bakery, projects->request_path, projects->result_path);
  projects->view = PROJECT_VIEW_PROGRESS;
  projects->progress_stage[0] = '\0';
  projects->progress_detail[0] = '\0';
  snprintf(projects->message, sizeof(projects->message),
           projects->job_id
               ? "Removing scene files..."
               : "Scene removed from project. Retry to erase its files.");
  project_refresh(projects);
}

/* Writes a delete_project request for an unpublished project and queues it.
 * The erase job refuses a published manifest, links and reparse points. */
static uint64_t project_start_delete_job(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  VkrEditorProjectError error = {0};
  char job_id[37];
  char job_directory[1024];
  char request_path[1100];
  char result_path[1100];
  char manifest_path[1100];
  if (!vkr_editor_project_id_generate(job_id, &error)) {
    return 0;
  }
  snprintf(job_directory, sizeof(job_directory), "%s/jobs/%s",
           projects->workspace.root, job_id);
  snprintf(request_path, sizeof(request_path), "%s/request.json",
           job_directory);
  snprintf(result_path, sizeof(result_path), "%s/result.json", job_directory);
  snprintf(manifest_path, sizeof(manifest_path), "%s/projects/%s/project.json",
           projects->workspace.root, projects->delete_project_id);
  String8 directory = project_string(job_directory);
  VkrJsonFileWriter file = {0};
  if (!file_ensure_directory(frame->ui->frame_allocator, &directory) ||
      !vkr_json_file_writer_begin(&file, project_string(request_path))) {
    return 0;
  }
  VkrJsonWriter *writer = &file.writer;
  const bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      project_json_number(writer, "version", 1) &&
      project_json_bool(writer, "read_only", false_v) &&
      project_json_text(writer, "operation", "delete_project") &&
      project_json_text(writer, "workspace_root", projects->workspace.root) &&
      project_json_text(writer, "project_path", manifest_path) &&
      vkr_json_writer_end_object(writer) && vkr_json_file_writer_commit(&file);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    return 0;
  }
  return vkr_editor_bakery_project_start(editor->bakery, request_path,
                                         result_path);
}

/* Unloads the project's scene when it is open, unpublishes its manifest so
 * discovery drops it, then erases its files in the background. */
static void project_delete_project(VkrEditorProjects *projects,
                                   VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  const bool8_t active =
      projects->project &&
      strcmp(projects->project->id, projects->delete_project_id) == 0;
  if (!projects->delete_project_waiting_unload) {
    if (projects->read_only || projects->job_id ||
        projects->waiting_activation || frame->scene_loading ||
        projects->delete_project_job ||
        vkr_editor_bakery_busy(editor->bakery)) {
      snprintf(projects->message, sizeof(projects->message),
               "Project deletion requires a writable, idle workspace.");
      return;
    }
    if (active) {
      if (!vkr_editor_content_stop_previews(editor->content)) {
        snprintf(projects->message, sizeof(projects->message),
                 "Cannot stop asset previews. Retry deleting the project.");
        return;
      }
      if (!project_finish_settings_save(projects)) {
        return;
      }
      if (frame->scene) {
        *frame->scene_request =
            (VkrSampleSceneRequest){.unload = true_v, .discard_edits = true_v};
        projects->delete_project_waiting_unload = true_v;
        return;
      }
    }
  } else if (frame->scene) {
    return;
  }
  projects->delete_project_waiting_unload = false_v;
  VkrEditorProjectError error = {0};
  if (!vkr_editor_project_unpublish(&projects->workspace,
                                    projects->delete_project_id, &error)) {
    project_error(projects, &error);
    return;
  }
  if (active) {
    vkr_editor_content_set_project(editor->content, "", "", "");
    if (projects->project_arena) {
      vkr_allocator_release_global_accounting(&projects->project_allocator);
      arena_destroy(projects->project_arena);
      projects->project_arena = NULL;
    }
    projects->project = NULL;
    projects->active_scene = UINT32_MAX;
    projects->settings_restored = false_v;
    projects->creating_project = false_v;
    projects->runtime_path[0] = '\0';
    projects->scene_manifest_path[0] = '\0';
    projects->scene_manifest_fingerprint = 0;
    projects->edit_path[0] = '\0';
    projects->content_scene[0] = '\0';
  }
  projects->delete_project_job =
      project_start_delete_job(projects, editor, frame);
  snprintf(projects->message, sizeof(projects->message),
           projects->delete_project_job
               ? "Project deleted. Its files are being erased."
               : "Project deleted. Its files will be erased by the next "
                 "asset job.");
  projects->view = PROJECT_VIEW_CHOOSER;
  project_refresh(projects);
}

static void project_create(VkrEditorProjects *projects, VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame) {
  VkrEditorProjectError error = {0};
  if (projects->read_only) {
    snprintf(projects->message, sizeof(projects->message),
             "Workspace is read-only while another editor owns it.");
    return;
  }
  if (!projects->workspace.initialized) {
    if (!vkr_editor_workspace_open(projects->workspace_directory, true_v,
                                   &projects->workspace, &error) ||
        !vkr_editor_workspace_lease_acquire(&projects->workspace,
                                            &projects->lease, &error)) {
      project_error(projects, &error);
      return;
    }
  }
  if (!project_save_settings(projects, editor, frame->dock)) {
    return;
  }
  if (!projects->discard_edits &&
      frame->edits->revision != frame->edits->saved_revision) {
    projects->resume_view = projects->view;
    projects->view = PROJECT_VIEW_CONFIRM;
    return;
  }
  if ((projects->view == PROJECT_VIEW_CREATE &&
       !vkr_editor_project_name_valid(projects->project_name, &error)) ||
      (projects->include_scene &&
       !vkr_editor_project_name_valid(projects->scene_name, &error))) {
    project_error(projects, &error);
    return;
  }
  if (projects->include_scene && projects->import_scene &&
      !projects->source_scene[0]) {
    snprintf(projects->message, sizeof(projects->message),
             "Choose a scene JSON file first.");
    return;
  }
  if (projects->view == PROJECT_VIEW_CREATE && !projects->creating_project) {
    Arena *arena = arena_create(MB(4), KB(256));
    if (!arena) {
      return;
    }
    VkrAllocator allocator = {.ctx = arena};
    vkr_allocator_arena(&allocator);
    VkrEditorProject *project = vkr_allocator_alloc(
        &allocator, sizeof(*project), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    /* Publish the manifest before the first job: a failed scene or font job
     * must leave the project listed, with zero scenes, rather than orphan an
     * unlisted directory. */
    if (!project ||
        !vkr_editor_project_create(&projects->workspace, projects->project_name,
                                   project, &error)) {
      project_error(projects, &error);
      vkr_allocator_release_global_accounting(&allocator);
      arena_destroy(arena);
      return;
    }
    if (projects->project_arena) {
      vkr_allocator_release_global_accounting(&projects->project_allocator);
      arena_destroy(projects->project_arena);
    }
    projects->project_arena = arena;
    projects->project_allocator = allocator;
    projects->project = project;
    projects->creating_project = true_v;
    projects->active_scene = UINT32_MAX;
    projects->settings_restored = false_v;
    project_refresh(projects);
  }
  if (projects->project->scene_count == VKR_EDITOR_PROJECT_MAX_SCENES ||
      !vkr_editor_project_id_generate(projects->scene_id, &error)) {
    project_error(projects, &error);
    return;
  }
  project_start_job(projects, editor, frame, true_v);
}

static void project_job_complete(VkrEditorProjects *projects,
                                 VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  if (!project_finish_settings_save(projects)) {
    return;
  }
  String8 result = {0};
  VkrEditorProjectError error = {0};
  char status[32] = {0};
  if (!project_read_file(projects->result_path, frame->ui->frame_allocator,
                         &result) ||
      !vkr_editor_project_json_string(result, "status", status, sizeof(status),
                                      &error)) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot read completed job result: %s", error.message);
    projects->job_id = 0;
    return;
  }
  const bool8_t unbuilt = strcmp(status, "unbuilt") == 0;
  projects->select_added_entity = false_v;
  if (!strcmp(projects->operation, "add_entities")) {
    VkrJsonReader reader = vkr_json_reader_from_string(result);
    int32_t index = -1;
    if (vkr_json_get_int(&reader, "added_scene_entity", &index) && index >= 0) {
      projects->added_scene_entity = (uint32_t)index;
      projects->select_added_entity = true_v;
    }
  }
  if (!strcmp(projects->operation, "delete_scene")) {
    projects->job_id = 0;
    projects->operation[0] = '\0';
    projects->view = PROJECT_VIEW_SCENES;
    snprintf(projects->message, sizeof(projects->message),
             "Scene and its files permanently deleted.");
    return;
  }
  if (!projects->job_creates_project) {
    char fingerprint[17] = {0};
    if (!vkr_editor_project_json_string(
            result, "scene_path", projects->scene_manifest_path,
            sizeof(projects->scene_manifest_path), &error) ||
        !vkr_editor_project_json_string(result, "manifest_fingerprint",
                                        fingerprint, sizeof(fingerprint),
                                        &error) ||
        strlen(fingerprint) != 16) {
      project_error(projects, &error);
      projects->job_id = 0;
      return;
    }
    char *end = NULL;
    projects->scene_manifest_fingerprint = strtoull(fingerprint, &end, 16);
    if (!end || *end) {
      snprintf(projects->message, sizeof(projects->message),
               "Invalid scene publication fingerprint.");
      projects->job_id = 0;
      return;
    }
  }
  if (!unbuilt && !projects->job_creates_project &&
      (!vkr_editor_project_json_string(
           result, "runtime_path", projects->runtime_path,
           sizeof(projects->runtime_path), &error) ||
       !vkr_editor_project_json_string(result, "edit_path", projects->edit_path,
                                       sizeof(projects->edit_path), &error))) {
    project_error(projects, &error);
    projects->job_id = 0;
    return;
  }
  String8 assets = project_member(result, "project_assets");
  String8 default_font = project_member(result, "default_font");
  if (assets.length && default_font.length) {
    if (!string8_equals(&assets, &projects->project->assets)) {
      String8 replacement = string8_duplicate(projects->allocator, &assets);
      if (!replacement.str) {
        snprintf(projects->message, sizeof(projects->message),
                 "Cannot retain project asset inventory.");
        projects->job_id = 0;
        return;
      }
      if (projects->owned_assets.str) {
        vkr_allocator_free(projects->allocator, projects->owned_assets.str,
                           projects->owned_assets.length + 1,
                           VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      projects->owned_assets = replacement;
      projects->project->assets = replacement;
    }
    if (!string8_equals(&default_font, &projects->project->default_font)) {
      String8 replacement =
          string8_duplicate(projects->allocator, &default_font);
      if (!replacement.str) {
        snprintf(projects->message, sizeof(projects->message),
                 "Cannot retain project font setting.");
        projects->job_id = 0;
        return;
      }
      if (projects->owned_default_font.str) {
        vkr_allocator_free(projects->allocator,
                           projects->owned_default_font.str,
                           projects->owned_default_font.length + 1,
                           VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      projects->owned_default_font = replacement;
      projects->project->default_font = replacement;
    }
  }
  if (projects->job_creates_project) {
    if (!vkr_editor_project_save(projects->project, &error)) {
      project_error(projects, &error);
      projects->job_id = 0;
      return;
    }
    projects->creating_project = false_v;
    project_restore_settings(projects, editor, frame);
    projects->view = PROJECT_VIEW_EDITOR;
    projects->job_id = 0;
    project_refresh(projects);
    projects->content_scene[0] = '\0';
    vkr_editor_content_set_project(editor->content, projects->workspace.root,
                                   projects->project->id, "");
    return;
  }
  if (projects->job_creates_scene) {
    VkrEditorProjectScene *scene =
        &projects->project->scenes[projects->project->scene_count];
    *scene = (VkrEditorProjectScene){0};
    snprintf(scene->id, sizeof(scene->id), "%s", projects->scene_id);
    snprintf(scene->name, sizeof(scene->name), "%s", projects->scene_name);
    snprintf(scene->path, sizeof(scene->path), "scenes/%s/scene.json",
             projects->scene_id);
    projects->pending_scene = projects->project->scene_count++;
    if (!vkr_editor_project_save(projects->project, &error)) {
      --projects->project->scene_count;
      project_error(projects, &error);
      projects->job_id = 0;
      return;
    }
    projects->creating_project = false_v;
    if (!projects->settings_restored) {
      project_restore_settings(projects, editor, frame);
    }
    project_refresh(projects);
  }
  if (unbuilt) {
    projects->job_id = 0;
    projects->view = PROJECT_VIEW_SCENES;
    snprintf(
        projects->message, sizeof(projects->message),
        "Scene saved without prepared assets. Select it to prepare and open.");
    return;
  }
  projects->font_system = frame->ui->fonts;
  for (uint32_t i = 0; i < projects->font_count; ++i) {
    vkr_font_system_release_by_handle(projects->font_system,
                                      projects->fonts[i]);
  }
  projects->font_count = 0;
  VkrJsonReader font_reader = vkr_json_reader_from_string(result);
  if (vkr_json_find_array(&font_reader, "fonts")) {
    while (vkr_json_next_array_element(&font_reader)) {
      VkrJsonReader entry;
      if (!vkr_json_enter_object(&font_reader, &entry)) {
        snprintf(projects->message, sizeof(projects->message),
                 "Malformed scene font result.");
        projects->job_id = 0;
        return;
      }
      String8 object = string8_create((uint8_t *)entry.data, entry.length);
      char name[128];
      char config[1024];
      VkrRendererError font_error = VKR_RENDERER_ERROR_NONE;
      if (!vkr_editor_project_json_string(object, "name", name, sizeof(name),
                                          &error) ||
          !vkr_editor_project_json_string(object, "config", config,
                                          sizeof(config), &error) ||
          !vkr_font_system_load_from_file(
              frame->ui->fonts, project_string(name), project_string(config),
              &font_error)) {
        snprintf(projects->message, sizeof(projects->message),
                 "Project saved; cannot load scene font (%u).", font_error);
        projects->job_id = 0;
        return;
      }
      if (projects->font_count == ArrayCount(projects->fonts)) {
        snprintf(projects->message, sizeof(projects->message),
                 "Too many scene fonts.");
        projects->job_id = 0;
        return;
      }
      VkrFontHandle font = vkr_font_system_acquire(
          frame->ui->fonts, project_string(name), true_v, &font_error);
      if (font_error != VKR_RENDERER_ERROR_NONE) {
        projects->job_id = 0;
        return;
      }
      projects->fonts[projects->font_count++] = font;
    }
  }
  *frame->scene_request = (VkrSampleSceneRequest){
      .select = true_v,
      .path = project_string(projects->runtime_path),
      .sidecar_path = project_string(projects->edit_path),
      .asset_root = project_string(projects->workspace.root),
      .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  projects->job_id = 0;
  projects->waiting_activation = true_v;
  if (!strcmp(projects->operation, "add_entities")) {
    /* A load retry must prepare the published scene, never append twice. */
    projects->operation[0] = '\0';
  }
  projects->view = PROJECT_VIEW_EDITOR;
  (void)vkr_ui_keyboard_layer_set(frame->ui, 0);
}

static void project_choose_workspace(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  if (!projects->discard_edits &&
      frame->edits->revision != frame->edits->saved_revision) {
    projects->resume_view = PROJECT_VIEW_CHOOSER;
    projects->view = PROJECT_VIEW_CONFIRM;
    return;
  }
  if (!project_save_settings(projects, editor, frame->dock)) {
    return;
  }
  char selected[1024] = {0};
  project_browse(projects, frame, "Choose a Projects workspace", NULL, 0,
                 true_v, selected, sizeof(selected));
  if (!selected[0]) {
    projects->discard_edits = false_v;
    return;
  }
  VkrEditorProjectError error = {0};
  VkrEditorWorkspace workspace = {0};
  if (!vkr_editor_workspace_open(selected, false_v, &workspace, &error)) {
    project_error(projects, &error);
    return;
  }
  if (strcmp(workspace.root, projects->workspace.root) == 0) {
    projects->discard_edits = false_v;
    return;
  }
  if (!vkr_editor_content_stop_previews(editor->content)) {
    snprintf(
        projects->message, sizeof(projects->message),
        "Unable to stop the asset preview worker. Retry switching workspaces.");
    return;
  }
  *frame->scene_request = (VkrSampleSceneRequest){
      .unload = true_v, .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  vkr_editor_content_set_project(editor->content, "", "", "");
  if (projects->project_arena) {
    vkr_allocator_release_global_accounting(&projects->project_allocator);
    arena_destroy(projects->project_arena);
    projects->project_arena = NULL;
  }
  projects->project = NULL;
  projects->settings_restored = false_v;
  projects->creating_project = false_v;
  vkr_editor_workspace_lease_release(&projects->lease);
  projects->workspace = workspace;
  projects->read_only = false_v;
  projects->message[0] = '\0';
  if (workspace.initialized && !vkr_editor_workspace_lease_acquire(
                                   &workspace, &projects->lease, &error)) {
    projects->read_only = true_v;
    project_error(projects, &error);
  }
  snprintf(projects->workspace_directory, sizeof(projects->workspace_directory),
           "%s", selected);
  if (!vkr_editor_workspace_locator_save(NULL, selected, &error)) {
    project_error(projects, &error);
  }
  project_refresh(projects);
}

VkrEditorProjects *vkr_editor_projects_create(VkrAllocator *allocator, int argc,
                                              char **argv) {
  VkrEditorProjects *projects = vkr_allocator_alloc(
      allocator, sizeof(*projects), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!projects) {
    return NULL;
  }
  MemZero(projects, sizeof(*projects));
  projects->allocator = allocator;
  projects->scene_states =
      vkr_allocator_alloc(allocator, VKR_EDITOR_PROJECT_JSON_LIMIT,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!projects->scene_states) {
    vkr_allocator_free(allocator, projects, sizeof(*projects),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
  if (vkr_platform_executable_path(projects->bootstrap_directory,
                                   sizeof(projects->bootstrap_directory))) {
    char *separator = strrchr(projects->bootstrap_directory, '/');
    const uint64_t used =
        separator ? (uint64_t)(separator + 1 - projects->bootstrap_directory)
                  : 0u;
    if (!separator ||
        !vkr_string_copy_bounded(separator + 1,
                                 sizeof(projects->bootstrap_directory) - used,
                                 "resources/editor")) {
      projects->bootstrap_directory[0] = '\0';
    }
  }
  projects->view = PROJECT_VIEW_CHOOSER;
  projects->active_scene = UINT32_MAX;
  project_reset_scene_draft(projects);
  for (int i = 1; i < argc; ++i) {
    char *destination = NULL;
    uint32_t capacity = 0;
    if (strcmp(argv[i], "--workspace") == 0) {
      destination = projects->workspace_directory;
      capacity = sizeof(projects->workspace_directory);
    } else if (strcmp(argv[i], "--project") == 0) {
      destination = projects->initial_project;
      capacity = sizeof(projects->initial_project);
    } else if (strcmp(argv[i], "--scene-id") == 0) {
      destination = projects->initial_scene;
      capacity = sizeof(projects->initial_scene);
    }
    if (destination) {
      if (i + 1 >= argc || strlen(argv[i + 1]) >= capacity) {
        snprintf(projects->message, sizeof(projects->message),
                 "Invalid Projects launch argument.");
        break;
      }
      snprintf(destination, capacity, "%s", argv[++i]);
    }
  }
  if (!projects->workspace_directory[0]) {
    VkrEditorProjectError error = {0};
    if (!vkr_editor_workspace_locator_load(NULL, projects->workspace_directory,
                                           &error)) {
      project_error(projects, &error);
    }
  }
  if (projects->workspace_directory[0]) {
    VkrEditorProjectError error = {0};
    if (!vkr_editor_workspace_open(projects->workspace_directory, false_v,
                                   &projects->workspace, &error)) {
      project_error(projects, &error);
    } else {
      if (projects->workspace.initialized &&
          !vkr_editor_workspace_lease_acquire(&projects->workspace,
                                              &projects->lease, &error)) {
        projects->read_only = true_v;
        project_error(projects, &error);
      }
      project_refresh(projects);
    }
  }
  return projects;
}

bool8_t vkr_editor_projects_destroy(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrUiDockTree *dock) {
  if (!projects) {
    return true_v;
  }
  const bool8_t saved = project_save_settings(projects, editor, dock);
  if (projects->settings_save && projects->settings_save->worker) {
    /* A failed join leaves worker ownership and its workspace lease intact. */
    return false_v;
  }
  if (projects->job_id) {
    vkr_editor_bakery_project_cancel(editor->bakery, projects->job_id);
  }
  if (projects->project_arena) {
    vkr_allocator_release_global_accounting(&projects->project_allocator);
    arena_destroy(projects->project_arena);
  }
  vkr_editor_workspace_lease_release(&projects->lease);
  if (projects->settings_save) {
    vkr_allocator_free(projects->allocator, projects->settings_save,
                       sizeof(*projects->settings_save) +
                           projects->settings_save->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  if (projects->owned_assets.str) {
    vkr_allocator_free(projects->allocator, projects->owned_assets.str,
                       projects->owned_assets.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (projects->owned_default_font.str) {
    vkr_allocator_free(projects->allocator, projects->owned_default_font.str,
                       projects->owned_default_font.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  for (uint32_t i = 0; i < projects->font_count; ++i) {
    vkr_font_system_release_by_handle(projects->font_system,
                                      projects->fonts[i]);
  }
  vkr_allocator_free(projects->allocator, projects->scene_states,
                     VKR_EDITOR_PROJECT_JSON_LIMIT,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  vkr_allocator_free(projects->allocator, projects, sizeof(*projects),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  return saved;
}

bool8_t vkr_editor_projects_loading(const VkrEditorProjects *projects) {
  return projects && ((projects->job_id && !projects->job_creates_project) ||
                      projects->waiting_activation);
}

bool8_t vkr_editor_projects_modal(const VkrEditorProjects *projects) {
  return projects && projects->view != PROJECT_VIEW_EDITOR &&
         projects->view != PROJECT_VIEW_PROGRESS;
}

bool8_t vkr_editor_projects_launcher(const VkrEditorProjects *projects) {
  return vkr_editor_projects_modal(projects) &&
         (!projects->project || projects->creating_project);
}

bool8_t vkr_editor_projects_can_add_entity(const VkrEditorProjects *projects,
                                           VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame) {
  return projects && projects->project && !projects->read_only &&
         projects->view == PROJECT_VIEW_EDITOR && !projects->dropdown &&
         projects->active_scene < projects->project->scene_count &&
         frame->scene && !frame->scene_loading && !projects->job_id &&
         !projects->waiting_activation &&
         !vkr_editor_bakery_busy(editor->bakery);
}

void vkr_editor_projects_add_entity(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  if (!vkr_editor_projects_can_add_entity(projects, editor, frame)) {
    return;
  }
  project_reset_scene_draft(projects);
  projects->adding_model = false_v;
  projects->light_count = 1;
  projects->lights[0] = (ProjectLightDraft){.name = "Light",
                                            .kind = 1,
                                            .position = {0, 2, 0},
                                            .color = {1, 1, 1},
                                            .intensity = 5,
                                            .range = 10,
                                            .direction = {0, -1, 0},
                                            .size = {1, 1},
                                            .inner_angle = .35f,
                                            .outer_angle = .6f,
                                            .casts_shadow = true_v};
  projects->models[0][0] = '\0';
  projects->pending_scene = projects->active_scene;
  projects->view = PROJECT_VIEW_ADD_ENTITY;
}

void vkr_editor_projects_update(VkrEditorProjects *projects,
                                VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  if (!projects) {
    return;
  }
  if (projects->settings_save && projects->settings_save->worker &&
      vkr_atomic_bool_load(&projects->settings_save->complete,
                           VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)project_finish_settings_save(projects);
  }
  if (!projects->defaults_captured) {
    projects->default_graphics = frame->graphics->settings;
    projects->default_preferences = frame->runtime_preferences;
    projects->defaults_captured = true_v;
  }
  projects->dialog_closed = false_v;
  if (frame->close_requested && !projects->closing) {
    if (frame->edits->revision == frame->edits->saved_revision &&
        !projects->job_id &&
        project_save_settings(projects, editor, frame->dock)) {
      *frame->close_response = VKR_SAMPLE_CLOSE_CONFIRM;
    } else {
      projects->closing = true_v;
      projects->resume_view = projects->view;
      projects->view = PROJECT_VIEW_CONFIRM;
    }
  }
  *frame->modal = vkr_editor_projects_modal(projects) || projects->dropdown;
  vkr_editor_content_set_read_only(editor->content, projects->read_only);
  vkr_editor_bakery_set_managed(
      editor->bakery, true_v,
      !projects->read_only && projects->project && frame->scene &&
          !projects->job_id && !projects->waiting_activation &&
          strcmp(projects->operation, "delete_scene"),
      projects->read_only ? projects->local_jobs_directory
                          : projects->workspace.root);
  vkr_editor_content_suspend_previews(
      editor->content, projects->read_only || frame->scene_loading ||
                           projects->job_id || projects->waiting_activation ||
                           vkr_editor_bakery_busy(editor->bakery));
  vkr_editor_bakery_update(editor->bakery);
  if (projects->delete_waiting_unload) {
    project_delete_scene(projects, editor, frame);
  }
  if (projects->delete_project_waiting_unload) {
    project_delete_project(projects, editor, frame);
  }
  if (projects->delete_project_job) {
    const VkrEditorProjectJobStatus status = vkr_editor_bakery_project_status(
        editor->bakery, projects->delete_project_job, NULL);
    if (status == VKR_EDITOR_PROJECT_JOB_FAILED ||
        status == VKR_EDITOR_PROJECT_JOB_CANCELLED) {
      snprintf(projects->message, sizeof(projects->message),
               "Project deleted, but erasing its files stopped. The next asset "
               "job removes them; see Bakery for details.");
    }
    if (status != VKR_EDITOR_PROJECT_JOB_QUEUED &&
        status != VKR_EDITOR_PROJECT_JOB_RUNNING) {
      projects->delete_project_job = 0;
    }
  }
  if (projects->scan_index < projects->card_count) {
    ProjectCard *card = &projects->cards[projects->scan_index++];
    VkrAllocatorScope scope =
        vkr_allocator_begin_scope(frame->ui->frame_allocator);
    VkrEditorProject *project =
        vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*project),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    VkrEditorProjectError error = {0};
    if (project &&
        vkr_editor_project_load(&projects->workspace, card->id,
                                frame->ui->frame_allocator, project, &error)) {
      snprintf(card->name, sizeof(card->name), "%s", project->name);
      card->scenes = project->scene_count;
    } else {
      snprintf(card->name, sizeof(card->name), "%s", card->id);
      snprintf(card->error, sizeof(card->error), "%.127s", error.message);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (projects->initial_project[0] && projects->workspace.root[0] &&
      projects->view != PROJECT_VIEW_CONFIRM) {
    char id[1024];
    snprintf(id, sizeof(id), "%s", projects->initial_project);
    projects->initial_project[0] = '\0';
    if (strlen(id) != 36) {
      snprintf(projects->message, sizeof(projects->message),
               "--project requires a project UUID from the chosen workspace.");
    } else if (project_load(projects, id, editor, frame) &&
               projects->initial_scene[0]) {
      bool8_t found = false_v;
      for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
        if (strcmp(projects->initial_scene, projects->project->scenes[i].id) ==
            0) {
          projects->pending_scene = i;
          project_start_job(projects, editor, frame, false_v);
          found = true_v;
          break;
        }
      }
      if (!found) {
        snprintf(projects->message, sizeof(projects->message),
                 "Requested scene does not belong to this project.");
      }
      projects->initial_scene[0] = '\0';
    }
  }
  if (projects->job_id) {
    const float64_t time = vkr_platform_get_absolute_time();
    if (time >= projects->next_progress_check) {
      projects->next_progress_check = time + .1;
      char progress_path[1100];
      snprintf(progress_path, sizeof(progress_path), "%s.progress.json",
               projects->result_path);
      String8 progress_json = {0};
      if (project_read_file(progress_path, frame->ui->frame_allocator,
                            &progress_json)) {
        (void)vkr_editor_project_json_string(
            progress_json, "stage", projects->progress_stage,
            sizeof(projects->progress_stage), NULL);
        (void)vkr_editor_project_json_string(
            progress_json, "detail", projects->progress_detail,
            sizeof(projects->progress_detail), NULL);
      }
    }
    const VkrEditorProjectJobStatus status = vkr_editor_bakery_project_status(
        editor->bakery, projects->job_id, NULL);
    if (status == VKR_EDITOR_PROJECT_JOB_SUCCEEDED) {
      project_job_complete(projects, editor, frame);
      /* The runtime consumes this frame's selection after UI construction.
       * Its current scene/status snapshot still belongs to the old request. */
      return;
    } else if (status == VKR_EDITOR_PROJECT_JOB_FAILED ||
               status == VKR_EDITOR_PROJECT_JOB_CANCELLED) {
      if (!strcmp(projects->operation, "add_entities")) {
        projects->job_id = 0;
        projects->view = PROJECT_VIEW_ADD_ENTITY;
        snprintf(projects->message, sizeof(projects->message),
                 "Entity was not added. Check the model and dependencies, or "
                 "light settings. See Bakery for details; correct the form or "
                 "cancel.");
      } else if (!strcmp(projects->operation, "delete_scene")) {
        snprintf(projects->message, sizeof(projects->message),
                 "Scene removed from project; file deletion is incomplete. "
                 "Retry to finish. See Bakery for details.");
      } else if (projects->creating_project) {
        snprintf(projects->message, sizeof(projects->message),
                 "%s. The project is saved without this scene; Back opens "
                 "it. See Bakery for the job output.",
                 status == VKR_EDITOR_PROJECT_JOB_CANCELLED
                     ? "Creation cancelled"
                     : "Creation failed");
      } else {
        snprintf(
            projects->message, sizeof(projects->message),
            "%s. Your source files and existing projects are unchanged. See "
            "Bakery for the job output.",
            status == VKR_EDITOR_PROJECT_JOB_CANCELLED ? "Creation cancelled"
                                                       : "Creation failed");
      }
    }
  }
  if (projects->waiting_activation) {
    if (frame->scene &&
        string8_equals(&frame->scene_path,
                       &(String8){.str = (uint8_t *)projects->runtime_path,
                                  .length = strlen(projects->runtime_path)})) {
      projects->active_scene = projects->pending_scene;
      snprintf(projects->content_scene, sizeof(projects->content_scene), "%s",
               projects->project->scenes[projects->active_scene].id);
      vkr_editor_content_set_project(editor->content, projects->workspace.root,
                                     projects->project->id,
                                     projects->content_scene);
      const String8 recall = project_member(
          projects->project->scene_editor_state, projects->content_scene);
      VkrSampleSceneRecall restored = {0};
      if (vkr_sample_scene_recall_read_json(project_member(recall, "viewport"),
                                            &restored)) {
        frame->editor_state_request->apply_recall = true_v;
        frame->editor_state_request->recall = restored;
      }
      (void)vkr_editor_scene_panels_read_scene_json(
          editor->scene_panels, frame, project_member(recall, "hierarchy"));
      if (projects->select_added_entity) {
        for (uint32_t i = 0; i < frame->scene->world->dir.capacity; ++i) {
          if (!frame->scene->world->dir.records[i].chunk) {
            continue;
          }
          const VkrEntityId entity =
              vkr_entity_id_from_index(frame->scene->world, i);
          VkrSampleEntityIdentity identity;
          if (vkr_sample_entity_identity(frame->scene, entity, &identity) &&
              identity.scene_entity == projects->added_scene_entity &&
              identity.gltf_node == UINT32_MAX) {
            frame->editor_state_request->apply_recall = true_v;
            frame->editor_state_request->recall.selection_valid = true_v;
            frame->editor_state_request->recall.selection = identity;
            break;
          }
        }
        projects->select_added_entity = false_v;
      }
      projects->waiting_activation = false_v;
      projects->view = PROJECT_VIEW_EDITOR;
    } else if (!frame->scene_loading && frame->scene_status.length &&
               string8_equals(
                   &frame->scene_path,
                   &(String8){.str = (uint8_t *)projects->runtime_path,
                              .length = strlen(projects->runtime_path)})) {
      snprintf(projects->message, sizeof(projects->message),
               "Project saved; scene could not open: %.*s",
               (int)frame->scene_status.length, frame->scene_status.str);
      projects->waiting_activation = false_v;
      projects->view = PROJECT_VIEW_PROGRESS;
    }
  }
  if (projects->settings_restored &&
      !frame->editor_state_request->apply_preferences) {
    projects->runtime_preferences = frame->runtime_preferences;
  }
  if (projects->settings_restored && !frame->graphics_request->apply) {
    projects->graphics = frame->graphics->settings;
  }
  if (!projects->job_id && !projects->waiting_activation &&
      strcmp(projects->operation, "delete_scene") &&
      vkr_editor_bakery_take_scene_bake(editor->bakery,
                                        &projects->bake_reflection,
                                        &projects->bake_diffuse) &&
      projects->project &&
      projects->active_scene < projects->project->scene_count) {
    projects->pending_scene = projects->active_scene;
    snprintf(projects->operation, sizeof(projects->operation), "bake_scene");
    if (frame->edits->revision != frame->edits->saved_revision) {
      projects->resume_view = PROJECT_VIEW_SCENES;
      projects->view = PROJECT_VIEW_CONFIRM;
    } else {
      project_start_job(projects, editor, frame, false_v);
    }
  }
  VkrEditorContentAction content_action;
  if (projects->view == PROJECT_VIEW_EDITOR && !projects->waiting_activation &&
      !projects->job_id &&
      vkr_editor_content_take_action(editor->content, &content_action)) {
    if (projects->read_only || !projects->project ||
        projects->active_scene >= projects->project->scene_count) {
      snprintf(projects->message, sizeof(projects->message),
               "Open a writable scene before importing or rebuilding assets.");
      projects->view = PROJECT_VIEW_SCENES;
    } else {
      projects->pending_scene = projects->active_scene;
      snprintf(projects->operation, sizeof(projects->operation), "%s",
               content_action.kind == VKR_EDITOR_CONTENT_ACTION_IMPORT
                   ? "import_assets"
               : content_action.kind == VKR_EDITOR_CONTENT_ACTION_REIMPORT
                   ? "reimport_asset"
               : content_action.kind == VKR_EDITOR_CONTENT_ACTION_RENAME
                   ? "rename_asset"
                   : "rebuild_asset");
      snprintf(projects->action_asset, sizeof(projects->action_asset), "%s",
               content_action.asset_id);
      projects->action_source[0] = '\0';
      snprintf(projects->action_name, sizeof(projects->action_name), "%s",
               content_action.name);
      if (content_action.kind != VKR_EDITOR_CONTENT_ACTION_REBUILD &&
          content_action.kind != VKR_EDITOR_CONTENT_ACTION_RENAME) {
        static const char *const extensions[] = {
            "gltf", "glb", "obj", "png", "jpg", "jpeg", "ttf", "otf", "mt"};
        project_browse(projects, frame,
                       "Choose an asset to copy into the scene", extensions,
                       ArrayCount(extensions), false_v, projects->action_source,
                       sizeof(projects->action_source));
      }
      if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_REBUILD ||
          content_action.kind == VKR_EDITOR_CONTENT_ACTION_RENAME ||
          projects->action_source[0]) {
        if (frame->edits->revision != frame->edits->saved_revision) {
          projects->resume_view = PROJECT_VIEW_SCENES;
          projects->view = PROJECT_VIEW_CONFIRM;
        } else {
          project_start_job(projects, editor, frame, false_v);
        }
      }
    }
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (projects->project && now >= projects->next_settings_check) {
    projects->next_settings_check = now + .25;
    project_remember_scene(projects, editor, frame);
    if (!projects->job_id && !projects->delete_waiting_unload &&
        !projects->delete_project_waiting_unload) {
      project_queue_settings_save(projects, editor, frame->dock);
    }
  }
}

static void project_begin_create(VkrEditorProjects *projects) {
  project_reset_scene_draft(projects);
  projects->project_name[0] = '\0';
  projects->project_font_source[0] = '\0';
  projects->creating_project = false_v;
  projects->view = PROJECT_VIEW_CREATE;
}

static void project_build_chooser(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  if (!projects->workspace_directory[0]) {
    /* First run: one clear step instead of a toolbar of disabled actions. */
    VkrUiWidgetConfig mark = project_widget(width * 0.5f - 32, 40, 64, 64);
    mark.style.background_color = vkr_ui_color_alpha(theme->accent, 0.16f);
    mark.style.corner_radius_pt = (Vec4){16, 16, 16, 16};
    mark.style.padding_pt = (VkrUiEdges){14, 14, 14, 14};
    mark.icon = VKR_UI_ICON_BRAND;
    mark.icon_size_pt = 36;
    mark.icon_color = theme->accent_hover;
    vkr_ui_label(ui, project_string("welcome.mark"), (String8){0}, &mark);
    VkrUiWidgetConfig title = project_widget(0, 118, width, 32);
    title.center = true_v;
    title.style.font_size_pt = theme->font_heading;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, project_string("welcome.title"),
                 project_string("Welcome to VKR"), &title);
    VkrUiWidgetConfig subtitle = project_widget(0, 156, width, 24);
    subtitle.center = true_v;
    subtitle.style.text_color = theme->text_secondary;
    vkr_ui_label(ui, project_string("welcome.subtitle"),
                 project_string("Choose a workspace folder for your projects."),
                 &subtitle);
    if (project_button(ui, "workspace.choose", "Choose workspace",
                       width * 0.5f - 90, 200, 180, false_v))
      project_choose_workspace(projects, editor, frame);
    return;
  }
  /* Hub toolbar: search on the left, workspace actions on the right, and
   * the workspace folder as a caption beneath. */
  const float32_t toolbar_y = 4.0f;
  const float32_t new_width = 140.0f;
  const float32_t change_width = 176.0f;
  const float32_t new_x = width - 12.0f - new_width;
  const float32_t change_x = new_x - 8.0f - change_width;
  const float32_t refresh_x = change_x - 6.0f - 30.0f;
  if (project_button(ui, "project.new", "New project", new_x, toolbar_y,
                     new_width, !projects->workspace.root[0])) {
    project_begin_create(projects);
    return;
  }
  if (project_button(ui, "workspace.change", "Change workspace", change_x,
                     toolbar_y, change_width, false_v)) {
    project_choose_workspace(projects, editor, frame);
    return;
  }
  VkrUiWidgetConfig refresh = vkr_editor_icon_button_config(
      0, 0, VKR_UI_ICON_REFRESH, string8_lit("Rescan the workspace"));
  refresh.placement = project_widget(refresh_x, toolbar_y, 30, 30).placement;
  refresh.disabled = !projects->workspace.root[0];
  if (vkr_ui_button(ui, project_string("projects.refresh"), (String8){0},
                    &refresh))
    project_refresh(projects);
  VkrUiTextEditBuffer search = {.data = (uint8_t *)projects->search,
                                .length = (uint32_t)strlen(projects->search),
                                .capacity = sizeof(projects->search)};
  VkrUiPlacement search_placement =
      project_widget(12, toolbar_y + (30.0f - theme->control_height) * 0.5f, 0,
                     0)
          .placement;
  search_placement.margin_pt.right =
      Max(0.0f, width - Min(refresh_x - 12.0f, 12.0f + 320.0f));
  (void)vkr_editor_search_field(
      ui, project_string("project.search"), &search, search_placement,
      project_string("Search projects"),
      project_string("Filter projects by name"), VKR_FONT_HANDLE_INVALID);
  VkrUiWidgetConfig path = project_widget(12, 44, width - 24, 22);
  path.style.font_size_pt = theme->font_caption;
  path.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
  path.style.text_color = theme->text_secondary;
  path.icon = VKR_UI_ICON_REVEAL;
  path.icon_size_pt = 13.0f;
  path.icon_color = theme->text_disabled;
  path.tooltip = string8_lit("Workspace folder");
  vkr_ui_label(ui, project_string("workspace"),
               project_string(projects->workspace_directory), &path);
  float32_t y = 80;
  if (!projects->card_count) {
    /* An empty workspace offers the one next step, centered. */
    VkrUiWidgetConfig mark = project_widget(12, y + 48, width - 24, 40);
    mark.center = true_v;
    mark.icon = VKR_UI_ICON_PROJECT;
    mark.icon_size_pt = 32;
    mark.icon_color = theme->text_disabled;
    vkr_ui_label(ui, project_string("projects.empty.mark"), (String8){0},
                 &mark);
    VkrUiWidgetConfig title = project_widget(12, y + 96, width - 24, 28);
    title.center = true_v;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, project_string("projects.empty.title"),
                 project_string("No projects yet"), &title);
    VkrUiWidgetConfig caption = project_widget(12, y + 124, width - 24, 24);
    caption.center = true_v;
    caption.style.text_color = theme->text_secondary;
    vkr_ui_label(ui, project_string("projects.empty.caption"),
                 project_string("Create a project to start building scenes."),
                 &caption);
    if (project_button(ui, "project.new.first", "Create project",
                       width * 0.5f - 80.0f, y + 164, 160,
                       !projects->workspace.root[0])) {
      project_begin_create(projects);
      return;
    }
  }
  for (uint32_t i = 0; i < projects->card_count; ++i) {
    ProjectCard *card = &projects->cards[i];
    if (projects->search[0] && !strstr(card->name, projects->search)) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    const bool8_t unavailable = card->error[0] || i >= projects->scan_index;
    VkrUiWidgetConfig row = project_widget(12, y, width - 24, 52);
    row.style.background_color = theme->raised;
    row.style.hover_background_color = theme->raised_hover;
    row.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
    row.style.border_color = theme->border;
    row.style.corner_radius_pt = (Vec4){8, 8, 8, 8};
    row.disabled = unavailable;
    row.tooltip = project_string(card->name);
    if (vkr_ui_button(ui, project_string("project.open"), (String8){0}, &row)) {
      if (frame->edits->revision != frame->edits->saved_revision) {
        snprintf(projects->initial_project, sizeof(projects->initial_project),
                 "%s", card->id);
        projects->resume_view = PROJECT_VIEW_CHOOSER;
        projects->view = PROJECT_VIEW_CONFIRM;
      } else {
        (void)project_load(projects, card->id, editor, frame);
      }
      (void)vkr_ui_pop_id(ui);
      return;
    }
    VkrUiWidgetConfig name = project_widget(24, y + 6, width - 170, 24);
    /* Project names are user content, so they keep the wide-coverage face. */
    name.style.font_size_pt = theme->font_emphasis;
    name.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
    name.icon = VKR_UI_ICON_PROJECT;
    name.icon_size_pt = 16;
    name.icon_color = theme->accent_hover;
    name.disabled = unavailable;
    vkr_ui_label(ui, project_string("project.name"), project_string(card->name),
                 &name);
    char detail[640];
    snprintf(detail, sizeof(detail), "%u %s%s", card->scenes,
             card->scenes == 1 ? "scene" : "scenes",
             card->error[0] ? "  \xc2\xb7  Unable to open" : "");
    VkrUiWidgetConfig caption = project_widget(48, y + 28, width - 190, 18);
    caption.style.padding_pt = (VkrUiEdges){0, 2, 0, 2};
    caption.style.font_size_pt = theme->font_caption;
    caption.style.text_color =
        card->error[0] ? theme->error : theme->text_secondary;
    vkr_ui_label(ui, project_string("project.detail"), project_string(detail),
                 &caption);
    VkrUiWidgetConfig remove = vkr_editor_icon_button_config(
        0, 0, VKR_UI_ICON_TRASH, string8_lit("Delete project permanently"));
    remove.placement = project_widget(width - 52, y + 13, 26, 26).placement;
    remove.style.hover_background_color =
        vkr_ui_color_alpha(theme->error, 0.35f);
    remove.disabled = projects->read_only || projects->job_id ||
                      projects->waiting_activation || frame->scene_loading ||
                      projects->delete_project_job ||
                      vkr_editor_bakery_busy(editor->bakery);
    if (vkr_ui_button(ui, project_string("project.delete"), (String8){0},
                      &remove)) {
      snprintf(projects->delete_project_id, sizeof(projects->delete_project_id),
               "%s", card->id);
      snprintf(projects->delete_project_name,
               sizeof(projects->delete_project_name), "%s", card->name);
      projects->message[0] = '\0';
      projects->resume_view = PROJECT_VIEW_CHOOSER;
      projects->view = PROJECT_VIEW_DELETE_PROJECT;
      (void)vkr_ui_pop_id(ui);
      return;
    }
    if (card->error[0]) {
      project_label(ui, "project.error", card->error, 20, y + 54, width - 40);
      y += 26;
    }
    (void)vkr_ui_pop_id(ui);
    y += 60;
  }
}

static void project_build_light_form(VkrUiSystem *ui, ProjectLightDraft *light,
                                     float32_t x, float32_t y,
                                     float32_t width) {
  project_field(ui, "light.name", light->name, sizeof(light->name), x, y,
                width * .65f);
  static const char *const kinds[] = {"Directional", "Point", "Spot",
                                      "Rectangle"};
  if (project_button(ui, "light.kind", kinds[light->kind], x + width * .68f, y,
                     width * .32f, false_v)) {
    light->kind = (light->kind + 1) % ArrayCount(kinds);
  }
  project_slider(ui, "light.x", "Position X", &light->position.x, -100, 100, x,
                 y + 38, width);
  project_slider(ui, "light.y", "Position Y", &light->position.y, -100, 100, x,
                 y + 70, width);
  project_slider(ui, "light.z", "Position Z", &light->position.z, -100, 100, x,
                 y + 102, width);
  project_slider(ui, "light.intensity", "Intensity", &light->intensity, 0, 100,
                 x, y + 134, width);
  project_slider(ui, "light.range", "Range", &light->range, .1f, 100, x,
                 y + 166, width);
  project_slider(ui, "light.red", "Red", &light->color.x, 0, 1, x, y + 198,
                 width);
  project_slider(ui, "light.green", "Green", &light->color.y, 0, 1, x, y + 230,
                 width);
  project_slider(ui, "light.blue", "Blue", &light->color.z, 0, 1, x, y + 262,
                 width);
}

static void project_build_entity_form(VkrEditorProjects *projects,
                                      const VkrSampleUiFrame *frame,
                                      float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const float32_t x = 12;
  width -= 24;
  if (project_button(ui, "entity.light", "Light", x, 0, width * .48f,
                     false_v)) {
    projects->adding_model = false_v;
    projects->light_count = 1;
  }
  if (project_button(ui, "entity.model", "Model", x + width * .51f, 0,
                     width * .49f, false_v)) {
    projects->adding_model = true_v;
    projects->light_count = 0;
  }
  if (projects->adding_model) {
    project_label(ui, "entity.model.label",
                  "Model / GLTF, GLB, OBJ or cooked VKB", x, 44, width);
    project_field(ui, "entity.model.path", projects->models[0],
                  sizeof(projects->models[0]), x, 80, width - 90);
    if (project_button(ui, "entity.model.browse", "Browse", x + width - 82, 80,
                       82, false_v)) {
      static const char *const extensions[] = {"gltf", "glb", "obj", "vkb"};
      project_browse(projects, frame, "Add model to scene", extensions,
                     ArrayCount(extensions), false_v, projects->models[0],
                     sizeof(projects->models[0]));
      return;
    }
    project_label(ui, "entity.model.hint",
                  "The model and its dependencies are copied into this scene. "
                  "Adjust its transform in Inspector after adding.",
                  x, 124, width);
  } else {
    ProjectLightDraft *light = &projects->lights[0];
    project_build_light_form(ui, light, x, 44, width);
    if (light->kind == 0 || light->kind == 2) {
      project_slider(ui, "light.dx", "Direction X", &light->direction.x, -1, 1,
                     x, 342, width);
      project_slider(ui, "light.dy", "Direction Y", &light->direction.y, -1, 1,
                     x, 374, width);
      project_slider(ui, "light.dz", "Direction Z", &light->direction.z, -1, 1,
                     x, 406, width);
    }
    if (light->kind == 1 || light->kind == 2) {
      project_check(ui, "light.shadow", "Cast shadows", &light->casts_shadow, x,
                    438, width);
    }
    if (light->kind == 2) {
      project_slider(ui, "light.inner", "Inner angle (radians)",
                     &light->inner_angle, 0, 1.55f, x, 474, width);
      project_slider(ui, "light.outer", "Outer angle (radians)",
                     &light->outer_angle, .01f, 1.56f, x, 506, width);
    }
    if (light->kind == 3) {
      project_slider(ui, "light.width", "Width", &light->size.x, .01f, 100, x,
                     342, width);
      project_slider(ui, "light.height", "Height", &light->size.y, .01f, 100, x,
                     374, width);
    }
  }
}

static bool8_t project_entity_draft_valid(const VkrEditorProjects *projects) {
  if (projects->adding_model) {
    return projects->models[0][0] != '\0';
  }
  const ProjectLightDraft *light = &projects->lights[0];
  if (!light->name[0]) {
    return false_v;
  }
  if ((light->kind == 0 || light->kind == 2) &&
      light->direction.x * light->direction.x +
              light->direction.y * light->direction.y +
              light->direction.z * light->direction.z <
          1e-8f) {
    return false_v;
  }
  return light->kind != 2 || light->outer_angle > light->inner_angle + .001f;
}

#define PROJECT_CARD_HEADER_PT 38.0f
#define PROJECT_CARD_PAD_PT 12.0f

/* A titled card behind a group of rows. Draw it before the rows it holds;
 * rows start at y + PROJECT_CARD_HEADER_PT inside the padding. */
static void project_card(VkrUiSystem *ui, const char *id, const char *title,
                         VkrUiIcon icon, float32_t x, float32_t y,
                         float32_t width, float32_t height) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig card = project_widget(x, y, width, height);
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt = (Vec4){8, 8, 8, 8};
  (void)vkr_ui_push_id_label(ui, project_string(id));
  vkr_ui_label(ui, string8_lit("card"), (String8){0}, &card);
  VkrUiWidgetConfig heading =
      project_widget(x + PROJECT_CARD_PAD_PT, y + 6, width - 24, 28);
  heading.style.padding_pt = (VkrUiEdges){0};
  heading.style.text_color = theme->text;
  heading.style.font_size_pt = theme->font_emphasis;
  heading.icon = icon;
  heading.icon_size_pt = 15.0f;
  heading.icon_color = theme->accent_hover;
  vkr_ui_label(ui, string8_lit("title"), project_string(title), &heading);
  (void)vkr_ui_pop_id(ui);
}

/* Heights of the variable cards, so each card can draw before its rows. */
static float32_t project_light_rows_height(const ProjectLightDraft *light) {
  float32_t height = 294.0f + 30.0f;
  if (light->kind == 0 || light->kind == 2)
    height += 96.0f;
  if (light->kind == 2 || light->kind == 3)
    height += 64.0f;
  return height;
}

static float32_t project_card_bottom(float32_t y, float32_t rows) {
  return y + PROJECT_CARD_HEADER_PT + rows + 10.0f;
}

static float32_t project_build_environment_card(VkrEditorProjects *projects,
                                                VkrUiSystem *ui, float32_t x,
                                                float32_t y, float32_t width) {
  const float32_t bottom = project_card_bottom(y, 2 * 30 + 3 * 32 + 30);
  project_card(ui, "card.environment", "Environment", VKR_UI_ICON_SKY, x, y,
               width, bottom - y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  float32_t row = y + PROJECT_CARD_HEADER_PT;
  const bool8_t had_sky = projects->sky_enabled;
  project_check(ui, "sky.atmosphere", "Physical sky and sun",
                &projects->sky_enabled, cx, row, cw);
  if (projects->sky_enabled && !had_sky) {
    projects->environment_enabled = true_v;
  }
  project_check(ui, "sky.enabled", "Sky lighting",
                &projects->environment_enabled, cx, row + 30, cw);
  project_slider(ui, "sky.intensity", "Intensity",
                 &projects->environment_intensity, 0, 10, cx, row + 62, cw);
  project_slider(ui, "sky.diffuse", "Diffuse", &projects->environment_diffuse,
                 0, 4, cx, row + 94, cw);
  project_slider(ui, "sky.specular", "Specular",
                 &projects->environment_specular, 0, 4, cx, row + 126, cw);
  project_check(ui, "probe.enabled", "Local reflection probes",
                &projects->reflection_enabled, cx, row + 158, cw);
  return bottom;
}

static float32_t project_build_probe_card(VkrEditorProjects *projects,
                                          VkrUiSystem *ui, float32_t x,
                                          float32_t y, float32_t width) {
  const float32_t bottom = project_card_bottom(y, 10 * 32 + 2 * 38);
  project_card(ui, "card.probe", "Reflection probe", VKR_UI_ICON_SPARKLE, x, y,
               width, bottom - y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  const float32_t row = y + PROJECT_CARD_HEADER_PT;
  ProjectProbeDraft *probe = &projects->probes[projects->probe_selected];
  static const char *const names[] = {
      "Center X", "Center Y", "Center Z",  "Extent X", "Extent Y",
      "Extent Z", "Blend",    "Intensity", "Diffuse",  "Specular"};
  static const char *const ids[] = {
      "probe.x",       "probe.y",       "probe.z",     "probe.ex",
      "probe.ey",      "probe.ez",      "probe.blend", "probe.intensity",
      "probe.diffuse", "probe.specular"};
  float32_t *values[] = {
      &probe->center.x,  &probe->center.y,  &probe->center.z, &probe->extents.x,
      &probe->extents.y, &probe->extents.z, &probe->blend,    &probe->intensity,
      &probe->diffuse,   &probe->specular};
  static const float32_t ranges[][2] = {
      {-100, 100}, {-100, 100}, {-100, 100}, {.1f, 100}, {.1f, 100},
      {.1f, 100},  {0, 10},     {0, 10},     {0, 4},     {0, 4}};
  for (uint32_t i = 0; i < ArrayCount(names); ++i)
    project_slider(ui, ids[i], names[i], values[i], ranges[i][0], ranges[i][1],
                   cx, row + i * 32.0f, cw);
  const float32_t buttons = row + 10 * 32.0f;
  char selection[80];
  snprintf(selection, sizeof(selection), "Probe %u of %u / next",
           projects->probe_selected + 1, projects->probe_count);
  if (project_button(ui, "probe.next", selection, cx, buttons, cw,
                     projects->probe_count < 2)) {
    projects->probe_selected =
        (projects->probe_selected + 1) % projects->probe_count;
  }
  if (project_button(ui, "probe.add", "Add probe", cx, buttons + 38, cw * .48f,
                     projects->probe_count == ArrayCount(projects->probes))) {
    projects->probes[projects->probe_count] =
        (ProjectProbeDraft){.center = {0, 2, 0},
                            .extents = {5, 3, 5},
                            .blend = .5f,
                            .intensity = 1,
                            .diffuse = 1,
                            .specular = 1};
    projects->probe_selected = projects->probe_count++;
  }
  if (project_button(ui, "probe.remove", "Remove probe", cx + cw * .51f,
                     buttons + 38, cw * .49f, projects->probe_count <= 1)) {
    for (uint32_t i = projects->probe_selected + 1; i < projects->probe_count;
         ++i) {
      projects->probes[i - 1] = projects->probes[i];
    }
    --projects->probe_count;
    projects->probe_selected =
        Min(projects->probe_selected, projects->probe_count - 1);
  }
  return bottom;
}

/* Returns false when a file dialog ran; the caller ends this build. */
static bool8_t project_build_models_card(VkrEditorProjects *projects,
                                         const VkrSampleUiFrame *frame,
                                         float32_t x, float32_t *y,
                                         float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const uint32_t lines = Max(1u, projects->model_count);
  const float32_t bottom = project_card_bottom(*y, 38 + lines * 22.0f);
  project_card(ui, "card.models", "Models", VKR_UI_ICON_MESH, x, *y, width,
               bottom - *y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  const float32_t row = *y + PROJECT_CARD_HEADER_PT;
  static const char *const model_extensions[] = {"gltf", "glb", "obj"};
  if (project_button(ui, "model.add", "Add GLTF / GLB / OBJ", cx, row,
                     cw * .64f, projects->model_count == PROJECT_MODEL_COUNT)) {
    char selected[1024] = {0};
    project_browse(projects, frame, "Import model", model_extensions, 3,
                   false_v, selected, sizeof(selected));
    if (selected[0]) {
      snprintf(projects->models[projects->model_count++], 1024, "%s", selected);
    }
    return false_v;
  }
  if (project_button(ui, "model.remove", "Remove last", cx + cw * .67f, row,
                     cw * .33f, !projects->model_count)) {
    --projects->model_count;
  }
  if (!projects->model_count) {
    project_label(ui, "models.none",
                  "No models yet; each is copied with its textures", cx,
                  row + 38, cw);
  }
  for (uint32_t i = 0; i < projects->model_count; ++i) {
    const char *path = projects->models[i];
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiWidgetConfig item = project_widget(cx, row + 38 + i * 22.0f, cw, 22);
    item.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
    item.style.text_color = vkr_ui_theme()->text;
    item.icon = VKR_UI_ICON_MESH;
    item.icon_size_pt = 12.0f;
    item.icon_color = vkr_ui_theme()->text_secondary;
    item.tooltip = project_string(path);
    vkr_ui_label(ui, string8_lit("model"),
                 file_path_get_name(project_string(path)), &item);
    (void)vkr_ui_pop_id(ui);
  }
  *y = bottom;
  return true_v;
}

static float32_t project_build_lights_card(VkrEditorProjects *projects,
                                           VkrUiSystem *ui, float32_t x,
                                           float32_t y, float32_t width) {
  ProjectLightDraft *light = projects->light_count
                                 ? &projects->lights[projects->light_selected]
                                 : NULL;
  const float32_t rows =
      38 + (light ? project_light_rows_height(light) + 38 : 22);
  const float32_t bottom = project_card_bottom(y, rows);
  project_card(ui, "card.lights", "Additional lights", VKR_UI_ICON_LIGHT, x, y,
               width, bottom - y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  float32_t row = y + PROJECT_CARD_HEADER_PT;
  if (project_button(ui, "light.add", "Add light", cx, row, cw * .48f,
                     projects->light_count == PROJECT_LIGHT_COUNT)) {
    const uint32_t i = projects->light_count++;
    projects->lights[i] = (ProjectLightDraft){.kind = 1,
                                              .color = {1, 1, 1},
                                              .position = {0, 2, 0},
                                              .intensity = 5,
                                              .range = 10,
                                              .direction = {0, -1, 0},
                                              .size = {1, 1},
                                              .inner_angle = .35f,
                                              .outer_angle = .6f,
                                              .casts_shadow = true_v};
    snprintf(projects->lights[i].name, sizeof(projects->lights[i].name),
             "Light %u", i + 1);
    projects->light_selected = i;
  }
  if (project_button(ui, "light.remove", "Remove last", cx + cw * .51f, row,
                     cw * .49f, !projects->light_count)) {
    --projects->light_count;
    projects->light_selected =
        projects->light_count ? projects->light_count - 1 : 0;
  }
  row += 38;
  /* The card height was fixed from the light shown before these buttons. */
  if (!light || !projects->light_count) {
    project_label(ui, "lights.none", "The scene keeps its default sun", cx, row,
                  cw);
    return bottom;
  }
  light = &projects->lights[projects->light_selected];
  project_build_light_form(ui, light, cx, row, cw);
  row += 294;
  project_check(ui, "light.shadows", "Cast shadows", &light->casts_shadow, cx,
                row, cw);
  row += 30;
  if (light->kind == 0 || light->kind == 2) {
    project_slider(ui, "light.dx", "Direction X", &light->direction.x, -1, 1,
                   cx, row, cw);
    project_slider(ui, "light.dy", "Direction Y", &light->direction.y, -1, 1,
                   cx, row + 32, cw);
    project_slider(ui, "light.dz", "Direction Z", &light->direction.z, -1, 1,
                   cx, row + 64, cw);
    row += 96;
  }
  if (light->kind == 2) {
    project_slider(ui, "light.inner", "Inner cone", &light->inner_angle, 0,
                   1.5f, cx, row, cw);
    project_slider(ui, "light.outer", "Outer cone", &light->outer_angle,
                   light->inner_angle, 1.55f, cx, row + 32, cw);
    row += 64;
  } else if (light->kind == 3) {
    project_slider(ui, "light.width", "Width", &light->size.x, .01f, 100, cx,
                   row, cw);
    project_slider(ui, "light.height", "Height", &light->size.y, .01f, 100, cx,
                   row + 32, cw);
    row += 64;
  }
  if (project_button(ui, "light.previous", "Previous light", cx, row, cw * .48f,
                     projects->light_count < 2)) {
    projects->light_selected =
        (projects->light_selected + projects->light_count - 1) %
        projects->light_count;
  }
  if (project_button(ui, "light.next", "Next light", cx + cw * .51f, row,
                     cw * .49f, projects->light_count < 2)) {
    projects->light_selected =
        (projects->light_selected + 1) % projects->light_count;
  }
  return bottom;
}

/* Returns false when a file dialog ran; the caller ends this build. */
static bool8_t project_build_font_card(VkrEditorProjects *projects,
                                       const VkrSampleUiFrame *frame,
                                       float32_t x, float32_t *y,
                                       float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const float32_t bottom = project_card_bottom(*y, 34 + 24);
  project_card(ui, "card.font", "Scene font", VKR_UI_ICON_FONT, x, *y, width,
               bottom - *y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  const float32_t row = *y + PROJECT_CARD_HEADER_PT;
  static const char *const font_extensions[] = {"ttf", "otf"};
  project_field(ui, "font.path", projects->font_source,
                sizeof(projects->font_source), cx, row, cw - 96);
  if (project_button(ui, "font.browse", "Browse", cx + cw - 88, row, 88,
                     false_v)) {
    project_browse(projects, frame, "Choose scene font", font_extensions, 2,
                   false_v, projects->font_source,
                   sizeof(projects->font_source));
    return false_v;
  }
  project_label(ui, "font.hint", "Leave empty to inherit the project font", cx,
                row + 32, cw);
  *y = bottom;
  return true_v;
}

static float32_t project_build_build_card(VkrEditorProjects *projects,
                                          VkrUiSystem *ui, float32_t x,
                                          float32_t y, float32_t width) {
  const float32_t bottom = project_card_bottom(y, 3 * 30 + 28);
  project_card(ui, "card.build", "Prepare and bake", VKR_UI_ICON_BAKERY, x, y,
               width, bottom - y);
  const float32_t cx = x + PROJECT_CARD_PAD_PT;
  const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
  const float32_t row = y + PROJECT_CARD_HEADER_PT;
  project_check(ui, "bake.prepare", "Prepare required assets now",
                &projects->prepare_assets, cx, row, cw);
  project_check(ui, "bake.reflection", "Bake local reflection probes",
                &projects->bake_reflection, cx, row + 30, cw);
  project_check(ui, "bake.diffuse",
                "Bake diffuse volume (needs enclosed bounds)",
                &projects->bake_diffuse, cx, row + 60, cw);
  project_label(ui, "bake.shared",
                "Unprepared scenes save now and build on first open", cx,
                row + 90, cw);
  return bottom;
}

/* Scene settings as titled cards: name and a Create/Import switch, then two
 * columns when the form is wide. Returns the height used, so the caller can
 * size its scroll area to the content instead of a fixed extent. */
static float32_t project_build_scene_form(VkrEditorProjects *projects,
                                          const VkrSampleUiFrame *frame,
                                          float32_t x, float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t switch_width = 232.0f;
  const bool8_t inline_switch = width >= 560.0f;
  const float32_t name_width =
      inline_switch ? width - switch_width - 12 : width;
  project_label(ui, "scene.name.label", "Scene name", x, 0, name_width);
  project_field(ui, "scene.name", projects->scene_name,
                sizeof(projects->scene_name), x, 28, name_width);
  /* Segmented switch between authoring a new scene and importing one. */
  const float32_t switch_x = inline_switch ? x + width - switch_width : x;
  const float32_t switch_y = inline_switch ? 28 : 68;
  static const char *const modes[] = {"Create new", "Import JSON"};
  static const VkrUiIcon mode_icons[] = {VKR_UI_ICON_ADD,
                                         VKR_UI_ICON_SCENE_LOAD};
  for (uint32_t i = 0; i < 2; ++i) {
    const bool8_t selected = projects->import_scene == (i == 1);
    VkrUiWidgetConfig segment = project_widget(
        switch_x + i * switch_width * .5f, switch_y, switch_width * .5f, 30);
    vkr_editor_toggle_style(&segment, selected);
    if (!selected) {
      segment.style.background_color = theme->field;
      segment.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      segment.style.border_color = theme->border;
    }
    segment.icon = mode_icons[i];
    segment.icon_size_pt = 13.0f;
    segment.icon_color = selected ? theme->accent_hover : theme->text_secondary;
    segment.style.corner_radius_pt =
        i == 0 ? (Vec4){6, 0, 0, 6} : (Vec4){0, 6, 6, 0};
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("scene.mode"), project_string(modes[i]),
                      &segment))
      projects->import_scene = i == 1;
    (void)vkr_ui_pop_id(ui);
  }
  const float32_t top = switch_y + 48;
  const bool8_t columns = width >= 760.0f;
  const float32_t column_width = columns ? (width - 16) * .5f : width;
  const float32_t right_x = columns ? x + column_width + 16 : x;
  float32_t left_y = top;
  float32_t right_y = top;
  if (projects->import_scene) {
    static const char *const scene_extensions[] = {"json"};
    const float32_t bottom = project_card_bottom(left_y, 26 + 36 + 26);
    project_card(ui, "card.import", "Import a scene", VKR_UI_ICON_SCENE_LOAD, x,
                 left_y, width, bottom - left_y);
    const float32_t cx = x + PROJECT_CARD_PAD_PT;
    const float32_t cw = width - PROJECT_CARD_PAD_PT * 2;
    const float32_t row = left_y + PROJECT_CARD_HEADER_PT;
    project_label(ui, "scene.source.label",
                  "Scene JSON; its referenced assets are copied", cx, row, cw);
    project_field(ui, "scene.source", projects->source_scene,
                  sizeof(projects->source_scene), cx, row + 26, cw - 96);
    if (project_button(ui, "scene.browse", "Browse", cx + cw - 88, row + 26, 88,
                       false_v)) {
      project_browse(projects, frame, "Import scene JSON", scene_extensions, 1,
                     false_v, projects->source_scene,
                     sizeof(projects->source_scene));
      return projects->form_height;
    }
    project_label(ui, "scene.source.info",
                  "Original files stay unchanged; missing dependencies are "
                  "reported.",
                  cx, row + 62, cw);
    left_y = right_y = bottom + 12;
  } else {
    left_y =
        project_build_environment_card(projects, ui, x, left_y, column_width) +
        12;
    if (projects->reflection_enabled)
      left_y =
          project_build_probe_card(projects, ui, x, left_y, column_width) + 12;
    if (!columns)
      right_y = left_y;
    if (!project_build_models_card(projects, frame, right_x, &right_y,
                                   column_width))
      return projects->form_height;
    right_y += 12;
    right_y = project_build_lights_card(projects, ui, right_x, right_y,
                                        column_width) +
              12;
    if (!columns)
      left_y = right_y;
  }
  if (!project_build_font_card(projects, frame, x, &left_y, column_width))
    return projects->form_height;
  left_y += 12;
  if (!columns)
    right_y = left_y;
  right_y =
      project_build_build_card(projects, ui, right_x, right_y, column_width) +
      12;
  return Max(left_y, right_y);
}

/* Project or scene switcher anchored beneath its navigation button. It sizes
 * to its rows, scrolls beyond eight, and offers the add action directly. */
static void project_build_dropdown(VkrEditorProjects *projects,
                                   VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const float32_t screen_width = ui->target_width / scale;
  const float32_t screen_height = ui->target_height / scale;
  const bool8_t project_list = projects->dropdown == 1;
  const uint32_t count =
      project_list ? projects->card_count : projects->project->scene_count;
  const float32_t row_height = 32.0f;
  const float32_t list_height =
      count ? Min(count, 8u) * row_height : row_height;
  const float32_t width = Min(360.0f, screen_width - 16);
  const float32_t height = Min(38.0f + list_height + 50.0f, screen_height - 16);
  const VkrUiRect anchor = projects->dropdown_anchor_px;
  const float32_t left =
      anchor.width > 0.0f
          ? vkr_clamp_f32(anchor.x / scale, 8.0f, screen_width - width - 8)
          : (screen_width - width) * .5f;
  const float32_t top = anchor.height > 0.0f
                            ? (anchor.y + anchor.height) / scale + 4.0f
                            : (screen_height - height) * .5f;
  const float32_t mx = (float32_t)ui->mouse_x / scale;
  const float32_t my = (float32_t)ui->mouse_y / scale;
  const bool8_t just_opened = projects->dropdown_opened;
  projects->dropdown_opened = false_v;
  if (input_key_just_pressed(frame->input, KEY_ESCAPE) ||
      (!just_opened && ui->mouse_pressed &&
       (mx < left || mx > left + width || my < top || my > top + height))) {
    projects->dropdown = 0;
    /* Consume dismissal so the release cannot reopen the navigation button
     * that received this press before the popup was built. */
    ui->active_id = VKR_UI_ID_NONE;
    (void)vkr_ui_keyboard_layer_set(ui, 0);
    return;
  }
  *frame->modal = true_v;
  (void)vkr_ui_input_layer_register(ui, PROJECT_MODAL_LAYER,
                                    (VkrUiRect){0, 0,
                                                (float32_t)ui->target_width,
                                                (float32_t)ui->target_height});
  (void)vkr_ui_input_layer_set(ui, PROJECT_MODAL_LAYER);
  (void)vkr_ui_keyboard_layer_set(ui, PROJECT_MODAL_LAYER);
  const VkrUiTrack one = {.unit = VKR_UI_TRACK_FR, .value = 1};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){.top = top, .left = left};
  panel.style = vkr_editor_glass_style();
  panel.style.background_color.w = 1.0f;
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.min_size_pt = (Vec2){width, height};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.columns = panel.rows = &one;
  panel.column_count = panel.row_count = 1;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("projects.dropdown"), &panel)) {
    return;
  }
  VkrUiWidgetConfig title = project_widget(12, 6, width - 24, 26);
  title.style.padding_pt = (VkrUiEdges){0};
  title.style.font_size_pt = theme->font_caption;
  title.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, string8_lit("selector.title"),
               project_list ? string8_lit("Switch project")
                            : string8_lit("Switch scene"),
               &title);
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement.column = list.placement.row = 0;
  list.placement.justify = list.placement.align = VKR_UI_ALIGN_START;
  list.placement.margin_pt = (VkrUiEdges){.top = 34, .left = 4};
  list.style.min_size_pt = (Vec2){width - 8, height - 34 - 50};
  list.style.max_size_pt = list.style.min_size_pt;
  const VkrUiTrack entries = {.unit = VKR_UI_TRACK_PX,
                              .value = Max(row_height, count * row_height)};
  list.columns = &one;
  list.column_count = 1;
  list.rows = &entries;
  list.row_count = 1;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("entries"), &list)) {
    for (uint32_t i = 0; i < count; ++i) {
      (void)vkr_ui_push_id_u64(ui, i);
      const char *name = project_list ? projects->cards[i].name
                                      : projects->project->scenes[i].name;
      const bool8_t current =
          project_list ? projects->project && !strcmp(projects->project->id,
                                                      projects->cards[i].id)
                       : projects->active_scene == i;
      VkrUiWidgetConfig row =
          project_widget(0, i * row_height, width - 16, row_height - 2);
      row.fill = true_v;
      vkr_editor_ghost_style(&row);
      row.style.padding_pt = (VkrUiEdges){4, 10, 4, 10};
      row.style.text_color = theme->text;
      row.icon = current        ? VKR_UI_ICON_CHECK
                 : project_list ? VKR_UI_ICON_PROJECT
                                : VKR_UI_ICON_SCENE;
      row.icon_size_pt = 14.0f;
      row.icon_color = current ? theme->accent_hover : theme->text_secondary;
      row.disabled = project_list &&
                     (i >= projects->scan_index || projects->cards[i].error[0]);
      /* Label text aligns left; the button only supplies the row. */
      VkrUiWidgetConfig hit = row;
      hit.icon = VKR_UI_ICON_NONE;
      const bool8_t chosen =
          vkr_ui_button(ui, string8_lit("entry"), (String8){0}, &hit);
      row.style.background_color = (Vec4){0};
      row.fill = false_v;
      vkr_ui_label(ui, string8_lit("name"), project_string(name), &row);
      if (chosen) {
        projects->dropdown = 0;
        projects->operation[0] = '\0';
        if (project_list) {
          if (frame->edits->revision != frame->edits->saved_revision) {
            snprintf(projects->initial_project,
                     sizeof(projects->initial_project), "%s",
                     projects->cards[i].id);
            projects->resume_view = PROJECT_VIEW_CHOOSER;
            projects->view = PROJECT_VIEW_CONFIRM;
          } else {
            (void)project_load(projects, projects->cards[i].id, editor, frame);
          }
        } else {
          projects->pending_scene = i;
          if (frame->edits->revision != frame->edits->saved_revision) {
            projects->resume_view = PROJECT_VIEW_SCENES;
            projects->view = PROJECT_VIEW_CONFIRM;
          } else {
            project_start_job(projects, editor, frame, false_v);
          }
        }
      }
      (void)vkr_ui_pop_id(ui);
    }
    if (!count) {
      VkrUiWidgetConfig empty = project_widget(0, 0, width - 16, row_height);
      empty.style.text_color = theme->text_secondary;
      empty.center = true_v;
      vkr_ui_label(ui, string8_lit("selector.empty"),
                   project_list ? string8_lit("No projects in this workspace")
                                : string8_lit("No scenes in this project yet"),
                   &empty);
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  const float32_t footer = height - 42;
  const float32_t half = (width - 32) * .5f;
  if (project_button(ui, "selector.manage",
                     project_list ? "Manage projects" : "Manage scenes", 12,
                     footer, half, false_v)) {
    projects->dropdown = 0;
    projects->view = project_list ? PROJECT_VIEW_CHOOSER : PROJECT_VIEW_SCENES;
  }
  if (project_button(ui, project_list ? "project.new" : "scene.add",
                     project_list ? "New project" : "Add scene", 20 + half,
                     footer, half, projects->read_only)) {
    projects->dropdown = 0;
    if (project_list) {
      project_begin_create(projects);
    } else {
      project_reset_scene_draft(projects);
      projects->view = PROJECT_VIEW_ADD_SCENE;
    }
  }
  (void)vkr_ui_panel_end(ui);
}

static void project_build_create_form(VkrEditorProjects *projects,
                                      const VkrSampleUiFrame *frame,
                                      float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTrack one = {.unit = VKR_UI_TRACK_FR, .value = 1};
  const bool8_t creating = projects->view == PROJECT_VIEW_CREATE;
  const float32_t left = creating && body_width >= 650 ? body_width * .35f : 0;
  if (creating) {
    project_label(ui, "project.name.label", "Project name", 12, 6,
                  left ? left - 24 : body_width - 24);
    project_field(ui, "project.name", projects->project_name,
                  sizeof(projects->project_name), 12, 36,
                  left ? left - 24 : body_width - 24);
    project_check(ui, "project.scene", "Add an initial scene",
                  &projects->include_scene, 12, 80,
                  left ? left - 24 : body_width - 24);
    const float32_t form_width = left ? left - 24 : body_width - 24;
    project_label(ui, "project.font.label",
                  "Default font / empty uses editor default", 12, 124,
                  form_width);
    project_field(ui, "project.font", projects->project_font_source,
                  sizeof(projects->project_font_source), 12, 158, form_width);
    if (project_button(ui, "project.font.browse", "Choose default font", 12,
                       194, form_width, false_v)) {
      static const char *const extensions[] = {"ttf", "otf"};
      project_browse(projects, frame, "Choose project default font", extensions,
                     2, false_v, projects->project_font_source,
                     sizeof(projects->project_font_source));
    }
    project_label(ui, "project.bootstrap",
                  "Editor resources: reuse validated bundle", 12, 240,
                  form_width);
    project_label(ui, "project.font.bake",
                  "Project font: prepare once when changed", 12, 272,
                  form_width);
  }
  if (projects->include_scene) {
    if (creating && !left) {
      VkrUiPanelConfig form = vkr_ui_panel_config_default();
      form.placement.column = 0;
      form.placement.row = 0;
      form.placement.margin_pt.top = 320;
      form.columns = &one;
      form.column_count = 1;
      form.rows = &one;
      form.row_count = 1;
      if (vkr_ui_panel_begin(ui, string8_lit("narrow.scene"), &form)) {
        projects->form_height =
            320.0f +
            project_build_scene_form(projects, frame, 12, body_width - 24);
        (void)vkr_ui_panel_end(ui);
      }
    } else {
      projects->form_height =
          Max(creating ? 320.0f : 0.0f,
              project_build_scene_form(projects, frame, left + 12,
                                       body_width - left - 24));
    }
  } else {
    projects->form_height = 320.0f;
  }
}

static void project_build_rename_form(VkrEditorProjects *projects,
                                      const VkrSampleUiFrame *frame,
                                      float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  project_label(ui, "rename.label", "Name", 12, 12, body_width - 24);
  project_field(ui, "rename.name", projects->rename_name,
                sizeof(projects->rename_name), 12, 46, body_width - 24);
  if (project_button(ui, "rename.save", "Save name", 12, 96, 150,
                     projects->read_only)) {
    VkrEditorProjectError error = {0};
    if (!vkr_editor_project_name_valid(projects->rename_name, &error)) {
      project_error(projects, &error);
    } else if (project_finish_settings_save(projects)) {
      char *target_name =
          projects->rename_project
              ? projects->project->name
              : projects->project->scenes[projects->pending_scene].name;
      char previous[VKR_EDITOR_PROJECT_NAME_CAPACITY];
      snprintf(previous, sizeof(previous), "%s", target_name);
      snprintf(target_name, VKR_EDITOR_PROJECT_NAME_CAPACITY, "%s",
               projects->rename_name);
      if (vkr_editor_project_save(projects->project, &error)) {
        projects->view = PROJECT_VIEW_SCENES;
        project_refresh(projects);
      } else {
        snprintf(target_name, VKR_EDITOR_PROJECT_NAME_CAPACITY, "%s", previous);
        project_error(projects, &error);
      }
    }
  }
}

static void project_build_delete_form(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  project_label(ui, "delete.name",
                projects->project->scenes[projects->pending_scene].name, 12, 12,
                body_width - 24);
  project_label(ui, "delete.warning",
                "Permanently erase this scene and its managed files.", 12, 56,
                body_width - 24);
  project_label(ui, "delete.edits",
                projects->pending_scene == projects->active_scene
                    ? "The scene will unload. Unsaved edits will be discarded."
                    : "The current scene will remain open.",
                12, 96, body_width - 24);
  project_label(ui, "delete.shared",
                "Project-shared assets are kept. This cannot be undone.", 12,
                136, body_width - 24);
  if (project_button(ui, "delete.confirm", "Delete permanently", 12, 184, 190,
                     projects->read_only || frame->scene_loading ||
                         projects->job_id || projects->waiting_activation ||
                         projects->delete_waiting_unload ||
                         vkr_editor_bakery_busy(editor->bakery))) {
    project_delete_scene(projects, editor, frame);
  }
}

static void project_build_delete_project_form(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame,
                                              float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  const bool8_t active =
      projects->project &&
      strcmp(projects->project->id, projects->delete_project_id) == 0;
  project_label(ui, "delete.project.name", projects->delete_project_name, 12,
                12, body_width - 24);
  project_label(ui, "delete.project.warning",
                "Permanently erase this project, all of its scenes and their "
                "managed files.",
                12, 56, body_width - 24);
  project_label(ui, "delete.project.edits",
                active ? "The open scene will unload. Unsaved edits will be "
                         "discarded."
                       : "The current project stays open.",
                12, 96, body_width - 24);
  project_label(ui, "delete.project.cache",
                "Workspace caches that no other project uses are removed. "
                "This cannot be undone.",
                12, 136, body_width - 24);
  if (project_button(ui, "delete.project.confirm", "Delete permanently", 12,
                     184, 190,
                     projects->read_only || frame->scene_loading ||
                         projects->job_id || projects->waiting_activation ||
                         projects->delete_project_waiting_unload ||
                         projects->delete_project_job ||
                         vkr_editor_bakery_busy(editor->bakery))) {
    project_delete_project(projects, editor, frame);
  }
}

/* Scenes of the open project as cards: click a card to open it; rename and
 * delete sit at its end. An empty project offers Add scene in place. */
static void project_build_scene_list(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig name = project_widget(12, 0, body_width - 330, 30);
  name.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
  name.style.font_size_pt = theme->font_emphasis;
  name.text.font = editor->heading_font;
  name.icon = VKR_UI_ICON_PROJECT;
  name.icon_size_pt = 16.0f;
  name.icon_color = theme->accent_hover;
  vkr_ui_label(ui, string8_lit("scene.project"),
               project_string(projects->project->name), &name);
  if (project_button(ui, "project.rename", "Rename project", body_width - 314,
                     0, 150, projects->read_only)) {
    projects->rename_project = true_v;
    snprintf(projects->rename_name, sizeof(projects->rename_name), "%s",
             projects->project->name);
    projects->view = PROJECT_VIEW_RENAME;
  }
  if (project_button(
          ui, "project.delete", "Delete project", body_width - 156, 0, 144,
          projects->read_only || frame->scene_loading || projects->job_id ||
              projects->waiting_activation || projects->delete_project_job ||
              vkr_editor_bakery_busy(editor->bakery))) {
    snprintf(projects->delete_project_id, sizeof(projects->delete_project_id),
             "%s", projects->project->id);
    snprintf(projects->delete_project_name,
             sizeof(projects->delete_project_name), "%s",
             projects->project->name);
    projects->message[0] = '\0';
    projects->resume_view = PROJECT_VIEW_SCENES;
    projects->view = PROJECT_VIEW_DELETE_PROJECT;
    return;
  }
  const bool8_t locked = frame->scene_loading || projects->job_id ||
                         projects->waiting_activation ||
                         vkr_editor_bakery_busy(editor->bakery);
  for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
    VkrEditorProjectScene *scene = &projects->project->scenes[i];
    const float32_t y = 48 + i * 56.0f;
    const bool8_t active = projects->active_scene == i;
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiWidgetConfig row = project_widget(12, y, body_width - 24, 48);
    row.style.background_color = theme->raised;
    row.style.hover_background_color = theme->raised_hover;
    row.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
    row.style.border_color = active ? theme->accent : theme->border;
    row.style.corner_radius_pt = (Vec4){8, 8, 8, 8};
    row.disabled = locked;
    row.tooltip = active ? string8_lit("Reopen this scene")
                         : string8_lit("Open this scene");
    if (vkr_ui_button(ui, string8_lit("scene.open"), (String8){0}, &row)) {
      projects->pending_scene = i;
      projects->operation[0] = '\0';
      if (frame->edits->revision != frame->edits->saved_revision) {
        projects->resume_view = PROJECT_VIEW_SCENES;
        projects->view = PROJECT_VIEW_CONFIRM;
      } else {
        project_start_job(projects, editor, frame, false_v);
      }
    }
    VkrUiWidgetConfig label = project_widget(24, y + 9, body_width - 200, 30);
    label.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
    label.style.text_color = theme->text;
    label.icon = VKR_UI_ICON_SCENE;
    label.icon_size_pt = 16.0f;
    label.icon_color = active ? theme->accent_hover : theme->text_secondary;
    vkr_ui_label(ui, string8_lit("scene.name"), project_string(scene->name),
                 &label);
    if (active) {
      VkrUiWidgetConfig badge =
          project_widget(body_width - 170, y + 14, 60, 20);
      badge.style.padding_pt = (VkrUiEdges){1, 8, 1, 8};
      badge.style.font_size_pt = theme->font_caption;
      badge.style.background_color = vkr_ui_color_alpha(theme->accent, 0.22f);
      badge.style.corner_radius_pt = (Vec4){10, 10, 10, 10};
      badge.style.text_color = theme->accent_hover;
      badge.center = true_v;
      vkr_ui_label(ui, string8_lit("scene.badge"), string8_lit("Open"), &badge);
    }
    VkrUiWidgetConfig rename = vkr_editor_icon_button_config(
        0, 0, VKR_UI_ICON_RENAME, string8_lit("Rename scene"));
    rename.placement =
        project_widget(body_width - 96, y + 11, 26, 26).placement;
    rename.disabled = projects->read_only;
    if (vkr_ui_button(ui, string8_lit("scene.rename"), (String8){0}, &rename)) {
      projects->pending_scene = i;
      projects->rename_project = false_v;
      snprintf(projects->rename_name, sizeof(projects->rename_name), "%s",
               scene->name);
      projects->view = PROJECT_VIEW_RENAME;
    }
    VkrUiWidgetConfig remove = vkr_editor_icon_button_config(
        0, 0, VKR_UI_ICON_TRASH, string8_lit("Delete scene permanently"));
    remove.placement =
        project_widget(body_width - 62, y + 11, 26, 26).placement;
    remove.style.hover_background_color =
        vkr_ui_color_alpha(theme->error, 0.35f);
    remove.disabled = projects->read_only || locked;
    if (vkr_ui_button(ui, string8_lit("scene.delete"), (String8){0}, &remove)) {
      projects->pending_scene = i;
      projects->message[0] = '\0';
      projects->view = PROJECT_VIEW_DELETE;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (!projects->project->scene_count) {
    VkrUiWidgetConfig mark = project_widget(12, 110, body_width - 24, 40);
    mark.center = true_v;
    mark.icon = VKR_UI_ICON_SCENE;
    mark.icon_size_pt = 32;
    mark.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("scenes.empty.mark"), (String8){0}, &mark);
    VkrUiWidgetConfig title = project_widget(12, 158, body_width - 24, 28);
    title.center = true_v;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("scenes.empty.title"),
                 string8_lit("No scenes yet"), &title);
    VkrUiWidgetConfig caption = project_widget(12, 186, body_width - 24, 24);
    caption.center = true_v;
    caption.style.text_color = theme->text_secondary;
    vkr_ui_label(ui, string8_lit("scenes.empty.caption"),
                 string8_lit("Create a scene or import one from JSON."),
                 &caption);
    if (project_button(ui, "scene.add.first", "Add scene",
                       body_width * 0.5f - 80.0f, 226, 160,
                       projects->read_only)) {
      project_reset_scene_draft(projects);
      projects->view = PROJECT_VIEW_ADD_SCENE;
    }
  }
}

static void project_build_confirm_form(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  project_label(ui, "dirty.message",
                projects->closing && projects->job_id
                    ? "An asset job is active. Closing cancels it and "
                      "waits for its worker."
                    : "Save your scene edits, discard them, or return to "
                      "the current scene.",
                12, 12, body_width - 24);
  if (project_button(ui, "dirty.save", "Save and continue", 12, 64, 170,
                     false_v)) {
    projects->awaiting_save = true_v;
    frame->scene_edit->action = VKR_SCENE_EDIT_SAVE;
  }
  if (project_button(ui, "dirty.discard", "Discard edits", 194, 64, 145,
                     false_v) ||
      (projects->awaiting_save &&
       frame->edits->revision == frame->edits->saved_revision)) {
    projects->awaiting_save = false_v;
    projects->discard_edits = true_v;
    if (projects->closing) {
      if (project_save_settings(projects, editor, frame->dock)) {
        *frame->close_response = VKR_SAMPLE_CLOSE_CONFIRM;
      }
    } else {
      projects->view = projects->resume_view;
    }
    if (!projects->closing && projects->view == PROJECT_VIEW_SCENES) {
      project_start_job(projects, editor, frame, false_v);
    }
  }
}

static void project_build_footer(VkrEditorProjects *projects,
                                 VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, float32_t width,
                                 float32_t height) {
  VkrUiSystem *ui = frame->ui;
  if (projects->view == PROJECT_VIEW_ADD_ENTITY) {
    const float32_t button_width = Min(180.0f, (width - 60) * .5f);
    if (project_button(ui, "entity.submit", "Add to scene",
                       width - 36 - button_width, height - 65, button_width,
                       projects->read_only ||
                           !project_entity_draft_valid(projects) ||
                           frame->scene_loading || projects->job_id ||
                           vkr_editor_bakery_busy(editor->bakery))) {
      snprintf(projects->operation, sizeof(projects->operation),
               "add_entities");
      if (frame->edits->revision != frame->edits->saved_revision) {
        projects->resume_view = PROJECT_VIEW_SCENES;
        projects->view = PROJECT_VIEW_CONFIRM;
      } else {
        project_start_job(projects, editor, frame, false_v);
      }
    }
  }
  project_label(ui, "projects.message",
                projects->read_only && !projects->message[0]
                    ? "Read-only workspace: another editor currently owns "
                      "the write lease."
                : projects->view == PROJECT_VIEW_ADD_ENTITY &&
                        !projects->message[0]
                    ? "Add saves the entity and reloads this scene."
                    : projects->message,
                12, height - 108, width - 48);
  const bool8_t drafting = projects->view == PROJECT_VIEW_CREATE ||
                           projects->view == PROJECT_VIEW_ADD_SCENE;
  if (drafting &&
      project_button(ui, "create.submit",
                     projects->include_scene ? "Create and prepare"
                                             : "Create empty project",
                     width - 216, height - 65, 180, false_v)) {
    project_create(projects, editor, frame);
  }
  /* An empty project shows Add scene in its empty state instead. */
  if (projects->view == PROJECT_VIEW_SCENES && projects->project->scene_count &&
      project_button(ui, "scene.add", "Add scene", width - 184, height - 65,
                     148, false_v)) {
    project_reset_scene_draft(projects);
    projects->view = PROJECT_VIEW_ADD_SCENE;
  }
  const bool8_t back_available =
      !(projects->view == PROJECT_VIEW_CHOOSER && !projects->project);
  if (back_available &&
      project_button(
          ui, "projects.back",
          projects->view == PROJECT_VIEW_ADD_ENTITY ||
                  projects->view == PROJECT_VIEW_CONFIRM ||
                  projects->view == PROJECT_VIEW_DELETE ||
                  projects->view == PROJECT_VIEW_DELETE_PROJECT
              ? "Cancel"
          : projects->project && !projects->creating_project ? "Back to editor"
                                                             : "Back",
          12, height - 65,
          projects->view == PROJECT_VIEW_ADD_ENTITY
              ? Min(148.0f, (width - 60) * .5f)
              : 148.0f,
          projects->delete_waiting_unload ||
              projects->delete_project_waiting_unload ||
              (projects->view == PROJECT_VIEW_CHOOSER && !projects->project))) {
    if ((projects->view == PROJECT_VIEW_ADD_ENTITY ||
         (projects->view == PROJECT_VIEW_CONFIRM &&
          !strcmp(projects->operation, "add_entities"))) &&
        !projects->closing) {
      projects->operation[0] = '\0';
      projects->message[0] = '\0';
      if (!frame->scene && !frame->scene_loading) {
        project_start_job(projects, editor, frame, false_v);
      } else {
        projects->view = PROJECT_VIEW_EDITOR;
      }
    } else if (projects->view == PROJECT_VIEW_DELETE_PROJECT) {
      projects->view = projects->resume_view;
    } else if (projects->view == PROJECT_VIEW_DELETE) {
      projects->view = PROJECT_VIEW_SCENES;
      projects->delete_waiting_unload = false_v;
      projects->operation[0] = '\0';
    } else if (projects->closing) {
      *frame->close_response = VKR_SAMPLE_CLOSE_CANCEL;
      projects->closing = false_v;
      projects->view = projects->resume_view;
    } else {
      projects->view = projects->project && !projects->creating_project
                           ? PROJECT_VIEW_EDITOR
                           : PROJECT_VIEW_CHOOSER;
    }
    projects->initial_project[0] = '\0';
    projects->discard_edits = false_v;
    projects->awaiting_save = false_v;
  }
}

static void project_build_view(VkrEditorProjects *projects, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (projects && projects->view == PROJECT_VIEW_EDITOR && projects->dropdown) {
    project_build_dropdown(projects, editor, frame);
    return;
  }
  if (!projects || projects->view == PROJECT_VIEW_EDITOR) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  *frame->modal = true_v;
  const float32_t total_width = ui->target_width / ui->content_scale;
  const float32_t total_height = ui->target_height / ui->content_scale;
  const VkrUiRect bounds = {0, 0, (float32_t)ui->target_width,
                            (float32_t)ui->target_height};
  (void)vkr_ui_input_layer_register(ui, PROJECT_MODAL_LAYER, bounds);
  (void)vkr_ui_input_layer_set(ui, PROJECT_MODAL_LAYER);
  (void)vkr_ui_keyboard_layer_set(ui, PROJECT_MODAL_LAYER);
  vkr_ui_keyboard_navigation_enabled(ui, true_v);
  const VkrUiTrack one = {.unit = VKR_UI_TRACK_FR, .value = 1};
  {
    VkrUiPanelConfig background = vkr_ui_panel_config_default();
    background.placement.column = 0;
    background.placement.row = 0;
    background.columns = &one;
    background.column_count = 1;
    background.rows = &one;
    background.row_count = 1;
    background.style.background_color = vkr_ui_theme()->window;
    if (vkr_ui_panel_begin(ui, string8_lit("projects.background"),
                           &background)) {
      (void)vkr_ui_panel_end(ui);
    }
  }
  /* Before a project opens, the Projects view is the whole launcher window;
   * afterwards it floats over the editor as a dialog. */
  const bool8_t launcher = vkr_editor_projects_launcher(projects);
  const float32_t width =
      launcher
          ? total_width
          : Min(projects->view == PROJECT_VIEW_ADD_ENTITY ? 620.0f : 1040.0f,
                Max(280.0f, total_width - 40));
  const float32_t height =
      launcher ? total_height : Min(740.0f, Max(240.0f, total_height - 40));
  VkrUiPanelConfig modal = vkr_ui_panel_config_default();
  modal.placement.column = 0;
  modal.placement.row = 0;
  modal.placement.justify = VKR_UI_ALIGN_CENTER;
  modal.placement.align = VKR_UI_ALIGN_CENTER;
  modal.columns = &one;
  modal.column_count = 1;
  modal.rows = &one;
  modal.row_count = 1;
  modal.style.min_size_pt = (Vec2){width, height};
  modal.style.max_size_pt = modal.style.min_size_pt;
  modal.style = vkr_editor_glass_style();
  modal.style.min_size_pt = (Vec2){width, height};
  modal.style.max_size_pt = modal.style.min_size_pt;
  modal.style.padding_pt = (VkrUiEdges){16, 16, 16, 16};
  modal.style.background_color = vkr_ui_theme()->panel;
  modal.style.corner_radius_pt = (Vec4){12, 12, 12, 12};
  modal.style.shadow_offset_pt = (Vec2){0.0f, 16.0f};
  modal.style.shadow_blur_pt = 40.0f;
  modal.clip_children = true_v;
  float32_t heading_x = 8.0f;
  if (launcher) {
    /* The heading shares the native title bar row with the window controls,
     * and that row drags the window. */
    modal.style.padding_pt.top = 0.0f;
    modal.style.border_pt = (VkrUiEdges){0};
    modal.style.corner_radius_pt = (Vec4){0};
    modal.style.shadow_color = (Vec4){0};
    heading_x =
        Max(heading_x, vkr_window_title_bar_inset(frame->window) - 8.0f);
    vkr_window_set_title_drag_region(
        frame->window, 0, 0, (int32_t)ui->target_width,
        (int32_t)(PROJECT_LAUNCHER_TITLE_PT * ui->content_scale),
        ui->hot_id == VKR_UI_ID_NONE);
  }
  if (!vkr_ui_panel_begin(ui, string8_lit("projects.modal"), &modal)) {
    return;
  }
  const char *title =
      projects->view == PROJECT_VIEW_CHOOSER      ? "Projects"
      : projects->view == PROJECT_VIEW_CREATE     ? "Create project"
      : projects->view == PROJECT_VIEW_ADD_SCENE  ? "Add scene"
      : projects->view == PROJECT_VIEW_ADD_ENTITY ? "Add entity"
      : projects->view == PROJECT_VIEW_SCENES     ? "Scenes"
      : projects->view == PROJECT_VIEW_RENAME     ? "Rename"
      : projects->view == PROJECT_VIEW_DELETE     ? "Delete scene permanently?"
      : projects->view == PROJECT_VIEW_DELETE_PROJECT
          ? "Delete project permanently?"
      : projects->view == PROJECT_VIEW_CONFIRM
          ? projects->closing ? "Close editor" : "Unsaved scene edits"
          : "Preparing your project";
  VkrUiWidgetConfig heading =
      project_widget(heading_x, 0, width - 32 - heading_x, 40);
  heading.style.font_size_pt = vkr_ui_theme()->font_heading;
  heading.text.font = editor->heading_font;
  heading.style.text_color = vkr_ui_theme()->text;
  vkr_ui_label(ui, string8_lit("title"), project_string(title), &heading);
  const VkrUiTrack content = {
      .unit = VKR_UI_TRACK_PX,
      .value = projects->view == PROJECT_VIEW_CREATE ||
                       projects->view == PROJECT_VIEW_ADD_SCENE
                   ? Max(height - 160, projects->form_height + 24)
               : projects->view == PROJECT_VIEW_ADD_ENTITY
                   ? 550
                   : Max(height - 175, projects->card_count * 70.0f + 160)};
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  scroll.placement.column = 0;
  scroll.placement.row = 0;
  scroll.placement.justify = VKR_UI_ALIGN_START;
  scroll.placement.align = VKR_UI_ALIGN_START;
  scroll.placement.margin_pt.top = 48;
  scroll.style.min_size_pt = (Vec2){width - 26, height - 160};
  scroll.style.max_size_pt = scroll.style.min_size_pt;
  scroll.columns = &one;
  scroll.column_count = 1;
  scroll.rows = &content;
  scroll.row_count = 1;
  if (vkr_ui_scroll_area_begin(ui, string8_lit("projects.content"), &scroll)) {
    const float32_t body_width = width - 46;
    if (projects->view == PROJECT_VIEW_CHOOSER) {
      project_build_chooser(projects, editor, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_ADD_ENTITY) {
      project_build_entity_form(projects, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_CREATE ||
               projects->view == PROJECT_VIEW_ADD_SCENE) {
      project_build_create_form(projects, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_RENAME) {
      project_build_rename_form(projects, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_DELETE_PROJECT) {
      project_build_delete_project_form(projects, editor, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_DELETE && projects->project &&
               projects->pending_scene < projects->project->scene_count) {
      project_build_delete_form(projects, editor, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_SCENES && projects->project) {
      project_build_scene_list(projects, editor, frame, body_width);
    } else if (projects->view == PROJECT_VIEW_CONFIRM) {
      project_build_confirm_form(projects, editor, frame, body_width);
    }
    (void)vkr_ui_scroll_area_end(ui);
  }
  if (!projects->dialog_closed) {
    project_build_footer(projects, editor, frame, width, height);
  }
  (void)vkr_ui_panel_end(ui);
}

/* A failed or cancelled job keeps its request so Retry can resubmit it. */
static bool8_t project_job_stopped(const VkrEditorProjects *projects,
                                   VkrEditorUi *editor) {
  if (!projects->job_id) {
    return false_v;
  }
  const VkrEditorProjectJobStatus status =
      vkr_editor_bakery_project_status(editor->bakery, projects->job_id, NULL);
  return status == VKR_EDITOR_PROJECT_JOB_FAILED ||
         status == VKR_EDITOR_PROJECT_JOB_CANCELLED;
}

/* Abandons a stopped request so navigation, scene actions and previews
 * resume. Committed project and scene files stay as the job left them. */
static void project_leave_failed_job(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     ProjectView view) {
  projects->job_id = 0;
  projects->job_creates_project = false_v;
  projects->job_creates_scene = false_v;
  projects->operation[0] = '\0';
  projects->progress_stage[0] = '\0';
  projects->progress_detail[0] = '\0';
  if (projects->creating_project) {
    /* Creation published the manifest; opening it restores its settings. */
    char id[37];
    snprintf(id, sizeof(id), "%s", projects->project->id);
    if (!project_load(projects, id, editor, frame)) {
      projects->view = PROJECT_VIEW_CHOOSER;
    }
    return;
  }
  projects->view = view;
}

void vkr_editor_projects_build_scene_progress(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame) {
  if (!projects || projects->view != PROJECT_VIEW_PROGRESS ||
      !frame->mapping_valid) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  if (viewport.z <= 0 || viewport.w <= 0) {
    return;
  }
  if (!projects->job_creates_project && frame->scene_backdrop_blur) {
    *frame->scene_backdrop_blur = true_v;
  }
  const VkrUiRect bounds = {viewport.x, viewport.y, viewport.z, viewport.w};
  (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER, bounds);
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER);
  const VkrUiTrack one = {.unit = VKR_UI_TRACK_FR, .value = 1};
  const float32_t width = viewport.z / ui->content_scale;
  const float32_t height = viewport.w / ui->content_scale;
  VkrUiPanelConfig overlay = vkr_ui_panel_config_default();
  overlay.placement.column = 0;
  overlay.placement.row = 0;
  overlay.placement.justify = VKR_UI_ALIGN_START;
  overlay.placement.align = VKR_UI_ALIGN_START;
  overlay.placement.margin_pt.left = viewport.x / ui->content_scale;
  overlay.placement.margin_pt.top = viewport.y / ui->content_scale;
  overlay.columns = &one;
  overlay.column_count = 1;
  overlay.rows = &one;
  overlay.row_count = 1;
  overlay.style.min_size_pt = (Vec2){width, height};
  overlay.style.max_size_pt = overlay.style.min_size_pt;
  overlay.style.background_color =
      vkr_ui_color_alpha(vkr_ui_theme()->window, 0.55f);
  overlay.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("scene.prepare.overlay"), &overlay)) {
    return;
  }
  const VkrEditorProjectJobStatus status =
      vkr_editor_bakery_project_status(editor->bakery, projects->job_id, NULL);
  const bool8_t running = status == VKR_EDITOR_PROJECT_JOB_RUNNING ||
                          status == VKR_EDITOR_PROJECT_JOB_QUEUED ||
                          projects->waiting_activation;
  const float32_t bar_width = Min(220.0f, Max(1.0f, width - 32));
  const float32_t top = Max(0.0f, (height - 82.0f) * .5f);
  VkrUiWidgetConfig label = vkr_ui_widget_config_default();
  label.placement.column = 0;
  label.placement.row = 0;
  label.placement.justify = VKR_UI_ALIGN_CENTER;
  label.placement.align = VKR_UI_ALIGN_START;
  label.placement.margin_pt.top = top;
  label.style.font_size_pt = 13;
  label.style.text_color = vkr_ui_theme()->text;
  label.style.padding_pt = (VkrUiEdges){6, 0, 6, 0};
  label.style.max_size_pt.x = Max(1.0f, width - 32);
  label.tooltip = project_string(
      projects->message[0] ? projects->message : projects->progress_detail);
  const char *stage =
      running ? projects->progress_stage[0] ? projects->progress_stage
                : !strcmp(projects->operation, "delete_scene")
                    ? "Deleting scene files..."
                : projects->job_creates_project ? "Creating project..."
                                                : "Preparing scene..."
      : !strcmp(projects->operation, "delete_scene")
          ? "File deletion incomplete"
      : status == VKR_EDITOR_PROJECT_JOB_CANCELLED ? "Preparation cancelled"
                                                   : "Preparation failed";
  vkr_ui_label(ui, string8_lit("scene.prepare.stage"), project_string(stage),
               &label);
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement.column = 0;
  bar.placement.row = 0;
  bar.placement.justify = VKR_UI_ALIGN_CENTER;
  bar.placement.align = VKR_UI_ALIGN_START;
  bar.placement.margin_pt.top = top + 36;
  bar.style.min_size_pt = (Vec2){bar_width, 4};
  bar.style.max_size_pt = bar.style.min_size_pt;
  bar.style.background_color = vkr_ui_theme()->raised;
  bar.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
  bar.columns = &one;
  bar.column_count = 1;
  bar.rows = &one;
  bar.row_count = 1;
  bar.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("scene.prepare.bar"), &bar)) {
    if (running) {
      VkrUiPanelConfig fill = bar;
      fill.placement.justify = VKR_UI_ALIGN_START;
      fill.placement.margin_pt.top = 0;
      /* Job percentages describe stage boundaries, not ongoing work. Keep
       * the activity indicator moving throughout each preparation stage. */
      fill.style.min_size_pt.x = bar_width * .25f;
      fill.placement.margin_pt.left =
          (bar_width - fill.style.min_size_pt.x) *
          (float32_t)(.5 - .5 * cos(vkr_platform_get_absolute_time() * 3));
      fill.style.max_size_pt = fill.style.min_size_pt;
      fill.style.background_color = vkr_ui_theme()->accent;
      fill.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
      if (vkr_ui_panel_begin(ui, string8_lit("scene.prepare.fill"), &fill)) {
        (void)vkr_ui_panel_end(ui);
      }
    }
    (void)vkr_ui_panel_end(ui);
  }
  VkrUiWidgetConfig action =
      project_widget(Max(0.0f, (width - 76) * .5f), top + 56, 76, 26);
  vkr_editor_action_style(&action, VKR_FONT_HANDLE_INVALID);
  action.disabled = projects->waiting_activation;
  if (running) {
    if (vkr_ui_button(ui, string8_lit("scene.prepare.cancel"),
                      string8_lit("Cancel"), &action)) {
      vkr_editor_bakery_project_cancel(editor->bakery, projects->job_id);
    }
  } else {
    VkrUiWidgetConfig back = action;
    back.placement.margin_pt.left = Max(0.0f, (width - 160) * .5f);
    action.placement.margin_pt.left = back.placement.margin_pt.left + 84;
    if (vkr_ui_button(ui, string8_lit("scene.prepare.back"),
                      string8_lit("Back"), &back)) {
      project_leave_failed_job(projects, editor, frame, PROJECT_VIEW_SCENES);
    } else if (vkr_ui_button(ui, string8_lit("scene.prepare.retry"),
                             string8_lit("Retry"), &action)) {
      if (!projects->job_id && strcmp(projects->operation, "delete_scene")) {
        project_job_complete(projects, editor, frame);
      } else {
        projects->job_id = vkr_editor_bakery_project_start(
            editor->bakery, projects->request_path, projects->result_path);
        projects->message[0] = '\0';
        projects->progress_stage[0] = '\0';
      }
    }
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_projects_build(VkrEditorProjects *projects, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (projects && projects->view != PROJECT_VIEW_PROGRESS) {
    project_build_view(projects, editor, frame);
  }
}

bool8_t vkr_editor_projects_save_scene(VkrEditorProjects *projects,
                                       VkrSceneEditState *edits,
                                       const VkrScene *scene,
                                       String8 runtime_scene_path) {
  if (!projects || projects->read_only || !projects->scene_manifest_path[0] ||
      !string8_equals(&runtime_scene_path,
                      &(String8){.str = (uint8_t *)projects->runtime_path,
                                 .length = strlen(projects->runtime_path)})) {
    snprintf(edits->status, sizeof(edits->status),
             "No writable managed scene is active.");
    return false_v;
  }
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&projects->project_allocator);
  VkrEditorProjectError error = {0};
  const bool8_t saved = vkr_editor_project_save_scene_overlay(
      projects->scene_manifest_path, &projects->scene_manifest_fingerprint,
      edits, scene, &projects->project_allocator, &error);
  if (!saved) {
    project_error(projects, &error);
    snprintf(edits->status, sizeof(edits->status), "%.191s", error.message);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return saved;
}

void vkr_editor_projects_scene_action(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  if (!projects) {
    return;
  }
  const VkrSceneEditAction action = frame->scene_edit->action;
  if ((frame->scene_loading || projects->job_id ||
       projects->waiting_activation ||
       !strcmp(projects->operation, "delete_scene")) &&
      (action == VKR_SCENE_EDIT_LOAD || action == VKR_SCENE_EDIT_RELOAD ||
       action == VKR_SCENE_EDIT_UNLOAD)) {
    frame->scene_edit->action = VKR_SCENE_EDIT_NONE;
    return;
  }
  if (action != VKR_SCENE_EDIT_LOAD && action != VKR_SCENE_EDIT_RELOAD) {
    return;
  }
  frame->scene_edit->action = VKR_SCENE_EDIT_NONE;
  if (projects->job_id || projects->waiting_activation) {
    return;
  }
  if (!projects->project ||
      projects->active_scene >= projects->project->scene_count) {
    projects->view =
        projects->project ? PROJECT_VIEW_SCENES : PROJECT_VIEW_CHOOSER;
    return;
  }
  projects->pending_scene = projects->active_scene;
  projects->operation[0] = '\0';
  if (frame->edits->revision != frame->edits->saved_revision) {
    projects->resume_view = PROJECT_VIEW_SCENES;
    projects->view = PROJECT_VIEW_CONFIRM;
  } else {
    project_start_job(projects, editor, frame, false_v);
  }
}

void vkr_editor_projects_navigation(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t first_column) {
  if (!projects) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  /* Only a queued or running job locks navigation; a stopped one is left. */
  const bool8_t stopped = project_job_stopped(projects, editor);
  for (uint32_t i = 0; i < 2; ++i) {
    const bool8_t active = projects->dropdown == i + 1;
    VkrUiWidgetConfig button = vkr_editor_menu_button_config(
        first_column + i, active, VKR_FONT_HANDLE_INVALID);
    button.icon = i == 1 ? VKR_UI_ICON_SCENE : VKR_UI_ICON_PROJECT;
    button.icon_size_pt = 14.0f;
    button.icon_color = vkr_ui_theme()->accent_hover;
    button.disabled =
        (projects->job_id && !stopped) || projects->waiting_activation ||
        (!stopped && !strcmp(projects->operation, "delete_scene")) ||
        (i == 1 && !projects->project);
    /* The open dropdown already names itself; no tooltip over it. */
    button.tooltip =
        active   ? (String8){0}
        : i == 1 ? string8_lit("Choose a scene in the current project")
        : projects->read_only
            ? string8_lit(
                  "Read-only workspace: another editor holds its write lease")
            : string8_lit("Choose a project in this workspace");
    const String8 title = i == 1                ? string8_lit("Scenes")
                          : projects->read_only ? string8_lit("Projects [RO]")
                                                : string8_lit("Projects");
    const String8 label =
        i == 1 ? string8_lit("scenes") : string8_lit("projects");
    const VkrUiId button_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, label);
    const bool8_t clicked = vkr_ui_button(ui, label, title, &button);
    /* The dropdown opens beneath the button that owns it. */
    if (active || clicked)
      (void)vkr_ui_widget_rect(ui, button_id, &projects->dropdown_anchor_px);
    if (clicked) {
      if (active) {
        projects->dropdown = 0;
        projects->dropdown_opened = false_v;
        (void)vkr_ui_keyboard_layer_set(ui, 0);
        continue;
      }
      if (projects->job_id && stopped) {
        project_leave_failed_job(projects, editor, frame, PROJECT_VIEW_EDITOR);
      }
      if (i == 0) {
        if (!project_save_settings(projects, editor, frame->dock)) {
          continue;
        }
        project_refresh(projects);
      }
      projects->dropdown = i + 1;
      projects->dropdown_opened = true_v;
    }
  }
}

bool8_t vkr_editor_projects_flush(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrUiDockTree *dock) {
  if (!projects) {
    return true_v;
  }
  const bool8_t saved = project_save_settings(projects, editor, dock);
  projects->settings_restored = false_v;
  return saved;
}

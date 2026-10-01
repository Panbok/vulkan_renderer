#include "editor_projects.h"
#include "editor_build.h"
#include "editor_content.h"
#include "editor_install.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include "editor_scene_panels.h"
#include <ctype.h>
#include <math.h>

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_hash.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_file_dialog.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_types.h"
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

typedef enum ProjectCreateStep {
  PROJECT_CREATE_CHOOSE,
  PROJECT_CREATE_SCENE,
  PROJECT_CREATE_IMPORT,
} ProjectCreateStep;

typedef enum VkrEditorProjectTemplate {
  PROJECT_TEMPLATE_BLANK,
  PROJECT_TEMPLATE_FPS_ARENA,
  PROJECT_TEMPLATE_RPG_GROUNDS,
  PROJECT_TEMPLATE_NONE,
  PROJECT_TEMPLATE_COUNT,
} VkrEditorProjectTemplate;

typedef struct s_EditorProjectTemplateInfo {
  const char *name;
  /* Base name of the scene in assets/templates and of its preview PNG in
     assets/templates/previews; empty for No starter scene. */
  const char *file;
  const char *detail;
  const char *camera;
  VkrUiIcon camera_icon;
  const char *footprint;
} VkrEditorProjectTemplateInfo;

static const VkrEditorProjectTemplateInfo
    project_templates[PROJECT_TEMPLATE_COUNT] = {
        [PROJECT_TEMPLATE_BLANK] =
            {.name = "Blank",
             .file = "blank",
             .detail = "A 20 m floor, origin markers and a playable spawn.",
             .camera = "First person",
             .camera_icon = VKR_UI_ICON_CROSSHAIR,
             .footprint = "20 x 20 m"},
        [PROJECT_TEMPLATE_FPS_ARENA] =
            {.name = "FPS Arena",
             .file = "fps_arena",
             .detail =
                 "Cover, range markers, stairs, ramps and elevated routes.",
             .camera = "First person",
             .camera_icon = VKR_UI_ICON_CROSSHAIR,
             .footprint = "48 x 40 m"},
        [PROJECT_TEMPLATE_RPG_GROUNDS] =
            {.name = "RPG Grounds",
             .file = "rpg_grounds",
             .detail =
                 "A third-person courtyard with terrain and vertical routes.",
             .camera = "Third person",
             .camera_icon = VKR_UI_ICON_PERSON_WALK,
             .footprint = "64 x 64 m"},
        [PROJECT_TEMPLATE_NONE] =
            {.name = "No starter scene",
             .file = "",
             .detail = "Only the World; create or import scenes in the editor.",
             .camera = "",
             .footprint = ""},
};

/* A template's preview image while the Create project view is open. Its
   texture request is released when the view closes. */
typedef struct ProjectTemplatePreview {
  char request_path[1100];
  VkrResourceHandleInfo request;
  VkrTextureHandle texture;
  bool8_t requested;
  bool8_t failed;
} ProjectTemplatePreview;

/* Where imported models go (ADR-076): a new scene holding them, the World's
   root, a project scene, or only Content. Each placement shows the model in
   the viewport once its job finishes. */
typedef enum ProjectImportTarget {
  PROJECT_IMPORT_NEW_SCENE,
  PROJECT_IMPORT_WORLD,
  PROJECT_IMPORT_SCENE,
  PROJECT_IMPORT_CONTENT,
  PROJECT_IMPORT_TARGET_COUNT,
} ProjectImportTarget;

typedef struct ProjectCard {
  char id[37];
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  char error[128];
  uint32_t scenes;
} ProjectCard;

/* Summary of an `inspect_scene` job. */
typedef struct ProjectInspection {
  bool8_t valid;
  bool8_t saved_edits;
  int32_t scene_version;
  uint32_t entities;
  uint32_t meshes;
  uint32_t materials;
  uint32_t missing_count;
  /* Missing model images, which placeholders can stand in for. */
  uint32_t missing_images;
  uint32_t warning_count;
  char missing[3][160];
  char warning[160];
  char error[256];
} ProjectInspection;

/* What the save prompt resumes once edits are saved or discarded. */
typedef enum ProjectResume {
  PROJECT_RESUME_NONE,
  /* The prepared job: open `pending_scene` or run `operation`. */
  PROJECT_RESUME_JOB,
  /* Close the open scene and show the World. */
  PROJECT_RESUME_UNLOAD,
  /* Make the added scene `swap_scene` primary. */
  PROJECT_RESUME_SET_PRIMARY,
  /* Continue a Build's preflight (docs/proposals/project-packaging.md). */
  PROJECT_RESUME_BUILD,
} ProjectResume;

/* A Build's preflight: none, waiting on the save prompt, or resolved. */
typedef enum ProjectBuildPreflight {
  PROJECT_BUILD_PREFLIGHT_NONE,
  PROJECT_BUILD_PREFLIGHT_PROMPTING,
  PROJECT_BUILD_PREFLIGHT_RESOLVED,
} ProjectBuildPreflight;

/* Set primary (ADR-076): the added scene is removed, opened as the primary,
   and the previous primary is added back beside it. */
typedef enum ProjectSwap {
  PROJECT_SWAP_NONE,
  PROJECT_SWAP_OPEN,
  PROJECT_SWAP_READD,
} ProjectSwap;

/* A project scene loaded beside the open one, by the runtime document its
   add request named. */
typedef struct ProjectAddedScene {
  char runtime_path[1024];
  char scene_id[37];
} ProjectAddedScene;

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
  /* The workspace whose logs folder holds this session's log file. */
  char log_root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char log_name[64];
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
  VkrEditorProjectTemplate project_template;
  ProjectTemplatePreview template_previews[PROJECT_TEMPLATE_COUNT];
  bool8_t template_previews_held;
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
  /* Create or import window (ADR-076): its current step. */
  uint32_t create_step;
  bool8_t import_scene;
  /* Scene form height measured on the previous build; sizes its scroll. */
  float32_t form_height;
  /* Pixel rectangle of the navigation button that opened `dropdown`. */
  VkrUiRect dropdown_anchor_px;
  ProjectLightDraft lights[PROJECT_LIGHT_COUNT];
  uint32_t light_count;
  uint32_t light_selected;
  bool8_t adding_model;
  /* A Content mesh dropped on the viewport (ADR-076): its asset reference
     and the ground point it lands on. */
  bool8_t placing_asset;
  /* The placement is a prefab instance of project scene `place_asset`. */
  bool8_t place_prefab;
  char place_asset[37];
  char place_scope[8];
  char place_name[128];
  Vec3 place_position;
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
  /* Files dropped from the OS for the import step (ADR-076), and the
     Content folder imported assets are filed into. */
  char import_sources[VKR_WINDOW_DROP_PATH_MAX][VKR_WINDOW_DROP_PATH_CAPACITY];
  uint32_t import_source_count;
  char import_folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  /* The import step's placement of models and, for PROJECT_IMPORT_SCENE,
     the project scene index. */
  ProjectImportTarget import_target;
  uint32_t import_scene_index;
  /* The running job came from the import step: a failure returns to the
     editor with a message instead of another form. */
  bool8_t job_imports_models;
  /* The running Content import places its meshes at the World's root. */
  bool8_t job_world_models;
  char progress_stage[128];
  char progress_detail[512];
  float64_t next_progress_check;
  char runtime_path[1024];
  /* Root World document and its edit sidecar beside project.json. */
  char world_path[1024];
  char world_sidecar[1024];
  /* Frames a requested World load still counts as busy: the runtime starts
     it after this build and reports it loading until it activates. */
  uint32_t world_wait;
  /* Project meshes the requested World document places; Content shows them
     loading while the World streams. */
  char world_meshes[32][37];
  uint32_t world_mesh_count;
  char scene_manifest_path[1024];
  uint64_t scene_manifest_fingerprint;
  char edit_path[1024];
  uint64_t job_id;
  bool8_t job_creates_scene;
  bool8_t job_creates_project;
  /* The running job imports project assets (ADR-076); the open scene stays
     loaded and no scene publishes. */
  bool8_t job_project_assets;
  /* Import preflight of the Create form's scene JSON: a read-only job whose
     summary gates creation. `legacy_root` is a located folder for missing
     dependencies, else the repository. */
  bool8_t job_inspect;
  ProjectInspection inspection;
  char inspected_source[1024];
  char legacy_root[1024];
  bool8_t include_edits;
  bool8_t use_placeholders;
  /** Published with defaults; its first job imports the project font and
   * first scene. Project settings stay unrestored until that job completes. */
  bool8_t creating_project;
  bool8_t waiting_activation;
  /* The running scene job activates its result beside the active scene
     instead of replacing it (ADR-076). */
  bool8_t additive_job;
  /* The open scene's paths, kept while an additive job's result reuses the
     shared result fields, and the added scene's paths its request borrows. */
  char primary_runtime_path[1024];
  char primary_manifest_path[1024];
  char primary_edit_path[1024];
  uint64_t primary_manifest_fingerprint;
  char additive_runtime_path[1024];
  char additive_edit_path[1024];
  ProjectAddedScene added[VKR_SCENE_ADDITIVE_MAX];
  uint32_t added_next;
  ProjectSwap swap;
  uint16_t swap_container;
  uint32_t swap_scene;
  uint32_t swap_back;
  bool8_t dialog_closed;
  /* While a project is open its dialogs float over the live editor: where,
     dragged by the title, and whether they hold the keyboard (clicking
     outside hands it back). */
  Vec2 dialog_offset_pt;
  Vec2 dialog_grab_pt;
  VkrUiRect dialog_rect_px;
  ProjectView dialog_view;
  bool8_t dialog_focus;
  bool8_t settings_dirty;
  bool8_t settings_restored;
  bool8_t discard_edits;
  ProjectResume resume_action;
  ProjectBuildPreflight build_preflight;
  bool8_t closing;
  bool8_t awaiting_save;
  bool8_t rename_project;
  bool8_t delete_waiting_unload;
  /** Project chosen for deletion; its erase job runs after unpublishing. */
  char delete_project_id[37];
  char delete_project_name[513];
  bool8_t delete_project_waiting_unload;
  uint64_t delete_project_job;
  /* Scenes and Content imports at the deferred or preview texture tier
     upgrade to the final tier in a background job that yields to every other
     project job (ADR-077). A project finalize publishes its inventory only
     while the inventory still hashes as it did when the job started. */
  char finalize_scene_id[37];
  char finalize_result_path[1100];
  uint64_t finalize_job;
  uint32_t finalize_failures;
  bool8_t finalize_project;
  bool8_t finalize_running_project;
  uint8_t finalize_assets_digest[VKR_SHA256_DIGEST_SIZE];
  /* The running finalize job's ready log: its complete lines replace the
     live materials they name as they arrive (ADR-077). A scene reloaded
     meanwhile (another generation) loses them, and a finished job then
     reopens it. Visible materials are those the request prioritized. */
  char finalize_ready_path[1100];
  uint64_t finalize_ready_offset;
  uint32_t finalize_ready_applied;
  uint32_t finalize_ready_rejected;
  uint64_t finalize_scene_generation;
  float64_t finalize_ready_polled;
  float64_t finalize_started;
  float64_t finalize_first_applied;
  uint64_t finalize_visible[128];
  uint32_t finalize_visible_count;
  uint32_t finalize_visible_applied;
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

/* Previews are rendered at this size by
   tools/blender/render_default_asset_previews.py; vkr_ui_image letterboxes
   any other aspect. */
static const Vec2 project_template_preview_size = {1280.0f, 800.0f};

static void project_release_template_previews(VkrEditorProjects *projects) {
  if (!projects->template_previews_held) {
    return;
  }
  for (uint32_t i = 0; i < PROJECT_TEMPLATE_COUNT; ++i) {
    ProjectTemplatePreview *preview = &projects->template_previews[i];
    if (preview->request.request_id) {
      vkr_resource_system_unload(&preview->request,
                                 project_string(preview->request_path));
    }
    MemZero(preview, sizeof(*preview));
  }
  projects->template_previews_held = false_v;
}

/* Requests a template's preview once and returns its texture after the load
   resolves. A missing or undecodable preview leaves the card's icon. */
static VkrTextureHandle
project_template_preview(VkrEditorProjects *projects, VkrUiSystem *ui,
                         VkrEditorProjectTemplate index) {
  const VkrEditorProjectTemplateInfo *template = &project_templates[index];
  ProjectTemplatePreview *preview = &projects->template_previews[index];
  if (!template->file[0] || preview->failed) {
    return (VkrTextureHandle){0};
  }
  if (!preview->requested) {
    preview->requested = true_v;
    projects->template_previews_held = true_v;
    const int written =
        snprintf(preview->request_path, sizeof(preview->request_path),
                 "%s/assets/templates/previews/%s.png?cs=srgb&source=only",
                 vkr_content_root(), template->file);
    if (written <= 0 || (uint32_t)written >= sizeof(preview->request_path)) {
      preview->failed = true_v;
      return (VkrTextureHandle){0};
    }
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_resource_system_load(
            VKR_RESOURCE_TYPE_TEXTURE, project_string(preview->request_path),
            ui->frame_allocator, &preview->request, &error)) {
      preview->failed = true_v;
      log_warn("Cannot queue the %s template preview (%u).", template->name,
               (uint32_t)error);
      return (VkrTextureHandle){0};
    }
  }
  VkrResourceHandleInfo resolved = {0};
  if (vkr_resource_system_try_get_resolved(&preview->request, &resolved)) {
    preview->texture = resolved.as.texture;
    return preview->texture;
  }
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrResourceLoadState state =
      vkr_resource_system_get_state(&preview->request, &error);
  if (state == VKR_RESOURCE_LOAD_STATE_FAILED ||
      state == VKR_RESOURCE_LOAD_STATE_CANCELED) {
    preview->failed = true_v;
    log_warn("Cannot load the %s template preview (%u).", template->name,
             (uint32_t)error);
  }
  return (VkrTextureHandle){0};
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
      stats.size > VKR_EDITOR_PROJECT_MANIFEST_LIMIT ||
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
      project_json_bool(&writer, "labels_environment",
                        editor->labels_environment) &&
      project_json_bool(&writer, "labels_markers", editor->labels_markers) &&
      project_json_bool(&writer, "labels_empty", editor->labels_empty) &&
      project_json_bool(&writer, "console_follow",
                        editor->console.follow_tail) &&
      project_json_bool(&writer, "console_verbose",
                        log_max_level_get() >= LOG_LEVEL_DEBUG) &&
      project_json_text(&writer, "console_search",
                        (char *)editor->console.search) &&
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
    // A project created this session has no loaded document yet.
    *destinations[i] =
        (String8){.str = save->bytes + offset, .length = views[i].length};
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
  editor->labels_environment = true_v;
  editor->labels_markers = true_v;
  editor->labels_empty = true_v;
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
  (void)vkr_json_get_bool(&panels, "labels_environment",
                          &editor->labels_environment);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "labels_markers", &editor->labels_markers);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "labels_empty", &editor->labels_empty);
  panels.pos = 0;
  (void)vkr_json_get_bool(&panels, "console_follow",
                          &editor->console.follow_tail);
  panels.pos = 0;
  float32_t ui_scale = 1.0f;
  (void)vkr_json_get_float(&panels, "ui_scale", &ui_scale);
  vkr_ui_system_set_user_scale(frame->ui, ui_scale);
  panels.pos = 0;
  bool8_t reduce_motion = false_v;
  (void)vkr_json_get_bool(&panels, "reduce_motion", &reduce_motion);
  frame->ui->reduce_motion = reduce_motion;
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

/* Unsaved edits: the scene's own, and the root World's, which survives scene
   switches and so only blocks project-level transitions. */
static bool8_t project_scene_dirty(const VkrSampleUiFrame *frame) {
  return frame->edits->revision != frame->edits->saved_revision;
}

static bool8_t project_world_dirty(const VkrSampleUiFrame *frame) {
  return frame->world && frame->world_edits &&
         frame->world_edits->revision != frame->world_edits->saved_revision;
}

/* Reloads the World after its document changed, keeping added scenes and the
   view; an asked-for discard drops its unsaved edits. */
static void project_reload_world(VkrEditorProjects *projects,
                                 const VkrSampleUiFrame *frame) {
  *frame->world_request = (VkrSampleWorldRequest){
      .path = project_string(projects->world_path),
      .sidecar_path = project_string(projects->world_sidecar),
      .load = true_v,
      .reload = true_v,
      .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  projects->world_wait = 2u;
  projects->world_mesh_count = vkr_editor_project_world_meshes(
      projects->world_path, frame->ui->frame_allocator, projects->world_meshes,
      ArrayCount(projects->world_meshes));
}

static bool8_t project_any_dirty(const VkrSampleUiFrame *frame) {
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i] && frame->additive_edits[i]->revision !=
                                  frame->additive_edits[i]->saved_revision)
      return true_v;
  }
  return project_scene_dirty(frame) || project_world_dirty(frame);
}

static float32_t project_world_number(VkrJsonReader graphics, const char *key,
                                      float32_t fallback, float32_t low,
                                      float32_t high) {
  float32_t value = fallback;
  graphics.pos = 0u;
  if (!vkr_json_get_float(&graphics, key, &value) || !isfinite(value)) {
    return fallback;
  }
  return Clamp(value, low, high);
}

/* One named World entity carrying a single live component. */
/* A new document-stable entity id (ADR-076). */
static bool8_t project_world_id(VkrJsonWriter *writer) {
  char id[37];
  VkrEditorProjectError error;
  return vkr_editor_project_id_generate(id, &error) &&
         vkr_json_writer_name(writer, string8_lit("id")) &&
         vkr_json_writer_string(writer, string8_create((uint8_t *)id, 36));
}

static bool8_t project_world_entity(VkrJsonWriter *writer, String8 name,
                                    const VkrTypeDesc *type,
                                    const void *value) {
  return vkr_json_writer_begin_object(writer) && project_world_id(writer) &&
         vkr_json_writer_name(writer, string8_lit("name")) &&
         vkr_json_writer_string(writer, name) &&
         vkr_json_writer_name(writer, string8_lit("components")) &&
         vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(
             writer, string8_create_from_cstr((const uint8_t *)type->name,
                                              strlen(type->name))) &&
         vkr_type_write_json(writer, type, value) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_end_object(writer);
}

/* The sun: a directional light that drives the atmosphere, placed above the
   grid so its icon is in view. */
static bool8_t project_world_sun(VkrJsonWriter *writer, Vec3 toward_sun) {
  return vkr_json_writer_begin_object(writer) && project_world_id(writer) &&
         vkr_json_writer_name(writer, string8_lit("name")) &&
         vkr_json_writer_string(writer, string8_lit("Directional Light")) &&
         vkr_json_writer_name(writer, string8_lit("parent")) &&
         vkr_json_writer_null(writer) &&
         vkr_json_writer_name(writer, string8_lit("transform")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_vec3(writer, "pos", vec3_new(0.0f, 3.0f, 0.0f)) &&
         vkr_json_writer_name(writer, string8_lit("rot")) &&
         vkr_json_writer_begin_array(writer) &&
         vkr_json_writer_f64(writer, 0) && vkr_json_writer_f64(writer, 0) &&
         vkr_json_writer_f64(writer, 0) && vkr_json_writer_f64(writer, 1) &&
         vkr_json_writer_end_array(writer) &&
         project_json_vec3(writer, "scale", vec3_one()) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("directional_light")) &&
         vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("enabled")) &&
         vkr_json_writer_bool(writer, true_v) &&
         project_json_number(writer, "intensity", 3.0) &&
         vkr_json_writer_name(writer, string8_lit("atmosphere_sun")) &&
         vkr_json_writer_bool(writer, true_v) &&
         project_json_vec3(writer, "direction_local",
                           vec3_scale(toward_sun, -1.0f)) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_end_object(writer);
}

/* Root World document beside project.json (ADR-076). A new project's World
   is a blank level: a sun, sky light, sky atmosphere, clouds, height fog and
   post process. Its grading carries the values older editors stored in the
   machine graphics preferences. */
static bool8_t project_world_prepare(VkrEditorProjects *projects) {
  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&projects->project_allocator);
  const String8 directory =
      file_path_get_directory(&projects->project_allocator,
                              project_string(projects->project->manifest_path));
  const bool8_t named =
      directory.length &&
      snprintf(projects->world_path, sizeof(projects->world_path),
               "%.*s/world.scene.json", (int)directory.length,
               directory.str) < (int)sizeof(projects->world_path) &&
      snprintf(projects->world_sidecar, sizeof(projects->world_sidecar),
               "%.*s/world.editor.json", (int)directory.length,
               directory.str) < (int)sizeof(projects->world_sidecar);
  if (vkr_allocator_scope_is_valid(&scope)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (!named) {
    return false_v;
  }
  const FilePath document = {.path = project_string(projects->world_path),
                             .type = FILE_PATH_TYPE_ABSOLUTE};
  if (file_exists(&document)) {
    return true_v;
  }

  ScenePostProcess post = vkr_scene_post_process_defaults();
  const String8 graphics =
      project_member(projects->project->editor_settings, "graphics");
  if (graphics.length) {
    const VkrJsonReader reader = vkr_json_reader_from_string(graphics);
    post.exposure_compensation_ev =
        project_world_number(reader, "brightness", 0.0f, -6.0f, 6.0f);
    post.contrast = project_world_number(reader, "contrast", 1.0f, 0.5f, 1.5f);
    post.saturation =
        project_world_number(reader, "saturation", 1.0f, 0.0f, 1.5f);
    post.white_balance_temperature =
        project_world_number(reader, "temperature", 0.0f, -1.0f, 1.0f);
    post.white_balance_tint =
        project_world_number(reader, "tint", 0.0f, -1.0f, 1.0f);
    post.sharpness =
        project_world_number(reader, "sharpness", post.sharpness, 0.0f, 1.0f);
    post.bloom_intensity = project_world_number(
        reader, "bloom_intensity", post.bloom_intensity, 0.0f, 1.0f);
    post.motion_blur_shutter_angle =
        project_world_number(reader, "motion_blur_amount", 0.5f, 0.0f, 1.0f) *
        360.0f;
  }
  const Vec3 toward_sun = vec3_normalize(vec3_new(0.35f, 0.55f, 0.75f));
  VkrAtmosphereSettings atmosphere;
  vkr_type_defaults(&vkr_scene_atmosphere_type, &atmosphere);
  /* The Directional Light is the sky's sun (ADR-058). */
  atmosphere.enabled = true_v;
  _Alignas(16) uint8_t clouds[VKR_TYPE_VALUE_MAX];
  vkr_type_defaults(&vkr_scene_clouds_type, clouds);
  _Alignas(16) uint8_t fog[VKR_TYPE_VALUE_MAX];
  vkr_type_defaults(&vkr_scene_fog_type, fog);
  const uint32_t clouds_enabled =
      vkr_type_find_property(&vkr_scene_clouds_type, string8_lit("enabled"));
  const uint32_t fog_enabled =
      vkr_type_find_property(&vkr_scene_fog_type, string8_lit("enabled"));
  const uint32_t fog_density =
      vkr_type_find_property(&vkr_scene_fog_type, string8_lit("density"));
  if (clouds_enabled != UINT32_MAX) {
    (void)vkr_property_set_number(
        &vkr_scene_clouds_type.properties[clouds_enabled], clouds, 1.0);
  }
  if (fog_enabled != UINT32_MAX && fog_density != UINT32_MAX) {
    (void)vkr_property_set_number(&vkr_scene_fog_type.properties[fog_enabled],
                                  fog, 1.0);
    (void)vkr_property_set_number(&vkr_scene_fog_type.properties[fog_density],
                                  fog, 0.002);
  }

  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, document.path)) {
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  const bool8_t written =
      vkr_json_writer_begin_object(writer) &&
      vkr_json_writer_name(writer, string8_lit("version")) &&
      vkr_json_writer_u64(writer, 2u) &&
      vkr_json_writer_name(writer, string8_lit("environment")) &&
      vkr_json_writer_begin_object(writer) &&
      vkr_json_writer_name(writer, string8_lit("enabled")) &&
      vkr_json_writer_bool(writer, true_v) &&
      vkr_json_writer_end_object(writer) &&
      vkr_json_writer_name(writer, string8_lit("entities")) &&
      vkr_json_writer_begin_array(writer) &&
      project_world_sun(writer, toward_sun) &&
      project_world_entity(writer, string8_lit("Sky Atmosphere"),
                           &vkr_scene_atmosphere_type, &atmosphere) &&
      project_world_entity(writer, string8_lit("Volumetric Clouds"),
                           &vkr_scene_clouds_type, clouds) &&
      project_world_entity(writer, string8_lit("Height Fog"),
                           &vkr_scene_fog_type, fog) &&
      project_world_entity(writer, string8_lit("Post Process"),
                           &vkr_scene_post_process_type, &post) &&
      vkr_json_writer_end_array(writer) && vkr_json_writer_end_object(writer);
  if (!written || !vkr_json_file_writer_commit(&file)) {
    vkr_json_file_writer_abort(&file);
    return false_v;
  }
  return true_v;
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
  /* Script modules build and load before the World and scenes load, so
     their component types register first (ADR-079). */
  char directory[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char scripts_directory[VKR_EDITOR_PROJECT_PATH_CAPACITY + 16u];
  char scripts_output[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  vkr_editor_code_close_saved(editor->code);
  if (vkr_editor_project_directory(candidate, directory) &&
      snprintf(scripts_directory, sizeof(scripts_directory), "%s/Scripts",
               directory) < (int32_t)sizeof(scripts_directory) &&
      snprintf(scripts_output, sizeof(scripts_output), "%s/scripts/%s",
               projects->workspace.root,
               candidate->id) < (int32_t)sizeof(scripts_output)) {
    vkr_editor_scripts_open(editor->scripts, scripts_directory, scripts_output,
                            vkr_editor_bakery_service(editor->bakery), frame);
  }
  if (project_world_prepare(projects)) {
    /* World models follow rebuilds published since the World last saved. */
    uint32_t moved = 0u;
    if (candidate->assets.length &&
        !vkr_editor_project_world_refresh(
            projects->world_path, candidate->assets, frame->ui->frame_allocator,
            &moved, &error)) {
      log_warn("Project: World models keep their paths: %s", error.message);
    }
    *frame->world_request = (VkrSampleWorldRequest){
        .path = project_string(projects->world_path),
        .sidecar_path = project_string(projects->world_sidecar),
        .load = true_v,
        .discard_edits = projects->discard_edits};
    projects->world_wait = 2u;
    projects->world_mesh_count = vkr_editor_project_world_meshes(
        projects->world_path, frame->ui->frame_allocator,
        projects->world_meshes, ArrayCount(projects->world_meshes));
  } else {
    snprintf(projects->message, sizeof(projects->message),
             "The project World could not be prepared.");
  }
  projects->discard_edits = false_v;
  /* Projects open on their World, like a blank level; scenes open from
     Content or the Scenes list. */
  projects->view = PROJECT_VIEW_EDITOR;
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
  projects->include_edits = true_v;
  projects->use_placeholders = false_v;
  projects->inspected_source[0] = '\0';
  projects->inspection = (ProjectInspection){0};
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
  projects->job_imports_models = false_v;
  projects->job_world_models = false_v;
  projects->message[0] = '\0';
}

/* A model entity referencing an existing mesh asset at its drop point, or
   a prefab instance of a project scene there. */
static bool8_t project_write_placed_asset(const VkrEditorProjects *projects,
                                          VkrJsonWriter *writer) {
  if (!vkr_json_writer_begin_object(writer)) {
    return false_v;
  }
  const bool8_t reference =
      projects->place_prefab
          ? project_json_text(writer, "scene_id", projects->place_asset)
          : vkr_json_writer_name(writer, string8_lit("asset")) &&
                vkr_json_writer_begin_object(writer) &&
                project_json_text(writer, "scope", projects->place_scope) &&
                project_json_text(writer, "id", projects->place_asset) &&
                vkr_json_writer_end_object(writer);
  return reference && project_json_text(writer, "name", projects->place_name) &&
         vkr_json_writer_name(writer, string8_lit("transform")) &&
         vkr_json_writer_begin_object(writer) &&
         project_json_vec3(writer, "pos", projects->place_position) &&
         vkr_json_writer_name(writer, string8_lit("rot")) &&
         vkr_json_writer_begin_array(writer) &&
         vkr_json_writer_f64(writer, 0.0) && vkr_json_writer_f64(writer, 0.0) &&
         vkr_json_writer_f64(writer, 0.0) && vkr_json_writer_f64(writer, 1.0) &&
         vkr_json_writer_end_array(writer) &&
         project_json_vec3(writer, "scale", vec3_new(1.0f, 1.0f, 1.0f)) &&
         vkr_json_writer_end_object(writer) &&
         vkr_json_writer_end_object(writer);
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
      /* Scene and Content imports open with untextured materials at once
         and a background finalize job adds full-quality textures. Textures
         only the editor shows encode with the fast encoder where the host
         has one. */
      project_json_text(writer, "texture_tier", "deferred") &&
      project_json_text(writer, "texture_encode_speed", "fast") &&
      project_json_text(writer, "legacy_root",
                        projects->legacy_root[0] ? projects->legacy_root
                                                 : vkr_content_root()) &&
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
         project_json_bool(writer, "include_edits", projects->include_edits) &&
         project_json_bool(writer, "use_placeholders",
                           projects->use_placeholders) &&
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
  } else if (!strcmp(projects->operation, "import_project_assets")) {
    /* Project assets belong to no scene. */
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
  if (!strcmp(projects->operation, "inspect_scene")) {
    ok =
        ok && project_json_text(writer, "source_scene", projects->source_scene);
  }
  if (!strcmp(projects->operation, "add_entities")) {
    ok = ok && vkr_json_writer_name(writer, string8_lit("models")) &&
         vkr_json_writer_begin_array(writer);
    if (projects->placing_asset && !projects->place_prefab) {
      ok = ok && project_write_placed_asset(projects, writer);
    } else if (projects->adding_model) {
      /* The Add entity form names one model; an import names each file. */
      const uint32_t count = Max(projects->model_count, 1u);
      for (uint32_t i = 0; ok && i < count; ++i) {
        ok =
            vkr_json_writer_string(writer, project_string(projects->models[i]));
      }
    }
    ok = ok && vkr_json_writer_end_array(writer) &&
         vkr_json_writer_name(writer, string8_lit("lights")) &&
         project_write_lights(projects, writer);
    if (projects->placing_asset && projects->place_prefab) {
      ok = ok && vkr_json_writer_name(writer, string8_lit("prefabs")) &&
           vkr_json_writer_begin_array(writer) &&
           project_write_placed_asset(projects, writer) &&
           vkr_json_writer_end_array(writer);
    }
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
    if (projects->import_source_count) {
      for (uint32_t i = 0; ok && i < projects->import_source_count; ++i) {
        ok = vkr_json_writer_string(
            writer, project_string(projects->import_sources[i]));
      }
    } else if (projects->action_source[0]) {
      ok = ok && vkr_json_writer_string(
                     writer, project_string(projects->action_source));
    }
    ok = ok && vkr_json_writer_end_array(writer);
  }
  ok = ok && vkr_json_writer_name(writer, string8_lit("tools")) &&
       vkr_json_writer_begin_object(writer) &&
       /* Every cooker is a `vkr_bakery tool`; the job maps keys to tools. */
       project_json_text(writer, "mesh",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "animation",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "collision",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "font",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "texture",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "harness",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_HARNESS)) &&
       project_json_text(writer, "hdr_packer",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
       project_json_text(writer, "diffuse",
                         vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY)) &&
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
  projects->job_project_assets =
      !strcmp(projects->operation, "import_project_assets");
  projects->job_inspect = !strcmp(projects->operation, "inspect_scene");
  return true_v;
}

// =============================================================================
// Background texture finalization
// =============================================================================

static void project_start_job(VkrEditorProjects *projects, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, bool8_t create);

/* Adopts a job result's project asset inventory and default font; the
   caller saves the project. */
/* Retains `assets` as the project inventory the next save publishes. */
static bool8_t project_replace_assets(VkrEditorProjects *projects,
                                      String8 assets) {
  if (string8_equals(&assets, &projects->project->assets)) {
    return true_v;
  }
  String8 replacement = string8_duplicate(projects->allocator, &assets);
  if (!replacement.str) {
    snprintf(projects->message, sizeof(projects->message),
             "Cannot retain project asset inventory.");
    return false_v;
  }
  if (projects->owned_assets.str) {
    vkr_allocator_free(projects->allocator, projects->owned_assets.str,
                       projects->owned_assets.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  projects->owned_assets = replacement;
  projects->project->assets = replacement;
  return true_v;
}

static bool8_t project_apply_inventory(VkrEditorProjects *projects,
                                       String8 result) {
  String8 assets = project_member(result, "project_assets");
  String8 default_font = project_member(result, "default_font");
  if (!assets.length || !default_font.length) {
    return true_v;
  }
  if (!project_replace_assets(projects, assets)) {
    return false_v;
  }
  if (!string8_equals(&default_font, &projects->project->default_font)) {
    String8 replacement = string8_duplicate(projects->allocator, &default_font);
    if (!replacement.str) {
      snprintf(projects->message, sizeof(projects->message),
               "Cannot retain project font setting.");
      return false_v;
    }
    if (projects->owned_default_font.str) {
      vkr_allocator_free(projects->allocator, projects->owned_default_font.str,
                         projects->owned_default_font.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    projects->owned_default_font = replacement;
    projects->project->default_font = replacement;
  }
  return true_v;
}

/* A published scene still holding deferred or preview textures queues its
   finalize job; the most recent scene wins. */
static void project_schedule_finalize(VkrEditorProjects *projects,
                                      String8 result) {
  VkrJsonReader reader = vkr_json_reader_from_string(result);
  int32_t preview = 0;
  char scene_id[37] = {0};
  VkrEditorProjectError error = {0};
  if (!vkr_json_get_int(&reader, "preview_assets", &preview) || preview <= 0 ||
      !vkr_editor_project_json_string(result, "scene_id", scene_id,
                                      sizeof(scene_id), &error)) {
    return;
  }
  if (strcmp(scene_id, projects->finalize_scene_id)) {
    projects->finalize_failures = 0u;
  }
  snprintf(projects->finalize_scene_id, sizeof(projects->finalize_scene_id),
           "%s", scene_id);
}

/* A user's job runs first: a running finalize job is cancelled, which
   publishes nothing, and retried once the editor is idle again. */
static void project_yield_finalize(VkrEditorProjects *projects,
                                   VkrEditorUi *editor) {
  if (!projects->finalize_job) {
    return;
  }
  vkr_editor_bakery_project_cancel(editor->bakery, projects->finalize_job);
  projects->finalize_job = 0;
}

static uint32_t project_scene_index(const VkrEditorProjects *projects,
                                    const char *scene_id) {
  for (uint32_t i = 0; projects->project && i < projects->project->scene_count;
       ++i) {
    if (!strcmp(projects->project->scenes[i].id, scene_id)) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* Adds each material of a visible instance to `coverage`, indexed by
   material slot, as the share of the Scene view its submesh's projected
   bounds cover; occlusion is not considered. */
static void project_material_coverage(const VkrSampleUiFrame *frame,
                                      float32_t *coverage, uint32_t capacity) {
  VkrMeshManager *meshes = &frame->assets->mesh_manager;
  const uint32_t instance_count = vkr_mesh_manager_instance_count(meshes);
  for (uint32_t live = 0; live < instance_count; ++live) {
    uint32_t slot = 0;
    VkrMeshInstance *instance =
        vkr_mesh_manager_get_instance_by_live_index(meshes, live, &slot);
    VkrMeshAsset *asset =
        instance && instance->visible
            ? vkr_mesh_manager_get_live_asset(meshes, instance->asset)
            : NULL;
    if (!asset) {
      continue;
    }
    const Mat4 clip = mat4_mul(frame->view_projection, instance->model);
    for (uint64_t s = 0; s < asset->submeshes.length; ++s) {
      const VkrMeshAssetSubmesh *submesh = &asset->submeshes.data[s];
      if (submesh->material.id == 0u || submesh->material.id > capacity) {
        continue;
      }
      float32_t min_x = 1.0f;
      float32_t min_y = 1.0f;
      float32_t max_x = -1.0f;
      float32_t max_y = -1.0f;
      bool8_t surrounds = false_v;
      for (uint32_t corner = 0; corner < 8u && !surrounds; ++corner) {
        const Vec4 point = mat4_mul_vec4(
            clip,
            vec4_new(
                corner & 1u ? submesh->max_extents.x : submesh->min_extents.x,
                corner & 2u ? submesh->max_extents.y : submesh->min_extents.y,
                corner & 4u ? submesh->max_extents.z : submesh->min_extents.z,
                1.0f));
        if (point.w <= 1e-4f) {
          /* A corner behind the camera: the bounds surround the view. */
          surrounds = true_v;
          break;
        }
        min_x = Min(min_x, point.x / point.w);
        min_y = Min(min_y, point.y / point.w);
        max_x = Max(max_x, point.x / point.w);
        max_y = Max(max_y, point.y / point.w);
      }
      const float32_t width =
          surrounds ? 2.0f
                    : Clamp(max_x, -1.0f, 1.0f) - Clamp(min_x, -1.0f, 1.0f);
      const float32_t height =
          surrounds ? 2.0f
                    : Clamp(max_y, -1.0f, 1.0f) - Clamp(min_y, -1.0f, 1.0f);
      if (width > 0.0f && height > 0.0f) {
        coverage[submesh->material.id - 1u] += width * height * 0.25f;
      }
    }
  }
}

/* Writes the finalize request fields that make its results progressive: the
   ready log the job appends finished materials to, and the materials of the
   open scene ordered by their share of the Scene view, which the cook
   writes first (ADR-077). */
static bool8_t project_write_progressive_fields(VkrEditorProjects *projects,
                                                const VkrSampleUiFrame *frame,
                                                VkrJsonWriter *writer,
                                                const char *job_directory) {
  snprintf(projects->finalize_ready_path, sizeof(projects->finalize_ready_path),
           "%s/ready.jsonl", job_directory);
  projects->finalize_ready_offset = 0u;
  projects->finalize_ready_applied = 0u;
  projects->finalize_ready_rejected = 0u;
  projects->finalize_ready_polled = 0.0;
  projects->finalize_scene_generation = frame->scene_generation;
  projects->finalize_started = vkr_platform_get_absolute_time();
  projects->finalize_first_applied = 0.0;
  projects->finalize_visible_count = 0u;
  projects->finalize_visible_applied = 0u;
  MemZero(projects->finalize_visible, sizeof(projects->finalize_visible));

  VkrMaterialSystem *materials = &frame->assets->material_system;
  const uint32_t capacity = (uint32_t)materials->materials.length;
  VkrAllocator *allocator = frame->ui->frame_allocator;
  float32_t *coverage = vkr_allocator_alloc(
      allocator, sizeof(*coverage) * capacity, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *order = vkr_allocator_alloc(allocator, sizeof(*order) * capacity,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t visible = 0u;
  if (coverage && order) {
    MemZero(coverage, sizeof(*coverage) * capacity);
    project_material_coverage(frame, coverage, capacity);
    for (uint32_t i = 0u; i < capacity; ++i) {
      if (coverage[i] > 0.0f && materials->materials.data[i].name) {
        order[visible++] = i;
        if (i < 64u * ArrayCount(projects->finalize_visible)) {
          projects->finalize_visible[i / 64u] |= 1ull << (i % 64u);
          projects->finalize_visible_count++;
        }
      }
    }
    /* Insertion sort, largest share first; a scene has a few hundred. */
    for (uint32_t i = 1u; i < visible; ++i) {
      const uint32_t key = order[i];
      uint32_t j = i;
      while (j > 0u && coverage[order[j - 1u]] < coverage[key]) {
        order[j] = order[j - 1u];
        --j;
      }
      order[j] = key;
    }
  }
  if (visible) {
    log_info("Textures: %u materials in view are encoded first", visible);
  }
  bool8_t ok =
      project_json_text(writer, "ready_log", projects->finalize_ready_path) &&
      vkr_json_writer_name(writer, project_string("material_priority")) &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0u; ok && i < visible; ++i) {
    ok = vkr_json_writer_string(
        writer, project_string(materials->materials.data[order[i]].name));
  }
  return ok && vkr_json_writer_end_array(writer);
}

/* Applies the complete lines the finalize job has added to its ready log
   since the last call: each replaces the live material it names, which
   publishes once its textures load. A material that is not live is not
   shown, so it is skipped. `drain` reads to the end; otherwise at most ten
   times a second. */
static void project_apply_ready(VkrEditorProjects *projects,
                                const VkrSampleUiFrame *frame, bool8_t drain) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (!projects->finalize_ready_path[0] ||
      (!drain && now < projects->finalize_ready_polled + 0.1)) {
    return;
  }
  projects->finalize_ready_polled = now;
  FILE *log = file_fopen(projects->finalize_ready_path, "rb");
  if (!log) {
    return;
  }
  VkrAllocator *allocator = frame->ui->frame_allocator;
  enum { ProjectReadyChunk = 1 << 20 };
  uint8_t *chunk = vkr_allocator_alloc(allocator, ProjectReadyChunk,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  bool8_t more = chunk != NULL;
  while (more &&
         fseek(log, (long)projects->finalize_ready_offset, SEEK_SET) == 0) {
    const size_t read = fread(chunk, 1u, ProjectReadyChunk, log);
    uint64_t consumed = 0u;
    for (uint64_t end = 0u; end < read; ++end) {
      if (chunk[end] != '\n') {
        continue;
      }
      const String8 line = {.str = chunk + consumed, .length = end - consumed};
      consumed = end + 1u;
      VkrAllocatorScope scope = vkr_allocator_begin_scope(allocator);
      String8 name = {0};
      String8 path = {0};
      String8 definition = {0};
      VkrJsonReader name_reader = vkr_json_reader_from_string(line);
      VkrJsonReader path_reader = vkr_json_reader_from_string(line);
      VkrJsonReader definition_reader = vkr_json_reader_from_string(line);
      if (!vkr_json_find_field(&name_reader, "material") ||
          !vkr_json_parse_string_decoded(&name_reader, allocator, &name) ||
          !vkr_json_find_field(&path_reader, "path") ||
          !vkr_json_parse_string_decoded(&path_reader, allocator, &path) ||
          !vkr_json_find_field(&definition_reader, "definition") ||
          !vkr_json_parse_string_decoded(&definition_reader, allocator,
                                         &definition)) {
        log_warn("Textures: skipping an unreadable finished-material record");
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
        continue;
      }
      VkrRendererError error = VKR_RENDERER_ERROR_NONE;
      if (vkr_material_loader_replace_live(&frame->assets->material_system,
                                           path, definition, allocator,
                                           &error)) {
        if (!projects->finalize_ready_applied++) {
          projects->finalize_first_applied = now;
        }
        const VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
            &frame->assets->material_system.material_by_name,
            (const char *)name.str);
        const bool8_t visible =
            entry && entry->id < 64u * ArrayCount(projects->finalize_visible) &&
            (projects->finalize_visible[entry->id / 64u] &
             (1ull << (entry->id % 64u)));
        const uint32_t shown =
            visible ? ++projects->finalize_visible_applied : 0u;
        if (visible && (shown == (projects->finalize_visible_count + 1u) / 2u ||
                        shown == projects->finalize_visible_count)) {
          log_info("Textures: %u of %u materials in view applied after %.1f s",
                   shown, projects->finalize_visible_count,
                   now - projects->finalize_started);
        }
      } else if (error != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED &&
                 !projects->finalize_ready_rejected++) {
        log_warn("Textures: a finished material could not be applied; the "
                 "scene reopens when the textures are ready");
      }
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    projects->finalize_ready_offset += consumed;
    /* A full chunk may hold more complete lines; a partial last line waits
       for the rest. */
    more = drain && read == ProjectReadyChunk && consumed > 0u;
  }
  fclose(log);
}

/* A finished finalize whose every material the open scene uses was applied
   as it arrived needs no reopen: the scene shows what a reopen would load.
   The records then cover every material the job rebuilt, since the job
   records those the cook did not. */
static bool8_t project_finalize_applied(const VkrEditorProjects *projects,
                                        const VkrSampleUiFrame *frame) {
  return projects->finalize_ready_applied > 0u &&
         projects->finalize_ready_rejected == 0u &&
         projects->finalize_scene_generation == frame->scene_generation;
}

/* Logs how the progressive finalize went, for scripted measurements. */
static void project_log_finalize_timing(const VkrEditorProjects *projects) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (projects->finalize_first_applied > 0.0) {
    log_info("Textures: %u materials applied; first after %.1f s, all after "
             "%.1f s",
             projects->finalize_ready_applied,
             projects->finalize_first_applied - projects->finalize_started,
             now - projects->finalize_started);
  }
}

void vkr_editor_projects_finalize_stats(const VkrEditorProjects *projects,
                                        bool8_t *out_running,
                                        uint32_t *out_applied) {
  *out_running = projects && projects->finalize_job != 0u;
  *out_applied = projects ? projects->finalize_ready_applied : 0u;
}

static uint64_t project_start_finalize(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame) {
  const uint32_t scene =
      project_scene_index(projects, projects->finalize_scene_id);
  VkrEditorProjectError error = {0};
  char scene_path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char job_id[37];
  if (scene == UINT32_MAX ||
      !vkr_editor_project_scene_path(projects->project, scene, true_v,
                                     scene_path, &error) ||
      !vkr_editor_project_id_generate(job_id, &error)) {
    projects->finalize_scene_id[0] = '\0';
    return 0;
  }
  char job_directory[1024];
  char request_path[1100];
  snprintf(job_directory, sizeof(job_directory), "%s/jobs/%s",
           projects->workspace.root, job_id);
  snprintf(request_path, sizeof(request_path), "%s/request.json",
           job_directory);
  snprintf(projects->finalize_result_path,
           sizeof(projects->finalize_result_path), "%s/result.json",
           job_directory);
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
      project_json_text(writer, "operation", "finalize_textures") &&
      project_json_text(writer, "texture_tier", "final") &&
      project_json_text(writer, "texture_encode_speed", "fast") &&
      project_json_text(writer, "workspace_root", projects->workspace.root) &&
      project_json_text(writer, "project_path",
                        projects->project->manifest_path) &&
      project_json_text(writer, "legacy_root",
                        projects->legacy_root[0] ? projects->legacy_root
                                                 : vkr_content_root()) &&
      project_json_text(writer, "runtime_directory", job_directory) &&
      project_json_text(writer, "scene_id",
                        projects->project->scenes[scene].id) &&
      project_json_text(writer, "scene_path", scene_path) &&
      project_write_progressive_fields(projects, frame, writer,
                                       job_directory) &&
      vkr_json_writer_end_object(writer) && vkr_json_file_writer_commit(&file);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    return 0;
  }
  return vkr_editor_bakery_project_start(editor->bakery, request_path,
                                         projects->finalize_result_path);
}

/* Rebuilds the project's deferred and preview Content imports at the final
   tier. The result carries the new inventory, published only while the
   inventory it started from is unchanged. */
static uint64_t project_start_project_finalize(VkrEditorProjects *projects,
                                               VkrEditorUi *editor,
                                               const VkrSampleUiFrame *frame) {
  VkrEditorProjectError error = {0};
  char job_id[37];
  if (!vkr_editor_project_id_generate(job_id, &error)) {
    return 0;
  }
  char job_directory[1024];
  char request_path[1100];
  snprintf(job_directory, sizeof(job_directory), "%s/jobs/%s",
           projects->workspace.root, job_id);
  snprintf(request_path, sizeof(request_path), "%s/request.json",
           job_directory);
  snprintf(projects->finalize_result_path,
           sizeof(projects->finalize_result_path), "%s/result.json",
           job_directory);
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
      project_json_text(writer, "operation", "finalize_project_assets") &&
      project_json_text(writer, "texture_tier", "final") &&
      project_json_text(writer, "texture_encode_speed", "fast") &&
      project_json_text(writer, "workspace_root", projects->workspace.root) &&
      project_json_text(writer, "project_path",
                        projects->project->manifest_path) &&
      project_json_text(writer, "legacy_root",
                        projects->legacy_root[0] ? projects->legacy_root
                                                 : vkr_content_root()) &&
      project_json_text(writer, "runtime_directory", job_directory) &&
      project_write_progressive_fields(projects, frame, writer,
                                       job_directory) &&
      vkr_json_writer_end_object(writer) && vkr_json_file_writer_commit(&file);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    return 0;
  }
  vkr_sha256(projects->project->assets.str, projects->project->assets.length,
             projects->finalize_assets_digest);
  return vkr_editor_bakery_project_start(editor->bakery, request_path,
                                         projects->finalize_result_path);
}

/* A finished project finalize: the new inventory replaces the one it was
   built from, and an unchanged open scene naming a finalized asset reopens.
   An inventory changed meanwhile keeps the job pending for a retry. */
static void project_finalize_project_complete(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame) {
  String8 result = {0};
  VkrEditorProjectError error = {0};
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256(projects->project->assets.str, projects->project->assets.length,
             digest);
  if (MemCompare(digest, projects->finalize_assets_digest, sizeof(digest))) {
    log_info("Textures: Content changed while encoding; retrying");
    return;
  }
  if (!project_read_file(projects->finalize_result_path,
                         frame->ui->frame_allocator, &result) ||
      !project_apply_inventory(projects, result) ||
      !vkr_editor_project_save(projects->project, &error)) {
    log_warn("Textures: cannot publish the finalized Content: %s",
             error.message[0] ? error.message : projects->message);
    return;
  }
  projects->finalize_project = false_v;
  vkr_editor_content_refresh(editor->content);

  /* World models name the rebuilt revisions from now on (ADR-076). Their
     materials were replaced live as they finished; otherwise a clean World
     reloads to show them. */
  uint32_t moved = 0u;
  if (projects->world_path[0] &&
      !vkr_editor_project_world_refresh(
          projects->world_path, projects->project->assets,
          frame->ui->frame_allocator, &moved, &error)) {
    log_warn("Textures: cannot point World models at the final textures: %s",
             error.message);
  }
  const bool8_t applied_live = projects->finalize_ready_applied > 0u &&
                               projects->finalize_ready_rejected == 0u;
  if (moved && !applied_live && !project_world_dirty(frame)) {
    project_reload_world(projects, frame);
  }

  /* Reopen only a clean open scene that names a finalized asset. */
  String8 scene = {0};
  bool8_t uses = false_v;
  if (projects->scene_manifest_path[0] &&
      project_read_file(projects->scene_manifest_path,
                        frame->ui->frame_allocator, &scene)) {
    VkrJsonReader reader = vkr_json_reader_from_string(result);
    if (vkr_json_find_array(&reader, "finalized_assets")) {
      while (!uses && vkr_json_next_array_element(&reader)) {
        String8 id = {0};
        uses = vkr_json_parse_string(&reader, &id) && id.length &&
               string8_contains(&scene, &id);
      }
    }
  }
  if (!uses) {
    log_info("Textures: full quality is ready for the imported Content");
    return;
  }
  if (project_finalize_applied(projects, frame)) {
    log_info("Textures: full quality is applied to the open scene");
    return;
  }
  if (!project_scene_dirty(frame) && !projects->job_id &&
      !projects->waiting_activation && !frame->scene_loading &&
      projects->project &&
      projects->active_scene < projects->project->scene_count) {
    log_info("Textures: full quality is ready; reopening the scene");
    projects->pending_scene = projects->active_scene;
    projects->operation[0] = '\0';
    project_start_job(projects, editor, frame, false_v);
    return;
  }
  log_info("Textures: full quality is ready; it loads the next time the "
           "scene opens");
}

/* A finished finalize job: an unchanged open scene reopens with its full
   quality textures; otherwise they load the next time the scene opens and
   later saves build on the new publication. */
static void project_finalize_complete(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  String8 result = {0};
  VkrEditorProjectError error = {0};
  char scene_id[37] = {0};
  char fingerprint[17] = {0};
  if (!project_read_file(projects->finalize_result_path,
                         frame->ui->frame_allocator, &result) ||
      !vkr_editor_project_json_string(result, "scene_id", scene_id,
                                      sizeof(scene_id), &error) ||
      !vkr_editor_project_json_string(result, "manifest_fingerprint",
                                      fingerprint, sizeof(fingerprint),
                                      &error)) {
    log_warn("Textures: cannot read the finalize result: %s", error.message);
    return;
  }
  if (!strcmp(projects->finalize_scene_id, scene_id)) {
    projects->finalize_scene_id[0] = '\0';
  }
  const bool8_t active =
      projects->project &&
      projects->active_scene < projects->project->scene_count &&
      !strcmp(projects->project->scenes[projects->active_scene].id, scene_id);
  if (!active) {
    log_info("Textures: full quality is ready for a scene that is not open");
    return;
  }
  if (project_finalize_applied(projects, frame)) {
    /* The open scene already shows the published revision's materials, so
       it adopts the revision as a reopen would, without reloading. */
    projects->scene_manifest_fingerprint = strtoull(fingerprint, NULL, 16);
    log_info("Textures: full quality is applied to the open scene");
    return;
  }
  if (!project_scene_dirty(frame) && !projects->job_id &&
      !projects->waiting_activation && !frame->scene_loading) {
    log_info("Textures: full quality is ready; reopening the scene");
    projects->pending_scene = projects->active_scene;
    projects->operation[0] = '\0';
    project_start_job(projects, editor, frame, false_v);
    return;
  }
  projects->scene_manifest_fingerprint = strtoull(fingerprint, NULL, 16);
  log_info("Textures: full quality is ready; it loads the next time the "
           "scene opens");
}

static void project_update_finalize(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  if (projects->finalize_job) {
    const VkrEditorProjectJobStatus status = vkr_editor_bakery_project_status(
        editor->bakery, projects->finalize_job, NULL);
    if (status == VKR_EDITOR_PROJECT_JOB_QUEUED ||
        status == VKR_EDITOR_PROJECT_JOB_RUNNING) {
      project_apply_ready(projects, frame, false_v);
      return;
    }
    /* Materials already applied stay: their textures are final either way,
       and a retry or the next open brings the rest. */
    project_apply_ready(projects, frame, true_v);
    projects->finalize_job = 0;
    if (status == VKR_EDITOR_PROJECT_JOB_SUCCEEDED) {
      project_log_finalize_timing(projects);
      if (projects->finalize_running_project) {
        project_finalize_project_complete(projects, editor, frame);
      } else {
        project_finalize_complete(projects, editor, frame);
      }
    } else if (status == VKR_EDITOR_PROJECT_JOB_FAILED) {
      /* A save while it ran changes the scene under it; that retries. */
      String8 result = {0};
      const bool8_t raced =
          project_read_file(projects->finalize_result_path,
                            frame->ui->frame_allocator, &result) &&
          strstr((const char *)result.str, "Scene changed") != NULL;
      if (!raced && ++projects->finalize_failures >= 2u) {
        log_warn("Textures: finalizing full-quality textures failed twice; "
                 "Rebuild the model to retry");
        if (projects->finalize_running_project) {
          projects->finalize_project = false_v;
        } else {
          projects->finalize_scene_id[0] = '\0';
        }
      }
    }
    return;
  }
  /* A Content import left at the deferred or preview tier, including one a
     previous session did not finish, still awaits its final textures. */
  if (!projects->finalize_project && projects->finalize_failures < 2u &&
      projects->project && projects->project->assets.length) {
    const String8 marker = string8_lit("\"texture_tier\"");
    projects->finalize_project =
        string8_contains(&projects->project->assets, &marker);
  }
  /* Start only while the editor is idle: no job, activation or load. A World
     still loading would miss the materials finished before it is live. */
  if ((!projects->finalize_scene_id[0] && !projects->finalize_project) ||
      projects->read_only || !projects->project || projects->job_id ||
      projects->waiting_activation || frame->scene_loading ||
      frame->world_loading || projects->world_wait ||
      projects->view != PROJECT_VIEW_EDITOR) {
    return;
  }
  /* Content imports first: they may be what the open scene shows. */
  projects->finalize_running_project = projects->finalize_project;
  projects->finalize_job =
      projects->finalize_project
          ? project_start_project_finalize(projects, editor, frame)
          : project_start_finalize(projects, editor, frame);
  if (projects->finalize_job) {
    log_info("Textures: encoding full-quality textures in the background");
  }
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
  project_yield_finalize(projects, editor);
  projects->job_id = vkr_editor_bakery_project_start(
      editor->bakery, projects->request_path, projects->result_path);
  if (!projects->job_id) {
    snprintf(projects->message, sizeof(projects->message),
             "Bakery queue is full. Finish or cancel pending jobs.");
    return;
  }
  if (projects->job_inspect) {
    /* A read-only preflight: the scene stays open and the form stays up. */
    projects->operation[0] = '\0';
    return;
  }
  if (!projects->additive_job && !projects->job_project_assets) {
    *frame->scene_request = (VkrSampleSceneRequest){
        .unload = true_v, .discard_edits = projects->discard_edits};
  }
  if (!projects->job_creates_project && !projects->job_project_assets &&
      frame->scene_backdrop_blur) {
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

/* Remember the project scene an add request loads. A reloaded document
   reuses its record; otherwise a record whose scene is no longer loaded is
   replaced. */
static void project_remember_added(VkrEditorProjects *projects,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t scene) {
  const String8 path = project_string(projects->additive_runtime_path);
  ProjectAddedScene *record = NULL;
  for (uint32_t i = 0; i < ArrayCount(projects->added) && !record; ++i) {
    const String8 recorded = project_string(projects->added[i].runtime_path);
    if (string8_equals(&recorded, &path)) {
      record = &projects->added[i];
    }
  }
  for (uint32_t i = 0; i < ArrayCount(projects->added) && !record; ++i) {
    const String8 recorded = project_string(projects->added[i].runtime_path);
    bool8_t loaded = false_v;
    for (uint32_t slot = 0; slot < VKR_SCENE_ADDITIVE_MAX && !loaded; ++slot) {
      loaded = frame->additive[slot] &&
               string8_equals(&frame->additive_names[slot], &recorded);
    }
    if (!loaded) {
      record = &projects->added[i];
    }
  }
  if (!record || scene >= projects->project->scene_count) {
    return;
  }
  snprintf(record->runtime_path, sizeof(record->runtime_path), "%s",
           projects->additive_runtime_path);
  snprintf(record->scene_id, sizeof(record->scene_id), "%s",
           projects->project->scenes[scene].id);
}

/* Index of the project scene loaded from an added runtime document, or
   UINT32_MAX. */
static uint32_t project_added_scene(const VkrEditorProjects *projects,
                                    String8 path) {
  for (uint32_t i = 0; projects->project && i < ArrayCount(projects->added);
       ++i) {
    const String8 recorded = project_string(projects->added[i].runtime_path);
    if (!path.length || !string8_equals(&recorded, &path)) {
      continue;
    }
    for (uint32_t s = 0; s < projects->project->scene_count; ++s) {
      if (!strcmp(projects->project->scenes[s].id,
                  projects->added[i].scene_id)) {
        return s;
      }
    }
  }
  return UINT32_MAX;
}

/* Load project scene `scene` beside the open one (ADR-076). The job lowers
   it; its result becomes an add request instead of replacing the open
   scene. */
static void project_add_scene(VkrEditorProjects *projects, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t scene) {
  projects->pending_scene = scene;
  projects->operation[0] = '\0';
  projects->additive_job = true_v;
  snprintf(projects->primary_runtime_path,
           sizeof(projects->primary_runtime_path), "%s",
           projects->runtime_path);
  snprintf(projects->primary_manifest_path,
           sizeof(projects->primary_manifest_path), "%s",
           projects->scene_manifest_path);
  snprintf(projects->primary_edit_path, sizeof(projects->primary_edit_path),
           "%s", projects->edit_path);
  projects->primary_manifest_fingerprint = projects->scene_manifest_fingerprint;
  project_start_job(projects, editor, frame, false_v);
}

/* Set primary: the added scene was removed last frame, so open it; once it
   is the open scene, add the previous primary back. A job that ends without
   activating it abandons the swap. */
static void project_set_primary_update(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame) {
  if (projects->swap == PROJECT_SWAP_NONE || projects->job_id ||
      projects->waiting_activation || frame->scene_loading) {
    return;
  }
  if (projects->swap == PROJECT_SWAP_OPEN) {
    projects->swap = PROJECT_SWAP_READD;
    projects->pending_scene = projects->swap_scene;
    projects->operation[0] = '\0';
    project_start_job(projects, editor, frame, false_v);
    return;
  }
  projects->swap = PROJECT_SWAP_NONE;
  if (projects->active_scene == projects->swap_scene &&
      projects->swap_back < projects->project->scene_count) {
    project_add_scene(projects, editor, frame, projects->swap_back);
  }
}

/* First Set primary step, once edits are saved or discarded: remove the
   added scene; the update opens it as the primary next frame. */
static void project_set_primary_begin(VkrEditorProjects *projects,
                                      const VkrSampleUiFrame *frame) {
  *frame->scene_request =
      (VkrSampleSceneRequest){.remove = true_v,
                              .container = projects->swap_container,
                              .discard_edits = projects->discard_edits};
  projects->swap = PROJECT_SWAP_OPEN;
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
  project_yield_finalize(projects, editor);
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
  project_yield_finalize(projects, editor);
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
    *frame->world_request =
        (VkrSampleWorldRequest){.unload = true_v, .discard_edits = true_v};
    vkr_editor_content_set_project(editor->content, "", "", "");
    vkr_editor_scripts_close_project(
        editor->scripts, vkr_editor_bakery_service(editor->bakery), frame);
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
  if (projects->view == PROJECT_VIEW_CREATE && !projects->creating_project) {
    const VkrEditorProjectTemplateInfo *template =
        &project_templates[projects->project_template];
    projects->include_scene =
        projects->project_template != PROJECT_TEMPLATE_NONE;
    projects->import_scene = projects->include_scene;
    if (projects->include_scene) {
      const int written =
          snprintf(projects->source_scene, sizeof(projects->source_scene),
                   "%s/assets/templates/%s.scene.json", vkr_content_root(),
                   template->file);
      const FilePath source = {.path = project_string(projects->source_scene),
                               .type = FILE_PATH_TYPE_ABSOLUTE};
      if (written <= 0 || (uint32_t)written >= sizeof(projects->source_scene) ||
          !file_exists(&source)) {
        snprintf(projects->message, sizeof(projects->message),
                 "%s template is unavailable in the editor content.",
                 template->name);
        return;
      }
      snprintf(projects->scene_name, sizeof(projects->scene_name), "%s",
               template->name);
    }
  }
  if (!projects->discard_edits && project_any_dirty(frame)) {
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

/* Unsupported features a job reports: each goes to the Console, and a toast
   says how many there were, so an import is never silently incomplete. */
static void project_report_warnings(VkrEditorUi *editor, String8 result) {
  VkrJsonReader reader = vkr_json_reader_from_string(result);
  if (!vkr_json_find_array(&reader, "warnings")) {
    return;
  }
  uint32_t count = 0u;
  String8 first = {0};
  do {
    String8 text = {0};
    if (!vkr_json_parse_string(&reader, &text)) {
      break;
    }
    log_warn("Project job: %.*s", (int)Min(text.length, 400u), text.str);
    first = count ? first : text;
    ++count;
  } while (vkr_json_next_array_element(&reader));
  if (count) {
    char message[256];
    snprintf(message, sizeof(message), "%.*s%s", (int)Min(first.length, 180u),
             (const char *)first.str, count > 1u ? " (and more warnings)" : "");
    vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, vkr_ui_theme()->warning,
                     message);
  }
}

/* Summary of an `inspect_scene` result for the Create form. */
static void project_read_inspection(VkrEditorProjects *projects,
                                    String8 result) {
  ProjectInspection *inspection = &projects->inspection;
  *inspection = (ProjectInspection){.valid = true_v};
  const char *counts[] = {"scene_version", "entities",      "meshes",
                          "materials",     "missing_count", "missing_images"};
  int32_t values[ArrayCount(counts)] = {0};
  for (uint32_t i = 0; i < ArrayCount(counts); ++i) {
    VkrJsonReader reader = vkr_json_reader_from_string(result);
    (void)vkr_json_get_int(&reader, counts[i], &values[i]);
  }
  inspection->scene_version = values[0];
  inspection->entities = (uint32_t)Max(values[1], 0);
  inspection->meshes = (uint32_t)Max(values[2], 0);
  inspection->materials = (uint32_t)Max(values[3], 0);
  inspection->missing_count = (uint32_t)Max(values[4], 0);
  inspection->missing_images = (uint32_t)Max(values[5], 0);
  VkrJsonReader reader = vkr_json_reader_from_string(result);
  (void)vkr_json_get_bool(&reader, "saved_edits", &inspection->saved_edits);
  reader = vkr_json_reader_from_string(result);
  if (vkr_json_find_array(&reader, "missing")) {
    uint32_t index = 0u;
    do {
      String8 text = {0};
      if (!vkr_json_parse_string(&reader, &text)) {
        break;
      }
      if (index < ArrayCount(inspection->missing)) {
        snprintf(inspection->missing[index++], sizeof(inspection->missing[0]),
                 "%.*s", (int)text.length, text.str);
      }
    } while (vkr_json_next_array_element(&reader));
  }
  reader = vkr_json_reader_from_string(result);
  if (vkr_json_find_array(&reader, "warnings")) {
    do {
      String8 text = {0};
      if (!vkr_json_parse_string(&reader, &text)) {
        break;
      }
      if (!inspection->warning_count++) {
        snprintf(inspection->warning, sizeof(inspection->warning), "%.*s",
                 (int)text.length, text.str);
      }
    } while (vkr_json_next_array_element(&reader));
  }
}

/* Places the meshes a Content import added at the World's root (ADR-076),
   once the inventory naming them is saved, and reloads the World to show
   them. */
static void project_place_world_models(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       const char (*ids)[37], uint32_t count) {
  projects->job_world_models = false_v;
  VkrEditorProjectError error = {0};
  uint32_t added = 0u;
  if (!ids || !vkr_editor_project_world_add_meshes(
                  projects->world_path, projects->project->assets, ids, count,
                  frame->ui->frame_allocator, &added, &error)) {
    snprintf(projects->message, sizeof(projects->message),
             "Imported into Content; the World could not take the model: %s",
             error.message[0] ? error.message : "no imported assets");
    log_warn("Import: %s", projects->message);
    vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, vkr_ui_theme()->warning,
                     projects->message);
    projects->discard_edits = false_v;
    return;
  }
  if (!added) {
    snprintf(projects->message, sizeof(projects->message),
             "Imported into Content; no model to place in the World.");
    projects->discard_edits = false_v;
    return;
  }
  project_reload_world(projects, frame);
  snprintf(projects->message, sizeof(projects->message),
           "Added %u model%s to the World.", added, added == 1u ? "" : "s");
  log_info("Import: %s", projects->message);
}

/* Publishes the inventory an asset import grew, files the new assets into the
   targeted Content folder and places requested World models. The open scene
   never unloaded. */
static void project_publish_imported_assets(VkrEditorProjects *projects,
                                            VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            String8 result) {
  VkrEditorProjectError error = {0};
  projects->job_id = 0;
  projects->job_project_assets = false_v;
  projects->operation[0] = '\0';
  projects->view = PROJECT_VIEW_EDITOR;
  if (!vkr_editor_project_save(projects->project, &error)) {
    project_error(projects, &error);
    return;
  }
  VkrJsonReader preview_reader = vkr_json_reader_from_string(result);
  int32_t preview = 0;
  if (vkr_json_get_int(&preview_reader, "preview_assets", &preview) &&
      preview > 0) {
    projects->finalize_project = true_v;
    projects->finalize_failures = 0u;
  }
  /* File the new assets into the folder the import targeted. */
  VkrJsonReader reader = vkr_json_reader_from_string(result);
  uint32_t imported = 0u;
  uint32_t id_capacity = 0u;
  char(*ids)[37] = NULL;
  if (vkr_json_find_array(&reader, "imported_assets")) {
    /* Each quoted identity and its separator take at least 38 bytes. */
    id_capacity = (uint32_t)Min(reader.length / 38u + 1u, 65536u);
    ids = vkr_allocator_alloc(frame->ui->frame_allocator,
                              sizeof(*ids) * id_capacity,
                              VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    while (vkr_json_next_array_element(&reader)) {
      String8 id = {0};
      char text[37];
      if (!vkr_json_parse_string(&reader, &id) || id.length >= sizeof(text)) {
        break;
      }
      MemCopy(text, id.str, id.length);
      text[id.length] = '\0';
      if (ids && imported < id_capacity) {
        MemCopy(ids[imported], text, sizeof(text));
      }
      ++imported;
      if (projects->import_folder[0]) {
        (void)vkr_editor_content_file_into(editor->content, text,
                                           projects->import_folder);
      }
    }
  }
  vkr_editor_content_refresh(editor->content);
  snprintf(projects->message, sizeof(projects->message),
           "Imported %u assets into Content%s%s.", imported,
           projects->import_folder[0] ? "/" : "", projects->import_folder);
  projects->import_source_count = 0u;
  projects->job_imports_models = false_v;
  if (projects->job_world_models) {
    project_place_world_models(projects, editor, frame, ids,
                               Min(imported, id_capacity));
  }
}

/* Replaces the scene fonts with those the completed job published. A failure
   reports its message, ends the job and returns false. */
static bool8_t project_load_scene_fonts(VkrEditorProjects *projects,
                                        const VkrSampleUiFrame *frame,
                                        String8 result) {
  VkrEditorProjectError error = {0};
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
        return false_v;
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
        return false_v;
      }
      if (projects->font_count == ArrayCount(projects->fonts)) {
        snprintf(projects->message, sizeof(projects->message),
                 "Too many scene fonts.");
        projects->job_id = 0;
        return false_v;
      }
      VkrFontHandle font = vkr_font_system_acquire(
          frame->ui->fonts, project_string(name), true_v, &font_error);
      if (font_error != VKR_RENDERER_ERROR_NONE) {
        projects->job_id = 0;
        return false_v;
      }
      projects->fonts[projects->font_count++] = font;
    }
  }
  return true_v;
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
  if (projects->job_inspect) {
    project_read_inspection(projects, result);
    projects->job_inspect = false_v;
    projects->job_id = 0;
    return;
  }
  project_report_warnings(editor, result);
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
  if (!projects->job_creates_project && !projects->job_project_assets) {
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
    project_schedule_finalize(projects, result);
  }
  if (!unbuilt && !projects->job_creates_project &&
      !projects->job_project_assets &&
      (!vkr_editor_project_json_string(
           result, "runtime_path", projects->runtime_path,
           sizeof(projects->runtime_path), &error) ||
       !vkr_editor_project_json_string(result, "edit_path", projects->edit_path,
                                       sizeof(projects->edit_path), &error))) {
    project_error(projects, &error);
    projects->job_id = 0;
    return;
  }
  if (!project_apply_inventory(projects, result)) {
    projects->job_id = 0;
    return;
  }
  if (projects->job_project_assets) {
    project_publish_imported_assets(projects, editor, frame, result);
    return;
  }
  if (projects->job_creates_project) {
    if (!vkr_editor_project_save(projects->project, &error)) {
      project_error(projects, &error);
      projects->job_id = 0;
      return;
    }
    projects->job_id = 0;
    project_refresh(projects);
    /* Opening the published project restores its settings and prepares its
       root World, as opening it from the chooser does (ADR-076). Loading
       replaces the project storage, so copy its identity first. */
    char id[37];
    snprintf(id, sizeof(id), "%s", projects->project->id);
    if (!project_load(projects, id, editor, frame)) {
      projects->view = PROJECT_VIEW_CHOOSER;
    }
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
    if (projects->creating_project) {
      /* A starter scene opens inside its new project the way the chooser
         opens a project: settings, Script modules and the root World
         (ADR-076). Loading clears creating_project only after it skips
         saving the previous project's settings into this one, and it
         replaces the project storage, so the scene is found again by its
         identity. */
      project_refresh(projects);
      char id[37];
      snprintf(id, sizeof(id), "%s", projects->project->id);
      if (!project_load(projects, id, editor, frame)) {
        projects->job_id = 0;
        projects->view = PROJECT_VIEW_CHOOSER;
        return;
      }
      projects->pending_scene = UINT32_MAX;
      for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
        if (!strcmp(projects->project->scenes[i].id, projects->scene_id)) {
          projects->pending_scene = i;
          break;
        }
      }
      if (projects->pending_scene == UINT32_MAX) {
        projects->job_id = 0;
        projects->view = PROJECT_VIEW_SCENES;
        snprintf(projects->message, sizeof(projects->message),
                 "The starter scene is missing from the project manifest.");
        return;
      }
    } else {
      if (!projects->settings_restored) {
        project_restore_settings(projects, editor, frame);
      }
      project_refresh(projects);
    }
  }
  if (unbuilt) {
    projects->job_id = 0;
    projects->view = PROJECT_VIEW_SCENES;
    snprintf(
        projects->message, sizeof(projects->message),
        "Scene saved without prepared assets. Select it to prepare and open.");
    return;
  }
  if (!project_load_scene_fonts(projects, frame, result)) {
    return;
  }
  if (projects->additive_job) {
    /* The request borrows the added scene's own copies; the open scene's
       paths return to the shared fields its saves use. */
    snprintf(projects->additive_runtime_path,
             sizeof(projects->additive_runtime_path), "%s",
             projects->runtime_path);
    snprintf(projects->additive_edit_path, sizeof(projects->additive_edit_path),
             "%s", projects->edit_path);
    snprintf(projects->runtime_path, sizeof(projects->runtime_path), "%s",
             projects->primary_runtime_path);
    snprintf(projects->scene_manifest_path,
             sizeof(projects->scene_manifest_path), "%s",
             projects->primary_manifest_path);
    snprintf(projects->edit_path, sizeof(projects->edit_path), "%s",
             projects->primary_edit_path);
    projects->scene_manifest_fingerprint =
        projects->primary_manifest_fingerprint;
    *frame->scene_request = (VkrSampleSceneRequest){
        .add = true_v,
        .path = project_string(projects->additive_runtime_path),
        .sidecar_path = project_string(projects->additive_edit_path)};
    project_remember_added(projects, frame, projects->pending_scene);
    projects->additive_job = false_v;
    projects->job_id = 0;
    projects->view = PROJECT_VIEW_EDITOR;
    return;
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
    projects->placing_asset = false_v;
  }
  projects->view = PROJECT_VIEW_EDITOR;
  (void)vkr_ui_keyboard_layer_set(frame->ui, 0);
}

static void project_choose_workspace(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  if (!projects->discard_edits && project_any_dirty(frame)) {
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
  *frame->world_request = (VkrSampleWorldRequest){
      .unload = true_v, .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  vkr_editor_content_set_project(editor->content, "", "", "");
  vkr_editor_scripts_close_project(
      editor->scripts, vkr_editor_bakery_service(editor->bakery), frame);
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
  project_release_template_previews(projects);
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

bool8_t vkr_editor_projects_busy(const VkrEditorProjects *projects) {
  return projects &&
         (projects->job_id || projects->waiting_activation ||
          projects->swap != PROJECT_SWAP_NONE || projects->world_wait);
}

/* A dialog is open: a project view other than the editor and its progress
   strip. */
static bool8_t project_dialog_open(const VkrEditorProjects *projects) {
  return projects && projects->view != PROJECT_VIEW_EDITOR &&
         projects->view != PROJECT_VIEW_PROGRESS;
}

bool8_t vkr_editor_projects_launcher(const VkrEditorProjects *projects) {
  return project_dialog_open(projects) &&
         (!projects->project || projects->creating_project);
}

/* Only the launcher, before a project opens, replaces the editor; an open
   project's dialogs float over it. */
bool8_t vkr_editor_projects_modal(const VkrEditorProjects *projects) {
  return vkr_editor_projects_launcher(projects);
}

bool8_t vkr_editor_projects_dialog_contains(const VkrEditorProjects *projects,
                                            float32_t x_px, float32_t y_px) {
  if (!project_dialog_open(projects)) {
    return false_v;
  }
  const VkrUiRect rect = projects->dialog_rect_px;
  return x_px >= rect.x && x_px < rect.x + rect.width && y_px >= rect.y &&
         y_px < rect.y + rect.height;
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
  projects->placing_asset = false_v;
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

/* Open a project scene by id in the Scene viewport, asking first when the
   open scene has unsaved edits. */
/* Whether project scene `scene` is loading or already loaded as the open
   scene: asking for it again neither interrupts nor reloads it. */
static bool8_t project_scene_current(const VkrEditorProjects *projects,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t scene) {
  const bool8_t busy = projects->job_id || projects->waiting_activation;
  if (busy) {
    return projects->pending_scene == scene && !projects->additive_job &&
           !projects->operation[0];
  }
  const String8 runtime = project_string(projects->runtime_path);
  return projects->active_scene == scene && frame->scene && runtime.length &&
         string8_equals(&frame->scene_path, &runtime);
}

static void project_open_scene_id(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  const char *scene_id) {
  for (uint32_t i = 0; projects->project && i < projects->project->scene_count;
       ++i) {
    if (strcmp(projects->project->scenes[i].id, scene_id)) {
      continue;
    }
    if (project_scene_current(projects, frame, i)) {
      return;
    }
    projects->pending_scene = i;
    projects->operation[0] = '\0';
    if (project_scene_dirty(frame)) {
      projects->resume_view = PROJECT_VIEW_EDITOR;
      projects->resume_action = PROJECT_RESUME_JOB;
      projects->view = PROJECT_VIEW_CONFIRM;
    } else {
      project_start_job(projects, editor, frame, false_v);
    }
    return;
  }
}

/* Place a Content mesh where it was dropped (ADR-076): the scene gains a
   model entity referencing the asset, then reloads. A dirty scene first asks
   to save or discard its edits. */
static void project_place_asset(VkrEditorProjects *projects,
                                VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                const VkrEditorContentAction *action) {
  if (!vkr_editor_projects_can_add_entity(projects, editor, frame)) {
    snprintf(projects->message, sizeof(projects->message),
             "Open a scene to place meshes in it.");
    return;
  }
  Vec3 position = vec3_new(0.0f, 0.0f, 0.0f);
  (void)vkr_editor_viewport_drop_point(frame, action->drop_px, &position);
  project_reset_scene_draft(projects);
  projects->adding_model = false_v;
  projects->light_count = 0;
  projects->placing_asset = true_v;
  projects->place_prefab = false_v;
  projects->place_position = position;
  snprintf(projects->place_asset, sizeof(projects->place_asset), "%s",
           action->asset_id);
  snprintf(projects->place_scope, sizeof(projects->place_scope), "%s",
           action->scope);
  snprintf(projects->place_name, sizeof(projects->place_name), "%s",
           action->name);
  projects->pending_scene = projects->active_scene;
  snprintf(projects->operation, sizeof(projects->operation), "add_entities");
  if (project_scene_dirty(frame)) {
    projects->resume_view = PROJECT_VIEW_EDITOR;
    projects->resume_action = PROJECT_RESUME_JOB;
    projects->view = PROJECT_VIEW_CONFIRM;
  } else {
    project_start_job(projects, editor, frame, false_v);
  }
}

/* Show the World document (ADR-076): an open World tab, else a new one. */
static void project_open_world(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  for (uint32_t i = 0; i < editor->viewport_tab_count; ++i) {
    if (!editor->viewport_tabs[i].scene_id[0]) {
      (void)vkr_editor_viewport_tab_show(editor, frame, i);
      return;
    }
  }
  (void)vkr_editor_viewport_tab_new(editor, frame);
}

/* A model file the import step can place: glTF, GLB, OBJ or cooked VKB. */
static bool8_t project_model_file(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false_v;
  }
  char suffix[8] = {0};
  for (uint32_t i = 0; i + 1u < sizeof(suffix) && dot[i]; ++i) {
    suffix[i] = (char)tolower((unsigned char)dot[i]);
  }
  static const char *const models[] = {".gltf", ".glb", ".obj", ".vkb"};
  for (uint32_t i = 0; i < ArrayCount(models); ++i) {
    if (!strcmp(suffix, models[i])) {
      return true_v;
    }
  }
  return false_v;
}

/* Every file of the import step is a model, so it can be placed. */
static bool8_t project_import_models(const VkrEditorProjects *projects) {
  if (!projects->import_source_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < projects->import_source_count; ++i) {
    if (!project_model_file(projects->import_sources[i])) {
      return false_v;
    }
  }
  return true_v;
}

/* Opens the import step for `import_sources` (ADR-076). Models go to the
   open scene by default, else to the World; a new scene takes the first
   file's name. */
static void project_open_import_step(VkrEditorProjects *projects,
                                     VkrEditorUi *editor) {
  const bool8_t open_scene =
      projects->active_scene < projects->project->scene_count;
  projects->import_target =
      open_scene ? PROJECT_IMPORT_SCENE : PROJECT_IMPORT_WORLD;
  projects->import_scene_index = open_scene ? projects->active_scene : 0u;
  const char *path = projects->import_sources[0];
  const char *name = path;
  for (const char *c = path; *c; ++c) {
    if (*c == '/' || *c == '\\') {
      name = c + 1;
    }
  }
  const char *dot = strrchr(name, '.');
  const int32_t stem =
      dot && dot > name ? (int32_t)(dot - name) : (int32_t)strlen(name);
  snprintf(projects->scene_name, sizeof(projects->scene_name), "%.*s", stem,
           name);
  projects->message[0] = '\0';
  projects->create_step = PROJECT_CREATE_IMPORT;
  editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = true_v;
}

/* Import into the project (ADR-076): every scene may use the result, and the
   open scene keeps running. Without `source` a file dialog asks for one; a
   chosen model opens the import step, which asks where it goes. */
static void project_import_assets(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  const char *source) {
  if (projects->read_only || !projects->project) {
    snprintf(projects->message, sizeof(projects->message),
             "Importing needs a writable project.");
    return;
  }
  snprintf(projects->operation, sizeof(projects->operation),
           "import_project_assets");
  projects->job_imports_models = false_v;
  projects->job_world_models = false_v;
  if (!projects->import_source_count) {
    snprintf(projects->import_folder, sizeof(projects->import_folder), "%s",
             vkr_editor_content_folder(editor->content));
  }
  projects->action_asset[0] = '\0';
  projects->action_name[0] = '\0';
  projects->action_source[0] = '\0';
  if (source) {
    snprintf(projects->action_source, sizeof(projects->action_source), "%s",
             source);
  } else {
    static const char *const extensions[] = {
        "gltf", "glb", "obj", "vkb", "png", "jpg", "jpeg", "ttf", "otf", "mt"};
    project_browse(projects, frame,
                   "Choose an asset to import into the project", extensions,
                   ArrayCount(extensions), false_v, projects->action_source,
                   sizeof(projects->action_source));
    if (projects->action_source[0] &&
        project_model_file(projects->action_source) &&
        strlen(projects->action_source) < sizeof(projects->import_sources[0])) {
      snprintf(projects->import_sources[0], sizeof(projects->import_sources[0]),
               "%s", projects->action_source);
      projects->import_source_count = 1u;
      projects->operation[0] = '\0';
      project_open_import_step(projects, editor);
      return;
    }
  }
  if (projects->action_source[0]) {
    project_start_job(projects, editor, frame, false_v);
  } else {
    projects->operation[0] = '\0';
  }
  if (!projects->job_id) {
    projects->import_source_count = 0u;
  }
}

/* Import, reimport, rebuild or rename through a scene job. Assets belong to
   the open scene, so these need a writable one. */
static bool8_t project_save_name(VkrEditorProjects *projects);

/* A Content scene folder's command (ADR-076): add the scene beside the open
   one, rename it, or ask to delete it. */
static void project_scene_content_action(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const VkrEditorContentAction *action) {
  uint32_t scene = UINT32_MAX;
  for (uint32_t i = 0; projects->project && i < projects->project->scene_count;
       ++i) {
    if (!strcmp(projects->project->scenes[i].id, action->asset_id)) {
      scene = i;
    }
  }
  if (scene == UINT32_MAX) {
    return;
  }
  if (action->kind == VKR_EDITOR_CONTENT_ACTION_ADD_SCENE) {
    if (scene == projects->active_scene ||
        projects->active_scene >= projects->project->scene_count) {
      snprintf(projects->message, sizeof(projects->message),
               "Open a different scene first; a scene is added beside it.");
      return;
    }
    project_add_scene(projects, editor, frame, scene);
    return;
  }
  projects->pending_scene = scene;
  projects->rename_project = false_v;
  if (action->kind == VKR_EDITOR_CONTENT_ACTION_RENAME_SCENE) {
    snprintf(projects->rename_name, sizeof(projects->rename_name), "%s",
             action->name);
    if (project_save_name(projects)) {
      vkr_editor_content_refresh(editor->content);
    }
    return;
  }
  projects->message[0] = '\0';
  projects->view = PROJECT_VIEW_DELETE;
}

static void project_content_action(VkrEditorProjects *projects,
                                   VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   const VkrEditorContentAction *action) {
  if (action->kind == VKR_EDITOR_CONTENT_ACTION_IMPORT) {
    project_import_assets(projects, editor, frame, NULL);
    return;
  }
  /* A rebuild after a source edit never interrupts: with unsaved scene edits
     the asset keeps its Changed state until the user rebuilds it. */
  if (action->automatic) {
    if (projects->read_only || !projects->project ||
        projects->active_scene >= projects->project->scene_count ||
        project_scene_dirty(frame)) {
      log_warn("Content: %s changed on disk; save the scene, then Rebuild it",
               action->name);
      return;
    }
    log_info("Content: %s changed on disk; rebuilding it", action->name);
  }
  if (projects->read_only || !projects->project ||
      projects->active_scene >= projects->project->scene_count) {
    snprintf(projects->message, sizeof(projects->message),
             "Open a writable scene before importing or rebuilding assets.");
    projects->view = PROJECT_VIEW_SCENES;
    return;
  }
  projects->pending_scene = projects->active_scene;
  snprintf(projects->operation, sizeof(projects->operation), "%s",
           action->kind == VKR_EDITOR_CONTENT_ACTION_IMPORT ? "import_assets"
           : action->kind == VKR_EDITOR_CONTENT_ACTION_REIMPORT
               ? "reimport_asset"
           : action->kind == VKR_EDITOR_CONTENT_ACTION_RENAME ? "rename_asset"
           : action->kind == VKR_EDITOR_CONTENT_ACTION_DELETE_ASSET
               ? "delete_asset"
               : "rebuild_asset");
  snprintf(projects->action_asset, sizeof(projects->action_asset), "%s",
           action->asset_id);
  projects->action_source[0] = '\0';
  snprintf(projects->action_name, sizeof(projects->action_name), "%s",
           action->name);
  const bool8_t sourced = action->kind == VKR_EDITOR_CONTENT_ACTION_IMPORT ||
                          action->kind == VKR_EDITOR_CONTENT_ACTION_REIMPORT;
  if (sourced) {
    static const char *const extensions[] = {"gltf", "glb", "obj", "png", "jpg",
                                             "jpeg", "ttf", "otf", "mt"};
    project_browse(projects, frame, "Choose an asset to copy into the scene",
                   extensions, ArrayCount(extensions), false_v,
                   projects->action_source, sizeof(projects->action_source));
  }
  if (!sourced || projects->action_source[0]) {
    if (project_scene_dirty(frame)) {
      projects->resume_view = PROJECT_VIEW_SCENES;
      projects->view = PROJECT_VIEW_CONFIRM;
    } else {
      project_start_job(projects, editor, frame, false_v);
    }
  }
}

/* Run the Content action the editor view can take now; while a job or an
   activation runs, the action waits. */
static void project_take_content_action(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame) {
  VkrEditorContentAction content_action;
  if (projects->view == PROJECT_VIEW_EDITOR && !projects->waiting_activation &&
      !projects->job_id &&
      vkr_editor_content_take_action(editor->content, &content_action)) {
    /* Import opens the Create or import window; it offers scenes too. */
    if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_IMPORT) {
      projects->create_step = PROJECT_CREATE_CHOOSE;
      editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = true_v;
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE) {
      project_open_scene_id(projects, editor, frame, content_action.asset_id);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_OPEN_WORLD) {
      project_open_world(editor, frame);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_OPEN_SCRIPT) {
      (void)vkr_editor_code_open(editor->code, editor, content_action.source);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_NEW_SCRIPT) {
      vkr_editor_code_new_script(editor->code, editor);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_DROP_SCRIPT) {
      vkr_editor_drop_script(editor, frame, content_action.name,
                             content_action.drop_px);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_CREATE_OBJECT) {
      /* Content shows the new object where it lives once it is selected. */
      if (vkr_editor_request_create(
              frame, content_action.object, vkr_editor_create_container(frame),
              content_action.dropped ? &content_action.drop_px : NULL)) {
        vkr_editor_content_reveal_created(editor->content,
                                          frame->selected_entity);
      }
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_PLACE_ASSET) {
      project_place_asset(projects, editor, frame, &content_action);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_ADD_SCENE ||
               content_action.kind == VKR_EDITOR_CONTENT_ACTION_RENAME_SCENE ||
               content_action.kind == VKR_EDITOR_CONTENT_ACTION_DELETE_SCENE) {
      project_scene_content_action(projects, editor, frame, &content_action);
    } else {
      project_content_action(projects, editor, frame, &content_action);
    }
  }
}

/* Each session appends its logs to <workspace>/logs/<UTC start>-<pid>.log;
   opening another workspace continues in a file there. */
static void project_session_log(VkrEditorProjects *projects,
                                const VkrSampleUiFrame *frame) {
  if (!projects->workspace.initialized ||
      !strcmp(projects->log_root, projects->workspace.root)) {
    return;
  }
  snprintf(projects->log_root, sizeof(projects->log_root), "%s",
           projects->workspace.root);
  if (!projects->log_name[0]) {
    VkrTime time = {0};
    (void)vkr_platform_get_utc_time(&time);
    snprintf(projects->log_name, sizeof(projects->log_name),
             "%04d%02d%02d-%02d%02d%02d-%u.log", time.year + 1900,
             time.month + 1, time.day, time.hours, time.minutes, time.seconds,
             vkr_platform_get_process_id());
  }
  char path[VKR_EDITOR_PROJECT_PATH_CAPACITY + 80];
  snprintf(path, sizeof(path), "%s/logs", projects->log_root);
  String8 directory = project_string(path);
  if (!file_ensure_directory(frame->ui->frame_allocator, &directory)) {
    log_warn("Cannot create the workspace logs folder %s", path);
    return;
  }
  snprintf(path, sizeof(path), "%s/logs/%s", projects->log_root,
           projects->log_name);
  if (!log_file_open(path)) {
    log_warn("Cannot open the session log %s", path);
  }
}

/* Reports a failed or cancelled project job for the operation it ran; true
 * when the update stops for this frame. */
static bool8_t project_job_failed(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  VkrEditorProjectJobStatus status) {
  if (projects->job_inspect) {
    /* The scene JSON could not be read; the form shows why. */
    String8 result = {0};
    projects->inspection = (ProjectInspection){0};
    if (!project_read_file(projects->result_path, frame->ui->frame_allocator,
                           &result) ||
        !vkr_editor_project_json_string(
            result, "error", projects->inspection.error,
            sizeof(projects->inspection.error), NULL)) {
      snprintf(projects->inspection.error, sizeof(projects->inspection.error),
               "The scene JSON could not be inspected.");
    }
    projects->job_inspect = false_v;
    projects->job_id = 0;
    return true_v;
  }
  if (!strcmp(projects->operation, "add_entities") && projects->placing_asset) {
    projects->job_id = 0;
    projects->placing_asset = false_v;
    projects->operation[0] = '\0';
    snprintf(projects->message, sizeof(projects->message),
             "Nothing was placed. Rebuild the asset or see Bakery for "
             "details.");
  } else if (!strcmp(projects->operation, "add_entities") &&
             projects->job_imports_models) {
    /* Nothing was added; the scene open before the import reopens. */
    projects->job_id = 0;
    projects->job_imports_models = false_v;
    projects->operation[0] = '\0';
    projects->view = PROJECT_VIEW_EDITOR;
    snprintf(projects->message, sizeof(projects->message),
             "%s. The model was not added; see Bakery for details.",
             status == VKR_EDITOR_PROJECT_JOB_CANCELLED ? "Import cancelled"
                                                        : "Import failed");
    vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, vkr_ui_theme()->warning,
                     projects->message);
    if (projects->active_scene < projects->project->scene_count) {
      projects->pending_scene = projects->active_scene;
      project_start_job(projects, editor, frame, false_v);
    }
  } else if (!strcmp(projects->operation, "add_entities")) {
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
             status == VKR_EDITOR_PROJECT_JOB_CANCELLED ? "Creation cancelled"
                                                        : "Creation failed");
  } else {
    snprintf(projects->message, sizeof(projects->message),
             "%s. Your source files and existing projects are unchanged. See "
             "Bakery for the job output.",
             status == VKR_EDITOR_PROJECT_JOB_CANCELLED ? "Creation cancelled"
                                                        : "Creation failed");
  }
  return false_v;
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
  if (projects->view != PROJECT_VIEW_CREATE) {
    project_release_template_previews(projects);
  }
  project_session_log(projects, frame);
  if (frame->close_requested && !projects->closing) {
    if (!project_any_dirty(frame) && !projects->job_id &&
        project_save_settings(projects, editor, frame->dock)) {
      *frame->close_response = VKR_SAMPLE_CLOSE_CONFIRM;
    } else {
      projects->closing = true_v;
      projects->resume_view = projects->view;
      projects->view = PROJECT_VIEW_CONFIRM;
    }
  }
  *frame->modal = vkr_editor_projects_launcher(projects) || projects->dropdown;
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
  if (projects->world_wait) {
    projects->world_wait =
        frame->world_loading ? 1u : projects->world_wait - 1u;
  }
  project_update_finalize(projects, editor, frame);
  /* Models of the World or scene streaming in show as loading (ADR-076). */
  const bool8_t world_loading = frame->world_loading || projects->world_wait;
  const bool8_t scene_loading =
      (frame->scene_loading || projects->waiting_activation) &&
      projects->project &&
      projects->pending_scene < projects->project->scene_count;
  vkr_editor_content_set_loading(
      editor->content, projects->world_meshes,
      world_loading ? projects->world_mesh_count : 0u,
      scene_loading ? projects->project->scenes[projects->pending_scene].id
                    : "");
  /* What the background finalize rebuilds shows as cooking (ADR-077). */
  vkr_editor_content_set_cooking(
      editor->content,
      projects->finalize_job && projects->finalize_running_project,
      projects->finalize_job && !projects->finalize_running_project
          ? projects->finalize_scene_id
          : "");
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
    } else if ((status == VKR_EDITOR_PROJECT_JOB_FAILED ||
                status == VKR_EDITOR_PROJECT_JOB_CANCELLED) &&
               project_job_failed(projects, editor, frame, status)) {
      return;
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
  project_set_primary_update(projects, editor, frame);
  if (!projects->job_id && !projects->waiting_activation &&
      strcmp(projects->operation, "delete_scene") &&
      vkr_editor_bakery_take_scene_bake(editor->bakery,
                                        &projects->bake_reflection,
                                        &projects->bake_diffuse) &&
      projects->project &&
      projects->active_scene < projects->project->scene_count) {
    projects->pending_scene = projects->active_scene;
    snprintf(projects->operation, sizeof(projects->operation), "bake_scene");
    if (project_scene_dirty(frame)) {
      projects->resume_view = PROJECT_VIEW_SCENES;
      projects->view = PROJECT_VIEW_CONFIRM;
    } else {
      project_start_job(projects, editor, frame, false_v);
    }
  }
  project_take_content_action(projects, editor, frame);
  const float64_t now = vkr_platform_get_absolute_time();
  if (projects->project && now >= projects->next_settings_check) {
    projects->next_settings_check = now + .25;
    project_remember_scene(projects, editor, frame);
    /* A package build reads project.json while it lowers scenes and fails
       if the file changes, so remembered viewports wait until it ends. */
    if (!projects->job_id && !projects->delete_waiting_unload &&
        !projects->delete_project_waiting_unload &&
        !vkr_editor_build_busy(editor->build)) {
      project_queue_settings_save(projects, editor, frame->dock);
    }
  }
}

/* A new project chooses a starter scene beside its World (ADR-076). */
static void project_begin_create(VkrEditorProjects *projects) {
  project_reset_scene_draft(projects);
  projects->project_template = PROJECT_TEMPLATE_BLANK;
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
      if (project_any_dirty(frame)) {
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
          if (project_any_dirty(frame)) {
            snprintf(projects->initial_project,
                     sizeof(projects->initial_project), "%s",
                     projects->cards[i].id);
            projects->resume_view = PROJECT_VIEW_CHOOSER;
            projects->view = PROJECT_VIEW_CONFIRM;
          } else {
            (void)project_load(projects, projects->cards[i].id, editor, frame);
          }
        } else if (!project_scene_current(projects, frame, i)) {
          projects->pending_scene = i;
          if (project_scene_dirty(frame)) {
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

/* A template's picture: its preview, or its icon while the preview loads or
   when it has none. */
static void project_template_picture(VkrEditorProjects *projects,
                                     VkrUiSystem *ui,
                                     VkrEditorProjectTemplate index,
                                     const char *id, float32_t x, float32_t y,
                                     float32_t width, float32_t height,
                                     bool8_t selected) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrTextureHandle texture =
      project_template_preview(projects, ui, index);
  VkrUiWidgetConfig picture = project_widget(x, y, width, height);
  picture.style.padding_pt = (VkrUiEdges){0};
  picture.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  if (texture.id) {
    vkr_ui_image(ui, project_string(id),
                 (VkrUiTextureRef){texture.id, texture.generation},
                 project_template_preview_size, &picture);
    return;
  }
  picture.style.background_color = theme->window;
  picture.icon =
      index == PROJECT_TEMPLATE_NONE ? VKR_UI_ICON_WORLD : VKR_UI_ICON_SCENE;
  picture.icon_size_pt = Min(48.0f, height * .4f);
  picture.icon_color = selected ? theme->accent_hover : theme->text_secondary;
  picture.center = true_v;
  vkr_ui_label(ui, project_string(id), (String8){0}, &picture);
}

/* One gallery card: picture over name. Returns its height. */
static float32_t project_template_card(VkrEditorProjects *projects,
                                       VkrUiSystem *ui,
                                       VkrEditorProjectTemplate index,
                                       float32_t x, float32_t y,
                                       float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorProjectTemplateInfo *template = &project_templates[index];
  const bool8_t selected = projects->project_template == index;
  const float32_t inset = 6.0f;
  const float32_t picture_width = width - inset * 2.0f;
  const float32_t picture_height = picture_width *
                                   project_template_preview_size.y /
                                   project_template_preview_size.x;
  const float32_t height = picture_height + inset + 38.0f;
  VkrUiWidgetConfig card = project_widget(x, y, width, height);
  vkr_editor_toggle_style(&card, selected);
  card.disabled = projects->read_only || projects->job_id;
  card.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  card.style.border_pt =
      selected ? (VkrUiEdges){2, 2, 2, 2} : (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = selected ? theme->accent : theme->border;
  card.tooltip = project_string(template->detail);
  (void)vkr_ui_push_id_u64(ui, index);
  if (vkr_ui_button(ui, string8_lit("project.template"), (String8){0}, &card)) {
    projects->project_template = index;
  }
  project_template_picture(projects, ui, index, "project.template.picture",
                           x + inset, y + inset, picture_width, picture_height,
                           selected);
  VkrUiWidgetConfig name = project_widget(
      x + inset, y + inset + picture_height + 4.0f, picture_width, 28.0f);
  name.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
  name.style.font_size_pt = theme->font_emphasis;
  name.style.text_color = selected ? theme->text : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("project.template.name"),
               project_string(template->name), &name);
  (void)vkr_ui_pop_id(ui);
  return height;
}

/* Wrapped secondary text. Returns the height it reserves. */
static float32_t project_note(VkrUiSystem *ui, const char *id, const char *text,
                              float32_t x, float32_t y, float32_t width,
                              float32_t height) {
  VkrUiWidgetConfig note = project_widget(x, y, width, height);
  note.style.padding_pt = (VkrUiEdges){2, 10, 2, 10};
  note.style.text_color = vkr_ui_theme()->text_secondary;
  note.text.layout.word_wrap = true_v;
  note.text.layout.max_width = Max(1.0f, width - 20.0f);
  vkr_ui_label(ui, project_string(id), project_string(text), &note);
  return height;
}

/* The selected template's summary and the project fields from `top`. Returns
   the column's bottom. */
static float32_t project_build_template_details(VkrEditorProjects *projects,
                                                VkrEditorUi *editor,
                                                const VkrSampleUiFrame *frame,
                                                float32_t x, float32_t top,
                                                float32_t width,
                                                bool8_t show_picture) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorProjectTemplateInfo *template =
      &project_templates[projects->project_template];
  float32_t y = top;
  if (show_picture) {
    const float32_t picture_height = width * project_template_preview_size.y /
                                     project_template_preview_size.x;
    project_template_picture(projects, ui, projects->project_template,
                             "project.template.selected", x, y, width,
                             picture_height, true_v);
    y += picture_height + 12.0f;
  }
  VkrUiWidgetConfig title = project_widget(x, y, width, 34);
  title.style.padding_pt = (VkrUiEdges){2, 10, 2, 10};
  title.style.font_size_pt = theme->font_title;
  title.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("project.template.title"),
               project_string(template->name), &title);
  y += 36.0f;
  y += project_note(ui, "project.template.detail", template->detail, x, y,
                    width, 44.0f);
  if (template->camera[0]) {
    const float32_t tag_width = (width - 8.0f) * .5f;
    VkrUiWidgetConfig camera = project_widget(x, y, tag_width, 26);
    camera.style.padding_pt = (VkrUiEdges){3, 10, 3, 10};
    camera.style.text_color = theme->text_secondary;
    camera.icon = template->camera_icon;
    camera.icon_size_pt = 14.0f;
    camera.icon_color = theme->accent_hover;
    vkr_ui_label(ui, string8_lit("project.template.camera"),
                 project_string(template->camera), &camera);
    VkrUiWidgetConfig footprint = camera;
    footprint.placement.margin_pt.left = x + tag_width + 8.0f;
    footprint.icon = VKR_UI_ICON_RULER;
    vkr_ui_label(ui, string8_lit("project.template.footprint"),
                 project_string(template->footprint), &footprint);
    y += 30.0f;
    y += project_note(ui, "project.template.controls",
                      "Start Simulation to play. WASD moves; Space jumps; "
                      "Ctrl crouches; V changes camera.",
                      x, y, width, 44.0f);
  }
  y += 10.0f;
  project_label(ui, "project.name.label", "Project name", x, y, width);
  project_field(ui, "project.name", projects->project_name,
                sizeof(projects->project_name), x, y + 30.0f, width);
  y += 74.0f;
  project_label(ui, "project.font.label",
                "Default font / empty uses editor default", x, y, width);
  project_field(ui, "project.font", projects->project_font_source,
                sizeof(projects->project_font_source), x, y + 30.0f, width);
  if (project_button(ui, "project.font.browse", "Choose default font", x,
                     y + 66.0f, width, false_v)) {
    static const char *const extensions[] = {"ttf", "otf"};
    project_browse(projects, frame, "Choose project default font", extensions,
                   2, false_v, projects->project_font_source,
                   sizeof(projects->project_font_source));
  }
  return y + 100.0f;
}

/* Create project, like an engine launcher's New Project page: a gallery of
   starter templates beside the selected template's summary and the project
   fields. Narrow bodies stack the gallery above the fields. */
static void project_build_template_form(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        float32_t body_width,
                                        float32_t body_height) {
  VkrUiSystem *ui = frame->ui;
  const float32_t gap = 12.0f;
  const bool8_t wide = body_width >= 760.0f;
  const float32_t details_width =
      wide ? vkr_clamp_f32(body_width * .34f, 280.0f, 400.0f) : 0.0f;
  const float32_t gallery_width =
      wide ? body_width - 24.0f - details_width - 24.0f : body_width - 24.0f;
  const uint32_t columns = gallery_width >= 4.0f * 200.0f + 3.0f * gap ? 4u
                           : gallery_width >= 2.0f * 150.0f + gap      ? 2u
                                                                       : 1u;
  /* Wide bodies fit every row of cards in the body's height; each card adds
     44 points of inset and name to its picture. */
  const uint32_t rows = (PROJECT_TEMPLATE_COUNT + columns - 1) / columns;
  const float32_t fit_picture =
      (body_height - 36.0f - gap * (float32_t)(rows - 1)) / (float32_t)rows -
      44.0f;
  const float32_t fit_width = fit_picture * project_template_preview_size.x /
                                  project_template_preview_size.y +
                              12.0f;
  float32_t card_width =
      (gallery_width - gap * (float32_t)(columns - 1)) / (float32_t)columns;
  if (wide) {
    card_width = Max(140.0f, Min(card_width, fit_width));
  }
  VkrUiWidgetConfig heading =
      vkr_editor_section_label_config(editor->heading_font);
  heading.placement = project_widget(12, 6, gallery_width, 24).placement;
  heading.style.min_size_pt = (Vec2){gallery_width, 24};
  heading.style.max_size_pt = heading.style.min_size_pt;
  vkr_ui_label(ui, string8_lit("project.template.label"),
               string8_lit("TEMPLATES"), &heading);
  float32_t y = 36.0f;
  float32_t row_height = 0.0f;
  for (uint32_t i = 0; i < PROJECT_TEMPLATE_COUNT; ++i) {
    const uint32_t column = i % columns;
    if (i && column == 0) {
      y += row_height + gap;
      row_height = 0.0f;
    }
    const float32_t x = 12.0f + (card_width + gap) * (float32_t)column;
    row_height =
        Max(row_height,
            project_template_card(projects, ui, (VkrEditorProjectTemplate)i, x,
                                  y, card_width));
  }
  const float32_t gallery_height = y + row_height;
  /* Large two-column cards and the stacked layout already show the selected
     preview; small cards repeat it above the details. */
  const float32_t details_bottom =
      wide ? project_build_template_details(projects, editor, frame,
                                            body_width - 12.0f - details_width,
                                            6.0f, details_width, columns > 2)
           : project_build_template_details(projects, editor, frame, 12.0f,
                                            gallery_height + 20.0f,
                                            body_width - 24.0f, false_v);
  projects->form_height = Max(gallery_height, details_bottom) + 12.0f;
}

static void project_build_create_form(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      float32_t body_width,
                                      float32_t body_height) {
  if (projects->view == PROJECT_VIEW_CREATE) {
    project_build_template_form(projects, editor, frame, body_width,
                                body_height);
    return;
  }
  if (projects->include_scene) {
    projects->form_height =
        project_build_scene_form(projects, frame, 12, body_width - 24);
  } else {
    projects->form_height = 320.0f;
  }
}

/* Save `rename_name` as the project's name or scene `pending_scene`'s; an
   invalid name or a failed save keeps the old one. */
static bool8_t project_save_name(VkrEditorProjects *projects) {
  VkrEditorProjectError error = {0};
  if (!vkr_editor_project_name_valid(projects->rename_name, &error)) {
    project_error(projects, &error);
    return false_v;
  }
  if (!project_finish_settings_save(projects)) {
    return false_v;
  }
  char *target_name =
      projects->rename_project
          ? projects->project->name
          : projects->project->scenes[projects->pending_scene].name;
  char previous[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  snprintf(previous, sizeof(previous), "%s", target_name);
  snprintf(target_name, VKR_EDITOR_PROJECT_NAME_CAPACITY, "%s",
           projects->rename_name);
  if (!vkr_editor_project_save(projects->project, &error)) {
    snprintf(target_name, VKR_EDITOR_PROJECT_NAME_CAPACITY, "%s", previous);
    project_error(projects, &error);
    return false_v;
  }
  project_refresh(projects);
  return true_v;
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
    if (project_save_name(projects)) {
      projects->view = PROJECT_VIEW_SCENES;
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
    row.tooltip = active ? string8_lit("This scene is open")
                         : string8_lit("Open this scene");
    if (vkr_ui_button(ui, string8_lit("scene.open"), (String8){0}, &row) &&
        !project_scene_current(projects, frame, i)) {
      projects->pending_scene = i;
      projects->operation[0] = '\0';
      if (project_scene_dirty(frame)) {
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
    /* Load beside the open scene as an additive container (ADR-076). */
    if (!active && projects->active_scene < projects->project->scene_count) {
      VkrUiWidgetConfig add = vkr_editor_icon_button_config(
          0, 0, VKR_UI_ICON_LAYERS,
          string8_lit("Add this scene beside the open one"));
      add.placement =
          project_widget(body_width - 130, y + 11, 26, 26).placement;
      add.disabled = locked;
      if (vkr_ui_button(ui, string8_lit("scene.add"), (String8){0}, &add)) {
        project_add_scene(projects, editor, frame, i);
      }
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

/* Close the open scene, leaving the World; unsaved edits were resolved. */
static void project_unload_scene(VkrEditorProjects *projects,
                                 VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  *frame->scene_request = (VkrSampleSceneRequest){
      .unload = true_v, .discard_edits = projects->discard_edits};
  projects->discard_edits = false_v;
  projects->active_scene = UINT32_MAX;
  projects->content_scene[0] = '\0';
  vkr_editor_content_set_project(editor->content, projects->workspace.root,
                                 projects->project->id, "");
}

/* Names of the loaded containers with unsaved edits, such as "World, Level
   One", for the save prompt. */
static void project_dirty_names(const VkrEditorProjects *projects,
                                const VkrSampleUiFrame *frame, char *out,
                                uint32_t capacity) {
  uint32_t length = 0u;
  out[0] = '\0';
#define PROJECT_DIRTY_NAME(format, ...)                                        \
  if (length < capacity) {                                                     \
    const int32_t written =                                                    \
        snprintf(out + length, capacity - length, "%s" format,                 \
                 length ? ", " : "", __VA_ARGS__);                             \
    length += written > 0 ? (uint32_t)written : 0u;                            \
  }
  if (frame->world && frame->world_edits &&
      frame->world_edits->revision != frame->world_edits->saved_revision) {
    PROJECT_DIRTY_NAME("%s", "World");
  }
  if (project_scene_dirty(frame)) {
    const bool8_t named =
        projects->project &&
        projects->active_scene < projects->project->scene_count;
    PROJECT_DIRTY_NAME(
        "%s", named ? projects->project->scenes[projects->active_scene].name
                    : "Scene");
  }
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i] && frame->additive_edits[i]->revision !=
                                  frame->additive_edits[i]->saved_revision) {
      const String8 added =
          vkr_editor_projects_added_name(projects, frame->additive_names[i]);
      const String8 name = added.length ? added : frame->additive_names[i];
      PROJECT_DIRTY_NAME("%.*s", (int)name.length, (const char *)name.str);
    }
  }
#undef PROJECT_DIRTY_NAME
}

static void project_build_confirm_form(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       float32_t body_width) {
  VkrUiSystem *ui = frame->ui;
  char dirty[256];
  project_dirty_names(projects, frame, dirty, sizeof(dirty));
  char message[384];
  snprintf(message, sizeof(message),
           "Unsaved edits in %s. Save them all, discard them, or return.",
           dirty[0] ? dirty : "this project");
  project_label(ui, "dirty.message",
                projects->closing && projects->job_id
                    ? "An asset job is active. Closing cancels it and "
                      "waits for its worker."
                    : message,
                12, 12, body_width - 24);
  if (project_button(ui, "dirty.save", "Save and continue", 12, 64, 170,
                     false_v)) {
    projects->awaiting_save = true_v;
    frame->scene_edit->action = VKR_SCENE_EDIT_SAVE;
  }
  if (project_button(ui, "dirty.discard", "Discard edits", 194, 64, 145,
                     false_v) ||
      (projects->awaiting_save && !project_any_dirty(frame))) {
    projects->awaiting_save = false_v;
    projects->discard_edits = true_v;
    if (projects->closing) {
      if (project_save_settings(projects, editor, frame->dock)) {
        *frame->close_response = VKR_SAMPLE_CLOSE_CONFIRM;
      }
    } else {
      projects->view = projects->resume_view;
    }
    const ProjectResume resume = projects->resume_action;
    projects->resume_action = PROJECT_RESUME_NONE;
    if (!projects->closing && resume == PROJECT_RESUME_UNLOAD) {
      project_unload_scene(projects, editor, frame);
    } else if (!projects->closing && resume == PROJECT_RESUME_SET_PRIMARY) {
      project_set_primary_begin(projects, frame);
    } else if (!projects->closing && resume == PROJECT_RESUME_BUILD) {
      /* A build packages the saved files; Discard leaves the unsaved edits
         in the editor rather than dropping them. */
      projects->discard_edits = false_v;
      projects->build_preflight = PROJECT_BUILD_PREFLIGHT_RESOLVED;
    } else if (!projects->closing && (projects->view == PROJECT_VIEW_SCENES ||
                                      resume == PROJECT_RESUME_JOB)) {
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
      if (project_scene_dirty(frame)) {
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
  if (drafting && project_button(ui, "create.submit",
                                 projects->view == PROJECT_VIEW_CREATE
                                     ? "Create project"
                                     : "Create and prepare",
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
      projects->placing_asset = false_v;
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
    projects->resume_action = PROJECT_RESUME_NONE;
  }
}

/* A floating dialog's size, fitted to what each view shows and to the
   window. */
static Vec2 project_dialog_size(const VkrEditorProjects *projects,
                                float32_t total_width, float32_t total_height) {
  /* Title row, body and the footer's message and buttons. */
  const float32_t chrome = 48.0f + 112.0f;
  float32_t width = 640.0f;
  float32_t body = 0.0f;
  switch (projects->view) {
  case PROJECT_VIEW_CONFIRM:
    width = 480.0f;
    body = 104.0f;
    break;
  case PROJECT_VIEW_RENAME:
    width = 460.0f;
    body = 136.0f;
    break;
  case PROJECT_VIEW_DELETE:
  case PROJECT_VIEW_DELETE_PROJECT:
    width = 480.0f;
    body = 224.0f;
    break;
  case PROJECT_VIEW_ADD_ENTITY:
    body = 550.0f;
    break;
  case PROJECT_VIEW_CREATE:
    /* Room for the template gallery beside the project fields. */
    width = 1040.0f;
    body = projects->form_height + 24.0f;
    break;
  case PROJECT_VIEW_ADD_SCENE:
    width = 720.0f;
    body = projects->form_height + 24.0f;
    break;
  case PROJECT_VIEW_SCENES:
    body = 48.0f +
           (projects->project ? projects->project->scene_count : 0u) * 56.0f +
           72.0f;
    break;
  default:
    body = projects->card_count * 70.0f + 160.0f;
    break;
  }
  return (Vec2){Min(width, Max(280.0f, total_width - 40.0f)),
                Min(body + chrome, Max(240.0f, total_height - 40.0f))};
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
  const float32_t total_width = ui->target_width / ui->content_scale;
  const float32_t total_height = ui->target_height / ui->content_scale;
  const VkrUiTrack one = {.unit = VKR_UI_TRACK_FR, .value = 1};
  /* Before a project opens, the Projects view is the whole launcher window;
   * afterwards each dialog is a compact floating window over the live
   * editor. */
  const bool8_t launcher = vkr_editor_projects_launcher(projects);
  const Vec2 size = project_dialog_size(projects, total_width, total_height);
  const float32_t width = launcher ? total_width : size.x;
  const float32_t height = launcher ? total_height : size.y;
  if (projects->dialog_view != projects->view) {
    /* A new dialog opens near the top centre and takes the keyboard. */
    projects->dialog_view = projects->view;
    projects->dialog_focus = true_v;
  }
  const Vec2 origin = {
      vkr_clamp_f32((total_width - width) * 0.5f + projects->dialog_offset_pt.x,
                    0.0f, Max(0.0f, total_width - width)),
      vkr_clamp_f32(72.0f + projects->dialog_offset_pt.y, 0.0f,
                    Max(0.0f, total_height - height))};
  const VkrUiRect bounds =
      launcher
          ? (VkrUiRect){0, 0, (float32_t)ui->target_width,
                        (float32_t)ui->target_height}
          : (VkrUiRect){origin.x * ui->content_scale,
                        origin.y * ui->content_scale, width * ui->content_scale,
                        height * ui->content_scale};
  projects->dialog_rect_px = bounds;
  const bool8_t inside = vkr_editor_projects_dialog_contains(
      projects, (float32_t)ui->mouse_x, (float32_t)ui->mouse_y);
  if (ui->mouse_pressed) {
    projects->dialog_focus = launcher || inside;
  }
  if (launcher) {
    *frame->modal = true_v;
  }
  (void)vkr_ui_input_layer_register(ui, PROJECT_MODAL_LAYER, bounds);
  (void)vkr_ui_input_layer_set(ui, PROJECT_MODAL_LAYER);
  if (launcher || projects->dialog_focus) {
    (void)vkr_ui_keyboard_layer_set(ui, PROJECT_MODAL_LAYER);
    vkr_ui_keyboard_navigation_enabled(ui, true_v);
  }
  if (launcher) {
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
  VkrUiPanelConfig modal = vkr_ui_panel_config_default();
  modal.placement.column = 0;
  modal.placement.row = 0;
  modal.placement.justify = launcher ? VKR_UI_ALIGN_CENTER : VKR_UI_ALIGN_START;
  modal.placement.align = launcher ? VKR_UI_ALIGN_CENTER : VKR_UI_ALIGN_START;
  modal.placement.margin_pt =
      launcher ? (VkrUiEdges){0} : (VkrUiEdges){origin.y, 0, 0, origin.x};
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
  heading.style.font_size_pt =
      launcher ? vkr_ui_theme()->font_heading : vkr_ui_theme()->font_body;
  heading.text.font = editor->heading_font;
  heading.style.text_color = vkr_ui_theme()->text;
  if (launcher) {
    vkr_ui_label(ui, string8_lit("title"), project_string(title), &heading);
  } else {
    /* The title bar drags the floating dialog. */
    heading.style.background_color = (Vec4){0};
    heading.style.hover_background_color = (Vec4){0};
    heading.style.active_background_color = (Vec4){0};
    heading.style.padding_pt = (VkrUiEdges){0, 4, 0, 4};
    heading.placement.justify = VKR_UI_ALIGN_START;
    const VkrUiId title_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("title"));
    (void)vkr_ui_button(ui, string8_lit("title"), project_string(title),
                        &heading);
    const Vec2 mouse = {(float32_t)ui->mouse_x / ui->content_scale,
                        (float32_t)ui->mouse_y / ui->content_scale};
    if (ui->active_id == title_id) {
      if (ui->mouse_pressed) {
        projects->dialog_grab_pt = mouse;
      }
      projects->dialog_offset_pt.x += mouse.x - projects->dialog_grab_pt.x;
      projects->dialog_offset_pt.y += mouse.y - projects->dialog_grab_pt.y;
      projects->dialog_grab_pt = mouse;
    }
  }
  const VkrUiTrack content = {
      .unit = VKR_UI_TRACK_PX,
      .value = projects->view == PROJECT_VIEW_CREATE ||
                       projects->view == PROJECT_VIEW_ADD_SCENE
                   ? Max(height - 160, projects->form_height + 24)
               : projects->view == PROJECT_VIEW_ADD_ENTITY ? 550
               : projects->view == PROJECT_VIEW_CONFIRM ||
                       projects->view == PROJECT_VIEW_SCENES ||
                       projects->view == PROJECT_VIEW_RENAME ||
                       projects->view == PROJECT_VIEW_DELETE ||
                       projects->view == PROJECT_VIEW_DELETE_PROJECT
                   ? height - 160
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
      /* The largest body this view can have, not this frame's fitted height,
         so the cards never size the dialog that sizes them. */
      const float32_t body_height =
          (launcher ? total_height : total_height - 40.0f) - 160.0f - 36.0f;
      project_build_create_form(projects, editor, frame, body_width,
                                body_height);
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
  if (projects->additive_job) {
    /* The open scene stays loaded; restore the paths its saves use. */
    projects->additive_job = false_v;
    snprintf(projects->runtime_path, sizeof(projects->runtime_path), "%s",
             projects->primary_runtime_path);
    snprintf(projects->scene_manifest_path,
             sizeof(projects->scene_manifest_path), "%s",
             projects->primary_manifest_path);
    snprintf(projects->edit_path, sizeof(projects->edit_path), "%s",
             projects->primary_edit_path);
    projects->scene_manifest_fingerprint =
        projects->primary_manifest_fingerprint;
  }
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
        project_yield_finalize(projects, editor);
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
  if (project_scene_dirty(frame)) {
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

VkrEditorBuildPreflight
vkr_editor_projects_build_preflight(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  if (!projects || !projects->project || projects->creating_project) {
    return VKR_EDITOR_BUILD_PREFLIGHT_UNAVAILABLE;
  }
  if (projects->build_preflight == PROJECT_BUILD_PREFLIGHT_PROMPTING) {
    if (projects->view == PROJECT_VIEW_CONFIRM) {
      return VKR_EDITOR_BUILD_PREFLIGHT_WAITING;
    }
    /* The prompt closed without Save or Discard. */
    projects->build_preflight = PROJECT_BUILD_PREFLIGHT_NONE;
    return VKR_EDITOR_BUILD_PREFLIGHT_CANCELLED;
  }
  if (projects->build_preflight == PROJECT_BUILD_PREFLIGHT_NONE &&
      project_any_dirty(frame)) {
    if (project_dialog_open(projects)) {
      return VKR_EDITOR_BUILD_PREFLIGHT_WAITING;
    }
    projects->build_preflight = PROJECT_BUILD_PREFLIGHT_PROMPTING;
    projects->resume_action = PROJECT_RESUME_BUILD;
    projects->resume_view = projects->view;
    projects->view = PROJECT_VIEW_CONFIRM;
    return VKR_EDITOR_BUILD_PREFLIGHT_WAITING;
  }
  projects->build_preflight = PROJECT_BUILD_PREFLIGHT_NONE;
  /* The preference writer drains, so project.json holds the viewport the
     package starts from. */
  return project_save_settings(projects, editor, frame->dock)
             ? VKR_EDITOR_BUILD_PREFLIGHT_READY
             : VKR_EDITOR_BUILD_PREFLIGHT_CANCELLED;
}

bool8_t vkr_editor_projects_adopt_inventory(
    VkrEditorProjects *projects, VkrEditorUi *editor,
    const VkrSampleUiFrame *frame, String8 assets,
    const uint8_t expected[VKR_SHA256_DIGEST_SIZE]) {
  if (!projects || !projects->project || projects->read_only ||
      !assets.length) {
    return false_v;
  }
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256(projects->project->assets.str, projects->project->assets.length,
             digest);
  if (MemCompare(digest, expected, sizeof(digest))) {
    log_info("Build: Content changed during the build; its finalized assets "
             "are not adopted");
    return false_v;
  }
  const String8 previous = projects->project->assets;
  VkrEditorProjectError error = {0};
  if (!project_replace_assets(projects, assets)) {
    return false_v;
  }
  if (!vkr_editor_project_save(projects->project, &error)) {
    projects->project->assets = previous;
    log_warn("Build: cannot publish the finalized Content: %s", error.message);
    return false_v;
  }
  vkr_editor_content_refresh(editor->content);
  /* World models name the finalized revisions, as after a finalize. */
  uint32_t moved = 0u;
  if (projects->world_path[0] &&
      !vkr_editor_project_world_refresh(
          projects->world_path, projects->project->assets,
          frame->ui->frame_allocator, &moved, &error)) {
    log_warn("Build: cannot point World models at the final textures: %s",
             error.message);
  }
  if (moved && !project_world_dirty(frame)) {
    project_reload_world(projects, frame);
  }
  log_info("Build: adopted the finalized project Content; open scenes use it "
           "the next time they open");
  return true_v;
}

const VkrEditorProject *
vkr_editor_projects_project(const VkrEditorProjects *projects) {
  return projects && !projects->creating_project ? projects->project : NULL;
}

const char *
vkr_editor_projects_workspace_root(const VkrEditorProjects *projects) {
  return projects ? projects->workspace.root : "";
}

bool8_t vkr_editor_projects_read_only(const VkrEditorProjects *projects) {
  return !projects || projects->read_only;
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

/* Runs the import step's choice (ADR-076). A placement is one job that
   imports the models and shows them: a new scene holding them opens, a
   project scene gains them and opens, or they join the World's root. Edits
   the placement would reload ask to be saved or discarded first; the form
   then waits for another Import. Returns false with a message when the
   choice cannot run. */
static bool8_t project_import_submit(VkrEditorProjects *projects,
                                     VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  if (projects->read_only || !projects->project ||
      !projects->import_source_count) {
    snprintf(projects->message, sizeof(projects->message),
             "Importing needs a writable project and a file.");
    return false_v;
  }
  const ProjectImportTarget target = project_import_models(projects)
                                         ? projects->import_target
                                         : PROJECT_IMPORT_CONTENT;
  if (target == PROJECT_IMPORT_CONTENT) {
    project_import_assets(projects, editor, frame, projects->import_sources[0]);
    return projects->job_id != 0;
  }
  const uint32_t count =
      Min(projects->import_source_count, PROJECT_MODEL_COUNT);
  if (target == PROJECT_IMPORT_NEW_SCENE) {
    char name[sizeof(projects->scene_name)];
    snprintf(name, sizeof(name), "%s", projects->scene_name);
    project_reset_scene_draft(projects);
    snprintf(projects->scene_name, sizeof(projects->scene_name), "%s", name);
  } else {
    project_reset_scene_draft(projects);
  }
  for (uint32_t i = 0; i < count; ++i) {
    snprintf(projects->models[i], sizeof(projects->models[i]), "%s",
             projects->import_sources[i]);
  }
  projects->model_count = count;
  projects->job_imports_models = true_v;
  if (target == PROJECT_IMPORT_NEW_SCENE) {
    project_create(projects, editor, frame);
    return projects->job_id != 0;
  }
  if (target == PROJECT_IMPORT_SCENE) {
    const uint32_t scene = projects->import_scene_index;
    if (scene >= projects->project->scene_count) {
      snprintf(projects->message, sizeof(projects->message),
               "Choose a scene to add the model to.");
      return false_v;
    }
    /* One document cannot be loaded twice: an added scene opens as the
       primary scene through Set primary first. */
    for (uint32_t slot = 0; slot < VKR_SCENE_ADDITIVE_MAX; ++slot) {
      if (frame->additive[slot] &&
          project_added_scene(projects, frame->additive_names[slot]) == scene) {
        snprintf(projects->message, sizeof(projects->message),
                 "%s is loaded beside the open scene; make it primary or "
                 "remove it first.",
                 projects->project->scenes[scene].name);
        return false_v;
      }
    }
    projects->adding_model = true_v;
    projects->placing_asset = false_v;
    projects->light_count = 0u;
    projects->pending_scene = scene;
    snprintf(projects->operation, sizeof(projects->operation), "add_entities");
    /* The scene opens with the model in place of the open one. */
    if (project_scene_dirty(frame)) {
      projects->resume_view = PROJECT_VIEW_EDITOR;
      projects->resume_action = PROJECT_RESUME_JOB;
      projects->view = PROJECT_VIEW_CONFIRM;
      return true_v;
    }
    project_start_job(projects, editor, frame, false_v);
    return projects->job_id != 0;
  }
  /* The World: the models import into Content, then join its root. */
  snprintf(projects->operation, sizeof(projects->operation),
           "import_project_assets");
  projects->action_asset[0] = '\0';
  projects->action_name[0] = '\0';
  snprintf(projects->action_source, sizeof(projects->action_source), "%s",
           projects->import_sources[0]);
  projects->job_world_models = true_v;
  if (project_world_dirty(frame)) {
    projects->resume_view = PROJECT_VIEW_EDITOR;
    projects->resume_action = PROJECT_RESUME_JOB;
    projects->view = PROJECT_VIEW_CONFIRM;
    return true_v;
  }
  project_start_job(projects, editor, frame, false_v);
  return projects->job_id != 0;
}

/* Import step for dropped or chosen files (ADR-076): what arrives, where
   models go, and the Content folder their assets are filed in. */
static void project_build_import_step(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      float32_t width, bool8_t busy) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t models = project_import_models(projects);
  /* The placement choices need more room than a saved window may have. */
  VkrEditorWindowState *window = &editor->windows[VKR_EDITOR_WINDOW_CREATE];
  window->size_pt.y = Max(window->size_pt.y, models ? 400.0f : 300.0f);
  project_label(ui, "import.title",
                projects->import_source_count == 1u ? "Import 1 file"
                                                    : "Import dropped files",
                16, 12, width - 32);
  const uint32_t shown = Min(projects->import_source_count, 3u);
  for (uint32_t i = 0; i < shown; ++i) {
    const char *path = projects->import_sources[i];
    const char *name = path;
    for (const char *c = path; *c; ++c) {
      if (*c == '/' || *c == '\\') {
        name = c + 1;
      }
    }
    VkrUiWidgetConfig row = project_widget(16, 40 + 24.0f * i, width - 32, 22);
    row.icon = project_model_file(path) ? VKR_UI_ICON_MESH : VKR_UI_ICON_FILE;
    row.icon_size_pt = 13.0f;
    row.tooltip = project_string(path);
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("import.file"), project_string(name), &row);
    (void)vkr_ui_pop_id(ui);
  }
  if (projects->import_source_count > shown) {
    VkrUiWidgetConfig more =
        project_widget(16, 40 + 24.0f * shown, width - 32, 22);
    more.style.text_color = theme->text_secondary;
    vkr_ui_label(
        ui, string8_lit("import.more"),
        string8_create_formatted(ui->frame_allocator, "and %u more",
                                 projects->import_source_count - shown),
        &more);
  }
  float32_t y = 40.0f + 24.0f * (shown + 1u) + 4.0f;
  if (models) {
    project_label(ui, "import.place", "Place the model in", 16, y, width - 32);
    y += 28.0f;
    static const char *const targets[PROJECT_IMPORT_TARGET_COUNT] = {
        "New scene", "World", "Scene", "Content only"};
    const float32_t segment_width = (width - 32) / PROJECT_IMPORT_TARGET_COUNT;
    for (uint32_t i = 0; i < PROJECT_IMPORT_TARGET_COUNT; ++i) {
      const bool8_t selected =
          projects->import_target == (ProjectImportTarget)i;
      VkrUiWidgetConfig segment =
          project_widget(16 + i * segment_width, y, segment_width, 30);
      vkr_editor_toggle_style(&segment, selected);
      if (!selected) {
        segment.style.background_color = theme->field;
        segment.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
        segment.style.border_color = theme->border;
      }
      segment.style.corner_radius_pt = i == 0 ? (Vec4){6, 0, 0, 6}
                                       : i + 1 == PROJECT_IMPORT_TARGET_COUNT
                                           ? (Vec4){0, 6, 6, 0}
                                           : (Vec4){0, 0, 0, 0};
      /* Scene needs a project scene to add to. */
      segment.disabled =
          i == PROJECT_IMPORT_SCENE && !projects->project->scene_count;
      (void)vkr_ui_push_id_u64(ui, i);
      if (vkr_ui_button(ui, string8_lit("import.target"),
                        project_string(targets[i]), &segment)) {
        projects->import_target = (ProjectImportTarget)i;
        projects->message[0] = '\0';
      }
      (void)vkr_ui_pop_id(ui);
    }
    y += 40.0f;
    const char *detail = "";
    switch (projects->import_target) {
    case PROJECT_IMPORT_NEW_SCENE:
      project_field(ui, "import.scene_name", projects->scene_name,
                    sizeof(projects->scene_name), 16, y, width - 32);
      y += 36.0f;
      detail = "Creates a scene holding the model and opens it.";
      break;
    case PROJECT_IMPORT_WORLD:
      detail = "Adds the model at the World's root; every scene that uses "
               "the World shows it.";
      break;
    case PROJECT_IMPORT_SCENE: {
      const uint32_t scene_count = projects->project->scene_count;
      if (scene_count) {
        projects->import_scene_index %= scene_count;
        if (project_button(ui, "import.scene.previous", "<", 16, y, 36,
                           scene_count < 2u)) {
          projects->import_scene_index =
              (projects->import_scene_index + scene_count - 1u) % scene_count;
        }
        VkrUiWidgetConfig chosen = project_widget(58, y, width - 116, 30);
        vkr_editor_field_style(&chosen);
        chosen.icon = VKR_UI_ICON_SCENE;
        chosen.icon_size_pt = 14.0f;
        vkr_ui_label(
            ui, string8_lit("import.scene"),
            project_string(
                projects->project->scenes[projects->import_scene_index].name),
            &chosen);
        if (project_button(ui, "import.scene.next", ">", width - 52, y, 36,
                           scene_count < 2u)) {
          projects->import_scene_index =
              (projects->import_scene_index + 1u) % scene_count;
        }
        y += 36.0f;
      }
      detail = "Adds the model to the scene and opens it.";
      break;
    }
    default:
      detail = "Imports the assets without placing them.";
      break;
    }
    project_label(ui, "import.detail", detail, 16, y, width - 32);
    y += 28.0f;
  }
  VkrUiWidgetConfig into = project_widget(16, y, width - 32, 22);
  into.icon = VKR_UI_ICON_FOLDER;
  into.icon_color = (Vec4){0.96f, 0.78f, 0.42f, 1.0f};
  vkr_ui_label(ui, string8_lit("import.folder"),
               string8_create_formatted(ui->frame_allocator,
                                        "Assets into Content%s%s",
                                        projects->import_folder[0] ? "/" : "",
                                        projects->import_folder),
               &into);
  y += 26.0f;
  project_label(ui, "create.message", projects->message, 16, y, width - 32);
  y += 30.0f;
  if (project_button(ui, "import.cancel", "Cancel", 16, y, 96, false_v)) {
    projects->import_source_count = 0u;
    projects->create_step = PROJECT_CREATE_CHOOSE;
    editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = false_v;
  }
  if (project_button(ui, "import.submit", "Import", width - 136, y, 120,
                     busy || !projects->import_source_count) &&
      project_import_submit(projects, editor, frame)) {
    editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = false_v;
    projects->create_step = PROJECT_CREATE_CHOOSE;
  }
}

/* Preflight summary of the chosen scene JSON (editor-projects import): a new
   source starts a read-only inspection; missing dependencies can be located
   under another folder. Returns the y where the form continues. */
static float32_t project_build_inspection(VkrEditorProjects *projects,
                                          VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame,
                                          float32_t width, bool8_t busy) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  ProjectInspection *inspection = &projects->inspection;
  if (projects->source_scene[0] && !projects->job_id && !busy &&
      strcmp(projects->inspected_source, projects->source_scene)) {
    snprintf(projects->inspected_source, sizeof(projects->inspected_source),
             "%s", projects->source_scene);
    *inspection = (ProjectInspection){0};
    snprintf(projects->operation, sizeof(projects->operation), "inspect_scene");
    project_start_job(projects, editor, frame, false_v);
  }
  float32_t y = 164.0f;
  VkrUiWidgetConfig line = project_widget(16, y, width - 32, 24);
  line.style.font_size_pt = theme->font_caption;
  line.style.text_color = theme->text_secondary;
  if (projects->job_inspect) {
    vkr_ui_label(ui, string8_lit("inspect.status"),
                 string8_lit("Inspecting the scene and its dependencies..."),
                 &line);
    return y + 30.0f;
  }
  if (inspection->error[0]) {
    line.style.text_color = theme->warning;
    vkr_ui_label(ui, string8_lit("inspect.status"),
                 project_string(inspection->error), &line);
    return y + 30.0f;
  }
  if (!inspection->valid) {
    return y;
  }
  vkr_ui_label(ui, string8_lit("inspect.status"),
               string8_create_formatted(
                   ui->frame_allocator,
                   "Scene version %d \xc2\xb7 %u entit%s \xc2\xb7 %u mesh%s "
                   "\xc2\xb7 %u material%s",
                   inspection->scene_version, inspection->entities,
                   inspection->entities == 1u ? "y" : "ies", inspection->meshes,
                   inspection->meshes == 1u ? "" : "es", inspection->materials,
                   inspection->materials == 1u ? "" : "s"),
               &line);
  y += 26.0f;
  if (inspection->missing_count) {
    VkrUiWidgetConfig missing = project_widget(16, y, width - 150, 24);
    missing.style.font_size_pt = theme->font_caption;
    missing.style.text_color = theme->warning;
    missing.icon = VKR_UI_ICON_LOG_WARNING;
    missing.icon_color = theme->warning;
    missing.tooltip = string8_create_formatted(
        ui->frame_allocator, "%s\n%s\n%s", inspection->missing[0],
        inspection->missing[1], inspection->missing[2]);
    vkr_ui_label(ui, string8_lit("inspect.missing"),
                 string8_create_formatted(
                     ui->frame_allocator, "%u missing, such as %s",
                     inspection->missing_count, inspection->missing[0]),
                 &missing);
    if (project_button(ui, "inspect.locate", "Locate folder", width - 130, y,
                       114, busy)) {
      project_browse(projects, frame,
                     "Locate the folder that holds the missing files", NULL, 0,
                     true_v, projects->legacy_root,
                     sizeof(projects->legacy_root));
      /* Inspect again against the located folder. */
      projects->inspected_source[0] = '\0';
    }
    y += 30.0f;
  } else if (inspection->warning_count) {
    VkrUiWidgetConfig note = line;
    note.placement.margin_pt.top = y;
    note.tooltip = project_string(inspection->warning);
    vkr_ui_label(
        ui, string8_lit("inspect.warning"),
        string8_create_formatted(
            ui->frame_allocator, "%u note%s: %s", inspection->warning_count,
            inspection->warning_count == 1u ? "" : "s", inspection->warning),
        &note);
    y += 26.0f;
  }
  /* Placeholders can stand in only when images are all that is missing. */
  if (inspection->missing_count &&
      inspection->missing_images == inspection->missing_count) {
    project_check(ui, "inspect.placeholders",
                  "Use placeholders for missing images",
                  &projects->use_placeholders, 16, y, width - 32);
    y += 30.0f;
  }
  if (inspection->saved_edits) {
    project_check(ui, "inspect.edits", "Include saved scene edits",
                  &projects->include_edits, 16, y, width - 32);
    y += 30.0f;
  }
  return y + 4.0f;
}

/* One large choice: icon, title and a line of explanation. */
static bool8_t project_create_choice(VkrUiSystem *ui, const char *id,
                                     VkrUiIcon icon, const char *title,
                                     const char *detail, float32_t y,
                                     float32_t width, bool8_t disabled) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig card = project_widget(16, y, width - 32, 72);
  vkr_editor_ghost_style(&card);
  card.fill = true_v;
  card.disabled = disabled;
  card.style.background_color = theme->field;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt = (Vec4){8, 8, 8, 8};
  card.tooltip = project_string(detail);
  const bool8_t clicked =
      vkr_ui_button(ui, project_string(id), (String8){0}, &card);
  VkrUiWidgetConfig mark = project_widget(30, y + 18, 36, 36);
  mark.icon = icon;
  mark.icon_size_pt = 22.0f;
  mark.icon_color = disabled ? theme->text_disabled : theme->accent_hover;
  vkr_ui_label(ui, string8_lit("mark"), (String8){0}, &mark);
  VkrUiWidgetConfig heading = project_widget(80, y + 10, width - 112, 26);
  heading.style.text_color = disabled ? theme->text_disabled : theme->text;
  heading.style.font_size_pt = theme->font_body + 1.0f;
  vkr_ui_label(ui, string8_lit("title"), project_string(title), &heading);
  VkrUiWidgetConfig line = project_widget(80, y + 36, width - 112, 26);
  line.style.text_color = theme->text_secondary;
  line.style.font_size_pt = theme->font_caption;
  vkr_ui_label(ui, string8_lit("detail"), project_string(detail), &line);
  return clicked && !disabled;
}

void vkr_editor_projects_build_create_window(VkrEditorProjects *projects,
                                             VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame,
                                             VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  const float32_t width = bounds.width / ui->content_scale;
  if (!projects || !projects->project || width < 240.0f) {
    return;
  }
  const bool8_t busy =
      projects->read_only || projects->job_id || projects->waiting_activation;
  if (projects->create_step == PROJECT_CREATE_CHOOSE) {

    (void)vkr_ui_push_id_label(ui, string8_lit("create.scene"));
    if (project_create_choice(
            ui, "choice", VKR_UI_ICON_SCENE, "Create scene",
            "A new scene in this project; the World supplies sky and sun.", 16,
            width, busy)) {
      project_reset_scene_draft(projects);
      projects->create_step = PROJECT_CREATE_SCENE;
    }
    (void)vkr_ui_pop_id(ui);
    (void)vkr_ui_push_id_label(ui, string8_lit("create.asset"));
    if (project_create_choice(
            ui, "choice", VKR_UI_ICON_IMPORT, "Import asset",
            "Models, textures and fonts every scene of this project can use.",
            100, width, busy)) {
      editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = false_v;
      projects->import_source_count = 0u;
      const VkrEditorContentAction action = {
          .kind = VKR_EDITOR_CONTENT_ACTION_IMPORT};
      project_content_action(projects, editor, frame, &action);
    }
    (void)vkr_ui_pop_id(ui);
    project_label(ui, "create.message", projects->message, 16, 186, width - 32);
    return;
  }

  if (projects->create_step == PROJECT_CREATE_IMPORT) {
    project_build_import_step(projects, editor, frame, width, busy);
    return;
  }

  /* Scene step: a name and whether it starts empty or from a JSON file. */
  project_label(ui, "create.name.label", "Scene name", 16, 12, width - 32);
  project_field(ui, "create.name", projects->scene_name,
                sizeof(projects->scene_name), 16, 40, width - 32);
  static const char *const modes[] = {"Empty scene", "Import scene JSON"};
  const float32_t half = (width - 32) * 0.5f;
  const VkrUiTheme *theme = vkr_ui_theme();
  for (uint32_t i = 0; i < 2; ++i) {
    const bool8_t selected = projects->import_scene == (i == 1);
    VkrUiWidgetConfig segment = project_widget(16 + i * half, 84, half, 30);
    vkr_editor_toggle_style(&segment, selected);
    if (!selected) {
      segment.style.background_color = theme->field;
      segment.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      segment.style.border_color = theme->border;
    }
    segment.style.corner_radius_pt =
        i == 0 ? (Vec4){6, 0, 0, 6} : (Vec4){0, 6, 6, 0};
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("create.mode"), project_string(modes[i]),
                      &segment)) {
      projects->import_scene = i == 1;
    }
    (void)vkr_ui_pop_id(ui);
  }
  float32_t footer = 170.0f;
  if (projects->import_scene) {
    /* The preflight summary needs room a smaller saved window lacks. */
    VkrEditorWindowState *window = &editor->windows[VKR_EDITOR_WINDOW_CREATE];
    window->size_pt.y = Max(window->size_pt.y, 360.0f);
    project_field(ui, "create.source", projects->source_scene,
                  sizeof(projects->source_scene), 16, 128, width - 128);
    if (project_button(ui, "create.browse", "Browse", width - 104, 128, 88,
                       busy)) {
      static const char *const extensions[] = {"json"};
      project_browse(projects, frame, "Import scene JSON", extensions, 1,
                     false_v, projects->source_scene,
                     sizeof(projects->source_scene));
    }
    footer = project_build_inspection(projects, editor, frame, width, busy);
  } else {
    project_label(ui, "create.empty",
                  "Starts empty. Add objects from the Outliner or drag assets "
                  "into the Scene.",
                  16, 128, width - 32);
  }
  project_label(ui, "create.message", projects->message, 16, footer,
                width - 32);
  if (project_button(ui, "create.back", "Back", 16, footer + 44, 96, false_v)) {
    projects->create_step = PROJECT_CREATE_CHOOSE;
    projects->message[0] = '\0';
  }
  /* An imported scene waits for a clean preflight. */
  const ProjectInspection *inspection = &projects->inspection;
  const bool8_t unready =
      projects->import_scene &&
      (!projects->source_scene[0] || projects->job_inspect ||
       strcmp(projects->inspected_source, projects->source_scene) ||
       !inspection->valid ||
       (inspection->missing_count &&
        !(projects->use_placeholders &&
          inspection->missing_images == inspection->missing_count)));
  if (project_button(ui, "create.submit", "Create scene", width - 156,
                     footer + 44, 140, busy || unready)) {
    project_create(projects, editor, frame);
    if (projects->job_id) {
      editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = false_v;
      projects->create_step = PROJECT_CREATE_CHOOSE;
    }
  }
}

bool8_t vkr_editor_projects_create_scene(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *name) {
  if (!projects || !projects->project || projects->read_only ||
      projects->job_id || projects->waiting_activation) {
    return false_v;
  }
  project_reset_scene_draft(projects);
  snprintf(projects->scene_name, sizeof(projects->scene_name), "%s", name);
  project_create(projects, editor, frame);
  return projects->job_id != 0;
}

String8 vkr_editor_projects_scene_name(const VkrEditorProjects *projects) {
  if (!projects || !projects->project ||
      projects->active_scene >= projects->project->scene_count) {
    return (String8){0};
  }
  return project_string(projects->project->scenes[projects->active_scene].name);
}

bool8_t vkr_editor_projects_open_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       String8 name, bool8_t *out_current) {
  *out_current = false_v;
  if (!projects || !projects->project || !name.length) {
    return false_v;
  }
  uint32_t match = UINT32_MAX;
  for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
    const String8 candidate = project_string(projects->project->scenes[i].name);
    if (string8_equals(&candidate, &name)) {
      match = i;
      break;
    }
    if (match == UINT32_MAX && string8_contains(&candidate, &name)) {
      match = i;
    }
  }
  /* The scene being loaded or already open stays as it is. */
  if (match == UINT32_MAX) {
    return false_v;
  }
  if (project_scene_current(projects, frame, match)) {
    *out_current = true_v;
    return true_v;
  }
  if (projects->job_id || projects->waiting_activation) {
    return false_v;
  }
  project_open_scene_id(projects, editor, frame,
                        projects->project->scenes[match].id);
  return true_v;
}

String8 vkr_editor_projects_scene_id(const VkrEditorProjects *projects) {
  if (!projects || !projects->project ||
      projects->active_scene >= projects->project->scene_count) {
    return (String8){0};
  }
  return project_string(projects->project->scenes[projects->active_scene].id);
}

bool8_t vkr_editor_projects_switch_ready(const VkrEditorProjects *projects) {
  return projects && projects->project &&
         projects->view == PROJECT_VIEW_EDITOR && !projects->job_id &&
         !projects->waiting_activation;
}

bool8_t vkr_editor_projects_show_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       const char *scene_id) {
  if (!vkr_editor_projects_switch_ready(projects)) {
    return false_v;
  }
  if (scene_id[0]) {
    project_open_scene_id(projects, editor, frame, scene_id);
    return true_v;
  }
  if (projects->active_scene >= projects->project->scene_count) {
    return true_v;
  }
  /* Unsaved edits ask first; the switch completes after Save or Discard. */
  if (project_scene_dirty(frame) && !projects->discard_edits) {
    projects->resume_view = PROJECT_VIEW_EDITOR;
    projects->resume_action = PROJECT_RESUME_UNLOAD;
    projects->view = PROJECT_VIEW_CONFIRM;
    return true_v;
  }
  project_unload_scene(projects, editor, frame);
  return true_v;
}

String8 vkr_editor_projects_added_name(const VkrEditorProjects *projects,
                                       String8 path) {
  const uint32_t scene =
      projects ? project_added_scene(projects, path) : UINT32_MAX;
  return scene == UINT32_MAX
             ? (String8){0}
             : project_string(projects->project->scenes[scene].name);
}

const char *vkr_editor_projects_added_id(const VkrEditorProjects *projects,
                                         String8 path) {
  const uint32_t scene =
      projects ? project_added_scene(projects, path) : UINT32_MAX;
  return scene == UINT32_MAX ? "" : projects->project->scenes[scene].id;
}

bool8_t vkr_editor_projects_add_scene(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 name) {
  if (!vkr_editor_projects_switch_ready(projects) || !name.length ||
      projects->active_scene >= projects->project->scene_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
    const String8 candidate = project_string(projects->project->scenes[i].name);
    if (i != projects->active_scene && string8_equals(&candidate, &name)) {
      project_add_scene(projects, editor, frame, i);
      return projects->job_id != 0;
    }
  }
  return false_v;
}

bool8_t vkr_editor_projects_instantiate_scene(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame,
                                              String8 name, Vec3 position) {
  if (!vkr_editor_projects_switch_ready(projects) ||
      !vkr_editor_projects_can_add_entity(projects, editor, frame)) {
    return false_v;
  }
  uint32_t scene = UINT32_MAX;
  for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
    const String8 candidate = project_string(projects->project->scenes[i].name);
    if (i != projects->active_scene && string8_equals(&candidate, &name)) {
      scene = i;
      break;
    }
  }
  if (scene == UINT32_MAX) {
    return false_v;
  }
  project_reset_scene_draft(projects);
  projects->adding_model = false_v;
  projects->light_count = 0;
  projects->placing_asset = true_v;
  projects->place_prefab = true_v;
  projects->place_position = position;
  snprintf(projects->place_asset, sizeof(projects->place_asset), "%s",
           projects->project->scenes[scene].id);
  /* The job names the instance after the scene. */
  projects->place_name[0] = '\0';
  projects->pending_scene = projects->active_scene;
  snprintf(projects->operation, sizeof(projects->operation), "add_entities");
  /* The scene reloads with the instance, so unsaved edits ask first. */
  if (project_scene_dirty(frame)) {
    projects->resume_view = PROJECT_VIEW_EDITOR;
    projects->resume_action = PROJECT_RESUME_JOB;
    projects->view = PROJECT_VIEW_CONFIRM;
  } else {
    project_start_job(projects, editor, frame, false_v);
  }
  return true_v;
}

bool8_t vkr_editor_projects_set_primary(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint16_t container) {
  const uint32_t slot = container - 1u;
  if (!vkr_editor_projects_switch_ready(projects) ||
      slot >= VKR_SCENE_ADDITIVE_MAX || !frame->additive[slot]) {
    return false_v;
  }
  const uint32_t scene =
      project_added_scene(projects, frame->additive_names[slot]);
  if (scene == UINT32_MAX) {
    snprintf(projects->message, sizeof(projects->message),
             "Only a project scene can become the primary scene.");
    return false_v;
  }
  (void)editor;
  projects->swap_container = container;
  projects->swap_scene = scene;
  projects->swap_back = projects->active_scene;
  /* Both scenes reload, so their unsaved edits ask first. */
  if (project_scene_dirty(frame) ||
      frame->additive_edits[slot]->revision !=
          frame->additive_edits[slot]->saved_revision) {
    projects->resume_view = PROJECT_VIEW_EDITOR;
    projects->resume_action = PROJECT_RESUME_SET_PRIMARY;
    projects->view = PROJECT_VIEW_CONFIRM;
    return true_v;
  }
  project_set_primary_begin(projects, frame);
  return true_v;
}

bool8_t vkr_editor_projects_import_asset(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *source) {
  if (!vkr_editor_projects_switch_ready(projects) || !source || !source[0]) {
    return false_v;
  }
  project_import_assets(projects, editor, frame, source);
  return projects->job_id != 0;
}

bool8_t vkr_editor_projects_import_files(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrWindowFileDrop *drop,
                                         const char *folder) {
  if (!projects || !projects->project || projects->read_only ||
      projects->view != PROJECT_VIEW_EDITOR || !drop || !drop->count) {
    return false_v;
  }
  projects->import_source_count = 0u;
  for (uint32_t i = 0; i < drop->count && i < VKR_WINDOW_DROP_PATH_MAX; ++i) {
    snprintf(projects->import_sources[projects->import_source_count++],
             sizeof(projects->import_sources[0]), "%s", drop->paths[i]);
  }
  snprintf(projects->import_folder, sizeof(projects->import_folder), "%s",
           folder ? folder : "");
  project_open_import_step(projects, editor);
  return true_v;
}

bool8_t vkr_editor_projects_import_to(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      const char *source, String8 target,
                                      String8 name) {
  if (!vkr_editor_projects_switch_ready(projects) || projects->read_only ||
      !source || !source[0] ||
      strlen(source) >= sizeof(projects->import_sources[0])) {
    if (projects) {
      snprintf(projects->message, sizeof(projects->message),
               "No writable, idle project can import now.");
    }
    return false_v;
  }
  snprintf(projects->import_sources[0], sizeof(projects->import_sources[0]),
           "%s", source);
  projects->import_source_count = 1u;
  snprintf(projects->import_folder, sizeof(projects->import_folder), "%s",
           vkr_editor_content_folder(editor->content));
  project_open_import_step(projects, editor);
  editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = false_v;
  projects->create_step = PROJECT_CREATE_CHOOSE;
  static const char *const targets[PROJECT_IMPORT_TARGET_COUNT] = {
      "new", "world", "scene", "content"};
  uint32_t chosen = PROJECT_IMPORT_TARGET_COUNT;
  for (uint32_t i = 0; i < PROJECT_IMPORT_TARGET_COUNT; ++i) {
    const String8 word = project_string(targets[i]);
    if (string8_equals(&target, &word)) {
      chosen = i;
    }
  }
  if (chosen == PROJECT_IMPORT_TARGET_COUNT) {
    snprintf(projects->message, sizeof(projects->message),
             "Choose world, new <name>, scene <name> or content.");
    return false_v;
  }
  projects->import_target = (ProjectImportTarget)chosen;
  if (chosen == PROJECT_IMPORT_NEW_SCENE && name.length) {
    snprintf(projects->scene_name, sizeof(projects->scene_name), "%.*s",
             (int)name.length, (const char *)name.str);
  }
  if (chosen == PROJECT_IMPORT_SCENE) {
    projects->import_scene_index = UINT32_MAX;
    for (uint32_t i = 0; i < projects->project->scene_count; ++i) {
      const String8 candidate =
          project_string(projects->project->scenes[i].name);
      if (string8_equals(&candidate, &name)) {
        projects->import_scene_index = i;
      }
    }
  }
  return project_import_submit(projects, editor, frame);
}

const char *vkr_editor_projects_message(const VkrEditorProjects *projects) {
  return projects ? projects->message : "";
}

bool8_t vkr_editor_projects_import_scene_form(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const char *source) {
  if (!projects || !projects->project || projects->read_only || !source ||
      !source[0] || strlen(source) >= sizeof(projects->source_scene)) {
    return false_v;
  }
  project_reset_scene_draft(projects);
  projects->import_scene = true_v;
  snprintf(projects->source_scene, sizeof(projects->source_scene), "%s",
           source);
  projects->create_step = PROJECT_CREATE_SCENE;
  editor->windows[VKR_EDITOR_WINDOW_CREATE].visible = true_v;
  return true_v;
}

#include "editor_content.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_json.h"
#include "core/vkr_threads.h"
#include "editor_install.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include "editor_scene_panels.h"
#include "editor_scripts.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#define CONTENT_MAX_ASSETS 8192u
#define CONTENT_CACHE_COUNT 64u
#define CONTENT_NONE UINT32_MAX
#define CONTENT_PATH 1024u
/* Virtual folders and tags (ADR-076): a folder tree and labels over item IDs,
   stored beside project.json; files never move. Content is the World: its
   root lists the World's objects, the project's scenes, assets and folders,
   and each scene is a folder of its objects and assets. System holds what
   the editor ships: Assets, Objects and Editor. */
#define CONTENT_LABEL_MAX 4096u
#define CONTENT_FOLDER_MAX 256u
/* The root, the system folders and every project folder. */
#define CONTENT_TREE_MAX (CONTENT_FOLDER_MAX + 128u)
#define CONTENT_TAG_MAX 16u
/* A shown entry with this bit names a folder tile by tree index. */
#define CONTENT_FOLDER_BIT 0x80000000u
/* Card sizes; the smallest shows one row per item. */
#define CONTENT_SIZE_LIST 64u
/* Grid rows built per frame: card rows, or list lines in the list view. */
#define CONTENT_LIST_ROWS 32u
#define CONTENT_TREE_ROW_PT 24.0f
/* Items the editor provides rather than a file: one object per creatable
   kind, and the objects of loaded containers. */
#define CONTENT_SCOPE_BUILTIN 3u
/* A scene's folder path is this byte and its id; users cannot type it. */
#define CONTENT_SCENE_MARK '\x01'
#define CONTENT_SYSTEM "System"

typedef enum ContentKind {
  CONTENT_TEXTURE,
  CONTENT_MATERIAL,
  CONTENT_MESH,
  CONTENT_FONT,
  CONTENT_ENVIRONMENT,
  CONTENT_SCENE,
  CONTENT_PROBE,
  CONTENT_ANIMATION,
  /* An object of a loaded World or scene. */
  CONTENT_ENTITY,
  /* A creatable object kind, in System/Objects. */
  CONTENT_OBJECT,
  CONTENT_PRESET,
  /* A project script source (ADR-079). */
  CONTENT_SCRIPT,
  CONTENT_OTHER,
  CONTENT_KIND_COUNT
} ContentKind;

typedef enum ContentSort {
  CONTENT_SORT_NAME,
  CONTENT_SORT_TYPE,
  CONTENT_SORT_LOCATION,
  CONTENT_SORT_TAGS,
  CONTENT_SORT_COUNT
} ContentSort;

typedef struct ContentAsset {
  char id[37];
  char name[513];
  char path[CONTENT_PATH];
  char source[CONTENT_PATH];
  char role[64];
  char fingerprint[96];
  char diagnostic[256];
  /* Build recipe, such as "mesh 1", and the dependency files it read. */
  char recipe[48];
  uint32_t dependencies;
  /* Texture size from the artifact header, read once when inspected. */
  uint32_t width;
  uint32_t height;
  bool8_t size_read;
  ContentKind kind;
  uint32_t scope;
  /* Object kind of a CONTENT_OBJECT item; preset index of a CONTENT_PRESET
     item. */
  uint32_t object;
  /* A scene's own asset or object lists in that scene's folder; empty for
     everything else. */
  char home[40];
  /* An asset's type folder in its home, such as "<scene>/Textures". */
  char type_folder[64];
  /* A CONTENT_ENTITY: the object and its Outliner icon. */
  VkrEntityId entity;
  VkrUiIcon entity_icon;
  /* Index into the labels document, resolved when the listing rebuilds. */
  uint32_t label;
  bool8_t missing;
  bool8_t stale;
  /* A source or artifact file changed on disk since the listing was read. */
  bool8_t changed;
  /* Its textures are still at the deferred or preview tier, or it shares a
     build revision with such a mesh: a background finalize rebuilds it
     (ADR-077). `revision` names its artifact's build directory. */
  bool8_t pending_tier;
  char revision[37];
  /* A CONTENT_ENTITY whose model renders a mesh a background finalize
     rebuilds. */
  bool8_t cooking_object;
} ContentAsset;

/* One row of the folder tree; parents precede their children. */
typedef struct ContentFolder {
  char path[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  /* Display name and icon: a scene's name, or the last path name. */
  char label[128];
  VkrUiIcon icon;
  uint32_t parent;
  uint32_t depth;
  /* Provided by the editor: it cannot be renamed, moved or deleted. */
  bool8_t system;
  bool8_t open;
  bool8_t shown;
  bool8_t has_children;
} ContentFolder;

typedef struct ContentPreview {
  uint64_t key;
  uint64_t last_seen;
  uint32_t asset;
  uint32_t size;
  uint32_t width;
  uint32_t height;
  char path[CONTENT_PATH];
  char request_path[CONTENT_PATH + 64];
  char diagnostic[256];
  VkrResourceHandleInfo request;
  VkrTextureHandle texture;
  bool8_t queued;
  bool8_t failed;
} ContentPreview;

struct VkrEditorContent {
  VkrAllocator *allocator;
  VkrRenderAssets *assets;
  ContentAsset *entries;
  uint32_t count;
  uint32_t capacity;
  /* Entries from files and built-ins come first; loaded containers' objects
     follow and are replaced when their key changes. */
  uint32_t asset_count;
  uint64_t objects_key;
  /* Shown folder tiles, then items: asset indices, or tree indices with
     CONTENT_FOLDER_BIT. */
  uint32_t *filtered;
  uint32_t filtered_count;
  uint32_t uploads_this_frame;
  uint32_t selected;
  uint32_t first_row;
  /* The grid scrolls the selection into view once, after a reveal. */
  bool8_t reveal_selection;
  float32_t tree_scroll;
  Keys navigation_key;
  float64_t navigation_elapsed;
  float64_t navigation_next;
  bool8_t details_hidden;
  uint32_t size;
  ContentSort sort;
  bool8_t reverse_sort;
  bool8_t filter_dirty;
  bool8_t suspended;
  bool8_t read_only;
  /* A background finalize rebuilds the project's Content imports or one
     scene's assets; their pending cards are locked until it ends. */
  bool8_t cooking_project;
  char cooking_scene[37];
  /* Project meshes of a World streaming in, and the scene whose meshes
     stream in; their cards show loading and are locked (ADR-076). */
  char loading_ids[32][37];
  uint32_t loading_count;
  char loading_scene[37];
  /* When object cooking marks were last computed, and for which cooking
     revisions. */
  float64_t objects_marked_at;
  uint64_t objects_marked_key;
  char workspace[CONTENT_PATH];
  char project[37];
  char scene[37];
  char diagnostic[512];
  uint8_t query[128];
  uint32_t query_length;
  uint8_t rename[513];
  uint32_t rename_length;
  char rename_asset[37];
  /* The project's script modules and the file list listed last. */
  const VkrEditorScripts *scripts;
  uint64_t scripts_revision;
  /* Double-click and drag of one item or folder (ADR-076). */
  uint32_t click_asset;
  float64_t click_time;
  uint32_t drag_asset;
  Vec2 drag_origin;
  bool8_t dragging;
  VkrUiRect drop_target;
  /* Folder under a dragged item this frame, found while building. */
  char drop_folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  bool8_t drop_folder_set;
  /* A right press this frame, from the editor's input, and the item whose
     context menu it opens (ADR-076). */
  bool8_t right_pressed;
  bool8_t menu_requested;
  /* The press landed on the shown folder's empty space, not an item. */
  bool8_t menu_folder;
  uint32_t menu_item;
  Vec2 menu_position_pt;
  /* An object created from Content is revealed once it becomes the
     selection; frames left to wait for it. */
  VkrEntityId reveal_previous;
  uint32_t reveal_frames;
  /* The item whose name is being edited in place. */
  uint32_t renaming;
  uint8_t rename_draft[513];
  uint32_t rename_draft_length;
  bool8_t rename_focus;
  /* Enter and Escape this frame, from the editor's input. */
  bool8_t rename_enter;
  bool8_t rename_escape;
  /* Pixel of an OS file drop this frame; folder widgets probe it like a
     dragged item. */
  Vec2 file_drop_px;
  bool8_t file_drop_armed;
  uint64_t generation;
  uint64_t inventory_key;
  uint64_t frame;
  ContentPreview cache[CONTENT_CACHE_COUNT];
  VkrThread worker;
  VkrPlatformProcessLock process_lock;
  VkrAtomicBool cancel;
  VkrAtomicBool complete;
  uint32_t running;
  uint64_t worker_generation;
  char worker_input[CONTENT_PATH];
  char worker_output[CONTENT_PATH];
  char worker_log[CONTENT_PATH];
  char worker_workspace[CONTENT_PATH];
  uint32_t worker_size;
  bool8_t worker_material;
  bool8_t worker_reveal;
  bool8_t worker_ok;
  bool8_t worker_timed_out;
  int32_t worker_exit;
  VkrEditorContentAction action;
  /* Change reports for the open project directory from the Bakery daemon.
     Files published by a job were written before the listing that shows them
     was read, so only files modified after `listed_at` (Unix seconds) count
     as edits. */
  EditorBakeryService *service;
  uint32_t watch;
  char watched[CONTENT_PATH];
  int64_t listed_at;
  /* The project's labels and presets documents; the arrays live as long as
     the browser. */
  VkrEditorContentLabels labels;
  VkrEditorPresets presets;
  ContentFolder tree[CONTENT_TREE_MAX];
  uint32_t tree_count;
  /* Current folder, and back/forward history of visited folders. */
  char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  char history[32][VKR_EDITOR_FOLDER_PATH_CAPACITY];
  uint32_t history_count;
  uint32_t history_index;
  /* A selected folder tile; exclusive with an item selection. */
  char selected_folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  /* Distinct tags, collected when the listing rebuilds; a chosen tag lists
     tagged items below the current folder. */
  char tags[CONTENT_TAG_MAX][32];
  uint32_t tag_count;
  char tag_filter[32];
  /* Label editor for the selected asset. */
  char label_asset[37];
  uint8_t label_folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  uint32_t label_folder_length;
  uint8_t label_tags[128];
  uint32_t label_tags_length;
  bool8_t label_dirty;
  /* Name draft for the selected folder. */
  char folder_draft_path[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  char folder_draft_label[128];
  uint8_t folder_draft[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  uint32_t folder_draft_length;
};

static void content_drag_update(VkrEditorContent *content, VkrUiSystem *ui);
static const char *content_cooking_status(bool8_t loading);

static const char *const content_kinds[] = {
    "Texture", "Material", "Mesh",      "Font",   "Environment",
    "Scene",   "Probe",    "Animation", "Object", "Object type",
    "Preset",  "Script",   "Other"};
static const VkrUiIcon content_icons[] = {
    VKR_UI_ICON_TEXTURE, VKR_UI_ICON_MATERIAL,    VKR_UI_ICON_MESH,
    VKR_UI_ICON_FONT,    VKR_UI_ICON_ENVIRONMENT, VKR_UI_ICON_SCENE,
    VKR_UI_ICON_PROBE,   VKR_UI_ICON_ANIMATION,   VKR_UI_ICON_EMPTY,
    VKR_UI_ICON_SHAPES,  VKR_UI_ICON_SPARKLE,     VKR_UI_ICON_CODE,
    VKR_UI_ICON_FILE};
/* The type folder each kind of asset lists in by default (ADR-076); NULL
   for items that are not assets. */
static const char *const content_type_folders[] = {
    "Textures", "Materials", "Meshes",     "Fonts", "Environments",
    NULL,       "Probes",    "Animations", NULL,    NULL,
    NULL,       "Scripts",   "Other"};
static const char *const content_scopes[] = {"Scene", "Project", "Editor",
                                             "Built-in"};
/* Folders the editor provides: System and its three, and the project's
   Presets. Editor bundle defaults such as the scene font are shipped
   Assets; the rest of the bundle is Editor. */
static const char *const content_system_folders[] = {
    CONTENT_SYSTEM, CONTENT_SYSTEM "/Assets", CONTENT_SYSTEM "/Objects",
    CONTENT_SYSTEM "/Editor", "Presets"};
/* Root folders of earlier layouts; a label naming one files its item in the
   item's default folder instead. */
static const char *const content_retired_folders[] = {"Objects", "Editor",
                                                      "Scene assets"};

static uint64_t content_hash(uint64_t hash, const void *data, uint64_t length) {
  const uint8_t *bytes = data;
  for (uint64_t i = 0; i < length; ++i) {
    hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
  }
  return hash;
}

static String8 content_string(const char *value) {
  return string8_create_from_cstr((const uint8_t *)value, strlen(value));
}

static bool8_t content_copy(char *output, uint32_t capacity,
                            const char *input) {
  if (!input || strlen(input) >= capacity) {
    return false_v;
  }
  MemCopy(output, input, strlen(input) + 1);
  return true_v;
}

static bool8_t content_cancelled(void *context) {
  VkrEditorContent *content = context;
  return vkr_atomic_bool_load(&content->cancel, VKR_MEMORY_ORDER_ACQUIRE);
}

static void *content_worker(void *context) {
  VkrEditorContent *content = context;
  const char *arguments[18];
  uint32_t count = 0;
  const char *executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_ASSET_PREVIEW);
  if (content->worker_material) {
    executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY);
    arguments[count++] = "preview";
    arguments[count++] = "material";
  }
  arguments[count++] = "--input";
  arguments[count++] = content->worker_input;
  arguments[count++] = "--output";
  arguments[count++] = content->worker_output;
  arguments[count++] = "--size";
  arguments[count++] = content->worker_size == 256 ? "256" : "128";
  if (content->worker_material) {
    arguments[count++] = "--workspace";
    arguments[count++] = content->worker_workspace;
    arguments[count++] = "--harness";
    arguments[count++] = vkr_editor_tool_path(VKR_EDITOR_TOOL_HARNESS);
  }
#if defined(PLATFORM_WINDOWS)
  char reveal_argument[CONTENT_PATH + 16];
#endif
  if (content->worker_reveal) {
#if defined(PLATFORM_WINDOWS)
    executable = "explorer.exe";
    snprintf(reveal_argument, sizeof(reveal_argument), "/select,%s",
             content->worker_input);
    arguments[0] = reveal_argument;
    count = 1;
#else
    executable = "/usr/bin/open";
    arguments[0] = "-R";
    arguments[1] = content->worker_input;
    count = 2;
#endif
  } else {
    char directory[CONTENT_PATH];
    snprintf(directory, sizeof(directory), "%s/cache/thumbnails",
             content->worker_workspace);
    const char *prune_arguments[] = {"preview", "prune", "--directory",
                                     directory};
    VkrPlatformProcessConfig prune = {
        .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
        .arguments = prune_arguments,
        .argument_count = ArrayCount(prune_arguments),
        .working_directory = vkr_content_root(),
        .stderr_path = content->worker_log,
        .timeout_ms = 10000,
        .termination_grace_ms = 250,
        .terminate_process_tree = true_v,
        .hidden = true_v,
        .is_cancelled = content_cancelled,
        .cancel_context = content};
    if (!vkr_platform_process_run(&prune, &content->worker_exit,
                                  &content->worker_timed_out) ||
        content->worker_exit || content->worker_timed_out) {
      content->worker_ok = false_v;
      vkr_atomic_bool_store(&content->complete, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
      return NULL;
    }
  }
  VkrPlatformProcessConfig config = {
      .executable = executable,
      .arguments = arguments,
      .argument_count = count,
      .working_directory = vkr_content_root(),
      .stderr_path = content->worker_log[0] ? content->worker_log : NULL,
      .timeout_ms = content->worker_material ? 180000 : 30000,
      .termination_grace_ms = 250,
      .terminate_process_tree = !content->worker_reveal,
      .hidden = true_v,
      .is_cancelled = content_cancelled,
      .cancel_context = content};
  content->worker_ok = vkr_platform_process_run(&config, &content->worker_exit,
                                                &content->worker_timed_out);
  vkr_atomic_bool_store(&content->complete, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

static void content_release_preview(ContentPreview *preview) {
  if (preview->request.request_id) {
    vkr_resource_system_unload(&preview->request,
                               content_string(preview->request_path));
  }
  MemZero(preview, sizeof(*preview));
}

static void content_cancel_worker(VkrEditorContent *content) {
  if (content->worker) {
    vkr_atomic_bool_store(&content->cancel, true_v, VKR_MEMORY_ORDER_RELEASE);
  }
}

bool8_t vkr_editor_content_stop_previews(VkrEditorContent *content) {
  if (!content) {
    return true_v;
  }
  content_cancel_worker(content);
  if (content->worker) {
    if (!vkr_thread_join(content->worker) ||
        !vkr_thread_destroy(content->allocator, &content->worker)) {
      return false_v;
    }
    vkr_platform_process_lock_release(&content->process_lock);
  }
  if (content->running < CONTENT_CACHE_COUNT) {
    content->cache[content->running].queued = false_v;
  }
  content->running = CONTENT_NONE;
  return true_v;
}

VkrEditorContent *vkr_editor_content_create(VkrAllocator *allocator,
                                            VkrRenderAssets *assets) {
  if (!allocator || allocator->type != VKR_ALLOCATOR_TYPE_DMEMORY || !assets) {
    return NULL;
  }
  VkrEditorContent *content = vkr_allocator_alloc(
      allocator, sizeof(*content), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!content) {
    return NULL;
  }
  MemZero(content, sizeof(*content));
  content->allocator = allocator;
  content->assets = assets;
  content->size = 128;
  content->selected = CONTENT_NONE;
  content->running = CONTENT_NONE;
  content->click_asset = CONTENT_NONE;
  content->drag_asset = CONTENT_NONE;
  content->renaming = CONTENT_NONE;
  content->menu_requested = false_v;
  content->labels.labels = vkr_allocator_alloc(
      allocator, CONTENT_LABEL_MAX * sizeof(*content->labels.labels),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  content->labels.folders = vkr_allocator_alloc(
      allocator, CONTENT_FOLDER_MAX * sizeof(*content->labels.folders),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  content->presets.presets = vkr_allocator_alloc(
      allocator, VKR_EDITOR_PRESET_MAX * sizeof(*content->presets.presets),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!content->labels.labels || !content->labels.folders ||
      !content->presets.presets) {
    vkr_editor_content_destroy(content);
    return NULL;
  }
  content->labels.label_capacity = CONTENT_LABEL_MAX;
  content->labels.folder_capacity = CONTENT_FOLDER_MAX;
  content->presets.capacity = VKR_EDITOR_PRESET_MAX;
  /* Built-in items and system folders show before any project opens. */
  vkr_editor_content_refresh(content);
  return content;
}

void vkr_editor_content_destroy(VkrEditorContent *content) {
  if (!content) {
    return;
  }
  if (!vkr_editor_content_stop_previews(content)) {
    return;
  }
  editor_bakery_service_unwatch(content->service, content->watch);
  for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
    content_release_preview(&content->cache[i]);
  }
  if (content->entries) {
    vkr_allocator_free(content->allocator, content->entries,
                       sizeof(*content->entries) * content->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (content->filtered) {
    vkr_allocator_free(content->allocator, content->filtered,
                       sizeof(*content->filtered) *
                           (CONTENT_MAX_ASSETS + CONTENT_TREE_MAX),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (content->labels.labels) {
    vkr_allocator_free(content->allocator, content->labels.labels,
                       sizeof(*content->labels.labels) * CONTENT_LABEL_MAX,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (content->labels.folders) {
    vkr_allocator_free(content->allocator, content->labels.folders,
                       sizeof(*content->labels.folders) * CONTENT_FOLDER_MAX,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (content->presets.presets) {
    vkr_allocator_free(content->allocator, content->presets.presets,
                       sizeof(*content->presets.presets) *
                           VKR_EDITOR_PRESET_MAX,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  vkr_allocator_free(content->allocator, content, sizeof(*content),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

static ContentKind content_kind(const char *name) {
  if (!strcmp(name, "texture") || !strcmp(name, "image")) {
    return CONTENT_TEXTURE;
  }
  if (!strcmp(name, "material")) {
    return CONTENT_MATERIAL;
  }
  if (!strcmp(name, "mesh") || !strcmp(name, "model")) {
    return CONTENT_MESH;
  }
  if (!strcmp(name, "font")) {
    return CONTENT_FONT;
  }
  if (!strcmp(name, "environment") || !strcmp(name, "hdr") ||
      !strcmp(name, "skybox")) {
    return CONTENT_ENVIRONMENT;
  }
  if (!strcmp(name, "scene")) {
    return CONTENT_SCENE;
  }
  if (!strcmp(name, "probe") || !strcmp(name, "reflection_probe")) {
    return CONTENT_PROBE;
  }
  if (!strcmp(name, "animation")) {
    return CONTENT_ANIMATION;
  }
  return CONTENT_OTHER;
}

static bool8_t content_reserve(VkrEditorContent *content) {
  if (content->count < content->capacity) {
    return true_v;
  }
  uint32_t capacity = content->capacity ? content->capacity * 2 : 64;
  if (capacity > CONTENT_MAX_ASSETS) {
    return false_v;
  }
  ContentAsset *entries = vkr_allocator_realloc(
      content->allocator, content->entries,
      sizeof(*entries) * content->capacity, sizeof(*entries) * capacity,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!entries) {
    return false_v;
  }
  content->entries = entries;
  content->capacity = capacity;
  return true_v;
}

/* Returns a null-terminated copy of a JSON document of at most 16 MiB, owned
 * by the content allocator with size + 1 bytes. Missing files are silent. */
static uint8_t *content_load_document(VkrEditorContent *content,
                                      const char *manifest,
                                      uint64_t *out_size) {
  FilePath path = {.path = content_string(manifest),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  FileStats stats = {0};
  if (file_stats(&path, &stats) != FILE_ERROR_NONE) {
    return NULL;
  }
  if (!stats.size || stats.size > MB(16)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Inventory is empty or exceeds 16 MiB: %.350s", manifest);
    return NULL;
  }
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle file = {0};
  uint8_t *bytes = vkr_allocator_alloc(content->allocator, stats.size + 1,
                                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  uint64_t read = 0;
  const bool8_t loaded =
      bytes && file_open(&path, mode, &file) == FILE_ERROR_NONE &&
      file_read_into(&file, bytes, stats.size, &read) == FILE_ERROR_NONE &&
      read == stats.size;
  file_close(&file);
  if (!loaded) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Cannot read inventory: %.350s", manifest);
    if (bytes) {
      vkr_allocator_free(content->allocator, bytes, stats.size + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    }
    return NULL;
  }
  bytes[read] = 0;
  content->inventory_key = content_hash(content->inventory_key, bytes, read);
  *out_size = read;
  return bytes;
}

/* Project scenes appear as Scene items (ADR-076); opening one loads it. */
static void content_read_scenes(VkrEditorContent *content, const char *root,
                                const char *manifest) {
  uint64_t size = 0;
  uint8_t *bytes = content_load_document(content, manifest, &size);
  if (!bytes) {
    return;
  }
  const String8 document = {.str = bytes, .length = size};
  VkrEditorProjectError error = {0};
  String8 scenes = {0};
  if (vkr_editor_project_json_member(document, "scenes", &scenes, &error) &&
      scenes.length && scenes.str[0] == '[') {
    VkrJsonReader list = vkr_json_reader_from_string(scenes);
    list.pos = 1;
    while (vkr_json_next_array_element(&list)) {
      VkrJsonReader object = {0};
      if (!vkr_json_enter_object(&list, &object) || !content_reserve(content)) {
        break;
      }
      const String8 record = {.str = (uint8_t *)object.data,
                              .length = object.length};
      ContentAsset entry = {.scope = 1, .kind = CONTENT_SCENE};
      char relative[CONTENT_PATH] = {0};
      if (!vkr_editor_project_json_string(record, "id", entry.id,
                                          sizeof(entry.id), &error) ||
          !vkr_editor_project_json_string(record, "name", entry.name,
                                          sizeof(entry.name), &error)) {
        continue;
      }
      if (vkr_editor_project_json_string(record, "path", relative,
                                         sizeof(relative), &error)) {
        (void)vkr_editor_project_resolve(root, relative, entry.path, &error);
      }
      /* The scene lists as its own folder, not as an item. */
      snprintf(entry.home, sizeof(entry.home), "%c%s", CONTENT_SCENE_MARK,
               entry.id);
      content->entries[content->count++] = entry;
    }
  }
  vkr_allocator_free(content->allocator, bytes, size + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
}

static void content_read_inventory(VkrEditorContent *content, const char *root,
                                   const char *manifest, uint32_t scope,
                                   const char *home) {
  uint64_t size = 0;
  uint8_t *bytes = content_load_document(content, manifest, &size);
  if (!bytes) {
    return;
  }
  String8 document = {.str = bytes, .length = size};
  VkrEditorProjectError error = {0};
  VkrJsonReader list = {0};
  char inventory_reference[CONTENT_PATH] = {0};
  if (scope == 0 &&
      vkr_editor_project_json_string(document, "inventory", inventory_reference,
                                     sizeof(inventory_reference), &error)) {
    /* Scene v4 keeps its records in an immutable inventory revision of up to
     * 16 MiB, so its array is walked in place rather than tokenized. */
    char inventory[CONTENT_PATH];
    vkr_allocator_free(content->allocator, bytes, size + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    bytes = NULL;
    if (!vkr_editor_project_resolve(root, inventory_reference, inventory,
                                    &error)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic), "%.450s",
               error.message);
      return;
    }
    bytes = content_load_document(content, inventory, &size);
    if (!bytes) {
      return;
    }
    list = vkr_json_reader_from_string((String8){.str = bytes, .length = size});
    if (!vkr_json_find_array(&list, "assets")) {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "Scene inventory has no asset list: %.350s", inventory);
      goto cleanup;
    }
  } else {
    String8 assets = {0};
    if (!vkr_editor_project_json_member(document, "assets", &assets, &error)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic), "%.450s",
               error.message);
      goto cleanup;
    }
    if (!assets.length || assets.str[0] != '[') {
      goto cleanup;
    }
    list = vkr_json_reader_from_string(assets);
    list.pos = 1;
  }
  const uint32_t first = content->count;
  /* The owner resolves once; each record's paths resolve beneath it. */
  char resolved_root[CONTENT_PATH];
  const FilePath owner = {.path = content_string(root),
                          .type = FILE_PATH_TYPE_ABSOLUTE};
  if (file_path_resolve(&owner, resolved_root, sizeof(resolved_root)) !=
      FILE_ERROR_NONE) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Inventory owner is inaccessible: %.400s", root);
    goto cleanup;
  }
  while (vkr_json_next_array_element(&list)) {
    VkrJsonReader object = {0};
    if (!vkr_json_enter_object(&list, &object) || !content_reserve(content)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "Content inventory exceeds the %u-asset limit or is invalid.",
               CONTENT_MAX_ASSETS);
      break;
    }
    String8 record = {.str = (uint8_t *)object.data, .length = object.length};
    ContentAsset entry = {.scope = scope};
    content_copy(entry.home, sizeof(entry.home), home);
    char kind[64] = {0};
    char relative[CONTENT_PATH] = {0};
    if (!vkr_editor_project_json_string(record, "id", entry.id,
                                        sizeof(entry.id), &error) ||
        !vkr_editor_project_json_string(record, "name", entry.name,
                                        sizeof(entry.name), &error) ||
        !vkr_editor_project_json_string(record, "kind", kind, sizeof(kind),
                                        &error)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "Invalid asset record: %.450s", error.message);
      continue;
    }
    entry.kind = content_kind(kind);
    if (content_type_folders[entry.kind]) {
      snprintf(entry.type_folder, sizeof(entry.type_folder), "%s%s%s",
               entry.home, entry.home[0] ? "/" : "",
               content_type_folders[entry.kind]);
    }
    (void)vkr_editor_project_json_string(record, "fingerprint",
                                         entry.fingerprint,
                                         sizeof(entry.fingerprint), &error);
    (void)vkr_editor_project_json_string(record, "diagnostic", entry.diagnostic,
                                         sizeof(entry.diagnostic), &error);
    String8 recipe = {0};
    if (vkr_editor_project_json_member(record, "recipe", &recipe, &error) &&
        recipe.length && recipe.str[0] == '{') {
      char tool[32] = {0};
      int32_t version = 0;
      (void)vkr_editor_project_json_string(recipe, "tool", tool, sizeof(tool),
                                           &error);
      VkrJsonReader reader = vkr_json_reader_from_string(recipe);
      (void)vkr_json_get_int(&reader, "version", &version);
      if (tool[0]) {
        snprintf(entry.recipe, sizeof(entry.recipe), "%s %d", tool, version);
      }
    }
    String8 closure = {0};
    if (vkr_editor_project_json_member(record, "closure", &closure, &error) &&
        closure.length && closure.str[0] == '{') {
      /* One dependency per member of the closure object. */
      VkrJsonReader reader = vkr_json_reader_from_string(closure);
      reader.pos = 1u;
      for (;;) {
        vkr_json_skip_whitespace(&reader);
        String8 key = {0};
        if (!vkr_json_parse_string(&reader, &key)) {
          break;
        }
        ++entry.dependencies;
        vkr_json_skip_to(&reader, ',');
        if (reader.pos >= reader.length) {
          break;
        }
        ++reader.pos;
      }
    }
    if (vkr_editor_project_json_string(record, "source", relative,
                                       sizeof(relative), &error) &&
        relative[0]) {
      (void)vkr_editor_project_resolve_within(resolved_root, relative,
                                              entry.source, &error);
    }
    String8 artifacts = {0};
    if (vkr_editor_project_json_member(record, "artifacts", &artifacts,
                                       &error) &&
        artifacts.length && artifacts.str[0] == '[') {
      VkrJsonReader artifact_list = vkr_json_reader_from_string(artifacts);
      artifact_list.pos = 1;
      VkrJsonReader artifact = {0};
      if (vkr_json_next_array_element(&artifact_list) &&
          vkr_json_enter_object(&artifact_list, &artifact)) {
        String8 artifact_object = {.str = (uint8_t *)artifact.data,
                                   .length = artifact.length};
        if (vkr_editor_project_json_string(artifact_object, "path", relative,
                                           sizeof(relative), &error)) {
          /* Artifacts live in builds/<revision>/ below their owner. */
          const char *revision =
              !strncmp(relative, "builds/", 7u) ? relative + 7u : NULL;
          const char *end = revision ? strchr(revision, '/') : NULL;
          if (end && (uint64_t)(end - revision) < sizeof(entry.revision)) {
            MemCopy(entry.revision, revision, (uint64_t)(end - revision));
            entry.revision[end - revision] = '\0';
          }
          entry.missing = !vkr_editor_project_resolve_within(
              resolved_root, relative, entry.path, &error);
          if (entry.missing) {
            snprintf(entry.diagnostic, sizeof(entry.diagnostic),
                     "Missing artifact: %.220s", relative);
          }
        }
        (void)vkr_editor_project_json_string(
            artifact_object, "role", entry.role, sizeof(entry.role), &error);
      }
    }
    if (!entry.path[0] && !entry.missing) {
      entry.stale = true_v;
      snprintf(entry.diagnostic, sizeof(entry.diagnostic),
               "No completed artifact. Build this asset.");
    }
    char tier[16] = {0};
    entry.pending_tier =
        vkr_editor_project_json_string(record, "texture_tier", tier,
                                       sizeof(tier), &error) &&
        (!strcmp(tier, "deferred") || !strcmp(tier, "preview"));
    content->entries[content->count++] = entry;
  }
  /* A pending mesh's materials share its build revision and are rebuilt
     with it. */
  for (uint32_t i = first; i < content->count; ++i) {
    const ContentAsset *pending = &content->entries[i];
    if (!pending->pending_tier || !pending->revision[0]) {
      continue;
    }
    for (uint32_t j = first; j < content->count; ++j) {
      ContentAsset *entry = &content->entries[j];
      entry->pending_tier |= !strcmp(entry->revision, pending->revision);
    }
  }
cleanup:
  if (bytes) {
    vkr_allocator_free(content->allocator, bytes, size + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
}

// ---- Labels: virtual folders and tags (ADR-076) ----

static bool8_t content_labels_path(const VkrEditorContent *content,
                                   char path[CONTENT_PATH]) {
  return content->workspace[0] && content->project[0] &&
         snprintf(path, CONTENT_PATH, "%s/projects/%s/content.labels.json",
                  content->workspace, content->project) < (int)CONTENT_PATH;
}

static const VkrEditorAssetLabel *content_label(const VkrEditorContent *content,
                                                const char *id) {
  for (uint32_t i = 0; i < content->labels.label_count; ++i) {
    if (!strcmp(content->labels.labels[i].id, id)) {
      return &content->labels.labels[i];
    }
  }
  return NULL;
}

/* The label resolved for an entry by the last listing rebuild. */
static const VkrEditorAssetLabel *
content_entry_label(const VkrEditorContent *content,
                    const ContentAsset *entry) {
  return entry->label < content->labels.label_count
             ? &content->labels.labels[entry->label]
             : NULL;
}

/* A scene's own folder. */
static bool8_t content_scene_folder(const char *path) {
  return path[0] == CONTENT_SCENE_MARK && !strchr(path, '/');
}

/* A scene's folder or one of its type folders. */
static bool8_t content_in_scene(const char *path) {
  return path[0] == CONTENT_SCENE_MARK;
}

/* The type folders assets group into under a home: the root, a scene. */
static bool8_t content_type_folder(const char *path) {
  const char *name = vkr_editor_folder_name(path);
  for (uint32_t i = 0; i < ArrayCount(content_type_folders); ++i) {
    if (content_type_folders[i] && !strcmp(name, content_type_folders[i])) {
      char parent[VKR_EDITOR_FOLDER_PATH_CAPACITY];
      vkr_editor_folder_parent(path, parent);
      return !parent[0] || content_scene_folder(parent);
    }
  }
  return false_v;
}

/* Folders the editor provides: System, its three, Presets and each scene's
   folder. They cannot be renamed, moved or deleted. */
static bool8_t content_system_folder(const char *path) {
  for (uint32_t i = 0; i < ArrayCount(content_system_folders); ++i) {
    if (!strcmp(path, content_system_folders[i])) {
      return true_v;
    }
  }
  return content_in_scene(path) || content_type_folder(path);
}

/* Whether a path lies in System, which takes no project items or folders. */
static bool8_t content_in_system(const char *path) {
  return vkr_editor_folder_within(path, CONTENT_SYSTEM) && path[0];
}

/* Project folders and project assets live at the Content root and in its
   project folders; System, Presets and scene folders keep their own. */
static bool8_t content_project_folder(const char *path) {
  return !content_in_system(path) && !content_system_folder(path) &&
         !content_in_scene(path);
}

/* An item's folder while no label files it elsewhere; the empty label folder
   stands for it. */
static const char *content_default_folder(const ContentAsset *entry) {
  if (entry->kind == CONTENT_PRESET) {
    return "Presets";
  }
  if (entry->kind == CONTENT_OBJECT) {
    return CONTENT_SYSTEM "/Objects";
  }
  if (entry->scope == 2) {
    return !strncmp(entry->id, "default-", 8) ? CONTENT_SYSTEM "/Assets"
                                              : CONTENT_SYSTEM "/Editor";
  }
  return entry->type_folder[0] ? entry->type_folder : entry->home;
}

/* Where an item appears: its label's folder, else its default. Objects and
   editor items stay in theirs; a label naming a folder that no longer
   exists, or a retired one, falls back too. */
static const char *content_item_folder(const VkrEditorContent *content,
                                       const ContentAsset *entry) {
  if (entry->scope >= 2u || entry->kind == CONTENT_PRESET) {
    return content_default_folder(entry);
  }
  const VkrEditorAssetLabel *label = content_entry_label(content, entry);
  if (label && label->folder[0] && !content_in_system(label->folder)) {
    bool8_t retired = false_v;
    for (uint32_t i = 0; i < ArrayCount(content_retired_folders); ++i) {
      retired |= !strcmp(label->folder, content_retired_folders[i]);
    }
    if (!retired) {
      return label->folder;
    }
  }
  return content_default_folder(entry);
}

/* The entry's build revision is being rebuilt by the running background
   finalize: Content imports, or the assets of the scene it finalizes. */
static bool8_t content_cooking(const VkrEditorContent *content,
                               const ContentAsset *entry) {
  if (entry->kind == CONTENT_ENTITY) {
    return entry->cooking_object;
  }
  if (!entry->pending_tier) {
    return false_v;
  }
  if (entry->scope == 1u) {
    return content->cooking_project;
  }
  return entry->scope == 0u && content->cooking_scene[0] &&
         entry->home[0] == CONTENT_SCENE_MARK &&
         !strcmp(entry->home + 1, content->cooking_scene);
}

/* The mesh streams into the World or the scene loading now. */
static bool8_t content_loading(const VkrEditorContent *content,
                               const ContentAsset *entry) {
  if (entry->kind != CONTENT_MESH) {
    return false_v;
  }
  if (entry->scope == 1u) {
    for (uint32_t i = 0; i < content->loading_count; ++i) {
      if (!strcmp(content->loading_ids[i], entry->id)) {
        return true_v;
      }
    }
    return false_v;
  }
  return entry->scope == 0u && content->loading_scene[0] &&
         entry->home[0] == CONTENT_SCENE_MARK &&
         !strcmp(entry->home + 1, content->loading_scene);
}

/* Loading or cooking: the item is locked until the work ends. */
static bool8_t content_locked(const VkrEditorContent *content,
                              const ContentAsset *entry) {
  return content_cooking(content, entry) || content_loading(content, entry);
}

static VkrUiIcon content_icon(const ContentAsset *entry) {
  return entry->kind == CONTENT_OBJECT
             ? vkr_editor_object_kind_icon(entry->object)
         : entry->kind == CONTENT_ENTITY ? entry->entity_icon
                                         : content_icons[entry->kind];
}

static void content_labels_save(VkrEditorContent *content) {
  content->filter_dirty = true_v;
  char path[CONTENT_PATH];
  if (content->read_only || !content_labels_path(content, path)) {
    return;
  }
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, content_string(path))) {
    return;
  }
  const bool8_t ok = vkr_editor_labels_write(&file.writer, &content->labels);
  if (!ok || !vkr_json_file_writer_commit(&file)) {
    vkr_json_file_writer_abort(&file);
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Content folders and labels could not be saved.");
  }
}

static void content_labels_load(VkrEditorContent *content) {
  content->labels.label_count = 0u;
  content->labels.folder_count = 0u;
  char path[CONTENT_PATH];
  if (!content_labels_path(content, path)) {
    return;
  }
  const FilePath file = {.path = content_string(path),
                         .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!file_exists(&file)) {
    return;
  }
  uint64_t length = 0u;
  uint8_t *bytes = content_load_document(content, path, &length);
  if (!bytes) {
    return;
  }
  VkrEditorProjectError error = {0};
  if (!vkr_editor_labels_parse(string8_create(bytes, length), &content->labels,
                               &error)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic), "%s",
             error.message);
  }
  vkr_allocator_free(content->allocator, bytes, length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
}

/* Replace an asset's label; empty folder and tags remove it. */
static bool8_t content_label_set(VkrEditorContent *content, const char *id,
                                 const char *folder, const char *tags) {
  VkrEditorContentLabels *labels = &content->labels;
  if (folder[0] && !vkr_editor_folders_add(labels, folder)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Content holds at most %u folders.", CONTENT_FOLDER_MAX);
    return false_v;
  }
  uint32_t index = labels->label_count;
  for (uint32_t i = 0; i < labels->label_count; ++i) {
    if (!strcmp(labels->labels[i].id, id)) {
      index = i;
      break;
    }
  }
  if (!folder[0] && !tags[0]) {
    if (index < labels->label_count) {
      labels->labels[index] = labels->labels[--labels->label_count];
    }
  } else {
    if (index == labels->label_count) {
      if (labels->label_count == labels->label_capacity) {
        return false_v;
      }
      ++labels->label_count;
    }
    VkrEditorAssetLabel *label = &labels->labels[index];
    content_copy(label->id, sizeof(label->id), id);
    content_copy(label->folder, sizeof(label->folder), folder);
    content_copy(label->tags, sizeof(label->tags), tags);
  }
  content_labels_save(content);
  return true_v;
}

/* Advances `cursor` to the next trimmed, non-empty name of comma-separated
   tags; false after the last. */
static bool8_t content_tags_next(const char **cursor, const char **name,
                                 uint64_t *length) {
  while (**cursor) {
    while (**cursor == ' ' || **cursor == ',') {
      ++*cursor;
    }
    const char *end = *cursor;
    while (*end && *end != ',') {
      ++end;
    }
    uint64_t size = (uint64_t)(end - *cursor);
    while (size && (*cursor)[size - 1u] == ' ') {
      --size;
    }
    *name = *cursor;
    *cursor = end;
    if (size) {
      *length = size;
      return true_v;
    }
  }
  return false_v;
}

/* True when the comma-separated `tags` contain `tag`, ignoring case. */
static bool8_t content_tags_contain(const char *tags, const char *tag) {
  const uint64_t wanted = strlen(tag);
  const char *cursor = tags;
  const char *name = NULL;
  uint64_t length = 0u;
  while (content_tags_next(&cursor, &name, &length)) {
    bool8_t same = length == wanted;
    for (uint64_t i = 0; same && i < length; ++i) {
      same = tolower((uint8_t)name[i]) == tolower((uint8_t)tag[i]);
    }
    if (same) {
      return true_v;
    }
  }
  return false_v;
}

/* Record each distinct tag once, for the Tags list. */
static void content_collect_tags(VkrEditorContent *content) {
  content->tag_count = 0u;
  for (uint32_t i = 0; i < content->labels.label_count; ++i) {
    const char *cursor = content->labels.labels[i].tags;
    const char *name = NULL;
    uint64_t length = 0u;
    while (content_tags_next(&cursor, &name, &length)) {
      if (length >= sizeof(content->tags[0])) {
        continue;
      }
      char tag[32];
      MemCopy(tag, name, length);
      tag[length] = '\0';
      bool8_t seen = false_v;
      for (uint32_t t = 0; !seen && t < content->tag_count; ++t) {
        seen = !strcmp(content->tags[t], tag);
      }
      if (!seen && content->tag_count < CONTENT_TAG_MAX) {
        content_copy(content->tags[content->tag_count++],
                     sizeof(content->tags[0]), tag);
      }
    }
  }
}

// ---- Presets (ADR-076) ----

static bool8_t content_presets_path(const VkrEditorContent *content,
                                    char path[CONTENT_PATH]) {
  return content->workspace[0] && content->project[0] &&
         snprintf(path, CONTENT_PATH, "%s/projects/%s/presets.json",
                  content->workspace, content->project) < (int)CONTENT_PATH;
}

static bool8_t content_presets_save(VkrEditorContent *content) {
  char path[CONTENT_PATH];
  if (content->read_only || !content_presets_path(content, path)) {
    return false_v;
  }
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, content_string(path))) {
    return false_v;
  }
  if (!vkr_editor_presets_write(&file.writer, &content->presets) ||
      !vkr_json_file_writer_commit(&file)) {
    vkr_json_file_writer_abort(&file);
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Presets could not be saved.");
    return false_v;
  }
  return true_v;
}

/* Presets load as Preset items; their index names the preset. */
static void content_presets_load(VkrEditorContent *content) {
  content->presets.count = 0u;
  char path[CONTENT_PATH];
  if (!content_presets_path(content, path)) {
    return;
  }
  const FilePath file = {.path = content_string(path),
                         .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!file_exists(&file)) {
    return;
  }
  uint64_t length = 0u;
  uint8_t *bytes = content_load_document(content, path, &length);
  if (!bytes) {
    return;
  }
  /* String properties decode through the freeable content allocator and
     are released by the reader. */
  VkrEditorProjectError error = {0};
  if (!vkr_editor_presets_parse(string8_create(bytes, length),
                                &content->presets, content->allocator,
                                &error)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic), "%s",
             error.message);
  }
  vkr_allocator_free(content->allocator, bytes, length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  for (uint32_t i = 0; i < content->presets.count && content_reserve(content);
       ++i) {
    const VkrEditorPreset *preset = &content->presets.presets[i];
    ContentAsset *entry = &content->entries[content->count++];
    MemZero(entry, sizeof(*entry));
    entry->kind = CONTENT_PRESET;
    entry->scope = 1u;
    entry->object = i;
    entry->label = CONTENT_NONE;
    content_copy(entry->id, sizeof(entry->id), preset->id);
    content_copy(entry->name, sizeof(entry->name), preset->name);
  }
}

static void content_rename_preset(VkrEditorContent *content, uint32_t index,
                                  const uint8_t *text, uint32_t length) {
  char name[sizeof(content->presets.presets[0].name)];
  vkr_editor_label_clean(name, sizeof(name), text, length);
  if (content->read_only || index >= content->presets.count || !name[0]) {
    return;
  }
  VkrEditorPreset *preset = &content->presets.presets[index];
  char previous[sizeof(preset->name)];
  content_copy(previous, sizeof(previous), preset->name);
  content_copy(preset->name, sizeof(preset->name), name);
  if (!content_presets_save(content)) {
    content_copy(preset->name, sizeof(preset->name), previous);
    return;
  }
  for (uint32_t i = 0; i < content->count; ++i) {
    ContentAsset *entry = &content->entries[i];
    if (entry->kind == CONTENT_PRESET && entry->object == index) {
      content_copy(entry->name, sizeof(entry->name), name);
    }
  }
  content->filter_dirty = true_v;
}

/* Removing a preset renumbers the rest, so the listing reloads. */
static void content_delete_preset(VkrEditorContent *content, uint32_t index) {
  VkrEditorPresets *presets = &content->presets;
  if (content->read_only || index >= presets->count) {
    return;
  }
  const VkrEditorPreset removed = presets->presets[index];
  MemCopy(presets->presets + index, presets->presets + index + 1u,
          (presets->count - index - 1u) * sizeof(presets->presets[0]));
  --presets->count;
  if (!content_presets_save(content)) {
    MemCopy(presets->presets + index + 1u, presets->presets + index,
            (presets->count - index) * sizeof(presets->presets[0]));
    presets->presets[index] = removed;
    ++presets->count;
    return;
  }
  vkr_editor_content_refresh(content);
}

// ---- Folder tree ----

/* Path order with '/' first, so each folder directly precedes its subtree. */
/* Tree order: the Content root and its subtree, then System's. */
static int32_t content_path_compare(const char *a, const char *b) {
  const bool8_t system_a = content_in_system(a);
  const bool8_t system_b = content_in_system(b);
  if (system_a != system_b) {
    return system_a ? 1 : -1;
  }
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  const int32_t left = *a == '/' ? 1 : (uint8_t)*a;
  const int32_t right = *b == '/' ? 1 : (uint8_t)*b;
  return left - right;
}

static uint32_t content_tree_find(const VkrEditorContent *content,
                                  const char *path) {
  for (uint32_t i = 0; i < content->tree_count; ++i) {
    if (!strcmp(content->tree[i].path, path)) {
      return i;
    }
  }
  return CONTENT_NONE;
}

static void content_tree_insert(ContentFolder *tree, uint32_t *count,
                                const char *path, bool8_t system) {
  for (uint32_t i = 0; i < *count; ++i) {
    if (!strcmp(tree[i].path, path)) {
      tree[i].system |= system;
      return;
    }
  }
  if (*count == CONTENT_TREE_MAX) {
    return;
  }
  uint32_t cursor = (*count)++;
  while (cursor && content_path_compare(tree[cursor - 1u].path, path) > 0) {
    tree[cursor] = tree[cursor - 1u];
    --cursor;
  }
  tree[cursor] = (ContentFolder){.system = system};
  content_copy(tree[cursor].path, sizeof(tree[cursor].path), path);
}

static int32_t content_compare_text(const char *a, const char *b);

/* Label and icon of a tree folder: the Content root is the World, a scene
   folder shows its scene, System's folders their role. */
static void content_folder_present(const VkrEditorContent *content,
                                   ContentFolder *folder) {
  static const struct {
    const char *path;
    const char *label;
    VkrUiIcon icon;
  } named[] = {
      {"", "Content", VKR_UI_ICON_WORLD},
      {CONTENT_SYSTEM, "System", VKR_UI_ICON_HARD_DRIVES},
      {CONTENT_SYSTEM "/Assets", "Assets", VKR_UI_ICON_DATABASE},
      {CONTENT_SYSTEM "/Objects", "Objects", VKR_UI_ICON_SHAPES},
      {CONTENT_SYSTEM "/Editor", "Editor", VKR_UI_ICON_WRENCH},
      {"Presets", "Presets", VKR_UI_ICON_SPARKLE},
  };
  for (uint32_t i = 0; i < ArrayCount(named); ++i) {
    if (!strcmp(folder->path, named[i].path)) {
      content_copy(folder->label, sizeof(folder->label), named[i].label);
      folder->icon = named[i].icon;
      return;
    }
  }
  folder->icon = VKR_UI_ICON_FOLDER;
  content_copy(folder->label, sizeof(folder->label),
               vkr_editor_folder_name(folder->path));
  if (!content_scene_folder(folder->path)) {
    return;
  }
  folder->icon = VKR_UI_ICON_SCENE;
  for (uint32_t i = 0; i < content->asset_count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (entry->kind == CONTENT_SCENE && !strcmp(entry->home, folder->path)) {
      content_copy(folder->label, sizeof(folder->label), entry->name);
      return;
    }
  }
}

/* Scene folders sort by id; order them by name, each with its type folders
   right below it. */
static void content_tree_sort_scenes(VkrEditorContent *content) {
  uint32_t first = CONTENT_NONE;
  uint32_t end = 0u;
  for (uint32_t i = 0; i < content->tree_count; ++i) {
    if (content_in_scene(content->tree[i].path)) {
      first = Min(first, i);
      end = i + 1u;
    }
  }
  if (first == CONTENT_NONE) {
    return;
  }
  uint32_t roots[CONTENT_TREE_MAX];
  uint32_t root_count = 0u;
  for (uint32_t i = first; i < end; ++i) {
    if (content_scene_folder(content->tree[i].path)) {
      uint32_t cursor = root_count++;
      while (cursor &&
             content_compare_text(content->tree[roots[cursor - 1u]].label,
                                  content->tree[i].label) > 0) {
        roots[cursor] = roots[cursor - 1u];
        --cursor;
      }
      roots[cursor] = i;
    }
  }
  static ContentFolder sorted[CONTENT_TREE_MAX];
  uint32_t out = 0u;
  for (uint32_t r = 0; r < root_count; ++r) {
    const char *root = content->tree[roots[r]].path;
    for (uint32_t i = first; i < end; ++i) {
      if (vkr_editor_folder_within(content->tree[i].path, root)) {
        sorted[out++] = content->tree[i];
      }
    }
  }
  MemCopy(&content->tree[first], sorted, out * sizeof(sorted[0]));
}

/* Rebuild the tree: the Content root with a folder per scene, Presets and
   the project's folders, then System and its three. Open folders stay
   open. */
static void content_tree_build(VkrEditorContent *content) {
  ContentFolder previous[CONTENT_TREE_MAX];
  const uint32_t previous_count = content->tree_count;
  MemCopy(previous, content->tree, previous_count * sizeof(previous[0]));
  uint32_t count = 0u;
  content_tree_insert(content->tree, &count, "", true_v);
  for (uint32_t i = 0; i < content->asset_count; ++i) {
    if (content->entries[i].kind == CONTENT_SCENE) {
      content_tree_insert(content->tree, &count, content->entries[i].home,
                          true_v);
    }
  }
  /* A type folder appears where an asset defaults to it. */
  for (uint32_t i = 0; i < content->asset_count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (entry->type_folder[0] && entry->scope < 2u &&
        !strcmp(content_item_folder(content, entry), entry->type_folder)) {
      content_tree_insert(content->tree, &count, entry->type_folder, true_v);
    }
  }
  for (uint32_t i = 0; i < ArrayCount(content_system_folders); ++i) {
    /* Presets appear once the project has one. */
    if (strcmp(content_system_folders[i], "Presets") ||
        content->presets.count) {
      content_tree_insert(content->tree, &count, content_system_folders[i],
                          true_v);
    }
  }
  for (uint32_t i = 0; i < content->labels.folder_count; ++i) {
    const char *path = content->labels.folders[i].path;
    if (!content_in_system(path) && !content_system_folder(path)) {
      content_tree_insert(content->tree, &count, path, false_v);
    }
  }
  content->tree_count = count;
  for (uint32_t i = 0; i < count; ++i) {
    content_folder_present(content, &content->tree[i]);
  }
  content_tree_sort_scenes(content);
  for (uint32_t i = 0; i < count; ++i) {
    ContentFolder *folder = &content->tree[i];
    /* System is a second root beside Content. */
    const bool8_t root = !i || !strcmp(folder->path, CONTENT_SYSTEM);
    char parent[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    vkr_editor_folder_parent(folder->path, parent);
    folder->parent = root ? CONTENT_NONE : content_tree_find(content, parent);
    folder->depth = 0u;
    for (const char *c = folder->path; *c; ++c) {
      folder->depth += *c == '/';
    }
    folder->depth += i && !content_in_system(folder->path) ? 1u : 0u;
    folder->open =
        root || vkr_editor_folder_within(content->folder, folder->path);
    for (uint32_t p = 0; !folder->open && p < previous_count; ++p) {
      folder->open =
          previous[p].open && !strcmp(previous[p].path, folder->path);
    }
    if (folder->parent < count) {
      content->tree[folder->parent].has_children = true_v;
    }
  }
  if (content_tree_find(content, content->folder) == CONTENT_NONE) {
    content->folder[0] = '\0';
  }
  if (content->selected_folder[0] &&
      content_tree_find(content, content->selected_folder) == CONTENT_NONE) {
    content->selected_folder[0] = '\0';
  }
}

static bool8_t content_matches(const char *text, const uint8_t *query,
                               uint32_t length);

/* The built-in items: one object per listed kind, in System/Objects. */
static void content_read_builtins(VkrEditorContent *content) {
  const uint32_t kinds = vkr_editor_object_kind_count();
  for (uint32_t i = 0; i < kinds && content_reserve(content); ++i) {
    if (!vkr_editor_object_kind_listed(i)) {
      continue;
    }
    ContentAsset *entry = &content->entries[content->count++];
    MemZero(entry, sizeof(*entry));
    entry->scope = CONTENT_SCOPE_BUILTIN;
    entry->label = CONTENT_NONE;
    entry->kind = CONTENT_OBJECT;
    entry->object = i;
    snprintf(entry->id, sizeof(entry->id), "object.%.28s",
             vkr_editor_object_kind_word(i));
    content_copy(entry->name, sizeof(entry->name),
                 vkr_editor_object_kind_label(i));
  }
}

/* Watches the open project's directory; a project change replaces the
   watch. */
static void content_watch_project(VkrEditorContent *content) {
  char root[CONTENT_PATH] = {0};
  if (content->service && content->project[0]) {
    snprintf(root, sizeof(root), "%s/projects/%s", content->workspace,
             content->project);
  }
  if (!strcmp(root, content->watched)) {
    return;
  }
  editor_bakery_service_unwatch(content->service, content->watch);
  content->watch = EDITOR_BAKERY_SERVICE_NONE;
  content_copy(content->watched, sizeof(content->watched), root);
  if (root[0]) {
    const char *paths[] = {root};
    content->watch = editor_bakery_service_watch(content->service, paths,
                                                 ArrayCount(paths), NULL, 0u);
  }
}

/* Canonical form of an entry path: change reports name real paths. */
static bool8_t content_real_path(const char *path, char *out,
                                 uint32_t capacity) {
  if (!path[0]) {
    return false_v;
  }
#if defined(_WIN32)
  return content_copy(out, capacity, path);
#else
  char resolved[PATH_MAX];
  if (realpath(path, resolved)) {
    return content_copy(out, capacity, resolved);
  }
  return content_copy(out, capacity, path);
#endif
}

/* Marks the entries whose source or artifact the change names. A deleted
   artifact is Missing; an edited source queues a rebuild of a scene-owned
   asset of the open scene when no other action waits. */
static void content_apply_change(VkrEditorContent *content, const char *path) {
  FileStats stats = {0};
  FilePath file = {.path = content_string(path),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  const bool8_t exists = file_stats(&file, &stats) == FILE_ERROR_NONE;
  if (exists && (int64_t)stats.last_modified <= content->listed_at) {
    return;
  }
  /* Real path of the open scene's directory with a trailing separator. */
  char scene_root[CONTENT_PATH];
  snprintf(scene_root, sizeof(scene_root), "%s/scenes/%s", content->watched,
           content->scene);
  char resolved_scene[CONTENT_PATH];
  (void)content_real_path(scene_root, resolved_scene,
                          sizeof(resolved_scene) - 1u);
  strcat(resolved_scene, "/");
  for (uint32_t i = 0u; i < content->asset_count; ++i) {
    ContentAsset *entry = &content->entries[i];
    char source[CONTENT_PATH];
    char artifact[CONTENT_PATH];
    const bool8_t is_source =
        content_real_path(entry->source, source, sizeof(source)) &&
        !strcmp(source, path);
    const bool8_t is_artifact =
        content_real_path(entry->path, artifact, sizeof(artifact)) &&
        !strcmp(artifact, path);
    /* One report per file until the listing is read again: an editor can
       save one file as several writes. */
    if ((!is_source && !is_artifact) || entry->changed || entry->missing) {
      continue;
    }
    if (!exists) {
      entry->missing = is_artifact;
      entry->changed = !is_artifact;
      snprintf(entry->diagnostic, sizeof(entry->diagnostic), "%s",
               is_artifact ? "Artifact removed on disk. Rebuild or Reimport."
                           : "Source removed on disk. Reimport to update.");
      continue;
    }
    entry->changed = true_v;
    if (!is_source) {
      snprintf(entry->diagnostic, sizeof(entry->diagnostic),
               "Artifact changed on disk. Rebuild or Reimport.");
      continue;
    }
    const uint64_t scene_length = strlen(resolved_scene);
    if (content->read_only || content->action.kind || entry->scope != 0u ||
        entry->kind == CONTENT_SCENE || !content->scene[0] ||
        strncmp(source, resolved_scene, scene_length)) {
      snprintf(entry->diagnostic, sizeof(entry->diagnostic),
               "Source changed on disk. Rebuild to update.");
      log_warn("Content: %s changed on disk; Rebuild to update it",
               entry->name);
      continue;
    }
    snprintf(entry->diagnostic, sizeof(entry->diagnostic),
             "Source changed on disk.");
    content->action = (VkrEditorContentAction){
        .kind = VKR_EDITOR_CONTENT_ACTION_REBUILD,
        .automatic = true_v,
    };
    content_copy(content->action.asset_id, sizeof(content->action.asset_id),
                 entry->id);
    content_copy(content->action.source, sizeof(content->action.source),
                 entry->source);
    content_copy(content->action.name, sizeof(content->action.name),
                 entry->name);
    content_copy(content->action.scope, sizeof(content->action.scope), "scene");
  }
}

static void content_poll_changes(VkrEditorContent *content) {
  if (!content->watch) {
    return;
  }
  char changed[16][EDITOR_BAKERY_SERVICE_PATH];
  uint32_t count = 0u;
  while ((count = editor_bakery_service_take_changes(
              content->service, content->watch, changed,
              ArrayCount(changed))) != 0u) {
    for (uint32_t i = 0u; i < count; ++i) {
      content_apply_change(content, changed[i]);
    }
    content->filter_dirty = true_v;
  }
}

void vkr_editor_content_set_scripts(VkrEditorContent *content,
                                    const VkrEditorScripts *scripts) {
  if (content) {
    content->scripts = scripts;
    content->scripts_revision = UINT64_MAX;
  }
}

/* One Script asset per project module, in Scripts (ADR-079): it opens the
   module's `<name>.c`, else its first C source, and drags onto objects to
   attach its script. */
static void content_read_scripts(VkrEditorContent *content) {
  const uint32_t modules = vkr_editor_scripts_module_count(content->scripts);
  const uint32_t files = vkr_editor_scripts_file_count(content->scripts);
  for (uint32_t m = 0; m < modules && content_reserve(content); ++m) {
    const VkrEditorScriptModule *module =
        vkr_editor_scripts_module(content->scripts, m);
    /* New modules name their source after the module in lower case. */
    char wanted[VKR_EDITOR_SCRIPT_NAME + 2u];
    snprintf(wanted, sizeof(wanted), "%s.c", module->name);
    for (char *c = wanted; *c; ++c) {
      *c = (char)tolower((unsigned char)*c);
    }
    const VkrEditorScriptFile *source = NULL;
    for (uint32_t f = 0; f < files; ++f) {
      const VkrEditorScriptFile *file =
          vkr_editor_scripts_file(content->scripts, f);
      const size_t length = strlen(file->name);
      if (file->module != m || length < 2u ||
          strcmp(file->name + length - 2u, ".c")) {
        continue;
      }
      if (!source || !strcmp(file->name, wanted)) {
        source = file;
      }
    }
    ContentAsset *entry = &content->entries[content->count++];
    MemZero(entry, sizeof(*entry));
    entry->scope = 1u;
    entry->label = CONTENT_NONE;
    entry->kind = CONTENT_SCRIPT;
    snprintf(entry->id, sizeof(entry->id), "script-%016llx",
             (unsigned long long)content_hash(UINT64_C(14695981039346656037),
                                              module->directory,
                                              strlen(module->directory)));
    content_copy(entry->name, sizeof(entry->name), module->name);
    content_copy(entry->path, sizeof(entry->path),
                 source ? source->path : module->description);
    content_copy(entry->source, sizeof(entry->source),
                 source ? source->path : module->description);
    content_copy(entry->role, sizeof(entry->role), module->name);
    content_copy(entry->type_folder, sizeof(entry->type_folder), "Scripts");
  }
}

void vkr_editor_content_set_service(VkrEditorContent *content,
                                    EditorBakeryService *service) {
  if (!content || content->service == service) {
    return;
  }
  editor_bakery_service_unwatch(content->service, content->watch);
  content->watch = EDITOR_BAKERY_SERVICE_NONE;
  content->watched[0] = '\0';
  content->service = service;
  content_watch_project(content);
}

void vkr_editor_content_refresh(VkrEditorContent *content) {
  if (!content) {
    return;
  }
  content_cancel_worker(content);
  ++content->generation;
  content->count = 0;
  content->selected = CONTENT_NONE;
  content->click_asset = CONTENT_NONE;
  content->drag_asset = CONTENT_NONE;
  content->renaming = CONTENT_NONE;
  content->menu_requested = false_v;
  content->dragging = false_v;
  content->first_row = 0;
  content->diagnostic[0] = 0;
  content->inventory_key = UINT64_C(14695981039346656037);
  for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
    content_release_preview(&content->cache[i]);
  }
  content_read_builtins(content);
  content->listed_at = (int64_t)time(NULL);
  content_watch_project(content);
  char root[CONTENT_PATH];
  char manifest[CONTENT_PATH];
  if (content->project[0]) {
    snprintf(root, sizeof(root), "%s/projects/%s", content->workspace,
             content->project);
    snprintf(manifest, sizeof(manifest), "%s/project.json", root);
    content_read_inventory(content, root, manifest, 1, "");
    content_read_scripts(content);
    content->scripts_revision = vkr_editor_scripts_revision(content->scripts);
    const uint32_t first_scene = content->count;
    content_read_scenes(content, root, manifest);
    /* Every scene's own assets list in its folder. */
    const uint32_t scene_end = content->count;
    for (uint32_t i = first_scene; i < scene_end; ++i) {
      char home[sizeof(content->entries[i].home)];
      content_copy(home, sizeof(home), content->entries[i].home);
      snprintf(root, sizeof(root), "%s/projects/%s/scenes/%s",
               content->workspace, content->project, content->entries[i].id);
      snprintf(manifest, sizeof(manifest), "%s/scene.json", root);
      content_read_inventory(content, root, manifest, 0, home);
    }
  }
  if (content->workspace[0]) {
    snprintf(root, sizeof(root), "%s/editor/bundles/1", content->workspace);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", root);
    content_read_inventory(content, root, manifest, 2, "");
  }
  content_presets_load(content);
  content_labels_load(content);
  content->asset_count = content->count;
  /* Loaded containers' objects follow on the next sync. */
  content->objects_key = 0u;
  content->label_asset[0] = '\0';
  content->filter_dirty = true_v;
}

/* Objects a container lists: authored ones, not the nodes a model imports. */
static bool8_t content_object_listed(const VkrScene *scene,
                                     VkrEntityId entity) {
  const SceneSourceIdentity *source = vkr_entity_get_component(
      scene->world, entity, scene->comp_source_identity);
  return (!source || source->gltf_node_index == UINT32_MAX) &&
         vkr_scene_get_name(scene, entity).length;
}

/* Visit the listed objects of every loaded container: the World, the
   primary scene and the added scenes, with each one's folder. */
typedef void (*ContentObjectVisit)(VkrEditorContent *content,
                                   const VkrScene *scene, VkrEntityId entity,
                                   const char *home, void *context);

static void
content_visit_objects(VkrEditorContent *content, const VkrSampleUiFrame *frame,
                      const char *const scene_ids[1 + VKR_SCENE_ADDITIVE_MAX],
                      ContentObjectVisit visit, void *context) {
  const VkrScene *scenes[2 + VKR_SCENE_ADDITIVE_MAX] = {frame->world,
                                                        frame->scene};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2 + i] = frame->additive[i];
  }
  for (uint32_t c = 0; c < ArrayCount(scenes); ++c) {
    const VkrScene *scene = scenes[c];
    if (!scene || !scene->world) {
      continue;
    }
    char home[40] = {0};
    const char *id = c ? scene_ids[c - 1u] : "";
    if (id && id[0]) {
      snprintf(home, sizeof(home), "%c%s", CONTENT_SCENE_MARK, id);
    }
    const VkrWorld *world = scene->world;
    for (uint32_t i = 0; i < world->dir.capacity; ++i) {
      if (!world->dir.records[i].chunk) {
        continue;
      }
      const VkrEntityId entity = vkr_entity_id_from_index(world, i);
      if (content_object_listed(scene, entity)) {
        visit(content, scene, entity, home, context);
      }
    }
  }
}

static void content_object_hash(VkrEditorContent *content,
                                const VkrScene *scene, VkrEntityId entity,
                                const char *home, void *context) {
  (void)content;
  uint64_t *key = context;
  const String8 name = vkr_scene_get_name(scene, entity);
  *key = content_hash(*key, &entity, sizeof(entity));
  *key = content_hash(*key, name.str, name.length);
  *key = content_hash(*key, home, strlen(home));
  *key = content_hash(*key, &scene->structure_revision,
                      sizeof(scene->structure_revision));
}

static void content_object_add(VkrEditorContent *content, const VkrScene *scene,
                               VkrEntityId entity, const char *home,
                               void *context) {
  (void)context;
  if (!content_reserve(content)) {
    return;
  }
  ContentAsset *entry = &content->entries[content->count++];
  MemZero(entry, sizeof(*entry));
  entry->kind = CONTENT_ENTITY;
  entry->scope = CONTENT_SCOPE_BUILTIN;
  entry->label = CONTENT_NONE;
  entry->entity = entity;
  Vec4 color;
  entry->entity_icon = vkr_editor_entity_icon(scene, entity, false_v, &color);
  snprintf(entry->id, sizeof(entry->id), "entity.%016llx",
           (unsigned long long)entity.u64);
  const String8 name = vkr_scene_get_name(scene, entity);
  snprintf(entry->name, sizeof(entry->name), "%.*s", (int)name.length,
           (const char *)name.str);
  content_copy(entry->home, sizeof(entry->home), home);
}

/* Marks objects whose subtree renders a mesh from a cooking build revision
   (ADR-077), after the objects or the revisions change and once a second
   while cooking, as models finish loading. */
static void content_mark_cooking_objects(VkrEditorContent *content,
                                         const VkrSampleUiFrame *frame,
                                         bool8_t rebuilt) {
  char revisions[16][37];
  const uint32_t count = vkr_editor_content_cooking(
      content, revisions, ArrayCount(revisions), NULL, 0u);
  uint64_t key = UINT64_C(14695981039346656037) ^ count;
  for (uint32_t r = 0u; r < count; ++r) {
    for (const char *byte = revisions[r]; *byte; ++byte) {
      key = (key ^ (uint8_t)*byte) * UINT64_C(1099511628211);
    }
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (!rebuilt && key == content->objects_marked_key &&
      (!count || now - content->objects_marked_at < 1.0)) {
    return;
  }
  content->objects_marked_key = key;
  content->objects_marked_at = now;
  for (uint32_t i = content->asset_count; i < content->count; ++i) {
    content->entries[i].cooking_object = false_v;
  }
  for (uint32_t i = content->asset_count; count && i < content->count; ++i) {
    ContentAsset *object = &content->entries[i];
    const VkrScene *scene = vkr_editor_entity_scene(frame, object->entity);
    if (object->kind != CONTENT_ENTITY || object->cooking_object || !scene) {
      continue;
    }
    for (uint32_t slot = 0u; slot < scene->world->dir.capacity; ++slot) {
      if (!scene->world->dir.records[slot].chunk) {
        continue;
      }
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, slot);
      if (!vkr_editor_entity_mesh_from(frame, scene, entity, revisions,
                                       count)) {
        continue;
      }
      /* Every object above the mesh cooks with it. */
      for (VkrEntityId ancestor = entity; ancestor.u64;) {
        for (uint32_t j = content->asset_count; j < content->count; ++j) {
          if (content->entries[j].entity.u64 == ancestor.u64) {
            content->entries[j].cooking_object = true_v;
          }
        }
        const SceneTransform *transform = vkr_entity_get_component(
            scene->world, ancestor, scene->comp_transform);
        ancestor = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
      }
    }
  }
}

static void content_reveal_created(VkrEditorContent *content,
                                   const VkrSampleUiFrame *frame);

void vkr_editor_content_sync_objects(
    VkrEditorContent *content, const VkrSampleUiFrame *frame,
    const char *const scene_ids[1 + VKR_SCENE_ADDITIVE_MAX]) {
  if (!content || !frame) {
    return;
  }
  content->right_pressed =
      !frame->mouse_captured &&
      input_button_just_pressed(frame->input, BUTTON_RIGHT);
  content->rename_enter = input_key_just_pressed(frame->input, KEY_ENTER);
  content->rename_escape = input_key_just_pressed(frame->input, KEY_ESCAPE);
  uint64_t key = UINT64_C(14695981039346656037);
  content_visit_objects(content, frame, scene_ids, content_object_hash, &key);
  if (key == content->objects_key) {
    content_mark_cooking_objects(content, frame, false_v);
    content_reveal_created(content, frame);
    return;
  }
  content->objects_key = key;
  /* Keep a selected object selected across the rebuild. */
  const bool8_t object_selected = content->selected < content->count &&
                                  content->selected >= content->asset_count;
  const VkrEntityId selected = object_selected
                                   ? content->entries[content->selected].entity
                                   : VKR_ENTITY_ID_INVALID;
  if (object_selected) {
    content->selected = CONTENT_NONE;
  }
  content->count = content->asset_count;
  content_visit_objects(content, frame, scene_ids, content_object_add, NULL);
  for (uint32_t i = content->asset_count; selected.u64 && i < content->count;
       ++i) {
    if (content->entries[i].entity.u64 == selected.u64) {
      content->selected = i;
    }
  }
  content_mark_cooking_objects(content, frame, true_v);
  content->filter_dirty = true_v;
  content_reveal_created(content, frame);
}

void vkr_editor_content_set_project(VkrEditorContent *content,
                                    const char *workspace_root,
                                    const char *project_id,
                                    const char *scene_id) {
  if (!content || !workspace_root || !project_id || !scene_id) {
    return;
  }
  if (!strcmp(content->workspace, workspace_root) &&
      !strcmp(content->project, project_id) &&
      !strcmp(content->scene, scene_id)) {
    return;
  }
  if (strcmp(content->project, project_id)) {
    content->folder[0] = '\0';
    content->selected_folder[0] = '\0';
    content->tag_filter[0] = '\0';
    content->history_count = 0;
    content->history_index = 0;
  }
  content->tree_scroll = 0;
  if (!content_copy(content->workspace, sizeof(content->workspace),
                    workspace_root) ||
      !content_copy(content->project, sizeof(content->project), project_id) ||
      !content_copy(content->scene, sizeof(content->scene), scene_id)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Content context path or ID exceeds supported capacity.");
    return;
  }
  vkr_editor_content_refresh(content);
}

void vkr_editor_content_set_read_only(VkrEditorContent *content,
                                      bool8_t read_only) {
  if (content) {
    content->read_only = read_only;
    if (read_only) {
      content->action = (VkrEditorContentAction){0};
    }
  }
}

void vkr_editor_content_set_cooking(VkrEditorContent *content, bool8_t project,
                                    const char *scene_id) {
  if (!content) {
    return;
  }
  content->cooking_project = project;
  content_copy(content->cooking_scene, sizeof(content->cooking_scene),
               scene_id ? scene_id : "");
}

void vkr_editor_content_set_loading(VkrEditorContent *content,
                                    const char (*ids)[37], uint32_t count,
                                    const char *scene_id) {
  if (!content) {
    return;
  }
  content->loading_count =
      Min(count, (uint32_t)ArrayCount(content->loading_ids));
  if (content->loading_count) {
    MemCopy(content->loading_ids, ids,
            content->loading_count * sizeof(content->loading_ids[0]));
  }
  content_copy(content->loading_scene, sizeof(content->loading_scene),
               scene_id ? scene_id : "");
}

bool8_t vkr_editor_content_loading(const VkrEditorContent *content, char *label,
                                   uint32_t label_capacity) {
  if (label && label_capacity) {
    label[0] = '\0';
  }
  if (!content || (!content->loading_count && !content->loading_scene[0])) {
    return false_v;
  }
  uint32_t count = 0u;
  const char *first = NULL;
  for (uint32_t i = 0; i < content->asset_count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (content_loading(content, entry)) {
      first = first ? first : entry->name;
      ++count;
    }
  }
  if (first && label && label_capacity) {
    if (count > 1u) {
      snprintf(label, label_capacity, "%s and %u more", first, count - 1u);
    } else {
      snprintf(label, label_capacity, "%s", first);
    }
  }
  return count != 0u;
}

uint32_t vkr_editor_content_cooking(const VkrEditorContent *content,
                                    char (*revisions)[37], uint32_t capacity,
                                    char *label, uint32_t label_capacity) {
  if (label && label_capacity) {
    label[0] = '\0';
  }
  if (!content || (!content->cooking_project && !content->cooking_scene[0])) {
    return 0u;
  }
  uint32_t count = 0u;
  const char *first = NULL;
  for (uint32_t i = 0; i < content->asset_count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (entry->kind != CONTENT_MESH || !entry->revision[0] ||
        !content_cooking(content, entry)) {
      continue;
    }
    first = first ? first : entry->name;
    if (count < capacity) {
      content_copy(revisions[count], sizeof(revisions[0]), entry->revision);
    }
    ++count;
  }
  if (first && label && label_capacity) {
    if (count > 1u) {
      snprintf(label, label_capacity, "%s and %u more", first, count - 1u);
    } else {
      snprintf(label, label_capacity, "%s", first);
    }
  }
  return Min(count, capacity);
}

void vkr_editor_content_suspend_previews(VkrEditorContent *content,
                                         bool8_t suspended) {
  if (!content) {
    return;
  }
  content->suspended = suspended;
  if (suspended) {
    content_cancel_worker(content);
  }
}

void vkr_editor_content_update(VkrEditorContent *content) {
  if (!content) {
    return;
  }
  ++content->frame;
  content->uploads_this_frame = 0;
  content_poll_changes(content);
  if (content->scripts && content->project[0] &&
      content->scripts_revision !=
          vkr_editor_scripts_revision(content->scripts)) {
    vkr_editor_content_refresh(content);
  }
  if (content->worker &&
      vkr_atomic_bool_load(&content->complete, VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)vkr_thread_join(content->worker);
    (void)vkr_thread_destroy(content->allocator, &content->worker);
    vkr_platform_process_lock_release(&content->process_lock);
    if (content->worker_generation == content->generation &&
        content->running < CONTENT_CACHE_COUNT) {
      ContentPreview *preview = &content->cache[content->running];
      preview->queued = false_v;
      if (!content->suspended && (!content->worker_ok || content->worker_exit ||
                                  content->worker_timed_out)) {
        preview->failed = true_v;
        snprintf(preview->diagnostic, sizeof(preview->diagnostic),
                 "Preview failed (exit %d). Retry preview.",
                 content->worker_exit);
        FilePath log = {.path = content_string(content->worker_log),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
        FileHandle file = {0};
        FileMode mode = bitset8_create();
        bitset8_set(&mode, FILE_MODE_READ);
        uint64_t read = 0;
        if (file_open(&log, mode, &file) == FILE_ERROR_NONE &&
            file_read_into(&file, preview->diagnostic,
                           sizeof(preview->diagnostic) - 1,
                           &read) == FILE_ERROR_NONE &&
            read) {
          preview->diagnostic[read] = 0;
        }
        file_close(&file);
      }
    }
    content->running = CONTENT_NONE;
  }
}

void vkr_editor_content_build_drag(VkrEditorContent *content, VkrUiSystem *ui) {
  if (content && ui) {
    content_drag_update(content, ui);
  }
}

void vkr_editor_content_set_drop_target(VkrEditorContent *content,
                                        VkrUiRect rect) {
  if (content) {
    content->drop_target = rect;
  }
}

void vkr_editor_content_search(VkrEditorContent *content, String8 query) {
  const uint64_t length = Min(query.length, sizeof(content->query) - 1u);
  MemCopy(content->query, query.str, length);
  content->query[length] = 0;
  content->query_length = (uint32_t)length;
  content->filter_dirty = true_v;
  content->first_row = 0;
}

bool8_t vkr_editor_content_take_action(VkrEditorContent *content,
                                       VkrEditorContentAction *action) {
  if (!content || !action || !content->action.kind) {
    return false_v;
  }
  *action = content->action;
  content->action = (VkrEditorContentAction){0};
  return true_v;
}

bool8_t vkr_editor_content_write_settings(VkrEditorContent *content,
                                          VkrJsonWriter *writer) {
  if (!content || !writer) {
    return false_v;
  }
  return vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("size")) &&
         vkr_json_writer_u64(writer, content->size) &&
         vkr_json_writer_name(writer, string8_lit("folder")) &&
         vkr_json_writer_string(writer, content_string(content->folder)) &&
         vkr_json_writer_name(writer, string8_lit("sort")) &&
         vkr_json_writer_u64(writer, content->sort) &&
         vkr_json_writer_name(writer, string8_lit("reverse")) &&
         vkr_json_writer_bool(writer, content->reverse_sort) &&
         vkr_json_writer_name(writer, string8_lit("details_hidden")) &&
         vkr_json_writer_bool(writer, content->details_hidden) &&
         vkr_json_writer_name(writer, string8_lit("search")) &&
         vkr_json_writer_string(writer,
                                content_string((char *)content->query)) &&
         vkr_json_writer_end_object(writer);
}

typedef struct ContentSettingsBuffer {
  uint8_t data[1024];
  uint32_t length;
} ContentSettingsBuffer;

static bool8_t content_settings_sink(void *context, const uint8_t *data,
                                     uint64_t length) {
  ContentSettingsBuffer *buffer = context;
  if (length > sizeof(buffer->data) - buffer->length) {
    return false_v;
  }
  MemCopy(buffer->data + buffer->length, data, length);
  buffer->length += (uint32_t)length;
  return true_v;
}

String8 vkr_editor_content_settings(VkrEditorContent *content,
                                    VkrAllocator *allocator) {
  if (!content || !allocator) {
    return (String8){0};
  }
  ContentSettingsBuffer buffer = {0};
  VkrJsonWriter writer = {0};
  vkr_json_writer_init(&writer, content_settings_sink, &buffer);
  if (!vkr_editor_content_write_settings(content, &writer) ||
      !vkr_json_writer_complete(&writer)) {
    return (String8){0};
  }
  String8 view = {.str = buffer.data, .length = buffer.length};
  return string8_duplicate(allocator, &view);
}

void vkr_editor_content_restore_settings(VkrEditorContent *content,
                                         String8 settings) {
  if (!content) {
    return;
  }
  content->size = 128;
  content->sort = CONTENT_SORT_NAME;
  content->folder[0] = '\0';
  content->selected_folder[0] = '\0';
  content->tag_filter[0] = '\0';
  content->reverse_sort = false_v;
  content->details_hidden = false_v;
  content->history_count = 0;
  content->history_index = 0;
  content->query[0] = 0;
  content->query_length = 0;
  content->first_row = 0;
  content->filter_dirty = true_v;
  if (!settings.str || !settings.length) {
    return;
  }
  VkrEditorProjectError error = {0};
  (void)vkr_editor_project_json_string(settings, "search",
                                       (char *)content->query,
                                       sizeof(content->query), &error);
  content->query_length = (uint32_t)strlen((char *)content->query);
  if (!vkr_editor_project_json_string(settings, "folder", content->folder,
                                      sizeof(content->folder), &error) ||
      !vkr_editor_folder_normalize(content->folder)) {
    content->folder[0] = '\0';
  }
  VkrJsonReader reader = vkr_json_reader_from_string(settings);
  int32_t size = 128;
  int32_t sort = 0;
  (void)vkr_json_get_int(&reader, "size", &size);
  reader.pos = 0;
  (void)vkr_json_get_int(&reader, "sort", &sort);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "reverse", &content->reverse_sort);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "details_hidden", &content->details_hidden);
  content->size = size == 256                 ? 256
                  : size == CONTENT_SIZE_LIST ? CONTENT_SIZE_LIST
                                              : 128;
  content->sort = sort >= 0 && sort < CONTENT_SORT_COUNT ? (ContentSort)sort
                                                         : CONTENT_SORT_NAME;
  content->filter_dirty = true_v;
}

static bool8_t content_matches(const char *text, const uint8_t *query,
                               uint32_t length) {
  if (!length) {
    return true_v;
  }
  uint64_t text_length = strlen(text);
  for (uint64_t start = 0; start + length <= text_length; ++start) {
    bool8_t match = true_v;
    for (uint32_t i = 0; i < length; ++i) {
      if (tolower((uint8_t)text[start + i]) != tolower(query[i])) {
        match = false_v;
        break;
      }
    }
    if (match) {
      return true_v;
    }
  }
  return false_v;
}

static int32_t content_compare_text(const char *a, const char *b) {
  for (;; ++a, ++b) {
    const int32_t left = tolower((uint8_t)*a);
    const int32_t right = tolower((uint8_t)*b);
    if (left != right || !left) {
      return left - right;
    }
  }
}

/* Order of two shown items by the sort column, then by name. */
static int32_t content_order(const VkrEditorContent *content, uint32_t a,
                             uint32_t b) {
  const ContentAsset *left = &content->entries[a];
  const ContentAsset *right = &content->entries[b];
  int32_t order = 0;
  if (content->sort == CONTENT_SORT_TYPE) {
    order = content_compare_text(content_kinds[left->kind],
                                 content_kinds[right->kind]);
  } else if (content->sort == CONTENT_SORT_LOCATION) {
    order = content_compare_text(content_item_folder(content, left),
                                 content_item_folder(content, right));
  } else if (content->sort == CONTENT_SORT_TAGS) {
    const VkrEditorAssetLabel *l = content_entry_label(content, left);
    const VkrEditorAssetLabel *r = content_entry_label(content, right);
    order = content_compare_text(l ? l->tags : "", r ? r->tags : "");
  }
  if (!order) {
    order = content_compare_text(left->name, right->name);
  }
  return content->reverse_sort ? -order : order;
}

/* Rebuild the shown list when inventory, labels, folder, search or sort
   change: the current folder's subfolders, then its items. A search or tag
   lists matching items anywhere below the current folder instead. Sorted
   insertion runs only here; card generation never sorts or scans files. */
static void content_filter(VkrEditorContent *content) {
  if (!content->filter_dirty) {
    return;
  }
  if (!content->filtered) {
    content->filtered = vkr_allocator_alloc(
        content->allocator,
        (CONTENT_MAX_ASSETS + CONTENT_TREE_MAX) * sizeof(*content->filtered),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!content->filtered) {
      return;
    }
  }
  content_tree_build(content);
  content_collect_tags(content);
  content->filtered_count = 0;
  const bool8_t searching = content->query_length || content->tag_filter[0];
  const uint32_t current = content_tree_find(content, content->folder);
  for (uint32_t i = 1; !searching && i < content->tree_count; ++i) {
    if (content->tree[i].parent != current) {
      continue;
    }
    uint32_t cursor = content->filtered_count++;
    const char *name = content->tree[i].label;
    while (cursor) {
      const char *other =
          content->tree[content->filtered[cursor - 1u] & ~CONTENT_FOLDER_BIT]
              .label;
      const int32_t order = content_compare_text(other, name);
      if (content->reverse_sort ? order >= 0 : order <= 0) {
        break;
      }
      content->filtered[cursor] = content->filtered[cursor - 1u];
      --cursor;
    }
    content->filtered[cursor] = CONTENT_FOLDER_BIT | i;
  }
  const uint32_t first_item = content->filtered_count;
  /* A search from Content leaves out what System ships. */
  const bool8_t in_system = content_in_system(content->folder);
  for (uint32_t i = 0; i < content->count; ++i) {
    ContentAsset *entry = &content->entries[i];
    /* A scene lists as its folder. */
    if (entry->kind == CONTENT_SCENE) {
      continue;
    }
    const VkrEditorAssetLabel *label = entry->scope == CONTENT_SCOPE_BUILTIN
                                           ? NULL
                                           : content_label(content, entry->id);
    entry->label =
        label ? (uint32_t)(label - content->labels.labels) : CONTENT_NONE;
    const char *folder = content_item_folder(content, entry);
    const char *tags = label ? label->tags : "";
    const bool8_t shown =
        searching
            ? vkr_editor_folder_within(folder, content->folder) &&
                  (in_system || !content_in_system(folder)) &&
                  (!content->tag_filter[0] ||
                   content_tags_contain(tags, content->tag_filter)) &&
                  (content_matches(entry->name, content->query,
                                   content->query_length) ||
                   content_matches(content_kinds[entry->kind], content->query,
                                   content->query_length) ||
                   content_matches(folder, content->query,
                                   content->query_length) ||
                   content_matches(tags, content->query, content->query_length))
            : !strcmp(folder, content->folder);
    if (!shown) {
      continue;
    }
    uint32_t cursor = content->filtered_count++;
    while (cursor > first_item &&
           content_order(content, content->filtered[cursor - 1u], i) > 0) {
      content->filtered[cursor] = content->filtered[cursor - 1u];
      --cursor;
    }
    content->filtered[cursor] = i;
  }
  content->filter_dirty = false_v;
}

static bool8_t content_png_size(const char *name, uint32_t *width,
                                uint32_t *height) {
  FilePath path = {.path = content_string(name),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  FileStats stats = {0};
  if (file_stats(&path, &stats) != FILE_ERROR_NONE || stats.size < 24 ||
      stats.size > MB(1)) {
    return false_v;
  }
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle file = {0};
  uint8_t header[24];
  uint64_t read = 0;
  bool8_t valid =
      file_open(&path, mode, &file) == FILE_ERROR_NONE &&
      file_read_into(&file, header, sizeof(header), &read) == FILE_ERROR_NONE &&
      read == sizeof(header);
  file_close(&file);
  static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (!valid || MemCompare(header, signature, sizeof(signature)) ||
      MemCompare(header + 12, "IHDR", 4)) {
    return false_v;
  }
  *width = (uint32_t)header[16] << 24 | (uint32_t)header[17] << 16 |
           (uint32_t)header[18] << 8 | header[19];
  *height = (uint32_t)header[20] << 24 | (uint32_t)header[21] << 16 |
            (uint32_t)header[22] << 8 | header[23];
  return *width > 0 && *height > 0 && *width <= 256 && *height <= 256;
}

static ContentPreview *content_preview(VkrEditorContent *content,
                                       VkrUiSystem *ui, uint32_t asset) {
  ContentAsset *entry = &content->entries[asset];
  if (entry->kind != CONTENT_TEXTURE && entry->kind != CONTENT_ENVIRONMENT &&
      entry->kind != CONTENT_MATERIAL) {
    return NULL;
  }
  if (entry->missing || entry->stale || !entry->path[0]) {
    return NULL;
  }
  uint64_t key =
      content_hash(content->inventory_key, entry->id, strlen(entry->id));
  key = content_hash(key, entry->fingerprint, strlen(entry->fingerprint));
  key = content_hash(key, &content->size, sizeof(content->size));
#if defined(PLATFORM_APPLE)
  static const char recipe[] = "preview-v1-metal-" __DATE__ "-" __TIME__;
#else
  static const char recipe[] = "preview-v1-vulkan-" __DATE__ "-" __TIME__;
#endif
  key = content_hash(key, recipe, sizeof(recipe));
  uint32_t slot = CONTENT_NONE;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
    if (content->cache[i].key == key) {
      slot = i;
      break;
    }
    if (i != content->running && content->cache[i].last_seen < content->frame &&
        content->cache[i].last_seen < oldest) {
      slot = i;
      oldest = content->cache[i].last_seen;
    }
  }
  if (slot == CONTENT_NONE) {
    return NULL;
  }
  ContentPreview *preview = &content->cache[slot];
  if (preview->key != key) {
    content_release_preview(preview);
    preview->key = key;
    preview->asset = asset;
    preview->size = content->size;
    int32_t length =
        snprintf(preview->path, sizeof(preview->path),
                 "%s/cache/thumbnails/%016llx-%u.png", content->workspace,
                 (unsigned long long)key, content->size);
    if (length <= 0 || (uint32_t)length >= sizeof(preview->path)) {
      preview->failed = true_v;
      snprintf(preview->diagnostic, sizeof(preview->diagnostic),
               "Thumbnail cache path exceeds supported capacity.");
    }
  }
  preview->last_seen = content->frame;
  if (preview->request.request_id) {
    VkrResourceHandleInfo resolved = {0};
    if (vkr_resource_system_try_get_resolved(&preview->request, &resolved)) {
      preview->texture = resolved.as.texture;
    } else {
      VkrRendererError error = VKR_RENDERER_ERROR_NONE;
      VkrResourceLoadState state =
          vkr_resource_system_get_state(&preview->request, &error);
      if (state == VKR_RESOURCE_LOAD_STATE_FAILED ||
          state == VKR_RESOURCE_LOAD_STATE_CANCELED) {
        preview->failed = true_v;
        snprintf(preview->diagnostic, sizeof(preview->diagnostic),
                 "Thumbnail decode/upload failed (%u).", (uint32_t)error);
      }
    }
  } else if (!preview->failed && !preview->queued && !content->suspended &&
             content->uploads_this_frame < 2) {
    FilePath path = {.path = content_string(preview->path),
                     .type = FILE_PATH_TYPE_ABSOLUTE};
    if (file_exists(&path)) {
      if (!content_png_size(preview->path, &preview->width, &preview->height)) {
        preview->failed = true_v;
        snprintf(preview->diagnostic, sizeof(preview->diagnostic),
                 "Cached preview is invalid or larger than 256 pixels. Retry "
                 "preview.");
      } else {
        snprintf(preview->request_path, sizeof(preview->request_path),
                 "%s?cs=srgb&source=only", preview->path);
        VkrRendererError error = VKR_RENDERER_ERROR_NONE;
        ++content->uploads_this_frame;
        if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE,
                                      content_string(preview->request_path),
                                      ui->frame_allocator, &preview->request,
                                      &error)) {
          preview->failed = true_v;
          snprintf(preview->diagnostic, sizeof(preview->diagnostic),
                   "Cannot queue preview texture (%u).", (uint32_t)error);
        }
      }
    } else {
      preview->queued = true_v;
    }
  }
  return preview;
}

static void content_start_preview(VkrEditorContent *content) {
  if (content->worker || content->suspended || !content->workspace[0]) {
    return;
  }
  for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
    ContentPreview *preview = &content->cache[i];
    if (!preview->queued || preview->last_seen != content->frame ||
        preview->asset >= content->count) {
      continue;
    }
    ContentAsset *entry = &content->entries[preview->asset];
    const char *input = entry->path;
    // Imported image source avoids decoding a target-specific GPU block format.
    if (entry->kind != CONTENT_MATERIAL && entry->source[0]) {
      const char *extension = strrchr(entry->source, '.');
      if (extension &&
          (!strcmp(extension, ".png") || !strcmp(extension, ".jpg") ||
           !strcmp(extension, ".jpeg") || !strcmp(extension, ".hdr") ||
           !strcmp(extension, ".tga") || !strcmp(extension, ".bmp"))) {
        input = entry->source;
      }
    }
    char directory[CONTENT_PATH];
    int32_t length =
        snprintf(directory, sizeof(directory), "%s/cache", content->workspace);
    if (length <= 0 || (uint32_t)length >= sizeof(directory)) {
      return;
    }
    FilePath cache = {.path = content_string(directory),
                      .type = FILE_PATH_TYPE_ABSOLUTE};
    if (!file_create_directory(&cache)) {
      return;
    }
    length = snprintf(directory, sizeof(directory), "%s/cache/thumbnails",
                      content->workspace);
    if (length <= 0 || (uint32_t)length >= sizeof(directory)) {
      return;
    }
    cache.path = content_string(directory);
    if (!file_create_directory(&cache)) {
      return;
    }
    VkrEditorProjectError error = {0};
    char canonical[CONTENT_PATH];
    if (!vkr_editor_project_resolve(content->workspace, "cache/thumbnails",
                                    canonical, &error)) {
      preview->failed = true_v;
      preview->queued = false_v;
      snprintf(preview->diagnostic, sizeof(preview->diagnostic),
               "Cache is outside the workspace or unreadable.");
      return;
    }
    if (!vkr_platform_process_lock_acquire(
            "VkrEditorBakery", content->workspace, &content->process_lock)) {
      return;
    }
    content_copy(content->worker_input, sizeof(content->worker_input), input);
    content_copy(content->worker_output, sizeof(content->worker_output),
                 preview->path);
    content_copy(content->worker_workspace, sizeof(content->worker_workspace),
                 content->workspace);
    snprintf(content->worker_log, sizeof(content->worker_log), "%.1000s.log",
             preview->path);
    content->worker_size = preview->size;
    content->worker_material = entry->kind == CONTENT_MATERIAL;
    content->worker_reveal = false_v;
    content->worker_generation = content->generation;
    content->worker_ok = false_v;
    content->worker_exit = -1;
    content->worker_timed_out = false_v;
    content->running = i;
    vkr_atomic_bool_store(&content->cancel, false_v, VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_bool_store(&content->complete, false_v,
                          VKR_MEMORY_ORDER_RELAXED);
    if (!vkr_thread_create(content->allocator, &content->worker, content_worker,
                           content)) {
      vkr_platform_process_lock_release(&content->process_lock);
      content->running = CONTENT_NONE;
      preview->queued = false_v;
      preview->failed = true_v;
      snprintf(preview->diagnostic, sizeof(preview->diagnostic),
               "Cannot start the preview worker.");
    }
    return;
  }
}

static VkrUiWidgetConfig content_widget(uint32_t column, uint32_t row) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.column = column;
  config.placement.row = row;
  const VkrUiTheme *theme = vkr_ui_theme();
  config.style.font_size_pt = theme->font_body;
  config.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
  config.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config.style.text_color = theme->text;
  return config;
}

/* A cell-filling button whose icon and text sit at the start, since
   buttons center their content: an empty button under a label. */
static bool8_t content_start_button(VkrUiSystem *ui, String8 id, String8 text,
                                    const VkrUiWidgetConfig *config) {
  VkrUiWidgetConfig hit = *config;
  hit.placement.justify = VKR_UI_ALIGN_STRETCH;
  hit.placement.align = VKR_UI_ALIGN_STRETCH;
  hit.fill = true_v;
  hit.icon = VKR_UI_ICON_NONE;
  const bool8_t clicked = vkr_ui_button(ui, id, (String8){0}, &hit);
  VkrUiWidgetConfig label = *config;
  label.placement.justify = VKR_UI_ALIGN_START;
  label.placement.align = VKR_UI_ALIGN_CENTER;
  label.style.background_color = (Vec4){0};
  label.tooltip = (String8){0};
  (void)vkr_ui_push_id_u64(ui, 0x1abe1u);
  vkr_ui_label(ui, id, text, &label);
  (void)vkr_ui_pop_id(ui);
  return clicked;
}

static void content_action(VkrEditorContent *content,
                           VkrEditorContentActionKind kind) {
  /* A read-only workspace still opens scenes and edits loaded objects; it
     cannot change project files. */
  const bool8_t browsing = kind == VKR_EDITOR_CONTENT_ACTION_DROP_SCRIPT ||
                           kind == VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE ||
                           kind == VKR_EDITOR_CONTENT_ACTION_ADD_SCENE ||
                           kind == VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY ||
                           kind == VKR_EDITOR_CONTENT_ACTION_FRAME_ENTITY ||
                           kind == VKR_EDITOR_CONTENT_ACTION_RENAME_ENTITY ||
                           kind == VKR_EDITOR_CONTENT_ACTION_DELETE_ENTITY ||
                           kind == VKR_EDITOR_CONTENT_ACTION_OPEN_SCRIPT;
  if (content->read_only && !browsing) {
    return;
  }
  content->action = (VkrEditorContentAction){.kind = kind};
  if (content->selected < content->count) {
    ContentAsset *asset = &content->entries[content->selected];
    content_copy(content->action.asset_id, sizeof(content->action.asset_id),
                 asset->id);
    content_copy(content->action.source, sizeof(content->action.source),
                 asset->source);
    content->action.object = asset->object;
    static const char *const owners[] = {"scene", "project", "editor"};
    if (asset->scope < ArrayCount(owners)) {
      content_copy(content->action.scope, sizeof(content->action.scope),
                   owners[asset->scope]);
    }
    content_copy(content->action.name, sizeof(content->action.name),
                 asset->name);
    content->action.entity = asset->entity;
  }
}

/* The scene entry a scene folder shows, or CONTENT_NONE. */
static uint32_t content_folder_scene(const VkrEditorContent *content,
                                     const char *path) {
  for (uint32_t i = 0; content_scene_folder(path) && i < content->asset_count;
       ++i) {
    if (content->entries[i].kind == CONTENT_SCENE &&
        !strcmp(content->entries[i].home, path)) {
      return i;
    }
  }
  return CONTENT_NONE;
}

/* An action on the scene a scene folder shows. */
static void content_scene_action(VkrEditorContent *content, const char *path,
                                 VkrEditorContentActionKind kind) {
  const uint32_t scene = content_folder_scene(content, path);
  if (scene == CONTENT_NONE) {
    return;
  }
  const uint32_t selected = content->selected;
  content->selected = scene;
  content_action(content, kind);
  content->selected = selected;
}

static bool8_t content_shown_folder(uint32_t shown) {
  return shown != CONTENT_NONE && (shown & CONTENT_FOLDER_BIT);
}

/* The selection as a shown value: an asset index or a folder tile. */
static uint32_t content_selection(const VkrEditorContent *content) {
  if (content->selected_folder[0]) {
    const uint32_t folder =
        content_tree_find(content, content->selected_folder);
    return folder == CONTENT_NONE ? CONTENT_NONE : CONTENT_FOLDER_BIT | folder;
  }
  return content->selected;
}

static void content_select(VkrEditorContent *content, uint32_t shown) {
  if (content_shown_folder(shown)) {
    content->selected = CONTENT_NONE;
    content_copy(content->selected_folder, sizeof(content->selected_folder),
                 content->tree[shown & ~CONTENT_FOLDER_BIT].path);
  } else {
    content->selected = shown;
    content->selected_folder[0] = '\0';
  }
}

/* Replace the `from` prefix of a folder path inside that subtree. */
static void content_rebase(char path[VKR_EDITOR_FOLDER_PATH_CAPACITY],
                           const char *from, const char *to) {
  if (!from[0] || !vkr_editor_folder_within(path, from)) {
    return;
  }
  char next[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  const int32_t length =
      snprintf(next, sizeof(next), "%s%s", to, path + strlen(from));
  if (length > 0 && (uint32_t)length < sizeof(next)) {
    MemCopy(path, next, (uint64_t)length + 1u);
  }
}

static void content_open_folder(VkrEditorContent *content, const char *path) {
  if (!strcmp(content->folder, path)) {
    return;
  }
  if (!content->history_count) {
    content_copy(content->history[0], sizeof(content->history[0]),
                 content->folder);
    content->history_count = 1;
    content->history_index = 0;
  }
  content->history_count = content->history_index + 1;
  if (content->history_count == ArrayCount(content->history)) {
    MemCopy(content->history, content->history + 1,
            (ArrayCount(content->history) - 1) * sizeof(content->history[0]));
    --content->history_count;
  }
  content_copy(content->history[content->history_count++],
               sizeof(content->history[0]), path);
  content->history_index = content->history_count - 1;
  content_copy(content->folder, sizeof(content->folder), path);
  content->selected_folder[0] = '\0';
  content->first_row = 0;
  content->filter_dirty = true_v;
}

static void content_history_step(VkrEditorContent *content, int32_t direction) {
  if (!content->history_count || (direction < 0 && !content->history_index) ||
      (direction > 0 && content->history_index + 1 == content->history_count)) {
    return;
  }
  content->history_index =
      (uint32_t)((int32_t)content->history_index + direction);
  content_copy(content->folder, sizeof(content->folder),
               content->history[content->history_index]);
  content->selected_folder[0] = '\0';
  content->first_row = 0;
  content->filter_dirty = true_v;
}

/* Double-click or Enter: a folder or scene opens to show what it holds, an
   object is selected, and an object type is added to the scene. */
static void content_activate(VkrEditorContent *content, uint32_t shown) {
  if (content_shown_folder(shown)) {
    content_open_folder(content,
                        content->tree[shown & ~CONTENT_FOLDER_BIT].path);
    return;
  }
  if (shown >= content->count) {
    return;
  }
  content->selected = shown;
  const ContentKind kind = content->entries[shown].kind;
  if (kind == CONTENT_ENTITY) {
    content_action(content, VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY);
    content->action.activated = true_v;
  } else if (kind == CONTENT_OBJECT) {
    content_action(content, VKR_EDITOR_CONTENT_ACTION_CREATE_OBJECT);
  } else if (kind == CONTENT_SCRIPT) {
    content_action(content, VKR_EDITOR_CONTENT_ACTION_OPEN_SCRIPT);
  }
}

/* A second click on the same item within 0.4 s activates it. */
static void content_click(VkrEditorContent *content, VkrUiSystem *ui,
                          uint32_t shown, VkrUiId grid_id) {
  const float64_t now = vkr_platform_get_absolute_time();
  const bool8_t twice =
      content->click_asset == shown && now - content->click_time < 0.4;
  content->click_asset = twice ? CONTENT_NONE : shown;
  content->click_time = now;
  content_select(content, shown);
  ui->focused_id = grid_id;
  ui->focused_is_text = false_v;
  if (twice) {
    content_activate(content, shown);
  }
}

/* Move an item or folder into `folder`. Built-in items and system folders
   stay in place, and Objects takes nothing else. */
static bool8_t content_move(VkrEditorContent *content, uint32_t shown,
                            const char *folder) {
  if (content->read_only || !content_project_folder(folder)) {
    return false_v;
  }
  if (content_shown_folder(shown)) {
    const uint32_t index = shown & ~CONTENT_FOLDER_BIT;
    if (index >= content->tree_count || content->tree[index].system) {
      return false_v;
    }
    char from[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    char to[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    content_copy(from, sizeof(from), content->tree[index].path);
    const char *name = vkr_editor_folder_name(from);
    const int32_t length =
        snprintf(to, sizeof(to), "%s%s%s", folder, folder[0] ? "/" : "", name);
    if (length <= 0 || (uint32_t)length >= sizeof(to) || !strcmp(from, to) ||
        !vkr_editor_folders_rename(&content->labels, from, to)) {
      return false_v;
    }
    content_rebase(content->folder, from, to);
    content_rebase(content->selected_folder, from, to);
    for (uint32_t i = 0; i < content->history_count; ++i) {
      content_rebase(content->history[i], from, to);
    }
    content_labels_save(content);
    return true_v;
  }
  if (shown >= content->count) {
    return false_v;
  }
  const ContentAsset *entry = &content->entries[shown];
  /* A scene's assets stay in its folder; only project assets move, and not
     while they cook. */
  if (entry->scope != 1u || entry->kind == CONTENT_PRESET ||
      content_locked(content, entry)) {
    return false_v;
  }
  const VkrEditorAssetLabel *label = content_label(content, entry->id);
  char tags[128];
  content_copy(tags, sizeof(tags), label ? label->tags : "");
  /* An empty label folder means the item's default folder. */
  const char *fallback = content_default_folder(entry);
  if (!folder[0] && fallback[0]) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "%s items stay in a folder, not at the root.",
             content_kinds[entry->kind]);
    return false_v;
  }
  return content_label_set(content, entry->id,
                           strcmp(folder, fallback) ? folder : "", tags);
}

/* A new empty folder in `parent`, named "New Folder" or the first free
   "New Folder N", selected for renaming. */
static bool8_t content_new_folder(VkrEditorContent *content,
                                  const char *parent) {
  if (content->read_only || !content->project[0] ||
      !content_project_folder(parent)) {
    return false_v;
  }
  char path[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  for (uint32_t n = 1; n < 100u; ++n) {
    char name[24];
    snprintf(name, sizeof(name), n == 1u ? "New Folder" : "New Folder %u", n);
    const int32_t length = snprintf(path, sizeof(path), "%s%s%s", parent,
                                    parent[0] ? "/" : "", name);
    if (length <= 0 || (uint32_t)length >= sizeof(path)) {
      return false_v;
    }
    if (content_tree_find(content, path) != CONTENT_NONE) {
      continue;
    }
    if (!vkr_editor_folders_add(&content->labels, path)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "Content holds at most %u folders.", CONTENT_FOLDER_MAX);
      return false_v;
    }
    content_labels_save(content);
    content->selected = CONTENT_NONE;
    content_copy(content->selected_folder, sizeof(content->selected_folder),
                 path);
    return true_v;
  }
  return false_v;
}

/* Rename the selected folder in place; `name` cannot contain '/'. */
static bool8_t content_rename_folder(VkrEditorContent *content,
                                     const char *from, const char *name) {
  char parent[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  char to[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  vkr_editor_folder_parent(from, parent);
  const int32_t length =
      snprintf(to, sizeof(to), "%s%s%s", parent, parent[0] ? "/" : "", name);
  if (content->read_only || content_system_folder(from) || !name[0] ||
      strchr(name, '/') || length <= 0 || (uint32_t)length >= sizeof(to) ||
      !content_project_folder(to) ||
      !vkr_editor_folders_rename(&content->labels, from, to)) {
    return false_v;
  }
  content_rebase(content->folder, from, to);
  content_rebase(content->selected_folder, from, to);
  for (uint32_t i = 0; i < content->history_count; ++i) {
    content_rebase(content->history[i], from, to);
  }
  content_labels_save(content);
  return true_v;
}

static const Vec4 content_type_colors[CONTENT_KIND_COUNT] = {
    {0.72f, 0.52f, 0.96f, 1}, {0.45f, 0.84f, 0.56f, 1},
    {0.42f, 0.76f, 0.95f, 1}, {0.96f, 0.64f, 0.40f, 1},
    {0.96f, 0.78f, 0.42f, 1}, {0.50f, 0.68f, 0.98f, 1},
    {0.42f, 0.84f, 0.86f, 1}, {0.86f, 0.56f, 0.72f, 1},
    {0.46f, 0.80f, 0.98f, 1}, {0.98f, 0.82f, 0.46f, 1},
    {0.92f, 0.62f, 0.94f, 1}, {0.80f, 0.66f, 0.98f, 1},
    {0.66f, 0.70f, 0.76f, 1},
};
static const Vec4 content_folder_color = {0.96f, 0.78f, 0.42f, 1.0f};

static bool8_t content_pointer_inside(VkrUiSystem *ui, VkrUiRect rect) {
  return !ui->mouse_captured && ui->input_layer == ui->mouse_input_layer &&
         ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
         ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height;
}

/* While an item is dragged, the folder widget under the pointer becomes the
   drop folder. Uses the widget's last presented rect, clipped to `area`. */
static bool8_t content_drop_probe(VkrEditorContent *content, VkrUiSystem *ui,
                                  String8 widget, const char *path,
                                  VkrUiRect area) {
  if (!content->dragging && !content->file_drop_armed) {
    return false_v;
  }
  const VkrUiId id = vkr_ui_id_stack_widget_label(&ui->id_stack, widget);
  VkrUiRect rect = {0};
  const float32_t x = content->file_drop_armed ? content->file_drop_px.x
                                               : (float32_t)ui->mouse_x;
  const float32_t y = content->file_drop_armed ? content->file_drop_px.y
                                               : (float32_t)ui->mouse_y;
  if (!vkr_ui_widget_rect(ui, id, &rect) || x < rect.x ||
      x >= rect.x + rect.width || y < rect.y || y >= rect.y + rect.height ||
      x < area.x || x >= area.x + area.width || y < area.y ||
      y >= area.y + area.height) {
    return false_v;
  }
  content->drop_folder_set = true_v;
  content_copy(content->drop_folder, sizeof(content->drop_folder), path);
  return true_v;
}

static uint32_t content_navigation_target(uint32_t index, uint32_t count,
                                          uint32_t columns,
                                          uint32_t visible_rows, Keys key) {
  if (!count) {
    return 0;
  }
  int64_t target = index;
  switch (key) {
  case KEY_LEFT:
    --target;
    break;
  case KEY_RIGHT:
    ++target;
    break;
  case KEY_UP:
    target -= columns;
    break;
  case KEY_DOWN:
    target += columns;
    break;
  case KEY_HOME:
    target = 0;
    break;
  case KEY_END:
    target = count - 1;
    break;
  case KEY_PRIOR:
    target -= columns * visible_rows;
    break;
  case KEY_NEXT:
    target += columns * visible_rows;
    break;
  default:
    break;
  }
  return (uint32_t)Max((int64_t)0, Min((int64_t)count - 1, target));
}

static void content_grid_keys(VkrEditorContent *content, VkrUiSystem *ui,
                              VkrUiId grid_id, uint32_t columns,
                              uint32_t visible_rows) {
  if (ui->mouse_captured || ui->focused_is_text || ui->focused_id != grid_id ||
      ui->input_layer != ui->keyboard_input_layer || !content->filtered_count) {
    content->navigation_key = KEY_MAX_KEYS;
    return;
  }
  const uint32_t selection = content_selection(content);
  if (input_key_just_pressed(ui->input, KEY_ENTER) &&
      selection != CONTENT_NONE) {
    content_activate(content, selection);
    ui->capture.keyboard = true_v;
    return;
  }
  const Keys keys[] = {KEY_LEFT, KEY_RIGHT, KEY_UP,    KEY_DOWN,
                       KEY_HOME, KEY_END,   KEY_PRIOR, KEY_NEXT};
  Keys key = KEY_MAX_KEYS;
  for (uint32_t i = 0; i < ArrayCount(keys); ++i) {
    if (input_key_just_pressed(ui->input, keys[i]) ||
        input_is_key_down(ui->input, keys[i])) {
      key = keys[i];
      break;
    }
  }
  if (key == KEY_MAX_KEYS) {
    content->navigation_key = key;
    return;
  }
  bool8_t move = input_key_just_pressed(ui->input, key);
  if (key != content->navigation_key) {
    content->navigation_key = key;
    content->navigation_elapsed = 0;
    content->navigation_next = .35;
  } else {
    content->navigation_elapsed += ui->delta_time;
    if (content->navigation_elapsed >= content->navigation_next) {
      move = true_v;
      content->navigation_next = content->navigation_elapsed + .07;
    }
  }
  if (!move) {
    return;
  }
  uint32_t position = 0;
  while (position < content->filtered_count &&
         content->filtered[position] != selection) {
    ++position;
  }
  position = position == content->filtered_count
                 ? 0
                 : content_navigation_target(position, content->filtered_count,
                                             columns, visible_rows, key);
  content_select(content, content->filtered[position]);
  const uint32_t row = position / columns;
  if (row < content->first_row) {
    content->first_row = row;
  } else if (row >= content->first_row + visible_rows) {
    content->first_row = row - visible_rows + 1;
  }
  ui->capture.keyboard = true_v;
}

/* Pressing an item or folder arms a drag; moving 6 points starts it. */
static void content_drag_begin(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t shown, String8 widget) {
  const VkrUiId id = vkr_ui_id_stack_widget_label(&ui->id_stack, widget);
  if (ui->active_id != id || content->dragging) {
    return;
  }
  const Vec2 mouse = {(float32_t)ui->mouse_x, (float32_t)ui->mouse_y};
  if (content->drag_asset != shown) {
    content->drag_asset = shown;
    content->drag_origin = mouse;
    return;
  }
  const float32_t reach = 6.0f * ui->content_scale;
  if (fabsf(mouse.x - content->drag_origin.x) > reach ||
      fabsf(mouse.y - content->drag_origin.y) > reach) {
    content->dragging = true_v;
  }
}

/* Folder tree, then the project's tags. A chevron opens a folder's
   children; clicking a folder shows it; dropping an item moves it there. */
/* The name a folder shows: its tree label, such as a scene's name. */
static const char *content_folder_label(const VkrEditorContent *content,
                                        const char *path) {
  const uint32_t index = content_tree_find(content, path);
  return index < content->tree_count ? content->tree[index].label
                                     : vkr_editor_folder_name(path);
}

static String8 content_folder_tooltip(const ContentFolder *folder) {
  if (!folder->path[0]) {
    return string8_lit("The World: its objects, scenes, assets and folders");
  }
  if (content_scene_folder(folder->path)) {
    return string8_lit("A scene: its objects and assets. Drag it into the "
                       "viewport to load it");
  }
  if (!strcmp(folder->path, CONTENT_SYSTEM)) {
    return string8_lit("What the editor ships");
  }
  return content_string(folder->label);
}

/* User folders move by dragging; a scene drags into the viewport to open. */
static bool8_t content_folder_draggable(const ContentFolder *folder) {
  return !folder->system || content_scene_folder(folder->path);
}

/* A right press on the widget `id` just built selects its item and asks the
   editor for that item's context menu. */
static void content_context_probe(VkrEditorContent *content, VkrUiSystem *ui,
                                  String8 id, uint32_t shown) {
  if (!content->right_pressed ||
      ui->hot_id != vkr_ui_id_stack_widget_label(&ui->id_stack, id)) {
    return;
  }
  content->right_pressed = false_v;
  content_select(content, shown);
  content->menu_requested = true_v;
  content->menu_folder = false_v;
  content->menu_item = shown;
  content->menu_position_pt =
      (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
             (float32_t)ui->mouse_y / ui->content_scale};
}

static void content_tree_panel(VkrEditorContent *content, VkrUiSystem *ui,
                               VkrUiRect area) {
  uint32_t shown_count = 0u;
  for (uint32_t i = 0; i < content->tree_count; ++i) {
    ContentFolder *folder = &content->tree[i];
    const ContentFolder *parent = folder->parent < content->tree_count
                                      ? &content->tree[folder->parent]
                                      : NULL;
    /* Content and System are both roots. */
    folder->shown = folder->parent == CONTENT_NONE ||
                    (parent && parent->shown && parent->open);
    shown_count += folder->shown;
  }
  VkrUiTrack rows[CONTENT_TREE_MAX + 1u + CONTENT_TAG_MAX];
  const uint32_t row_count =
      shown_count + (content->tag_count ? 1u + content->tag_count : 0u);
  for (uint32_t i = 0; i < row_count; ++i) {
    rows[i] = (VkrUiTrack){CONTENT_TREE_ROW_PT, VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.rows = rows;
  panel.row_count = row_count;
  const VkrUiTheme *theme = vkr_ui_theme();
  panel.style.background_color = theme->header;
  panel.style.padding_pt = (VkrUiEdges){6, 18, 6, 6};
  panel.clip_children = true_v;
  const uint32_t visible =
      Max(1u, (uint32_t)(Max(0.0f, area.height / ui->content_scale - 12) /
                         CONTENT_TREE_ROW_PT));
  if (content_pointer_inside(ui, area) && ui->mouse_wheel) {
    content->tree_scroll -= ui->mouse_wheel * CONTENT_TREE_ROW_PT;
    ui->capture.mouse = true_v;
  }
  const uint32_t tree_max = row_count > visible ? row_count - visible : 0;
  content->tree_scroll =
      vkr_clamp_f32(content->tree_scroll, 0, tree_max * CONTENT_TREE_ROW_PT);
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("tree"), &panel)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &content->tree_scroll);
  uint32_t row = 0;
  for (uint32_t i = 0; i < content->tree_count; ++i) {
    ContentFolder *folder = &content->tree[i];
    if (!folder->shown) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui,
                             content_hash(UINT64_C(14695981039346656037),
                                          folder->path, strlen(folder->path)));
    const float32_t indent = 14.0f * folder->depth;
    const bool8_t drop = content_drop_probe(content, ui, string8_lit("folder"),
                                            folder->path, area);
    const bool8_t current = !strcmp(content->folder, folder->path);
    VkrUiWidgetConfig name = content_widget(0, row++);
    name.placement.margin_pt.left = indent + 18.0f;
    const bool8_t plain = folder->icon == VKR_UI_ICON_FOLDER;
    name.icon = plain && folder->open && folder->has_children
                    ? VKR_UI_ICON_REVEAL
                    : folder->icon;
    name.icon_color = plain ? content_folder_color : theme->accent_hover;
    name.style.background_color = drop
                                      ? vkr_ui_color_alpha(theme->accent, 0.35f)
                                  : current ? theme->selection
                                            : (Vec4){0};
    name.style.hover_background_color = theme->row_hover;
    name.tooltip = content_folder_tooltip(folder);
    if (content_start_button(ui, string8_lit("folder"),
                             content_string(folder->label), &name)) {
      content_open_folder(content, folder->path);
      folder->open = true_v;
    }
    content_context_probe(content, ui, string8_lit("folder"),
                          CONTENT_FOLDER_BIT | i);
    /* The chevron comes last so a press on it wins over the row. */
    if (folder->has_children && folder->parent != CONTENT_NONE) {
      VkrUiWidgetConfig chevron = content_widget(0, row - 1u);
      vkr_editor_ghost_style(&chevron);
      chevron.placement.justify = VKR_UI_ALIGN_START;
      chevron.placement.margin_pt.left = indent;
      chevron.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
      chevron.icon =
          folder->open ? VKR_UI_ICON_CHEVRON_DOWN : VKR_UI_ICON_CHEVRON_RIGHT;
      chevron.icon_size_pt = 11;
      chevron.icon_color = theme->text_secondary;
      if (vkr_ui_button(ui, string8_lit("open"), (String8){0}, &chevron)) {
        folder->open = !folder->open;
      }
    }
    if (content_folder_draggable(folder)) {
      content_drag_begin(content, ui, CONTENT_FOLDER_BIT | i,
                         string8_lit("folder"));
    }
    (void)vkr_ui_pop_id(ui);
  }
  /* A tag lists tagged items below the current folder; clicking again
     clears it. */
  if (content->tag_count) {
    VkrUiWidgetConfig label = content_widget(0, row++);
    label.style.text_color = theme->text_secondary;
    label.style.font_size_pt = theme->font_caption;
    vkr_ui_label(ui, string8_lit("tags"), string8_lit("Tags"), &label);
  }
  for (uint32_t i = 0; i < content->tag_count; ++i) {
    (void)vkr_ui_push_id_u64(ui, 0x7a900000u + i);
    const bool8_t chosen = !strcmp(content->tag_filter, content->tags[i]);
    VkrUiWidgetConfig tag = content_widget(0, row++);
    tag.placement.justify = VKR_UI_ALIGN_STRETCH;
    tag.fill = true_v;
    tag.icon = VKR_UI_ICON_TAG;
    tag.icon_color = (Vec4){0.62f, 0.78f, 0.98f, 1.0f};
    tag.style.background_color = chosen ? theme->selection : (Vec4){0};
    tag.style.hover_background_color = theme->row_hover;
    if (content_start_button(ui, string8_lit("tag"),
                             content_string(content->tags[i]), &tag)) {
      content_copy(content->tag_filter, sizeof(content->tag_filter),
                   chosen ? "" : content->tags[i]);
      content->filter_dirty = true_v;
      content->first_row = 0;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_scroll_area_end(ui);
}

/* Back, forward, up, then one button per folder from the current folder's
   root, Content or System, down to it. */
static void content_build_navigation(VkrEditorContent *content,
                                     VkrUiSystem *ui) {
  enum { SEGMENT_MAX = 6 };
  uint32_t chain[SEGMENT_MAX];
  uint32_t chain_count = 0u;
  bool8_t elided = false_v;
  /* Walk up from the current folder so the deepest folders stay visible. */
  uint32_t node = content_tree_find(content, content->folder);
  while (node != CONTENT_NONE) {
    if (chain_count == SEGMENT_MAX) {
      elided = true_v;
      break;
    }
    MemCopy(chain + 1, chain, chain_count * sizeof(chain[0]));
    chain[0] = node;
    ++chain_count;
    node = content->tree[node].parent;
  }
  VkrUiTrack columns[3 + SEGMENT_MAX + 1];
  const uint32_t column_count = 3u + chain_count + 1u;
  columns[0] = columns[1] = columns[2] = (VkrUiTrack){28, VKR_UI_TRACK_PX};
  for (uint32_t i = 3; i < column_count - 1u; ++i) {
    columns[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};
  }
  columns[column_count - 1u] = (VkrUiTrack){1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig navigation = vkr_ui_panel_config_default();
  navigation.placement.column = 4;
  navigation.placement.row = 0;
  navigation.columns = columns;
  navigation.column_count = column_count;
  navigation.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("navigation"), &navigation)) {
    return;
  }
  VkrUiWidgetConfig back = vkr_editor_icon_button_config(
      0, 0, VKR_UI_ICON_ARROW_LEFT, string8_lit("Back to the previous folder"));
  back.disabled = !content->history_count || !content->history_index;
  if (vkr_ui_button(ui, string8_lit("back"), (String8){0}, &back)) {
    content_history_step(content, -1);
  }
  VkrUiWidgetConfig forward = vkr_editor_icon_button_config(
      1, 0, VKR_UI_ICON_ARROW_RIGHT, string8_lit("Forward to the next folder"));
  forward.disabled = !content->history_count ||
                     content->history_index + 1 == content->history_count;
  if (vkr_ui_button(ui, string8_lit("forward"), (String8){0}, &forward)) {
    content_history_step(content, 1);
  }
  const uint32_t current = content_tree_find(content, content->folder);
  const uint32_t parent = current < content->tree_count
                              ? content->tree[current].parent
                              : CONTENT_NONE;
  VkrUiWidgetConfig up = vkr_editor_icon_button_config(
      2, 0, VKR_UI_ICON_ARROW_UP, string8_lit("Up to the containing folder"));
  up.disabled = parent == CONTENT_NONE;
  if (vkr_ui_button(ui, string8_lit("up"), (String8){0}, &up)) {
    content_open_folder(content, content->tree[parent].path);
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  for (uint32_t i = 0; i < chain_count; ++i) {
    const ContentFolder *folder = &content->tree[chain[i]];
    VkrUiWidgetConfig crumb = content_widget(3 + i, 0);
    vkr_editor_ghost_style(&crumb);
    crumb.style.text_color =
        i + 1u == chain_count ? theme->text : theme->text_secondary;
    crumb.style.padding_pt = (VkrUiEdges){3, 4, 3, 4};
    if (!i) {
      crumb.icon = folder->icon;
      crumb.icon_color = theme->accent_hover;
    }
    String8 text = !i ? string8_create_formatted(
                            ui->frame_allocator, "%s%s", folder->label,
                            elided ? "  /  \xe2\x80\xa6" : "")
                      : string8_create_formatted(ui->frame_allocator, "/  %s",
                                                 folder->label);
    crumb.tooltip = string8_lit("Show this folder");
    (void)vkr_ui_push_id_u64(ui, i);
    const bool8_t drop =
        content_drop_probe(content, ui, string8_lit("crumb"), folder->path,
                           (VkrUiRect){0, 0, 1e9f, 1e9f});
    if (drop) {
      crumb.style.background_color = vkr_ui_color_alpha(theme->accent, 0.35f);
    }
    if (vkr_ui_button(ui, string8_lit("crumb"), text, &crumb)) {
      content_open_folder(content, folder->path);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static void content_build_toolbar(VkrEditorContent *content, VkrUiSystem *ui,
                                  float32_t width, float32_t height) {
  const VkrUiTrack toolbar_rows[] = {{28, VKR_UI_TRACK_PX},
                                     {28, VKR_UI_TRACK_PX}};
  const VkrUiTrack toolbar_columns[] = {
      {82, VKR_UI_TRACK_PX}, {32, VKR_UI_TRACK_PX}, {32, VKR_UI_TRACK_PX},
      {32, VKR_UI_TRACK_PX}, {1, VKR_UI_TRACK_FR},  {100, VKR_UI_TRACK_PX},
      {44, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig tools = vkr_ui_panel_config_default();
  tools.placement.column = 0;
  tools.placement.row = 0;
  tools.columns = toolbar_columns;
  tools.column_count = ArrayCount(toolbar_columns);
  tools.rows = toolbar_rows;
  tools.row_count = ArrayCount(toolbar_rows);
  tools.style.gap_pt = 4;
  tools.style.padding_pt = (VkrUiEdges){4, 8, 4, 8};
  tools.style.background_color = vkr_ui_theme()->panel;
  if (!vkr_ui_panel_begin(ui, string8_lit("tools"), &tools)) {
    return;
  }
  VkrUiWidgetConfig import = content_widget(0, 0);
  vkr_editor_primary_style(&import, VKR_FONT_HANDLE_INVALID);
  import.icon = VKR_UI_ICON_IMPORT;
  import.icon_size_pt = 14;
  import.disabled = content->read_only || !content->project[0];
  import.tooltip = string8_lit("Import models, textures and fonts, or create a "
                               "scene");
  if (vkr_ui_button(ui, string8_lit("import"), string8_lit("Import"),
                    &import)) {
    content_action(content, VKR_EDITOR_CONTENT_ACTION_IMPORT);
  }
  VkrUiWidgetConfig folder = vkr_editor_icon_button_config(
      1, 0, VKR_UI_ICON_ADD, string8_lit("New folder in this folder"));
  folder.disabled = content->read_only || !content->project[0] ||
                    !content_project_folder(content->folder);
  if (vkr_ui_button(ui, string8_lit("new-folder"), (String8){0}, &folder)) {
    content_new_folder(content, content->folder);
  }
  VkrUiWidgetConfig script = vkr_editor_icon_button_config(
      2, 0, VKR_UI_ICON_CODE,
      string8_lit("New script module in Scripts (ADR-079)"));
  script.disabled = content->read_only || !content->project[0];
  if (vkr_ui_button(ui, string8_lit("new-script"), (String8){0}, &script)) {
    content_action(content, VKR_EDITOR_CONTENT_ACTION_NEW_SCRIPT);
  }
  VkrUiWidgetConfig refresh = vkr_editor_icon_button_config(
      3, 0, VKR_UI_ICON_REFRESH, string8_lit("Refresh asset inventories"));
  if (vkr_ui_button(ui, string8_lit("refresh"), (String8){0}, &refresh)) {
    vkr_editor_content_refresh(content);
  }
  content_build_navigation(content, ui);
  VkrUiWidgetConfig details = content_widget(5, 0);
  vkr_editor_ghost_style(&details);
  details.icon = VKR_UI_ICON_SIDEBAR;
  details.icon_size_pt = 14;
  details.tooltip = string8_lit("Show or hide the details panel");
  vkr_editor_toggle_style(&details, !content->details_hidden);
  details.disabled = width < 1040 || height < 240;
  if (vkr_ui_button(ui, string8_lit("details"), string8_lit("Details"),
                    &details)) {
    content->details_hidden = !content->details_hidden;
  }
  VkrUiWidgetConfig size = vkr_editor_icon_button_config(
      6, 0,
      content->size == CONTENT_SIZE_LIST ? VKR_UI_ICON_GRID
      : content->size == 128             ? VKR_UI_ICON_ZOOM_IN
                                         : VKR_UI_ICON_LIST,
      content->size == CONTENT_SIZE_LIST ? string8_lit("Show tiles")
      : content->size == 128             ? string8_lit("Larger tiles")
                                         : string8_lit("Show as a list"));
  if (vkr_ui_button(ui, string8_lit("size"), (String8){0}, &size)) {
    content->size = content->size == CONTENT_SIZE_LIST ? 128
                    : content->size == 128             ? 256
                                                       : CONTENT_SIZE_LIST;
  }
  VkrUiTextEditBuffer query = {.data = content->query,
                               .length = content->query_length,
                               .capacity = sizeof(content->query)};
  VkrUiPlacement search = VKR_UI_PLACEMENT_DEFAULT;
  search.column = 0;
  search.row = 1;
  search.column_span = 5;
  if (vkr_editor_search_field(
          ui, string8_lit("search"), &query, search,
          string8_lit("Search this folder"),
          string8_lit("Search names, types, folders and tags below this "
                      "folder"),
          VKR_FONT_HANDLE_INVALID)) {
    content->query_length = query.length;
    content->filter_dirty = true_v;
    content->first_row = 0;
  }
  VkrUiWidgetConfig tag = content_widget(5, 1);
  vkr_editor_ghost_style(&tag);
  tag.icon = VKR_UI_ICON_TAG;
  tag.icon_size_pt = 13;
  tag.style.text_color = vkr_ui_theme()->text;
  tag.disabled = !content->tag_filter[0];
  tag.tooltip = string8_lit("Clear the tag filter");
  if (vkr_ui_button(ui, string8_lit("tag"),
                    content_string(content->tag_filter[0] ? content->tag_filter
                                                          : "Any tag"),
                    &tag)) {
    content->tag_filter[0] = '\0';
    content->filter_dirty = true_v;
  }
  VkrUiWidgetConfig sort = vkr_editor_icon_button_config(
      6, 1,
      content->reverse_sort ? VKR_UI_ICON_SORT_DESCENDING
                            : VKR_UI_ICON_SORT_ASCENDING,
      content->reverse_sort ? string8_lit("Sorted Z to A")
                            : string8_lit("Sorted A to Z"));
  if (vkr_ui_button(ui, string8_lit("sort"), (String8){0}, &sort)) {
    content->reverse_sort = !content->reverse_sort;
    content->filter_dirty = true_v;
  }
  (void)vkr_ui_panel_end(ui);
}

/* Hover description: what the item is, where it lives and its state. */
static String8 content_description(const VkrEditorContent *content,
                                   VkrUiSystem *ui, const ContentAsset *entry) {
  if (entry->kind == CONTENT_ENTITY) {
    return string8_create_formatted(
        ui->frame_allocator,
        "%s\nObject in %s\nDouble-click to select it; right-click to frame, "
        "rename or delete it.",
        entry->name, entry->home[0] ? "its scene" : "the World");
  }
  if (entry->kind == CONTENT_PRESET && entry->object < content->presets.count) {
    return string8_create_formatted(
        ui->frame_allocator,
        "%s\nPreset \xc2\xb7 %s\nApply it from the Presets menu of a %s "
        "component in Details.",
        entry->name, content->presets.presets[entry->object].type->label,
        content->presets.presets[entry->object].type->label);
  }
  if (entry->kind == CONTENT_OBJECT) {
    return string8_create_formatted(
        ui->frame_allocator,
        "%s\nObject type\nDouble-click or drag into the viewport to add "
        "it to the scene.",
        entry->name);
  }
  if (entry->kind == CONTENT_SCRIPT) {
    return string8_create_formatted(
        ui->frame_allocator,
        "%s\nScript\nDrag it onto an object in the viewport or Outliner to "
        "attach it, or onto empty space to add an object running it. "
        "Double-click to edit it.",
        entry->name);
  }
  const VkrEditorAssetLabel *label = content_entry_label(content, entry);
  const char *folder = content_item_folder(content, entry);
  const uint32_t folder_index = content_tree_find(content, folder);
  const bool8_t tags = label && label->tags[0];
  return string8_create_formatted(
      ui->frame_allocator, "%s\n%s \xc2\xb7 %s asset\nIn: %s%s%s\n%s",
      entry->name, content_kinds[entry->kind], content_scopes[entry->scope],
      folder_index < content->tree_count ? content->tree[folder_index].label
                                         : "Content",
      tags ? "\nTags: " : "", tags ? label->tags : "",
      content_loading(content, entry)
          ? "Loading into the viewport; locked until it is shown"
      : content_cooking(content, entry)
          ? "Cooking full-quality textures in the background; locked until "
            "they are ready"
      : entry->diagnostic[0] ? entry->diagnostic
                             : "Ready");
}

/* Whether the viewport accepts a dropped item: object types, scripts and
   built meshes. A scene folder dropped there opens. */
static bool8_t content_viewport_accepts(const ContentAsset *entry) {
  return entry->kind == CONTENT_OBJECT || entry->kind == CONTENT_SCRIPT ||
         (entry->kind == CONTENT_MESH && !entry->missing && !entry->stale);
}

/* The dragged item follows the pointer. Releasing it over a folder moves
   it; over the viewport, a scene opens, the World shows, an object is added
   and a mesh is placed where it lands. */
static void content_drag_update(VkrEditorContent *content, VkrUiSystem *ui) {
  const bool8_t over_folder = content->drop_folder_set;
  content->drop_folder_set = false_v;
  if (!content->dragging) {
    if (ui->mouse_released) {
      content->drag_asset = CONTENT_NONE;
    }
    return;
  }
  const uint32_t shown = content->drag_asset;
  const bool8_t folder = content_shown_folder(shown);
  if (folder ? (shown & ~CONTENT_FOLDER_BIT) >= content->tree_count
             : shown >= content->count) {
    content->dragging = false_v;
    return;
  }
  const ContentAsset *entry = folder ? NULL : &content->entries[shown];
  const ContentFolder *dragged_folder =
      folder ? &content->tree[shown & ~CONTENT_FOLDER_BIT] : NULL;
  const bool8_t scene =
      dragged_folder && content_scene_folder(dragged_folder->path);
  const float32_t x = (float32_t)ui->mouse_x;
  const float32_t y = (float32_t)ui->mouse_y;
  const VkrUiRect target = content->drop_target;
  const bool8_t over_viewport =
      !over_folder && (scene || (entry && content_viewport_accepts(entry))) &&
      target.width > 0.0f && x >= target.x && x < target.x + target.width &&
      y >= target.y && y < target.y + target.height;
  if (ui->mouse_released) {
    if (over_folder) {
      (void)content_move(content, shown, content->drop_folder);
    } else if (over_viewport && scene) {
      content_scene_action(content, dragged_folder->path,
                           VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE);
    } else if (over_viewport && entry->kind == CONTENT_MESH) {
      content->selected = shown;
      content_action(content, VKR_EDITOR_CONTENT_ACTION_PLACE_ASSET);
      content->action.drop_px = (Vec2){x, y};
    } else if (over_viewport && entry->kind == CONTENT_SCRIPT) {
      /* The editor picks under the drop to attach, else adds an object. */
      content->selected = shown;
      content_action(content, VKR_EDITOR_CONTENT_ACTION_DROP_SCRIPT);
      content->action.drop_px = (Vec2){x, y};
      content->action.dropped = true_v;
    } else if (over_viewport) {
      content_activate(content, shown);
      content->action.drop_px = (Vec2){x, y};
      content->action.dropped = true_v;
    }
    content->dragging = false_v;
    content->drag_asset = CONTENT_NONE;
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  /* A translucent icon of the item rides under the pointer, as in UE5,
     brighter where a drop would take effect; the chip names it. */
  const float32_t ghost_pt = 76.0f;
  const Vec4 kind_color = scene    ? content_type_colors[CONTENT_SCENE]
                          : folder ? content_folder_color
                                   : content_type_colors[entry->kind];
  /* Root overlays sit in the root's single cell; an automatic cell lies
     outside it and fails the frame's layout. */
  VkrUiWidgetConfig ghost = vkr_ui_widget_config_default();
  ghost.placement = VKR_UI_PLACEMENT_DEFAULT;
  ghost.placement.column = 0;
  ghost.placement.row = 0;
  ghost.placement.justify = ghost.placement.align = VKR_UI_ALIGN_START;
  ghost.placement.margin_pt =
      (VkrUiEdges){y / ui->content_scale - ghost_pt * 0.5f, 0, 0,
                   x / ui->content_scale - ghost_pt * 0.5f};
  ghost.style.min_size_pt = ghost.style.max_size_pt =
      (Vec2){ghost_pt, ghost_pt};
  const bool8_t accepted = over_viewport || over_folder;
  ghost.style.background_color = vkr_ui_color_alpha(theme->raised, 0.55f);
  ghost.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  ghost.style.border_color =
      vkr_ui_color_alpha(accepted ? theme->accent : kind_color, 0.6f);
  ghost.style.corner_radius_pt = (Vec4){10, 10, 10, 10};
  ghost.icon = folder ? dragged_folder->icon : content_icon(entry);
  ghost.icon_size_pt = 40.0f;
  /* An icon-only label draws at its leading edge unless centered. */
  ghost.center = true_v;
  ghost.style.padding_pt = (VkrUiEdges){0};
  ghost.icon_color = vkr_ui_color_alpha(kind_color, accepted ? 0.9f : 0.6f);
  vkr_ui_label(ui, string8_lit("content.drag.ghost"), (String8){0}, &ghost);

  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = 0;
  chip.placement.row = 0;
  chip.placement.justify = chip.placement.align = VKR_UI_ALIGN_START;
  chip.placement.margin_pt =
      (VkrUiEdges){y / ui->content_scale + ghost_pt * 0.5f + 4.0f, 0, 0,
                   x / ui->content_scale - ghost_pt * 0.5f};
  chip.style.background_color = accepted ? theme->accent : theme->raised;
  chip.style.text_color = theme->text;
  chip.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
  chip.style.padding_pt = (VkrUiEdges){4, 10, 4, 10};
  const char *name = folder ? dragged_folder->label : entry->name;
  String8 text = over_folder
                     ? string8_create_formatted(
                           ui->frame_allocator, "%s  \xe2\x86\x92  %s", name,
                           content_folder_label(content, content->drop_folder))
                     : content_string(name);
  vkr_ui_label(ui, string8_lit("content.drag"), text, &chip);
}

/* Whether an item's name edits in place, and commit a new name: folders
   and presets rename here, objects, scenes and scene assets through their
   owners (ADR-076). */
static bool8_t content_renamable(const VkrEditorContent *content,
                                 uint32_t shown) {
  if (content->read_only) {
    return false_v;
  }
  if (content_shown_folder(shown)) {
    const ContentFolder *folder = &content->tree[shown & ~CONTENT_FOLDER_BIT];
    return !folder->system || content_scene_folder(folder->path);
  }
  if (shown >= content->count) {
    return false_v;
  }
  const ContentAsset *entry = &content->entries[shown];
  if (content_locked(content, entry)) {
    return false_v;
  }
  return entry->kind == CONTENT_ENTITY || entry->kind == CONTENT_PRESET ||
         (entry->scope == 0u && entry->kind != CONTENT_SCENE);
}

static void content_commit_rename(VkrEditorContent *content, uint32_t shown) {
  char name[513];
  vkr_editor_label_clean(name, sizeof(name), content->rename_draft,
                         content->rename_draft_length);
  if (!name[0]) {
    return;
  }
  if (content_shown_folder(shown)) {
    const ContentFolder *folder = &content->tree[shown & ~CONTENT_FOLDER_BIT];
    if (content_scene_folder(folder->path)) {
      const uint32_t selected = content->selected;
      content->selected = content_folder_scene(content, folder->path);
      content_action(content, VKR_EDITOR_CONTENT_ACTION_RENAME_SCENE);
      content_copy(content->action.name, sizeof(content->action.name), name);
      content->selected = selected;
    } else if (!content_rename_folder(content, folder->path, name)) {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "A folder name cannot contain '/' or name a system folder.");
    }
    return;
  }
  const ContentAsset *entry = &content->entries[shown];
  if (!strcmp(name, entry->name)) {
    return;
  }
  if (entry->kind == CONTENT_PRESET) {
    content_rename_preset(content, entry->object, (const uint8_t *)name,
                          (uint32_t)strlen(name));
    return;
  }
  content->selected = shown;
  content_action(content, entry->kind == CONTENT_ENTITY
                              ? VKR_EDITOR_CONTENT_ACTION_RENAME_ENTITY
                              : VKR_EDITOR_CONTENT_ACTION_RENAME);
  content_copy(content->action.name, sizeof(content->action.name), name);
}

/* Start editing an item's name in place with the current name. */
static void content_begin_rename(VkrEditorContent *content, uint32_t shown) {
  if (!content_renamable(content, shown)) {
    return;
  }
  const char *name = content_shown_folder(shown)
                         ? content->tree[shown & ~CONTENT_FOLDER_BIT].label
                         : content->entries[shown].name;
  content->renaming = shown;
  content->rename_draft_length =
      (uint32_t)Min(strlen(name), sizeof(content->rename_draft) - 1u);
  MemCopy(content->rename_draft, name, content->rename_draft_length);
  content->rename_focus = true_v;
}

/* An item's name: a label, or while renaming a field that commits on Enter
   or when it loses focus and cancels on Escape. */
static void content_build_name(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t shown, const char *name,
                               VkrUiWidgetConfig *config) {
  if (content->renaming != shown) {
    vkr_ui_label(ui, string8_lit("name"), content_string(name), config);
    return;
  }
  const VkrUiId id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("rename"));
  if (content->rename_focus) {
    ui->focused_id = id;
    ui->focused_is_text = true_v;
    content->rename_focus = false_v;
  }
  VkrUiTextEditBuffer buffer = {.data = content->rename_draft,
                                .length = content->rename_draft_length,
                                .capacity = sizeof(content->rename_draft) - 1u};
  config->style.background_color = vkr_ui_theme()->field;
  if (vkr_ui_text_field(ui, string8_lit("rename"), &buffer, config)) {
    content->rename_draft_length = buffer.length;
  }
  if (content->rename_escape) {
    content->renaming = CONTENT_NONE;
  } else if (content->rename_enter || ui->focused_id != id) {
    content->renaming = CONTENT_NONE;
    content_commit_rename(content, shown);
  }
}

/* The whole tile or row is one button under its labels, so any point of it
   selects, opens on double-click and starts a drag. */
static void content_item_button(VkrEditorContent *content, VkrUiSystem *ui,
                                uint32_t shown, uint32_t span,
                                uint32_t row_span, String8 tooltip,
                                VkrUiId grid_id, VkrUiRect area,
                                bool8_t *drop) {
  VkrUiWidgetConfig hit = content_widget(0, 0);
  hit.placement.column_span = span;
  hit.placement.row_span = row_span;
  hit.placement.justify = VKR_UI_ALIGN_STRETCH;
  hit.placement.align = VKR_UI_ALIGN_STRETCH;
  hit.fill = true_v;
  hit.style.background_color = (Vec4){0};
  hit.style.hover_background_color = vkr_ui_theme()->row_hover;
  hit.style.active_background_color = vkr_ui_theme()->row_hover;
  hit.tooltip = tooltip;
  const bool8_t folder = content_shown_folder(shown);
  const ContentFolder *tree =
      folder ? &content->tree[shown & ~CONTENT_FOLDER_BIT] : NULL;
  *drop = folder && content_drop_probe(content, ui, string8_lit("item"),
                                       tree->path, area);
  if (*drop) {
    hit.style.background_color =
        vkr_ui_color_alpha(vkr_ui_theme()->accent, 0.35f);
  }
  if (vkr_ui_button(ui, string8_lit("item"), (String8){0}, &hit)) {
    content_click(content, ui, shown, grid_id);
  }
  content_context_probe(content, ui, string8_lit("item"), shown);
  /* Scene folders are system folders that still drag, into the viewport. */
  if (!folder || content_folder_draggable(tree)) {
    content_drag_begin(content, ui, shown, string8_lit("item"));
  }
}

static bool8_t content_is_selected(const VkrEditorContent *content,
                                   uint32_t shown) {
  if (content_shown_folder(shown)) {
    return !strcmp(content->selected_folder,
                   content->tree[shown & ~CONTENT_FOLDER_BIT].path);
  }
  return shown == content->selected;
}

static VkrUiPanelConfig content_card_panel(const VkrEditorContent *content,
                                           uint32_t shown, uint32_t column,
                                           uint32_t row,
                                           const VkrUiTrack rows[4]) {
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement.column = column;
  card.placement.row = row;
  card.rows = rows;
  card.row_count = 4;
  card.clip_children = true_v;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t selected = content_is_selected(content, shown);
  card.style.background_color =
      selected ? vkr_ui_color_alpha(theme->accent, 0.16f) : theme->raised;
  card.style.border_pt =
      selected ? (VkrUiEdges){2, 2, 2, 2} : (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = selected ? theme->accent : theme->border;
  card.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
  return card;
}

static void content_build_folder_card(VkrEditorContent *content,
                                      VkrUiSystem *ui, uint32_t shown,
                                      uint32_t column, uint32_t row,
                                      VkrUiId grid_id, VkrUiRect area) {
  const ContentFolder *folder = &content->tree[shown & ~CONTENT_FOLDER_BIT];
  const VkrUiTrack rows[] = {{3, VKR_UI_TRACK_PX},
                             {1, VKR_UI_TRACK_FR},
                             {22, VKR_UI_TRACK_PX},
                             {18, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card = content_card_panel(content, shown, column, row, rows);
  if (!vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const char *name = folder->label;
  bool8_t drop = false_v;
  const bool8_t scene = content_scene_folder(folder->path);
  content_item_button(content, ui, shown, 1, 4, content_folder_tooltip(folder),
                      grid_id, area, &drop);
  VkrUiWidgetConfig picture = content_widget(0, 1);
  picture.icon = folder->icon;
  picture.icon_size_pt = 44;
  picture.icon_color = drop    ? theme->accent_hover
                       : scene ? content_type_colors[CONTENT_SCENE]
                       : folder->icon == VKR_UI_ICON_FOLDER
                           ? content_folder_color
                           : theme->accent_hover;
  picture.placement.justify = VKR_UI_ALIGN_CENTER;
  picture.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_ui_label(ui, string8_lit("picture"), (String8){0}, &picture);
  VkrUiWidgetConfig label = content_widget(0, 2);
  content_build_name(content, ui, shown, name, &label);
  /* A folder holding a loading or cooking item says so. */
  bool8_t loading = false_v;
  bool8_t busy = false_v;
  for (uint32_t i = 0; i < content->count && !loading; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (content_locked(content, entry) &&
        vkr_editor_folder_within(content_item_folder(content, entry),
                                 folder->path)) {
      busy = true_v;
      loading = content_loading(content, entry);
    }
  }
  if (busy) {
    VkrUiWidgetConfig lock = content_widget(0, 1);
    lock.icon = VKR_UI_ICON_LOCK;
    lock.icon_size_pt = 14;
    lock.icon_color = theme->accent_hover;
    lock.placement.justify = VKR_UI_ALIGN_END;
    lock.placement.align = VKR_UI_ALIGN_START;
    lock.style.padding_pt = (VkrUiEdges){4, 6, 4, 6};
    vkr_ui_label(ui, string8_lit("cooking"), (String8){0}, &lock);
  }
  VkrUiWidgetConfig info = content_widget(0, 3);
  info.style.font_size_pt = theme->font_caption;
  info.style.text_color = busy ? theme->accent_hover : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("status"),
               busy    ? content_string(content_cooking_status(loading))
               : scene ? string8_lit("Scene")
               : folder->system ? string8_lit("System folder")
                                : string8_lit("Folder"),
               &info);
  (void)vkr_ui_panel_end(ui);
}

/* "Cooking" with dots that advance twice a second, so a locked card shows
   the work is alive. */
static const char *content_cooking_status(bool8_t loading) {
  static const char *const frames[2][4] = {
      {"Cooking", "Cooking.", "Cooking..", "Cooking..."},
      {"Loading", "Loading.", "Loading..", "Loading..."}};
  const uint64_t step = (uint64_t)(vkr_platform_get_absolute_time() * 2.0);
  return frames[loading ? 1 : 0][step % ArrayCount(frames[0])];
}

static void content_build_card(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t asset, uint32_t column, uint32_t row,
                               float32_t card_width, VkrUiId grid_id,
                               VkrUiRect area) {
  ContentAsset *entry = &content->entries[asset];
  const VkrUiTrack rows[] = {{3, VKR_UI_TRACK_PX},
                             {1, VKR_UI_TRACK_FR},
                             {22, VKR_UI_TRACK_PX},
                             {18, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig card = content_card_panel(content, asset, column, row, rows);
  if (!vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  ContentPreview *preview = content_preview(content, ui, asset);
  bool8_t drop = false_v;
  content_item_button(content, ui, asset, 1, 4,
                      content_description(content, ui, entry), grid_id, area,
                      &drop);
  VkrUiWidgetConfig strip = content_widget(0, 0);
  strip.style.background_color = content_type_colors[entry->kind];
  strip.style.padding_pt = (VkrUiEdges){0};
  strip.style.corner_radius_pt = (Vec4){5, 5, 0, 0};
  vkr_ui_label(ui, string8_lit("type-color"), (String8){0}, &strip);
  if (preview && preview->texture.id) {
    VkrUiWidgetConfig image = content_widget(0, 1);
    image.style.max_size_pt = (Vec2){card_width - 12, card_width - 12};
    vkr_ui_image(
        ui, string8_lit("preview"),
        (VkrUiTextureRef){preview->texture.id, preview->texture.generation},
        (Vec2){(float32_t)preview->width, (float32_t)preview->height}, &image);
  } else {
    VkrUiWidgetConfig picture = content_widget(0, 1);
    picture.icon = content_icon(entry);
    picture.icon_size_pt = 36;
    picture.icon_color = content_type_colors[entry->kind];
    picture.placement.justify = VKR_UI_ALIGN_CENTER;
    picture.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("picture"), (String8){0}, &picture);
  }
  VkrUiWidgetConfig name = content_widget(0, 2);
  content_build_name(content, ui, asset, entry->name, &name);
  const bool8_t loading = content_loading(content, entry);
  const bool8_t cooking = loading || content_cooking(content, entry);
  if (cooking) {
    /* Locked while its final textures encode (ADR-077). */
    VkrUiWidgetConfig lock = content_widget(0, 1);
    lock.icon = VKR_UI_ICON_LOCK;
    lock.icon_size_pt = 14;
    lock.icon_color = theme->accent_hover;
    lock.placement.justify = VKR_UI_ALIGN_END;
    lock.placement.align = VKR_UI_ALIGN_START;
    lock.style.padding_pt = (VkrUiEdges){4, 6, 4, 6};
    vkr_ui_label(ui, string8_lit("cooking"), (String8){0}, &lock);
  }
  const bool8_t preset =
      entry->kind == CONTENT_PRESET && entry->object < content->presets.count;
  const char *status =
      cooking  ? content_cooking_status(loading)
      : preset ? content->presets.presets[entry->object].type->label
      /* An object names the container it belongs to. */
      : entry->kind == CONTENT_ENTITY
          ? (entry->home[0] ? content_folder_label(content, entry->home)
                            : "World")
      : entry->scope == CONTENT_SCOPE_BUILTIN ? "Built-in"
      : entry->missing                        ? "Missing"
      : entry->stale                          ? "Stale"
      : entry->changed                        ? "Changed"
      : preview && preview->failed            ? "Preview error"
      : preview && preview->queued            ? "Building preview"
                                              : "Current";
  VkrUiWidgetConfig info = content_widget(0, 3);
  info.style.font_size_pt = theme->font_caption;
  info.style.text_color = cooking ? theme->accent_hover
                          : entry->missing || entry->stale || entry->changed ||
                                  (preview && preview->failed)
                              ? theme->warning
                              : theme->text_secondary;
  String8 line = string8_create_formatted(ui->frame_allocator, "%s · %s",
                                          content_kinds[entry->kind], status);
  vkr_ui_label(ui, string8_lit("status"), line, &info);
  (void)vkr_ui_panel_end(ui);
}

static const VkrUiTrack content_list_columns[] = {{2, VKR_UI_TRACK_FR},
                                                  {96, VKR_UI_TRACK_PX},
                                                  {1, VKR_UI_TRACK_FR},
                                                  {1, VKR_UI_TRACK_FR}};

/* List header: clicking a column sorts by it; again reverses. */
static void content_build_list_header(VkrEditorContent *content,
                                      VkrUiSystem *ui) {
  static const char *const titles[CONTENT_SORT_COUNT] = {"Name", "Type",
                                                         "Location", "Tags"};
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement.column = 0;
  header.placement.row = 0;
  header.columns = content_list_columns;
  header.column_count = ArrayCount(content_list_columns);
  header.style.background_color = vkr_ui_theme()->header;
  header.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
  if (!vkr_ui_panel_begin(ui, string8_lit("list-header"), &header)) {
    return;
  }
  for (uint32_t i = 0; i < CONTENT_SORT_COUNT; ++i) {
    VkrUiWidgetConfig title = content_widget(i, 0);
    vkr_editor_ghost_style(&title);
    title.placement.justify = VKR_UI_ALIGN_STRETCH;
    title.fill = true_v;
    title.style.font_size_pt = vkr_ui_theme()->font_caption;
    title.style.text_color = content->sort == (ContentSort)i
                                 ? vkr_ui_theme()->text
                                 : vkr_ui_theme()->text_secondary;
    if (content->sort == (ContentSort)i) {
      title.icon = content->reverse_sort ? VKR_UI_ICON_SORT_DESCENDING
                                         : VKR_UI_ICON_SORT_ASCENDING;
      title.icon_size_pt = 12;
    }
    title.tooltip =
        string8_create_formatted(ui->frame_allocator, "Sort by %s", titles[i]);
    (void)vkr_ui_push_id_u64(ui, i);
    if (content_start_button(ui, string8_lit("column"),
                             content_string(titles[i]), &title)) {
      content->reverse_sort =
          content->sort == (ContentSort)i && !content->reverse_sort;
      content->sort = (ContentSort)i;
      content->filter_dirty = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* List view row, like a file manager: icon and name, type, folder, tags. */
static void content_build_row(VkrEditorContent *content, VkrUiSystem *ui,
                              uint32_t shown, uint32_t row, VkrUiId grid_id,
                              VkrUiRect area) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t folder = content_shown_folder(shown);
  const ContentFolder *tree =
      folder ? &content->tree[shown & ~CONTENT_FOLDER_BIT] : NULL;
  const ContentAsset *entry = folder ? NULL : &content->entries[shown];
  VkrUiPanelConfig line = vkr_ui_panel_config_default();
  line.placement.column = 0;
  line.placement.row = row;
  line.columns = content_list_columns;
  line.column_count = ArrayCount(content_list_columns);
  line.clip_children = true_v;
  line.style.background_color = content_is_selected(content, shown)
                                    ? vkr_ui_color_alpha(theme->accent, 0.16f)
                                    : (Vec4){0};
  line.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
  if (!vkr_ui_panel_begin(ui, string8_lit("row"), &line)) {
    return;
  }
  const VkrEditorAssetLabel *label =
      entry ? content_entry_label(content, entry) : NULL;
  char parent[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  if (folder) {
    vkr_editor_folder_parent(tree->path, parent);
  }
  const char *location = folder ? parent : content_item_folder(content, entry);
  bool8_t drop = false_v;
  content_item_button(content, ui, shown, ArrayCount(content_list_columns), 1,
                      folder ? content_folder_tooltip(tree)
                             : content_description(content, ui, entry),
                      grid_id, area, &drop);
  const bool8_t scene = folder && content_scene_folder(tree->path);
  VkrUiWidgetConfig name = content_widget(0, 0);
  name.icon = folder ? tree->icon : content_icon(entry);
  name.icon_size_pt = 14;
  name.icon_color = scene    ? content_type_colors[CONTENT_SCENE]
                    : folder ? content_folder_color
                             : content_type_colors[entry->kind];
  content_build_name(content, ui, shown, folder ? tree->label : entry->name,
                     &name);
  const bool8_t loading = !folder && content_loading(content, entry);
  const bool8_t cooking =
      loading || (!folder && content_cooking(content, entry));
  String8 texts[] = {
      cooking ? string8_create_formatted(ui->frame_allocator, "%s \xc2\xb7 %s",
                                         content_kinds[entry->kind],
                                         content_cooking_status(loading))
              : content_string(scene ? "Scene"
                               : folder
                                   ? (tree->system ? "System folder" : "Folder")
                                   : content_kinds[entry->kind]),
      content_string(content_folder_label(content, location)),
      content_string(label ? label->tags : "")};
  for (uint32_t i = 0; i < ArrayCount(texts); ++i) {
    VkrUiWidgetConfig cell = content_widget(1 + i, 0);
    cell.style.text_color = theme->text_secondary;
    cell.style.font_size_pt = theme->font_caption;
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("cell"), texts[i], &cell);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The grid lays out the rows from first_row inside a scroll area as tall as
   every row, so the shared scrollbar drags, pages and shows the position;
   scrolling snaps to rows. */
static void content_build_grid(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t columns, uint32_t rows,
                               uint32_t max_first, float32_t card_width,
                               float32_t card_height, VkrUiId grid_id,
                               VkrUiRect area) {
  const bool8_t list = content->size == CONTENT_SIZE_LIST;
  const uint32_t header = list ? 1u : 0u;
  const float32_t gap = list ? 2.0f : 6.0f;
  const float32_t pitch = card_height + gap;
  const uint32_t total_rows = (content->filtered_count + columns - 1) / columns;
  const VkrUiTrack extent = {
      Max(max_first * pitch + area.height / ui->content_scale,
          (list ? 22.0f + gap : 0.0f) + total_rows * pitch + 8.0f),
      VKR_UI_TRACK_PX};
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  scroll.placement.column = 1;
  scroll.placement.row = 0;
  scroll.rows = &extent;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("grid.scroll"), &scroll)) {
    return;
  }
  float32_t offset = content->first_row * pitch;
  (void)vkr_ui_scroll_area_offset(ui, &offset);
  if (offset != content->first_row * pitch) {
    content->first_row = Min(max_first, (uint32_t)roundf(offset / pitch));
    offset = content->first_row * pitch;
    (void)vkr_ui_scroll_area_offset(ui, &offset);
  }
  VkrUiTrack column_tracks[16], row_tracks[CONTENT_LIST_ROWS + 1u];
  for (uint32_t i = 0; i < columns; ++i) {
    column_tracks[i] = (VkrUiTrack){1, VKR_UI_TRACK_FR};
  }
  if (list) {
    row_tracks[0] = (VkrUiTrack){22, VKR_UI_TRACK_PX};
  }
  for (uint32_t i = 0; i < rows; ++i) {
    row_tracks[header + i] = (VkrUiTrack){card_height, VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig grid = vkr_ui_panel_config_default();
  grid.placement.column = 0;
  grid.placement.row = 0;
  grid.placement.margin_pt.top = offset;
  grid.columns = column_tracks;
  grid.column_count = columns;
  grid.rows = row_tracks;
  grid.row_count = header + rows;
  grid.style.gap_pt = gap;
  grid.style.padding_pt = (VkrUiEdges){4, 14, 4, 6};
  grid.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("grid"), &grid)) {
    (void)vkr_ui_scroll_area_end(ui);
    return;
  }
  if (list) {
    content_build_list_header(content, ui);
  }
  const uint32_t start = content->first_row * columns;
  const uint32_t end = Min(content->filtered_count, start + columns * rows);
  if (start == end) {
    VkrUiWidgetConfig empty = content_widget(0, header);
    empty.placement.column_span = columns;
    empty.style.text_color = vkr_ui_theme()->text_secondary;
    vkr_ui_label(ui, string8_lit("empty"),
                 content_string(content->query_length || content->tag_filter[0]
                                    ? "No matching items below this folder"
                                    : "This folder is empty. Import content "
                                      "or drag items here."),
                 &empty);
  }
  for (uint32_t index = start; index < end; ++index) {
    const uint32_t shown = content->filtered[index];
    const bool8_t folder = content_shown_folder(shown);
    const char *key = folder ? content->tree[shown & ~CONTENT_FOLDER_BIT].path
                             : content->entries[shown].id;
    (void)vkr_ui_push_id_u64(
        ui, content_hash(folder ? 0xf01du : 0x17e3u, key, strlen(key)));
    if (list) {
      content_build_row(content, ui, shown, header + index - start, grid_id,
                        area);
    } else if (folder) {
      content_build_folder_card(content, ui, shown, (index - start) % columns,
                                (index - start) / columns, grid_id, area);
    } else {
      content_build_card(content, ui, shown, (index - start) % columns,
                         (index - start) / columns, card_width, grid_id, area);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_scroll_area_end(ui);
}

static void content_build_rename(VkrEditorContent *content, VkrUiSystem *ui,
                                 const ContentAsset *entry) {
  const VkrUiTrack rename_columns[] = {{1, VKR_UI_TRACK_FR},
                                       {80, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig rename_panel = vkr_ui_panel_config_default();
  rename_panel.placement.column = 0;
  rename_panel.placement.row = 3;
  rename_panel.columns = rename_columns;
  rename_panel.column_count = ArrayCount(rename_columns);
  if (vkr_ui_panel_begin(ui, string8_lit("rename-panel"), &rename_panel)) {
    VkrUiWidgetConfig name_field = content_widget(0, 0);
    name_field.tooltip = string8_lit(
        "Asset display name; stable ID and references remain unchanged");
    VkrUiTextEditBuffer name_buffer = {.data = content->rename,
                                       .length = content->rename_length,
                                       .capacity = sizeof(content->rename)};
    if (vkr_ui_text_field(ui, string8_lit("asset-name"), &name_buffer,
                          &name_field)) {
      content->rename_length = name_buffer.length;
    }
    VkrUiWidgetConfig rename_button = content_widget(1, 0);
    const bool8_t preset = entry->kind == CONTENT_PRESET;
    rename_button.disabled = content->read_only || !content->rename_length ||
                             (entry->scope != 0 && !preset);
    /* Presets rename in place; scene assets rename through a job. */
    if (vkr_ui_button(ui, string8_lit("rename-asset"), string8_lit("Rename"),
                      &rename_button)) {
      if (preset) {
        content_rename_preset(content, entry->object, content->rename,
                              content->rename_length);
      } else {
        content_action(content, VKR_EDITOR_CONTENT_ACTION_RENAME);
        content_copy(content->action.name, sizeof(content->action.name),
                     (const char *)content->rename);
      }
    }
    (void)vkr_ui_panel_end(ui);
  }
}

static void content_build_asset_actions(VkrEditorContent *content,
                                        VkrUiSystem *ui,
                                        const ContentAsset *entry) {
  const VkrUiTrack action_columns[] = {{1, VKR_UI_TRACK_FR},
                                       {1, VKR_UI_TRACK_FR}};
  const VkrUiTrack action_rows[] = {{26, VKR_UI_TRACK_PX},
                                    {26, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig actions = vkr_ui_panel_config_default();
  actions.placement.column = 0;
  actions.placement.row = 2;
  actions.rows = action_rows;
  actions.row_count = ArrayCount(action_rows);
  actions.columns = action_columns;
  actions.column_count = ArrayCount(action_columns);
  if (vkr_ui_panel_begin(ui, string8_lit("actions"), &actions)) {
    const bool8_t scene = entry->kind == CONTENT_SCENE;
    VkrUiWidgetConfig retry = content_widget(0, 0);
    retry.icon = scene ? VKR_UI_ICON_SCENE_LOAD : VKR_UI_ICON_REFRESH;
    retry.disabled = content->read_only;
    if (scene) {
      /* A scene opens in the viewport; it has no preview to retry. */
      if (vkr_ui_button(ui, string8_lit("open"), string8_lit("Open scene"),
                        &retry)) {
        content_action(content, VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE);
      }
    } else if (vkr_ui_button(ui, string8_lit("retry"),
                             string8_lit("Retry preview"), &retry)) {
      for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
        ContentPreview *preview = &content->cache[i];
        if (preview->key && preview->asset == content->selected &&
            i != content->running) {
          FilePath path = {.path = content_string(preview->path),
                           .type = FILE_PATH_TYPE_ABSOLUTE};
          (void)file_remove(&path);
          content_release_preview(preview);
        }
      }
    }
    VkrUiWidgetConfig reveal = content_widget(1, 0);
    reveal.icon = VKR_UI_ICON_FOLDER;
    reveal.disabled = content->worker || !entry->path[0];
    if (vkr_ui_button(ui, string8_lit("reveal"), string8_lit("Reveal"),
                      &reveal)) {
      content_copy(content->worker_input, sizeof(content->worker_input),
                   entry->path);
      content->worker_reveal = true_v;
      content->worker_material = false_v;
      content->worker_generation = content->generation;
      content->running = CONTENT_NONE;
      content->worker_log[0] = 0;
      vkr_atomic_bool_store(&content->cancel, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      vkr_atomic_bool_store(&content->complete, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
      (void)vkr_thread_create(content->allocator, &content->worker,
                              content_worker, content);
    }
    VkrUiWidgetConfig reimport = content_widget(0, 1);
    /* Reimport and rebuild run as jobs on scene-owned records. */
    reimport.disabled = content->read_only || !entry->source[0] ||
                        entry->scope != 0 || content_locked(content, entry);
    if (vkr_ui_button(ui, string8_lit("reimport"), string8_lit("Reimport"),
                      &reimport)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_REIMPORT);
    }
    VkrUiWidgetConfig rebuild = content_widget(1, 1);
    rebuild.disabled = content->read_only || scene || entry->scope != 0 ||
                       content_locked(content, entry);
    if (vkr_ui_button(ui, string8_lit("rebuild"), string8_lit("Rebuild"),
                      &rebuild)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_REBUILD);
    }
    (void)vkr_ui_panel_end(ui);
  }
}

/* Folder and tags of the selected asset. Typing edits a draft; the label
   saves once neither field has focus. */
static void content_build_labels(VkrEditorContent *content, VkrUiSystem *ui,
                                 const ContentAsset *entry) {
  if (strcmp(content->label_asset, entry->id)) {
    const VkrEditorAssetLabel *label = content_label(content, entry->id);
    content_copy(content->label_asset, sizeof(content->label_asset), entry->id);
    content_copy((char *)content->label_folder, sizeof(content->label_folder),
                 content_item_folder(content, entry));
    content_copy((char *)content->label_tags, sizeof(content->label_tags),
                 label ? label->tags : "");
    content->label_folder_length =
        (uint32_t)strlen((char *)content->label_folder);
    content->label_tags_length = (uint32_t)strlen((char *)content->label_tags);
    content->label_dirty = false_v;
  }
  const VkrUiTrack columns[] = {{52, VKR_UI_TRACK_PX}, {1, VKR_UI_TRACK_FR}};
  const VkrUiTrack rows[] = {{26, VKR_UI_TRACK_PX}, {26, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 4;
  panel.columns = columns;
  panel.column_count = ArrayCount(columns);
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style.gap_pt = 3;
  if (!vkr_ui_panel_begin(ui, string8_lit("labels"), &panel)) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  bool8_t focused = false_v;
  struct {
    const char *caption;
    const char *id;
    const char *placeholder;
    uint8_t *data;
    uint32_t *length;
    uint32_t capacity;
  } fields[] = {
      {"Folder", "label.folder", "Props/Bistro", content->label_folder,
       &content->label_folder_length, sizeof(content->label_folder)},
      {"Tags", "label.tags", "exterior, lod", content->label_tags,
       &content->label_tags_length, sizeof(content->label_tags)},
  };
  for (uint32_t i = 0; i < ArrayCount(fields); ++i) {
    VkrUiWidgetConfig caption = content_widget(0, i);
    caption.style.text_color = theme->text_secondary;
    caption.style.font_size_pt = theme->font_caption;
    vkr_ui_label(ui, content_string(fields[i].id),
                 content_string(fields[i].caption), &caption);
    VkrUiWidgetConfig field = content_widget(1, i);
    field.placement.justify = VKR_UI_ALIGN_STRETCH;
    field.fill = true_v;
    field.read_only = content->read_only;
    vkr_editor_field_style(&field);
    field.tooltip = i ? string8_lit("Comma-separated tags for search and "
                                    "the Tags list")
                      : string8_lit("Content folder, such as Props/Bistro; "
                                    "the file does not move");
    VkrUiTextEditBuffer buffer = {.data = fields[i].data,
                                  .length = *fields[i].length,
                                  .capacity = fields[i].capacity};
    (void)vkr_ui_push_id_u64(ui, i);
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("value"));
    if (vkr_ui_text_field(ui, string8_lit("value"), &buffer, &field)) {
      *fields[i].length = buffer.length;
      content->label_dirty = true_v;
    }
    focused |= ui->focused_id == id;
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  if (content->label_dirty && !focused && !content->read_only) {
    char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    char tags[128];
    vkr_editor_label_clean(folder, sizeof(folder), content->label_folder,
                           content->label_folder_length);
    vkr_editor_label_clean(tags, sizeof(tags), content->label_tags,
                           content->label_tags_length);
    /* The default folder is stored as no folder; Objects takes nothing. */
    const char *fallback = content_default_folder(entry);
    if (!vkr_editor_folder_normalize(folder) ||
        !content_project_folder(folder)) {
      folder[0] = '\0';
    }
    if (!strcmp(folder, fallback)) {
      folder[0] = '\0';
    }
    (void)content_label_set(content, entry->id, folder, tags);
    content->label_dirty = false_v;
    content->label_asset[0] = '\0';
  }
}

/* Context menu commands (ADR-076): what Open, Put into viewport, Rename
   and Delete mean for each kind of item. */
static bool8_t content_menu_enabled(const VkrEditorContent *content,
                                    uint32_t shown,
                                    VkrEditorContentCommand command) {
  if (content_shown_folder(shown)) {
    const uint32_t index = shown & ~CONTENT_FOLDER_BIT;
    if (index >= content->tree_count) {
      return false_v;
    }
    const ContentFolder *folder = &content->tree[index];
    const bool8_t scene = content_scene_folder(folder->path);
    switch (command) {
    case VKR_EDITOR_CONTENT_COMMAND_LOAD:
      return scene;
    case VKR_EDITOR_CONTENT_COMMAND_OPEN:
      return true_v;
    case VKR_EDITOR_CONTENT_COMMAND_PLACE:
      /* The Content root is the World: it shows in a document. */
      return (scene && !content->read_only) ||
             (!folder->path[0] && content->project[0]);
    case VKR_EDITOR_CONTENT_COMMAND_RENAME:
      return content_renamable(content, shown);
    default:
      return !content->read_only && (scene || !folder->system);
    }
  }
  if (shown >= content->count) {
    return false_v;
  }
  const ContentAsset *entry = &content->entries[shown];
  /* A cooking asset is locked until its final textures publish. */
  const bool8_t writable =
      !content->read_only && !content_locked(content, entry);
  switch (command) {
  case VKR_EDITOR_CONTENT_COMMAND_LOAD:
    return false_v;
  case VKR_EDITOR_CONTENT_COMMAND_OPEN:
    return entry->kind != CONTENT_OBJECT;
  case VKR_EDITOR_CONTENT_COMMAND_PLACE:
    return entry->kind == CONTENT_ENTITY ||
           (writable && content_viewport_accepts(entry));
  case VKR_EDITOR_CONTENT_COMMAND_RENAME:
    return content_renamable(content, shown);
  default:
    /* Script sources are files of their module, not inventory assets. */
    return writable && entry->kind != CONTENT_SCRIPT &&
           (entry->kind == CONTENT_ENTITY || entry->kind == CONTENT_PRESET ||
            (entry->scope <= 1u && entry->kind != CONTENT_SCENE));
  }
}

static void content_menu_run(VkrEditorContent *content, uint32_t shown,
                             VkrEditorContentCommand command) {
  if (!content_menu_enabled(content, shown, command)) {
    return;
  }
  if (command == VKR_EDITOR_CONTENT_COMMAND_RENAME) {
    content_begin_rename(content, shown);
    return;
  }
  const VkrUiRect target = content->drop_target;
  const Vec2 centre = {target.x + target.width * 0.5f,
                       target.y + target.height * 0.5f};
  if (content_shown_folder(shown)) {
    const ContentFolder *folder = &content->tree[shown & ~CONTENT_FOLDER_BIT];
    char path[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    content_copy(path, sizeof(path), folder->path);
    const bool8_t scene = content_scene_folder(path);
    if (command == VKR_EDITOR_CONTENT_COMMAND_LOAD) {
      content_scene_action(content, path, VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE);
    } else if (command == VKR_EDITOR_CONTENT_COMMAND_OPEN) {
      content_open_folder(content, path);
    } else if (command == VKR_EDITOR_CONTENT_COMMAND_PLACE && !path[0]) {
      content->action = (VkrEditorContentAction){
          .kind = VKR_EDITOR_CONTENT_ACTION_OPEN_WORLD};
    } else if (command == VKR_EDITOR_CONTENT_COMMAND_PLACE) {
      content_scene_action(content, path, VKR_EDITOR_CONTENT_ACTION_ADD_SCENE);
    } else if (scene) {
      content_scene_action(content, path,
                           VKR_EDITOR_CONTENT_ACTION_DELETE_SCENE);
    } else if (vkr_editor_folders_remove(&content->labels, path)) {
      content->selected_folder[0] = '\0';
      content_labels_save(content);
    } else {
      snprintf(content->diagnostic, sizeof(content->diagnostic),
               "Only an empty folder can be deleted.");
    }
    return;
  }
  const ContentAsset *entry = &content->entries[shown];
  content->selected = shown;
  content->selected_folder[0] = '\0';
  const bool8_t object = entry->kind == CONTENT_ENTITY;
  switch (command) {
  case VKR_EDITOR_CONTENT_COMMAND_OPEN:
    if (object) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY);
    } else if (entry->kind == CONTENT_SCRIPT) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_OPEN_SCRIPT);
    } else {
      content->details_hidden = false_v;
    }
    break;
  case VKR_EDITOR_CONTENT_COMMAND_PLACE:
    if (object) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_FRAME_ENTITY);
    } else if (entry->kind == CONTENT_MESH) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_PLACE_ASSET);
      content->action.drop_px = centre;
    } else {
      content_activate(content, shown);
      content->action.drop_px = centre;
      content->action.dropped = true_v;
    }
    break;
  default:
    if (object) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_DELETE_ENTITY);
    } else if (entry->kind == CONTENT_PRESET) {
      content_delete_preset(content, entry->object);
    } else {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_DELETE_ASSET);
    }
    break;
  }
}

/* Details of the selected folder: rename, open and delete when empty. */
static void content_build_folder_details(VkrEditorContent *content,
                                         VkrUiSystem *ui) {
  const uint32_t index = content_tree_find(content, content->selected_folder);
  if (index == CONTENT_NONE) {
    return;
  }
  const ContentFolder *folder = &content->tree[index];
  /* The draft starts from the shown name: a scene's name, never the id its
     folder path carries. */
  if (strcmp(content->folder_draft_path, folder->path) ||
      strcmp(content->folder_draft_label, folder->label)) {
    content_copy(content->folder_draft_path, sizeof(content->folder_draft_path),
                 folder->path);
    content_copy(content->folder_draft_label,
                 sizeof(content->folder_draft_label), folder->label);
    content_copy((char *)content->folder_draft, sizeof(content->folder_draft),
                 folder->label);
    content->folder_draft_length =
        (uint32_t)strlen((char *)content->folder_draft);
  }
  VkrUiWidgetConfig path = content_widget(0, 1);
  path.style.text_color = vkr_ui_theme()->text_secondary;
  path.style.font_size_pt = vkr_ui_theme()->font_caption;
  const bool8_t scene = content_scene_folder(folder->path);
  vkr_ui_label(ui, string8_lit("path"),
               scene ? string8_lit("A project scene: its objects and "
                                   "assets")
               : folder->system ? string8_lit("Provided by the editor")
                                : string8_lit("Project folder"),
               &path);
  const VkrUiTrack columns[] = {{1, VKR_UI_TRACK_FR}, {1, VKR_UI_TRACK_FR}};
  VkrUiPanelConfig actions = vkr_ui_panel_config_default();
  actions.placement.column = 0;
  actions.placement.row = 2;
  actions.columns = columns;
  actions.column_count = ArrayCount(columns);
  if (vkr_ui_panel_begin(ui, string8_lit("folder-actions"), &actions)) {
    VkrUiWidgetConfig open = content_widget(0, 0);
    open.icon = scene ? VKR_UI_ICON_SCENE_LOAD : VKR_UI_ICON_REVEAL;
    if (vkr_ui_button(ui, string8_lit("open"),
                      scene ? string8_lit("Load scene") : string8_lit("Open"),
                      &open)) {
      content_menu_run(content, CONTENT_FOLDER_BIT | index,
                       scene ? VKR_EDITOR_CONTENT_COMMAND_LOAD
                             : VKR_EDITOR_CONTENT_COMMAND_OPEN);
    }
    VkrUiWidgetConfig remove = content_widget(1, 0);
    remove.icon = VKR_UI_ICON_TRASH;
    remove.disabled = !content_menu_enabled(content, CONTENT_FOLDER_BIT | index,
                                            VKR_EDITOR_CONTENT_COMMAND_DELETE);
    remove.tooltip = scene ? string8_lit("Delete this scene permanently")
                           : string8_lit("Delete this folder when it holds "
                                         "nothing");
    if (vkr_ui_button(ui, string8_lit("delete"), string8_lit("Delete"),
                      &remove)) {
      content_menu_run(content, CONTENT_FOLDER_BIT | index,
                       VKR_EDITOR_CONTENT_COMMAND_DELETE);
    }
    (void)vkr_ui_panel_end(ui);
  }
  const VkrUiTrack rename_columns[] = {{1, VKR_UI_TRACK_FR},
                                       {80, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig rename = vkr_ui_panel_config_default();
  rename.placement.column = 0;
  rename.placement.row = 3;
  rename.columns = rename_columns;
  rename.column_count = ArrayCount(rename_columns);
  if (vkr_ui_panel_begin(ui, string8_lit("folder-rename"), &rename)) {
    VkrUiWidgetConfig field = content_widget(0, 0);
    vkr_editor_field_style(&field);
    field.placement.justify = VKR_UI_ALIGN_STRETCH;
    field.fill = true_v;
    field.read_only = !content_renamable(content, CONTENT_FOLDER_BIT | index);
    field.tooltip = scene ? string8_lit("Scene name; its id never changes")
                          : string8_lit("Folder name; items keep their files");
    VkrUiTextEditBuffer buffer = {.data = content->folder_draft,
                                  .length = content->folder_draft_length,
                                  .capacity = sizeof(content->folder_draft)};
    const VkrUiId field_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("folder-name"));
    if (vkr_ui_text_field(ui, string8_lit("folder-name"), &buffer, &field)) {
      content->folder_draft_length = buffer.length;
    }
    /* Enter in the field renames too. */
    const bool8_t entered = ui->focused_id == field_id &&
                            input_key_just_pressed(ui->input, KEY_ENTER);
    VkrUiWidgetConfig apply = content_widget(1, 0);
    apply.disabled = field.read_only || !content->folder_draft_length;
    if ((vkr_ui_button(ui, string8_lit("rename-folder"), string8_lit("Rename"),
                       &apply) ||
         entered) &&
        !apply.disabled) {
      /* The same commit as renaming in place: a scene through its project,
         a folder through its labels. */
      content->rename_draft_length = (uint32_t)Min(
          content->folder_draft_length, sizeof(content->rename_draft) - 1u);
      MemCopy(content->rename_draft, content->folder_draft,
              content->rename_draft_length);
      content_commit_rename(content, CONTENT_FOLDER_BIT | index);
    }
    (void)vkr_ui_panel_end(ui);
  }
}

/* Details of an object or an object type: what it is and how to use it. */
static void content_build_builtin_details(VkrEditorContent *content,
                                          VkrUiSystem *ui,
                                          const ContentAsset *entry) {
  const bool8_t object = entry->kind == CONTENT_ENTITY;
  VkrUiWidgetConfig about = content_widget(0, 1);
  about.style.text_color = vkr_ui_theme()->text_secondary;
  about.style.font_size_pt = vkr_ui_theme()->font_caption;
  vkr_ui_label(ui, string8_lit("about"),
               object ? string8_lit("An object of a loaded scene or the "
                                    "World.")
                      : string8_lit("An object type. Drag it into the "
                                    "viewport or add it here."),
               &about);
  VkrUiWidgetConfig use = content_widget(0, 2);
  use.placement.align = VKR_UI_ALIGN_START;
  use.icon = object ? VKR_UI_ICON_SELECTION : VKR_UI_ICON_ADD;
  use.disabled = content->read_only && !object;
  if (vkr_ui_button(
          ui, string8_lit("use"),
          object ? string8_lit("Select") : string8_lit("Add to scene"), &use)) {
    content_activate(content, content->selected);
  }
}

/* Details of a preset: its component type, rename and delete. */
static void content_build_preset_details(VkrEditorContent *content,
                                         VkrUiSystem *ui,
                                         const ContentAsset *entry) {
  if (entry->object >= content->presets.count) {
    return;
  }
  const VkrEditorPreset *preset = &content->presets.presets[entry->object];
  VkrUiWidgetConfig about = content_widget(0, 1);
  about.style.text_color = vkr_ui_theme()->text_secondary;
  about.style.font_size_pt = vkr_ui_theme()->font_caption;
  vkr_ui_label(ui, string8_lit("about"),
               string8_create_formatted(ui->frame_allocator,
                                        "%s preset. Apply it from the "
                                        "component's Presets menu.",
                                        preset->type->label),
               &about);
  VkrUiWidgetConfig remove = content_widget(0, 2);
  remove.placement.align = VKR_UI_ALIGN_START;
  remove.icon = VKR_UI_ICON_TRASH;
  remove.disabled = content->read_only;
  if (vkr_ui_button(ui, string8_lit("delete-preset"),
                    string8_lit("Delete preset"), &remove)) {
    content_delete_preset(content, entry->object);
    return;
  }
  if (strcmp(content->rename_asset, entry->id)) {
    content_copy(content->rename_asset, sizeof(content->rename_asset),
                 entry->id);
    content_copy((char *)content->rename, sizeof(content->rename), entry->name);
    content->rename_length = (uint32_t)strlen((char *)content->rename);
  }
  content_build_rename(content, ui, entry);
}

/* Pixel size of a KTX2 (.vkt) texture artifact: the header's pixelWidth
   and pixelHeight follow its 12-byte identifier and two 32-bit fields. */
static void content_texture_size(ContentAsset *entry) {
  entry->size_read = true_v;
  static const uint8_t identifier[12] = {0xAB, 'K',  'T',  'X',  ' ',  '2',
                                         '0',  0xBB, '\r', '\n', 0x1A, '\n'};
  FilePath path = {.path = content_string(entry->path),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle file = {0};
  uint8_t header[28];
  uint64_t read = 0;
  const bool8_t valid =
      entry->path[0] && file_open(&path, mode, &file) == FILE_ERROR_NONE &&
      file_read_into(&file, header, sizeof(header), &read) == FILE_ERROR_NONE &&
      read == sizeof(header) &&
      !MemCompare(header, identifier, sizeof(identifier));
  file_close(&file);
  if (valid) {
    MemCopy(&entry->width, header + 20, sizeof(entry->width));
    MemCopy(&entry->height, header + 24, sizeof(entry->height));
  }
}

static void content_build_inspector(VkrEditorContent *content, VkrUiSystem *ui,
                                    float32_t inspector_height,
                                    bool8_t show_details) {
  VkrUiPanelConfig inspector = vkr_ui_panel_config_default();
  inspector.placement.column = 2;
  inspector.placement.row = 0;
  inspector.style.background_color = vkr_ui_theme()->header;
  inspector.style.padding_pt = (VkrUiEdges){8, 8, 8, 8};
  inspector.clip_children = true_v;
  const VkrUiTrack inspector_rows[] = {{30, VKR_UI_TRACK_PX},
                                       {42, VKR_UI_TRACK_PX},
                                       {58, VKR_UI_TRACK_PX},
                                       {28, VKR_UI_TRACK_PX},
                                       {60, VKR_UI_TRACK_PX}};
  inspector.rows = inspector_rows;
  inspector.row_count = inspector_height > 30 ? ArrayCount(inspector_rows) : 1;
  if (!show_details ||
      !vkr_ui_panel_begin(ui, string8_lit("inspect"), &inspector)) {
    return;
  }
  VkrUiWidgetConfig text = content_widget(0, 0);
  if (content->selected_folder[0]) {
    vkr_ui_label(
        ui, string8_lit("selection"),
        content_string(content_folder_label(content, content->selected_folder)),
        &text);
    if (inspector_height > 30) {
      content_build_folder_details(content, ui);
    }
  } else if (content->selected < content->count) {
    ContentAsset *entry = &content->entries[content->selected];
    String8 summary = content_string(entry->name);
    text.tooltip = summary;
    vkr_ui_label(ui, string8_lit("selection"), summary, &text);
    if (inspector_height > 30 && entry->scope == CONTENT_SCOPE_BUILTIN) {
      content_build_builtin_details(content, ui, entry);
    } else if (inspector_height > 30 && entry->kind == CONTENT_PRESET) {
      content_build_preset_details(content, ui, entry);
    } else if (inspector_height > 30) {
      VkrUiWidgetConfig provenance = content_widget(0, 1);
      const char *diagnostic = entry->diagnostic;
      for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
        if (content->cache[i].key &&
            content->cache[i].asset == content->selected &&
            content->cache[i].diagnostic[0]) {
          diagnostic = content->cache[i].diagnostic;
        }
      }
      if (entry->kind == CONTENT_TEXTURE && !entry->size_read) {
        content_texture_size(entry);
      }
      const char *size =
          entry->width ? (const char *)string8_create_formatted(
                             ui->frame_allocator, "\n%u \xc3\x97 %u pixels",
                             entry->width, entry->height)
                             .str
                       : "";
      /* Cooked-only imports keep no source, so they cannot be reimported. */
      String8 details = string8_create_formatted(
          ui->frame_allocator, "%s%s%s%s%s",
          diagnostic[0]      ? diagnostic
          : entry->source[0] ? entry->source
          : entry->scope < 2u && entry->kind != CONTENT_SCENE
              ? "Source unavailable: reimport disabled"
              : entry->path,
          entry->recipe[0] ? "\nRecipe: " : "", entry->recipe,
          entry->dependencies
              ? (const char *)string8_create_formatted(
                    ui->frame_allocator, " \xc2\xb7 %u dependencies",
                    entry->dependencies)
                    .str
              : "",
          size);
      provenance.tooltip = details;
      vkr_ui_label(ui, string8_lit("provenance"), details, &provenance);
      if (strcmp(content->rename_asset, entry->id)) {
        content_copy(content->rename_asset, sizeof(content->rename_asset),
                     entry->id);
        content_copy((char *)content->rename, sizeof(content->rename),
                     entry->name);
        content->rename_length = (uint32_t)strlen((char *)content->rename);
      }
      content_build_rename(content, ui, entry);
      content_build_asset_actions(content, ui, entry);
      content_build_labels(content, ui, entry);
    }
  } else {
    vkr_ui_label(
        ui, string8_lit("hint"),
        content_string(content->diagnostic[0]
                           ? content->diagnostic
                           : "Select an item or folder to see its details"),
        &text);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_content_build(VkrEditorContent *content, VkrUiSystem *ui,
                              VkrUiRect rect, VkrFontHandle heading) {
  (void)heading;
  if (!content || !ui || rect.width <= 0 || rect.height <= 0) {
    return;
  }
  const float32_t scale = ui->content_scale;
  const float32_t width = rect.width / scale;
  const float32_t height = rect.height / scale;
  const bool8_t list = content->size == CONTENT_SIZE_LIST;
  const float32_t card_width = content->size == 256 ? 164.0f : 112.0f;
  const float32_t tools_height = 64;
  const float32_t grid_height = Max(1.0f, height - tools_height - 22);
  /* A short panel shrinks the picture so a tile's name stays visible. */
  const float32_t card_height =
      list ? 24.0f : Max(84.0f, Min(card_width + 43.0f, grid_height - 14.0f));
  const bool8_t show_tree = width >= 620;
  const bool8_t show_details =
      width >= 1040 && height >= 240 && !content->details_hidden;
  const float32_t tree_width = show_tree ? 190.0f : 0.0f;
  const float32_t detail_width = show_details ? 260.0f : 0.0f;
  const float32_t asset_width = Max(1.0f, width - tree_width - detail_width);
  const float32_t inspector_height = show_details ? height - 86.0f : 0.0f;
  const float32_t header_height = list ? 24.0f : 0.0f;
  const uint32_t columns =
      list ? 1u
           : Max(1u, Min(16u,
                         (uint32_t)(Max(0.0f, asset_width - 16) / card_width)));
  const uint32_t row_cap = list ? CONTENT_LIST_ROWS - 1u : 8u;
  const uint32_t visible_rows = Max(
      1u, Min(row_cap, (uint32_t)(Max(0.0f, grid_height - header_height - 8) /
                                  (card_height + (list ? 2 : 6)))));
  const uint32_t rows = Min(row_cap + 1u, visible_rows + 1);
  const VkrUiRect grid_area = {rect.x + tree_width * scale,
                               rect.y + tools_height * scale,
                               asset_width * scale, grid_height * scale};
  content_filter(content);
  const VkrUiTrack root_rows[] = {{tools_height, VKR_UI_TRACK_PX},
                                  {1, VKR_UI_TRACK_FR},
                                  {22, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig root = vkr_ui_panel_config_default();
  root.placement.column = 0;
  root.placement.row = 0;
  root.rows = root_rows;
  root.row_count = ArrayCount(root_rows);
  root.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("content.browser"), &root)) {
    return;
  }
  content_build_toolbar(content, ui, width, height);
  const VkrUiTrack body_columns[] = {{tree_width, VKR_UI_TRACK_PX},
                                     {1, VKR_UI_TRACK_FR},
                                     {detail_width, VKR_UI_TRACK_PX}};
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement.column = 0;
  body.placement.row = 1;
  body.columns = body_columns;
  body.column_count = ArrayCount(body_columns);
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("body"), &body)) {
    (void)vkr_ui_panel_end(ui);
    return;
  }
  if (show_tree) {
    content_tree_panel(content, ui,
                       (VkrUiRect){rect.x, rect.y + tools_height * scale,
                                   tree_width * scale, grid_height * scale});
  }
  const uint32_t total_rows = (content->filtered_count + columns - 1) / columns;
  const uint32_t max_first =
      total_rows > visible_rows ? total_rows - visible_rows : 0;
  if (content_pointer_inside(ui, grid_area) && ui->mouse_wheel) {
    const int64_t next = (int64_t)content->first_row - ui->mouse_wheel;
    content->first_row =
        (uint32_t)Max((int64_t)0, Min((int64_t)max_first, next));
    ui->capture.mouse = true_v;
  }
  content->first_row = Min(content->first_row, max_first);
  VkrUiWidgetConfig grid_focus = content_widget(1, 0);
  grid_focus.style.background_color = (Vec4){0};
  grid_focus.style.hover_background_color = VKR_UI_COLOR_NONE;
  grid_focus.style.active_background_color = VKR_UI_COLOR_NONE;
  const VkrUiId grid_id = vkr_ui_id_stack_widget_label(
      &ui->id_stack, string8_lit("asset-grid-focus"));
  (void)vkr_ui_button(ui, string8_lit("asset-grid-focus"), (String8){0},
                      &grid_focus);
  content_grid_keys(content, ui, grid_id, columns, visible_rows);
  if (content->reveal_selection && !content->filter_dirty) {
    content->reveal_selection = false_v;
    const uint32_t selection = content_selection(content);
    for (uint32_t position = 0; position < content->filtered_count;
         ++position) {
      if (content->filtered[position] == selection) {
        const uint32_t row = position / columns;
        if (row < content->first_row ||
            row >= content->first_row + visible_rows) {
          content->first_row = Min(row, max_first);
        }
        break;
      }
    }
  }
  content_build_grid(content, ui, columns, rows, max_first, card_width,
                     card_height, grid_id, grid_area);
  /* A right press on empty space asks for the shown folder's menu. */
  if (content->right_pressed && content_pointer_inside(ui, grid_area)) {
    content->right_pressed = false_v;
    content->menu_requested = true_v;
    content->menu_folder = true_v;
    content->menu_position_pt =
        (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
               (float32_t)ui->mouse_y / ui->content_scale};
  }
  content_build_inspector(content, ui, inspector_height, show_details);
  (void)vkr_ui_panel_end(ui);
  VkrUiWidgetConfig footer = content_widget(0, 2);
  footer.style.font_size_pt = vkr_ui_theme()->font_caption;
  footer.style.text_color = vkr_ui_theme()->text_secondary;
  const char *selection =
      content->selected_folder[0]
          ? content_folder_label(content, content->selected_folder)
      : content->selected < content->count
          ? content->entries[content->selected].name
          : "";
  String8 count = string8_create_formatted(
      ui->frame_allocator, "%u items%s%s", content->filtered_count,
      selection[0] ? "  \xc2\xb7  Selected: " : "", selection);
  vkr_ui_label(ui, string8_lit("asset-count"), count, &footer);
  (void)vkr_ui_panel_end(ui);
  content_start_preview(content);
}

bool8_t vkr_editor_content_take_object_action(VkrEditorContent *content,
                                              VkrEditorContentAction *action) {
  if (!content) {
    return false_v;
  }
  const VkrEditorContentActionKind kind = content->action.kind;
  if (kind != VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY &&
      kind != VKR_EDITOR_CONTENT_ACTION_FRAME_ENTITY &&
      kind != VKR_EDITOR_CONTENT_ACTION_RENAME_ENTITY &&
      kind != VKR_EDITOR_CONTENT_ACTION_DELETE_ENTITY) {
    return false_v;
  }
  *action = content->action;
  content->action.kind = VKR_EDITOR_CONTENT_ACTION_NONE;
  return true_v;
}

bool8_t vkr_editor_content_take_menu(VkrEditorContent *content,
                                     Vec2 *position_pt, bool8_t *folder) {
  if (!content || !content->menu_requested) {
    return false_v;
  }
  content->menu_requested = false_v;
  *position_pt = content->menu_position_pt;
  *folder = content->menu_folder;
  return true_v;
}

bool8_t vkr_editor_content_dragged_script(const VkrEditorContent *content,
                                          char *module, uint32_t capacity) {
  if (!content || !content->dragging ||
      content_shown_folder(content->drag_asset) ||
      content->drag_asset >= content->count ||
      content->entries[content->drag_asset].kind != CONTENT_SCRIPT) {
    return false_v;
  }
  snprintf(module, capacity, "%s", content->entries[content->drag_asset].role);
  return true_v;
}

void vkr_editor_content_end_drag(VkrEditorContent *content) {
  if (content) {
    content->dragging = false_v;
    content->drag_asset = CONTENT_NONE;
  }
}

VkrEntityId vkr_editor_content_menu_entity(const VkrEditorContent *content) {
  if (!content || content->menu_folder ||
      content_shown_folder(content->menu_item) ||
      content->menu_item >= content->count ||
      content->entries[content->menu_item].kind != CONTENT_ENTITY) {
    return VKR_ENTITY_ID_INVALID;
  }
  return content->entries[content->menu_item].entity;
}

bool8_t
vkr_editor_content_folder_available(const VkrEditorContent *content,
                                    VkrEditorContentFolderCommand command) {
  if (!content) {
    return false_v;
  }
  const bool8_t project = !content->read_only && content->project[0];
  switch (command) {
  case VKR_EDITOR_CONTENT_FOLDER_NEW_FOLDER:
    return project && content_project_folder(content->folder);
  case VKR_EDITOR_CONTENT_FOLDER_NEW_SCRIPT:
  case VKR_EDITOR_CONTENT_FOLDER_IMPORT:
    return project;
  default:
    return true_v;
  }
}

void vkr_editor_content_folder_command(VkrEditorContent *content,
                                       VkrEditorContentFolderCommand command) {
  if (!vkr_editor_content_folder_available(content, command)) {
    return;
  }
  switch (command) {
  case VKR_EDITOR_CONTENT_FOLDER_NEW_FOLDER:
    (void)content_new_folder(content, content->folder);
    break;
  case VKR_EDITOR_CONTENT_FOLDER_NEW_SCRIPT:
    content_action(content, VKR_EDITOR_CONTENT_ACTION_NEW_SCRIPT);
    break;
  case VKR_EDITOR_CONTENT_FOLDER_IMPORT:
    content_action(content, VKR_EDITOR_CONTENT_ACTION_IMPORT);
    break;
  default:
    vkr_editor_content_refresh(content);
    break;
  }
}

void vkr_editor_content_reveal_created(VkrEditorContent *content,
                                       VkrEntityId selection) {
  if (content) {
    content->reveal_previous = selection;
    content->reveal_frames = 120u;
  }
}

/* Opens the folder of the object an earlier creation selected, once its
   entry exists. */
static void content_reveal_created(VkrEditorContent *content,
                                   const VkrSampleUiFrame *frame) {
  if (!content->reveal_frames) {
    return;
  }
  --content->reveal_frames;
  const VkrEntityId selected = frame->selected_entity;
  if (!selected.u64 || selected.u64 == content->reveal_previous.u64) {
    return;
  }
  for (uint32_t i = content->asset_count; i < content->count; ++i) {
    if (content->entries[i].entity.u64 == selected.u64) {
      content->reveal_frames = 0u;
      content->query[0] = 0;
      content->query_length = 0;
      content->tag_filter[0] = '\0';
      content_open_folder(content,
                          content_item_folder(content, &content->entries[i]));
      content_select(content, i);
      content->reveal_selection = true_v;
      content->filter_dirty = true_v;
      return;
    }
  }
}

const char *vkr_editor_content_menu_label(const VkrEditorContent *content,
                                          VkrEditorContentCommand command) {
  const uint32_t shown = content->menu_item;
  const bool8_t folder = content_shown_folder(shown);
  const char *path =
      folder && (shown & ~CONTENT_FOLDER_BIT) < content->tree_count
          ? content->tree[shown & ~CONTENT_FOLDER_BIT].path
          : NULL;
  const bool8_t scene = path && content_scene_folder(path);
  const bool8_t world = path && !path[0];
  const ContentKind kind = !folder && shown < content->count
                               ? content->entries[shown].kind
                               : CONTENT_OTHER;
  switch (command) {
  case VKR_EDITOR_CONTENT_COMMAND_LOAD:
    return scene ? "Load scene" : NULL;
  case VKR_EDITOR_CONTENT_COMMAND_OPEN:
    return scene                    ? "Open scene"
           : kind == CONTENT_ENTITY ? "Select"
           : kind == CONTENT_SCRIPT ? "Edit script"
           : folder                 ? "Open"
                                    : "Show details";
  case VKR_EDITOR_CONTENT_COMMAND_PLACE:
    return world                    ? "Show the World"
           : scene                  ? "Add beside the open scene"
           : kind == CONTENT_ENTITY ? "Frame in viewport"
           : kind == CONTENT_OBJECT ? "Add to scene"
                                    : "Place in viewport";
  case VKR_EDITOR_CONTENT_COMMAND_RENAME:
    return "Rename";
  default:
    return "Delete";
  }
}

bool8_t vkr_editor_content_menu_available(const VkrEditorContent *content,
                                          VkrEditorContentCommand command) {
  return content && content_menu_enabled(content, content->menu_item, command);
}

void vkr_editor_content_menu_command(VkrEditorContent *content,
                                     VkrEditorContentCommand command) {
  if (content) {
    content_menu_run(content, content->menu_item, command);
  }
}

/* Whether a folder's labels from Content down spell a shown path. */
static bool8_t content_shown_path_matches(const VkrEditorContent *content,
                                          uint32_t index, const char *path) {
  size_t end = strlen(path);
  while (index < content->tree_count && content->tree[index].path[0]) {
    const ContentFolder *folder = &content->tree[index];
    const size_t length = strlen(folder->label);
    if (length > end ||
        MemCompare(path + end - length, folder->label, length)) {
      return false_v;
    }
    end -= length;
    index = folder->parent;
    if (index < content->tree_count && content->tree[index].path[0]) {
      if (end == 0u || path[end - 1u] != '/') {
        return false_v;
      }
      --end;
    }
  }
  return end == 0u;
}

const char *vkr_editor_content_selected_name(const VkrEditorContent *content) {
  return content && content->selected < content->count
             ? content->entries[content->selected].name
             : "";
}

bool8_t vkr_editor_content_reveal_path(VkrEditorContent *content,
                                       const char *path) {
  if (!content || !path || !path[0]) {
    return false_v;
  }
  /* The asset whose artifact is the file, else the one whose build revision
     holds it: a mesh owns the materials and textures it built. */
  uint32_t found = CONTENT_NONE;
  uint32_t revision_match = CONTENT_NONE;
  for (uint32_t i = 0; found == CONTENT_NONE && i < content->asset_count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if (!entry->path[0] || entry->kind == CONTENT_SCENE) {
      continue;
    }
    if (!strcmp(entry->path, path)) {
      found = i;
      continue;
    }
    const char *builds = strstr(entry->path, "/builds/");
    const char *end = builds ? strchr(builds + 8, '/') : NULL;
    if (end && revision_match == CONTENT_NONE &&
        !strncmp(entry->path, path, (size_t)(end - entry->path + 1))) {
      revision_match = i;
    }
  }
  if (found == CONTENT_NONE) {
    found = revision_match;
  }
  if (found == CONTENT_NONE) {
    return false_v;
  }
  content->query[0] = 0;
  content->query_length = 0;
  content->tag_filter[0] = '\0';
  content->filter_dirty = true_v;
  content_open_folder(content,
                      content_item_folder(content, &content->entries[found]));
  content_select(content, found);
  content->reveal_selection = true_v;
  return true_v;
}

bool8_t vkr_editor_content_open_folder(VkrEditorContent *content,
                                       String8 path) {
  char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  if (!content || path.length >= sizeof(folder)) {
    return false_v;
  }
  MemCopy(folder, path.str, path.length);
  folder[path.length] = '\0';
  content_filter(content);
  if (!vkr_editor_folder_normalize(folder)) {
    return false_v;
  }
  /* A path, else a folder's shown name or shown path below Content, such
     as "Bistro/Textures". */
  uint32_t index = content_tree_find(content, folder);
  for (uint32_t i = 0; index == CONTENT_NONE && i < content->tree_count; ++i) {
    if (!strcmp(content->tree[i].label, folder) ||
        content_shown_path_matches(content, i, folder)) {
      index = i;
    }
  }
  if (index == CONTENT_NONE) {
    return false_v;
  }
  content_open_folder(content, content->tree[index].path);
  return true_v;
}

bool8_t vkr_editor_content_new_folder(VkrEditorContent *content, String8 path) {
  char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  if (!content || content->read_only || !content->project[0] ||
      path.length >= sizeof(folder)) {
    return false_v;
  }
  vkr_editor_label_clean(folder, sizeof(folder), path.str, path.length);
  if (!vkr_editor_folder_normalize(folder) || !folder[0] ||
      !content_project_folder(folder) ||
      !vkr_editor_folders_add(&content->labels, folder)) {
    return false_v;
  }
  content_labels_save(content);
  return true_v;
}

bool8_t vkr_editor_content_move(VkrEditorContent *content, String8 item,
                                String8 folder) {
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  char target[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  if (!content || item.length >= sizeof(name) ||
      folder.length >= sizeof(target)) {
    return false_v;
  }
  MemCopy(name, item.str, item.length);
  name[item.length] = '\0';
  MemCopy(target, folder.str, folder.length);
  target[folder.length] = '\0';
  content_filter(content);
  if (!vkr_editor_folder_normalize(target) ||
      content_tree_find(content, target) == CONTENT_NONE) {
    return false_v;
  }
  /* A folder path names a folder; otherwise the first item by id or name. */
  uint32_t shown = content_tree_find(content, name);
  if (shown != CONTENT_NONE && shown) {
    shown |= CONTENT_FOLDER_BIT;
  } else {
    shown = CONTENT_NONE;
    for (uint32_t i = 0; i < content->count && shown == CONTENT_NONE; ++i) {
      if (!strcmp(content->entries[i].id, name) ||
          !strcmp(content->entries[i].name, name)) {
        shown = i;
      }
    }
  }
  return shown != CONTENT_NONE && content_move(content, shown, target);
}

void vkr_editor_content_set_list(VkrEditorContent *content, bool8_t list) {
  if (content) {
    content->size = list ? CONTENT_SIZE_LIST : 128;
  }
}

bool8_t vkr_editor_content_command(VkrEditorContent *content, String8 item,
                                   VkrEditorContentCommand command) {
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  if (!content || item.length >= sizeof(name)) {
    return false_v;
  }
  MemCopy(name, item.str, item.length);
  name[item.length] = '\0';
  content_filter(content);
  /* A folder by path or shown name, else an item by id or name. */
  uint32_t shown = CONTENT_NONE;
  for (uint32_t i = 0; shown == CONTENT_NONE && i < content->tree_count; ++i) {
    if (!strcmp(content->tree[i].path, name) ||
        !strcmp(content->tree[i].label, name)) {
      shown = CONTENT_FOLDER_BIT | i;
    }
  }
  /* Objects and assets win over the object type of the same name. */
  for (uint32_t pass = 0; pass < 2u; ++pass) {
    for (uint32_t i = 0; shown == CONTENT_NONE && i < content->count; ++i) {
      const ContentAsset *entry = &content->entries[i];
      if (entry->kind != CONTENT_SCENE &&
          (entry->kind == CONTENT_OBJECT) == (pass == 1u) &&
          (!strcmp(entry->id, name) || !strcmp(entry->name, name))) {
        shown = i;
      }
    }
  }
  if (shown == CONTENT_NONE || !content_menu_enabled(content, shown, command)) {
    return false_v;
  }
  content_select(content, shown);
  content_menu_run(content, shown, command);
  return true_v;
}

bool8_t vkr_editor_content_place(VkrEditorContent *content, String8 item,
                                 Vec2 drop_px) {
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  if (!content || item.length >= sizeof(name)) {
    return false_v;
  }
  MemCopy(name, item.str, item.length);
  name[item.length] = '\0';
  for (uint32_t i = 0; i < content->count; ++i) {
    const ContentAsset *entry = &content->entries[i];
    if ((strcmp(entry->id, name) && strcmp(entry->name, name)) ||
        !content_viewport_accepts(entry)) {
      continue;
    }
    content->selected = i;
    content->selected_folder[0] = '\0';
    if (entry->kind != CONTENT_MESH) {
      content_activate(content, i);
      return content->action.kind != VKR_EDITOR_CONTENT_ACTION_NONE;
    }
    content_action(content, VKR_EDITOR_CONTENT_ACTION_PLACE_ASSET);
    content->action.drop_px = drop_px;
    return content->action.kind != VKR_EDITOR_CONTENT_ACTION_NONE;
  }
  return false_v;
}

const char *vkr_editor_content_folder(const VkrEditorContent *content) {
  return content ? content->folder : "";
}

void vkr_editor_content_arm_file_drop(VkrEditorContent *content, Vec2 drop_px) {
  if (content) {
    content->file_drop_px = drop_px;
    content->file_drop_armed = true_v;
    content->drop_folder_set = false_v;
  }
}

void vkr_editor_content_file_drop_folder(
    VkrEditorContent *content, char out[VKR_EDITOR_FOLDER_PATH_CAPACITY]) {
  out[0] = '\0';
  if (!content) {
    return;
  }
  content_copy(out, VKR_EDITOR_FOLDER_PATH_CAPACITY,
               content->drop_folder_set ? content->drop_folder
                                        : content->folder);
  /* Imports are project assets: they file into a project folder. */
  if (!content_project_folder(out)) {
    out[0] = '\0';
  }
  content->file_drop_armed = false_v;
  content->drop_folder_set = false_v;
}

bool8_t vkr_editor_content_file_into(VkrEditorContent *content, const char *id,
                                     const char *folder) {
  if (!content || !id || !folder || content->read_only) {
    return false_v;
  }
  const VkrEditorAssetLabel *label = content_label(content, id);
  char tags[128];
  content_copy(tags, sizeof(tags), label ? label->tags : "");
  return content_label_set(content, id, folder, tags);
}

uint32_t vkr_editor_content_preset_count(const VkrEditorContent *content) {
  return content ? content->presets.count : 0u;
}

const VkrEditorPreset *
vkr_editor_content_preset(const VkrEditorContent *content, uint32_t index) {
  return content && index < content->presets.count
             ? &content->presets.presets[index]
             : NULL;
}

bool8_t vkr_editor_content_save_preset(VkrEditorContent *content,
                                       const VkrTypeDesc *type,
                                       const void *value) {
  if (!content || content->read_only || !content->project[0] || !type ||
      !value ||
      vkr_editor_preset_type(string8_create_from_cstr(
          (const uint8_t *)type->name, strlen(type->name))) != type ||
      !vkr_type_validate(type, value, NULL, 0u) ||
      content->presets.count == content->presets.capacity) {
    return false_v;
  }
  VkrEditorPreset *preset = &content->presets.presets[content->presets.count];
  MemZero(preset, sizeof(*preset));
  VkrEditorProjectError error = {0};
  if (!vkr_editor_project_id_generate(preset->id, &error)) {
    return false_v;
  }
  preset->type = type;
  MemCopy(preset->value, value, type->size);
  /* "Post process", then "Post process 2" and on for the first free name. */
  for (uint32_t n = 1u; n < 1000u; ++n) {
    if (n == 1u) {
      snprintf(preset->name, sizeof(preset->name), "%.48s", type->label);
    } else {
      snprintf(preset->name, sizeof(preset->name), "%.48s %u", type->label, n);
    }
    bool8_t taken = false_v;
    for (uint32_t i = 0; !taken && i < content->presets.count; ++i) {
      taken = !strcmp(content->presets.presets[i].name, preset->name);
    }
    if (!taken) {
      break;
    }
  }
  ++content->presets.count;
  if (!content_presets_save(content)) {
    --content->presets.count;
    return false_v;
  }
  if (content_reserve(content)) {
    ContentAsset *entry = &content->entries[content->count++];
    MemZero(entry, sizeof(*entry));
    entry->kind = CONTENT_PRESET;
    entry->scope = 1u;
    entry->object = content->presets.count - 1u;
    entry->label = CONTENT_NONE;
    content_copy(entry->id, sizeof(entry->id), preset->id);
    content_copy(entry->name, sizeof(entry->name), preset->name);
  }
  content->filter_dirty = true_v;
  return true_v;
}

#include "editor_content.h"
#include "core/vkr_atomic.h"
#include "core/vkr_json.h"
#include "core/vkr_threads.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include "filesystem/filesystem.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_resource_system.h"
#include <ctype.h>
#include <math.h>

#define CONTENT_MAX_ASSETS 8192u
#define CONTENT_CACHE_COUNT 64u
#define CONTENT_NONE UINT32_MAX
#define CONTENT_PATH 1024u

typedef enum ContentKind {
  CONTENT_TEXTURE,
  CONTENT_MATERIAL,
  CONTENT_MESH,
  CONTENT_FONT,
  CONTENT_ENVIRONMENT,
  CONTENT_SCENE,
  CONTENT_PROBE,
  CONTENT_OTHER,
  CONTENT_KIND_COUNT
} ContentKind;

typedef struct ContentAsset {
  char id[37];
  char name[513];
  char path[CONTENT_PATH];
  char source[CONTENT_PATH];
  char role[64];
  char fingerprint[96];
  char diagnostic[256];
  ContentKind kind;
  uint32_t scope;
  bool8_t missing;
  bool8_t stale;
} ContentAsset;

typedef struct ContentLocation {
  uint32_t scope;
  uint32_t kind;
} ContentLocation;

typedef struct ContentScrollbar {
  bool8_t dragging;
  float32_t grab_offset;
} ContentScrollbar;

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
  uint32_t *filtered;
  uint32_t filtered_count;
  uint32_t uploads_this_frame;
  uint32_t selected;
  uint32_t first_row;
  uint32_t source_first_row;
  ContentScrollbar grid_scrollbar;
  ContentScrollbar source_scrollbar;
  ContentLocation history[32];
  uint32_t history_count;
  uint32_t history_index;
  Keys navigation_key;
  float64_t navigation_elapsed;
  float64_t navigation_next;
  uint32_t type_filter;
  uint32_t source_counts[3][CONTENT_KIND_COUNT];
  bool8_t details_hidden;
  uint32_t scope_filter;
  uint32_t size;
  bool8_t reverse_sort;
  bool8_t filter_dirty;
  bool8_t suspended;
  bool8_t read_only;
  char workspace[CONTENT_PATH];
  char project[37];
  char scene[37];
  char diagnostic[512];
  uint8_t query[128];
  uint32_t query_length;
  uint8_t rename[513];
  uint32_t rename_length;
  char rename_asset[37];
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
};

static const char *const content_kinds[] = {"Texture", "Material",    "Mesh",
                                            "Font",    "Environment", "Scene",
                                            "Probe",   "Other"};
static const VkrUiIcon content_icons[] = {
    VKR_UI_ICON_TEXTURE, VKR_UI_ICON_MATERIAL,    VKR_UI_ICON_MESH,
    VKR_UI_ICON_FONT,    VKR_UI_ICON_ENVIRONMENT, VKR_UI_ICON_SCENE,
    VKR_UI_ICON_PROBE,   VKR_UI_ICON_FOLDER};
static const char *const content_scopes[] = {"Scene", "Project", "Editor"};

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
  const char *executable = VKR_EDITOR_ASSET_PREVIEW_PATH;
  if (content->worker_material) {
    executable = VKR_EDITOR_PYTHON_PATH;
    arguments[count++] = PROJECT_SOURCE_DIR "tools/editor_material_preview.py";
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
    arguments[count++] = "--mesh-cooker";
    arguments[count++] = VKR_EDITOR_MESH_COOKER_PATH;
    arguments[count++] = "--harness";
    arguments[count++] = VKR_EDITOR_HARNESS_PATH;
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
    const char *prune_arguments[] = {PROJECT_SOURCE_DIR
                                     "tools/editor_preview_cache.py",
                                     "--directory", directory};
    VkrPlatformProcessConfig prune = {.executable = VKR_EDITOR_PYTHON_PATH,
                                      .arguments = prune_arguments,
                                      .argument_count =
                                          ArrayCount(prune_arguments),
                                      .working_directory = PROJECT_SOURCE_DIR,
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
      .working_directory = PROJECT_SOURCE_DIR,
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
  return content;
}

void vkr_editor_content_destroy(VkrEditorContent *content) {
  if (!content) {
    return;
  }
  if (!vkr_editor_content_stop_previews(content)) {
    return;
  }
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
                       sizeof(*content->filtered) * CONTENT_MAX_ASSETS,
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

static void content_read_inventory(VkrEditorContent *content, const char *root,
                                   const char *manifest, uint32_t scope) {
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
    /* Scene v4 keeps its records in an immutable inventory revision that may
     * exceed the 1 MiB manifest parser, so its array is walked in place. */
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
    (void)vkr_editor_project_json_string(record, "fingerprint",
                                         entry.fingerprint,
                                         sizeof(entry.fingerprint), &error);
    (void)vkr_editor_project_json_string(record, "diagnostic", entry.diagnostic,
                                         sizeof(entry.diagnostic), &error);
    if (vkr_editor_project_json_string(record, "source", relative,
                                       sizeof(relative), &error) &&
        relative[0]) {
      (void)vkr_editor_project_resolve(root, relative, entry.source, &error);
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
          entry.missing =
              !vkr_editor_project_resolve(root, relative, entry.path, &error);
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
    ++content->source_counts[scope][entry.kind];
    content->entries[content->count++] = entry;
  }
cleanup:
  if (bytes) {
    vkr_allocator_free(content->allocator, bytes, size + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
}

void vkr_editor_content_refresh(VkrEditorContent *content) {
  if (!content) {
    return;
  }
  content_cancel_worker(content);
  ++content->generation;
  content->count = 0;
  MemZero(content->source_counts, sizeof(content->source_counts));
  content->selected = CONTENT_NONE;
  content->first_row = 0;
  content->diagnostic[0] = 0;
  content->inventory_key = UINT64_C(14695981039346656037);
  for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
    content_release_preview(&content->cache[i]);
  }
  char root[CONTENT_PATH];
  char manifest[CONTENT_PATH];
  if (content->project[0]) {
    snprintf(root, sizeof(root), "%s/projects/%s", content->workspace,
             content->project);
    snprintf(manifest, sizeof(manifest), "%s/project.json", root);
    content_read_inventory(content, root, manifest, 1);
    if (content->scene[0]) {
      snprintf(root, sizeof(root), "%s/projects/%s/scenes/%s",
               content->workspace, content->project, content->scene);
      snprintf(manifest, sizeof(manifest), "%s/scene.json", root);
      content_read_inventory(content, root, manifest, 0);
    }
  }
  if (content->workspace[0]) {
    snprintf(root, sizeof(root), "%s/editor/bundles/1", content->workspace);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", root);
    content_read_inventory(content, root, manifest, 2);
  }
  content->filter_dirty = true_v;
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
  if (!content_copy(content->workspace, sizeof(content->workspace),
                    workspace_root) ||
      !content_copy(content->project, sizeof(content->project), project_id) ||
      !content_copy(content->scene, sizeof(content->scene), scene_id)) {
    snprintf(content->diagnostic, sizeof(content->diagnostic),
             "Content context path or ID exceeds supported capacity.");
    return;
  }
  content->history_count = 0;
  content->history_index = 0;
  content->source_first_row = 0;
  content->grid_scrollbar = (ContentScrollbar){0};
  content->source_scrollbar = (ContentScrollbar){0};
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
         vkr_json_writer_name(writer, string8_lit("type")) &&
         vkr_json_writer_u64(writer, content->type_filter) &&
         vkr_json_writer_name(writer, string8_lit("scope")) &&
         vkr_json_writer_u64(writer, content->scope_filter) &&
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
  content->type_filter = 0;
  content->scope_filter = 0;
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
  VkrJsonReader reader = vkr_json_reader_from_string(settings);
  int32_t size = 128, type = 0, scope = 0;
  (void)vkr_json_get_int(&reader, "size", &size);
  reader.pos = 0;
  (void)vkr_json_get_int(&reader, "type", &type);
  reader.pos = 0;
  (void)vkr_json_get_int(&reader, "scope", &scope);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "reverse", &content->reverse_sort);
  reader.pos = 0;
  (void)vkr_json_get_bool(&reader, "details_hidden", &content->details_hidden);
  content->size = size == 256 ? 256 : 128;
  content->type_filter =
      type >= 0 && type <= CONTENT_KIND_COUNT ? (uint32_t)type : 0;
  content->scope_filter = scope >= 0 && scope <= 3 ? (uint32_t)scope : 0;
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

static void content_filter(VkrEditorContent *content) {
  if (!content->filter_dirty) {
    return;
  }
  if (!content->filtered) {
    content->filtered = vkr_allocator_alloc(
        content->allocator, CONTENT_MAX_ASSETS * sizeof(*content->filtered),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!content->filtered) {
      return;
    }
  }
  content->filtered_count = 0;
  for (uint32_t i = 0; i < content->count; ++i) {
    ContentAsset *entry = &content->entries[i];
    if ((content->type_filter &&
         content->type_filter != (uint32_t)entry->kind + 1) ||
        (content->scope_filter && content->scope_filter != entry->scope + 1) ||
        !content_matches(entry->name, content->query, content->query_length)) {
      continue;
    }
    // Sorted insertion runs only when inventory/search/filter changes; bounded
    // card generation below never sorts or scans files in the frame hot path.
    uint32_t cursor = content->filtered_count++;
    while (cursor > 0) {
      int32_t order = strcmp(
          content->entries[content->filtered[cursor - 1]].name, entry->name);
      if (content->reverse_sort ? order >= 0 : order <= 0) {
        break;
      }
      content->filtered[cursor] = content->filtered[cursor - 1];
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

static void content_action(VkrEditorContent *content,
                           VkrEditorContentActionKind kind) {
  if (content->read_only) {
    return;
  }
  content->action = (VkrEditorContentAction){.kind = kind};
  if (content->selected < content->count) {
    ContentAsset *asset = &content->entries[content->selected];
    content_copy(content->action.asset_id, sizeof(content->action.asset_id),
                 asset->id);
    content_copy(content->action.source, sizeof(content->action.source),
                 asset->source);
  }
}

static const Vec4 content_type_colors[CONTENT_KIND_COUNT] = {
    {0.72f, 0.52f, 0.96f, 1}, {0.45f, 0.84f, 0.56f, 1},
    {0.42f, 0.76f, 0.95f, 1}, {0.96f, 0.64f, 0.40f, 1},
    {0.96f, 0.78f, 0.42f, 1}, {0.50f, 0.68f, 0.98f, 1},
    {0.42f, 0.84f, 0.86f, 1}, {0.66f, 0.70f, 0.76f, 1},
};

static void content_navigate(VkrEditorContent *content, uint32_t scope,
                             uint32_t kind) {
  if (content->scope_filter == scope && content->type_filter == kind) {
    return;
  }
  if (!content->history_count) {
    content->history[0] =
        (ContentLocation){content->scope_filter, content->type_filter};
    content->history_count = 1;
    content->history_index = 0;
  }
  content->history_count = content->history_index + 1;
  if (content->history_count == ArrayCount(content->history)) {
    MemCopy(content->history, content->history + 1,
            (ArrayCount(content->history) - 1) * sizeof(content->history[0]));
    --content->history_count;
  }
  content->history[content->history_count++] = (ContentLocation){scope, kind};
  content->history_index = content->history_count - 1;
  content->scope_filter = scope;
  content->type_filter = kind;
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
  ContentLocation location = content->history[content->history_index];
  content->scope_filter = location.scope;
  content->type_filter = location.kind;
  content->first_row = 0;
  content->filter_dirty = true_v;
}

static bool8_t content_pointer_inside(VkrUiSystem *ui, VkrUiRect rect) {
  return !ui->mouse_captured && ui->input_layer == ui->mouse_input_layer &&
         ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
         ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height;
}

static void content_scrollbar(VkrUiSystem *ui, String8 id, uint32_t column,
                              VkrUiRect area, uint32_t total, uint32_t visible,
                              uint32_t *first, ContentScrollbar *state) {
  const uint32_t maximum = total > visible ? total - visible : 0;
  *first = Min(*first, maximum);
  const float32_t scale = ui->content_scale;
  const float32_t height = area.height / scale;
  const float32_t thumb_height =
      maximum ? Min(height, Max(24.0f, height * visible / total)) : height;
  const float32_t travel = Max(0.0f, height - thumb_height);
  const float32_t thumb_top = maximum ? travel * *first / maximum : 0;
  VkrUiRect track = {area.x + area.width - 14 * scale, area.y, 14 * scale,
                     area.height};
  const bool8_t eligible =
      !ui->mouse_captured && ui->input_layer == ui->mouse_input_layer;
  if (!eligible || !maximum) {
    state->dragging = false_v;
  }
  if (maximum && content_pointer_inside(ui, track) && ui->mouse_pressed) {
    const float32_t pointer = (ui->mouse_y - area.y) / scale;
    state->grab_offset =
        pointer >= thumb_top && pointer < thumb_top + thumb_height
            ? pointer - thumb_top
            : thumb_height * .5f;
    state->dragging = true_v;
  }
  if (state->dragging) {
    const float32_t fraction =
        travel > 0 ? vkr_clamp_f32(
                         ((ui->mouse_y - area.y) / scale - state->grab_offset) /
                             travel,
                         0, 1)
                   : 0;
    *first = (uint32_t)roundf(fraction * maximum);
    ui->capture.mouse = true_v;
    if (ui->mouse_released || !input_is_button_down(ui->input, BUTTON_LEFT)) {
      state->dragging = false_v;
    }
  }
  if (!maximum) {
    return;
  }
  VkrUiWidgetConfig background = content_widget(column, 0);
  background.placement.justify = VKR_UI_ALIGN_END;
  background.placement.align = VKR_UI_ALIGN_START;
  background.style.min_size_pt = (Vec2){14, height};
  background.style.max_size_pt = background.style.min_size_pt;
  background.style.padding_pt = (VkrUiEdges){0};
  background.style.background_color = (Vec4){0};
  (void)vkr_ui_push_id_label(ui, id);
  vkr_ui_label(ui, string8_lit("track"), (String8){0}, &background);
  VkrUiWidgetConfig thumb = background;
  thumb.placement.margin_pt.top = travel * *first / maximum;
  thumb.placement.margin_pt.right = 3;
  thumb.style.min_size_pt = (Vec2){6, thumb_height};
  thumb.style.max_size_pt = thumb.style.min_size_pt;
  thumb.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
  thumb.style.background_color =
      state->dragging ? vkr_ui_theme()->accent : vkr_ui_theme()->border_strong;
  vkr_ui_label(ui, string8_lit("thumb"), (String8){0}, &thumb);
  (void)vkr_ui_pop_id(ui);
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
         content->filtered[position] != content->selected) {
    ++position;
  }
  position = position == content->filtered_count
                 ? 0
                 : content_navigation_target(position, content->filtered_count,
                                             columns, visible_rows, key);
  content->selected = content->filtered[position];
  const uint32_t row = position / columns;
  if (row < content->first_row) {
    content->first_row = row;
  } else if (row >= content->first_row + visible_rows) {
    content->first_row = row - visible_rows + 1;
  }
  ui->capture.keyboard = true_v;
}

static void content_sources(VkrEditorContent *content, VkrUiSystem *ui,
                            VkrUiRect area) {
  const uint32_t shown_scope = content->scope_filter;
  VkrUiTrack rows[14];
  for (uint32_t i = 0; i < ArrayCount(rows); ++i) {
    rows[i] = (VkrUiTrack){26, VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.rows = rows;
  panel.row_count = 5 + (shown_scope ? CONTENT_KIND_COUNT : 0);
  const VkrUiTheme *theme = vkr_ui_theme();
  panel.style.background_color = theme->header;
  panel.style.padding_pt = (VkrUiEdges){6, 18, 6, 6};
  panel.clip_children = true_v;
  const uint32_t visible =
      Max(1u, (uint32_t)(Max(0.0f, area.height / ui->content_scale - 8) / 26));
  if (content_pointer_inside(ui, area) && ui->mouse_wheel) {
    content->source_first_row = (uint32_t)Max(
        (int64_t)0, (int64_t)content->source_first_row - ui->mouse_wheel);
    ui->capture.mouse = true_v;
  }
  const uint32_t source_max =
      panel.row_count > visible ? panel.row_count - visible : 0;
  content->source_first_row = Min(content->source_first_row, source_max);
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("sources"), &panel)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset_set(ui, content->source_first_row * 26.0f);
  VkrUiWidgetConfig heading = content_widget(0, 0);
  heading.style.text_color = theme->text_secondary;
  heading.style.font_size_pt = theme->font_caption;
  vkr_ui_label(ui, string8_lit("heading"), string8_lit("Sources"), &heading);
  VkrUiWidgetConfig all = content_widget(0, 1);
  all.placement.justify = VKR_UI_ALIGN_STRETCH;
  all.fill = true_v;
  all.icon = VKR_UI_ICON_CONTENT;
  all.icon_color = theme->accent_hover;
  all.style.background_color =
      !content->scope_filter ? theme->selection : (Vec4){0};
  if (vkr_ui_button(ui, string8_lit("all"), string8_lit("All assets"), &all)) {
    content_navigate(content, 0, 0);
  }
  uint32_t row = 2;
  for (uint32_t scope = 0; scope < 3; ++scope) {
    (void)vkr_ui_push_id_u64(ui, scope);
    VkrUiWidgetConfig folder = content_widget(0, row++);
    folder.placement.justify = VKR_UI_ALIGN_STRETCH;
    folder.fill = true_v;
    folder.icon = content->scope_filter == scope + 1 ? VKR_UI_ICON_REVEAL
                                                     : VKR_UI_ICON_FOLDER;
    folder.icon_color = (Vec4){0.96f, 0.78f, 0.42f, 1.0f};
    folder.style.background_color =
        content->scope_filter == scope + 1 && !content->type_filter
            ? theme->selection
            : (Vec4){0};
    if (vkr_ui_button(ui, string8_lit("scope"),
                      content_string(content_scopes[scope]), &folder)) {
      content_navigate(content, scope + 1, 0);
    }
    if (shown_scope == scope + 1) {
      for (uint32_t kind = 0; kind < CONTENT_KIND_COUNT; ++kind) {
        VkrUiWidgetConfig child = content_widget(0, row++);
        child.placement.justify = VKR_UI_ALIGN_STRETCH;
        child.fill = true_v;
        child.placement.margin_pt.left = 16;
        child.icon = content_icons[kind];
        child.icon_color = content_type_colors[kind];
        child.style.background_color =
            content->type_filter == kind + 1 ? theme->selection : (Vec4){0};
        String8 label = string8_create_formatted(
            ui->frame_allocator, "%s (%u)", content_kinds[kind],
            content->source_counts[scope][kind]);
        if (vkr_ui_button(ui, content_string(content_kinds[kind]), label,
                          &child)) {
          content_navigate(content, scope + 1, kind + 1);
        }
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_scroll_area_end(ui);
  content_scrollbar(ui, string8_lit("source-scroll"), 0, area, panel.row_count,
                    visible, &content->source_first_row,
                    &content->source_scrollbar);
}

static void content_build_navigation(VkrEditorContent *content,
                                     VkrUiSystem *ui) {
  const VkrUiTrack navigation_columns[] = {{28, VKR_UI_TRACK_PX},
                                           {28, VKR_UI_TRACK_PX},
                                           {28, VKR_UI_TRACK_PX},
                                           {1, VKR_UI_TRACK_FR}};
  VkrUiPanelConfig navigation = vkr_ui_panel_config_default();
  navigation.placement.column = 2;
  navigation.placement.row = 0;
  navigation.columns = navigation_columns;
  navigation.column_count = ArrayCount(navigation_columns);
  if (vkr_ui_panel_begin(ui, string8_lit("navigation"), &navigation)) {
    VkrUiWidgetConfig back = vkr_editor_icon_button_config(
        0, 0, VKR_UI_ICON_ARROW_LEFT,
        string8_lit("Back to previous source folder"));
    back.disabled = !content->history_count || !content->history_index;
    if (vkr_ui_button(ui, string8_lit("back"), (String8){0}, &back)) {
      content_history_step(content, -1);
    }
    VkrUiWidgetConfig forward = vkr_editor_icon_button_config(
        1, 0, VKR_UI_ICON_ARROW_RIGHT,
        string8_lit("Forward to next source folder"));
    forward.disabled = !content->history_count ||
                       content->history_index + 1 == content->history_count;
    if (vkr_ui_button(ui, string8_lit("forward"), (String8){0}, &forward)) {
      content_history_step(content, 1);
    }
    VkrUiWidgetConfig up = vkr_editor_icon_button_config(
        2, 0, VKR_UI_ICON_ARROW_UP,
        string8_lit("Up to containing source folder"));
    up.disabled = !content->scope_filter && !content->type_filter;
    if (vkr_ui_button(ui, string8_lit("up"), (String8){0}, &up)) {
      content_navigate(content,
                       content->type_filter ? content->scope_filter : 0, 0);
    }
    VkrUiWidgetConfig breadcrumb = content_widget(3, 0);
    vkr_editor_ghost_style(&breadcrumb);
    breadcrumb.style.text_color = vkr_ui_theme()->text_secondary;
    breadcrumb.icon = VKR_UI_ICON_FOLDER;
    breadcrumb.icon_color = (Vec4){0.96f, 0.78f, 0.42f, 1.0f};
    breadcrumb.tooltip = string8_lit("Go up to the containing source folder");
    String8 path = string8_create_formatted(
        ui->frame_allocator, "Content / %s%s%s",
        content->scope_filter ? content_scopes[content->scope_filter - 1]
                              : "All assets",
        content->type_filter ? " / " : "",
        content->type_filter ? content_kinds[content->type_filter - 1] : "");
    if (vkr_ui_button(ui, string8_lit("breadcrumb"), path, &breadcrumb)) {
      content_navigate(content,
                       content->type_filter ? content->scope_filter : 0, 0);
    }
    (void)vkr_ui_panel_end(ui);
  }
}

static void content_build_toolbar(VkrEditorContent *content, VkrUiSystem *ui,
                                  float32_t width, float32_t height) {
  const VkrUiTrack toolbar_rows[] = {{28, VKR_UI_TRACK_PX},
                                     {28, VKR_UI_TRACK_PX}};
  const VkrUiTrack toolbar_columns[] = {{82, VKR_UI_TRACK_PX},
                                        {32, VKR_UI_TRACK_PX},
                                        {1, VKR_UI_TRACK_FR},
                                        {100, VKR_UI_TRACK_PX},
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
  if (vkr_ui_panel_begin(ui, string8_lit("tools"), &tools)) {
    VkrUiWidgetConfig import = content_widget(0, 0);
    vkr_editor_primary_style(&import, VKR_FONT_HANDLE_INVALID);
    import.icon = VKR_UI_ICON_IMPORT;
    import.icon_size_pt = 14;
    import.disabled = content->read_only || !content->project[0];
    import.tooltip = string8_lit("Import models, textures and fonts");
    if (vkr_ui_button(ui, string8_lit("import"), string8_lit("Import"),
                      &import)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_IMPORT);
    }
    VkrUiWidgetConfig refresh = vkr_editor_icon_button_config(
        1, 0, VKR_UI_ICON_REFRESH, string8_lit("Refresh asset inventories"));
    if (vkr_ui_button(ui, string8_lit("refresh"), (String8){0}, &refresh)) {
      vkr_editor_content_refresh(content);
    }
    content_build_navigation(content, ui);
    VkrUiWidgetConfig details = content_widget(3, 0);
    vkr_editor_ghost_style(&details);
    details.icon = VKR_UI_ICON_SIDEBAR;
    details.icon_size_pt = 14;
    details.tooltip = string8_lit("Show or hide the asset details panel");
    vkr_editor_toggle_style(&details, !content->details_hidden);
    details.disabled = width < 1040 || height < 240;
    if (vkr_ui_button(ui, string8_lit("details"), string8_lit("Details"),
                      &details)) {
      content->details_hidden = !content->details_hidden;
    }
    VkrUiWidgetConfig size = vkr_editor_icon_button_config(
        4, 0, content->size == 128 ? VKR_UI_ICON_ZOOM_IN : VKR_UI_ICON_ZOOM_OUT,
        content->size == 128 ? string8_lit("Larger asset cards")
                             : string8_lit("Smaller asset cards"));
    if (vkr_ui_button(ui, string8_lit("size"), (String8){0}, &size)) {
      content->size = content->size == 128 ? 256 : 128;
    }
    VkrUiTextEditBuffer query = {.data = content->query,
                                 .length = content->query_length,
                                 .capacity = sizeof(content->query)};
    VkrUiPlacement search = VKR_UI_PLACEMENT_DEFAULT;
    search.column = 0;
    search.row = 1;
    search.column_span = 3;
    if (vkr_editor_search_field(ui, string8_lit("search"), &query, search,
                                string8_lit("Search assets"),
                                string8_lit("Search assets by name"),
                                VKR_FONT_HANDLE_INVALID)) {
      content->query_length = query.length;
      content->filter_dirty = true_v;
      content->first_row = 0;
    }
    VkrUiWidgetConfig type = content_widget(3, 1);
    vkr_editor_ghost_style(&type);
    type.style.text_color = vkr_ui_theme()->text;
    type.icon = VKR_UI_ICON_FILTER;
    type.icon_size_pt = 13;
    type.tooltip = string8_lit("Cycle the asset type filter");
    if (vkr_ui_button(
            ui, string8_lit("type"),
            content_string(content->type_filter
                               ? content_kinds[content->type_filter - 1]
                               : "All types"),
            &type)) {
      content_navigate(content, content->scope_filter,
                       (content->type_filter + 1) % (CONTENT_KIND_COUNT + 1));
    }
    VkrUiWidgetConfig sort = vkr_editor_icon_button_config(
        4, 1,
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
}

static void content_build_card(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t asset, uint32_t column, uint32_t row,
                               float32_t card_width, VkrUiId grid_id) {
  ContentAsset *entry = &content->entries[asset];
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement.column = column;
  card.placement.row = row;
  const VkrUiTrack card_rows[] = {{3, VKR_UI_TRACK_PX},
                                  {1, VKR_UI_TRACK_FR},
                                  {22, VKR_UI_TRACK_PX},
                                  {18, VKR_UI_TRACK_PX}};
  card.rows = card_rows;
  card.row_count = ArrayCount(card_rows);
  card.clip_children = true_v;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t selected = asset == content->selected;
  card.style.background_color =
      selected ? vkr_ui_color_alpha(theme->accent, 0.16f) : theme->raised;
  card.style.border_pt =
      selected ? (VkrUiEdges){2, 2, 2, 2} : (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = selected ? theme->accent : theme->border;
  card.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
  if (vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
    ContentPreview *preview = content_preview(content, ui, asset);
    VkrUiWidgetConfig strip = content_widget(0, 0);
    strip.style.background_color = content_type_colors[entry->kind];
    strip.style.padding_pt = (VkrUiEdges){0};
    strip.style.corner_radius_pt = (Vec4){5, 5, 0, 0};
    vkr_ui_label(ui, string8_lit("type-color"), (String8){0}, &strip);
    VkrUiWidgetConfig picture = content_widget(0, 1);
    picture.icon = content_icons[entry->kind];
    picture.icon_size_pt = 36;
    picture.icon_color = content_type_colors[entry->kind];
    picture.style.hover_background_color = theme->row_hover;
    picture.tooltip = content_string(entry->name);
    if (preview && preview->texture.id) {
      picture.icon = VKR_UI_ICON_NONE;
    }
    if (vkr_ui_button(ui, string8_lit("select"), (String8){0}, &picture)) {
      content->selected = asset;
      ui->focused_id = grid_id;
      ui->focused_is_text = false_v;
    }
    if (preview && preview->texture.id) {
      VkrUiWidgetConfig image = content_widget(0, 1);
      image.style.max_size_pt = (Vec2){card_width - 12, card_width - 12};
      vkr_ui_image(
          ui, string8_lit("preview"),
          (VkrUiTextureRef){preview->texture.id, preview->texture.generation},
          (Vec2){(float32_t)preview->width, (float32_t)preview->height},
          &image);
    }
    VkrUiWidgetConfig name = content_widget(0, 2);
    vkr_editor_ghost_style(&name);
    name.style.hover_background_color = VKR_UI_COLOR_NONE;
    name.style.text_color = theme->text;
    name.tooltip = content_string(entry->name);
    if (vkr_ui_button(ui, string8_lit("name"), content_string(entry->name),
                      &name)) {
      content->selected = asset;
      ui->focused_id = grid_id;
      ui->focused_is_text = false_v;
    }
    const char *status = entry->missing               ? "Missing"
                         : entry->stale               ? "Stale"
                         : preview && preview->failed ? "Preview error"
                         : preview && preview->queued ? "Building preview"
                                                      : "Current";
    VkrUiWidgetConfig info = content_widget(0, 3);
    info.style.font_size_pt = theme->font_caption;
    info.style.text_color =
        entry->missing || entry->stale || (preview && preview->failed)
            ? theme->warning
            : theme->text_secondary;
    String8 line = string8_create_formatted(ui->frame_allocator, "%s · %s",
                                            content_kinds[entry->kind], status);
    vkr_ui_label(ui, string8_lit("status"), line, &info);
    (void)vkr_ui_panel_end(ui);
  }
}

static void content_build_grid(VkrEditorContent *content, VkrUiSystem *ui,
                               uint32_t columns, uint32_t rows,
                               float32_t card_width, float32_t card_height,
                               VkrUiId grid_id) {
  VkrUiTrack column_tracks[16], row_tracks[8];
  for (uint32_t i = 0; i < columns; ++i) {
    column_tracks[i] = (VkrUiTrack){1, VKR_UI_TRACK_FR};
  }
  for (uint32_t i = 0; i < rows; ++i) {
    row_tracks[i] = (VkrUiTrack){card_height, VKR_UI_TRACK_PX};
  }
  VkrUiPanelConfig grid = vkr_ui_panel_config_default();
  grid.placement.column = 1;
  grid.placement.row = 0;
  grid.columns = column_tracks;
  grid.column_count = columns;
  grid.rows = row_tracks;
  grid.row_count = rows;
  grid.style.gap_pt = 6;
  grid.style.padding_pt = (VkrUiEdges){4, 22, 4, 6};
  grid.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("grid"), &grid)) {
    uint32_t start = content->first_row * columns;
    uint32_t end = Min(content->filtered_count, start + columns * rows);
    if (start == end) {
      VkrUiWidgetConfig empty = content_widget(0, 0);
      empty.placement.column_span = columns;
      vkr_ui_label(ui, string8_lit("empty"),
                   content_string(content->count
                                      ? "No matching assets"
                                      : "Import content to see assets here"),
                   &empty);
    }
    for (uint32_t index = start; index < end; ++index) {
      uint32_t asset = content->filtered[index];
      ContentAsset *entry = &content->entries[asset];
      if (!vkr_ui_push_id_label(ui, content_string(entry->id))) {
        continue;
      }
      content_build_card(content, ui, asset, (index - start) % columns,
                         (index - start) / columns, card_width, grid_id);
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
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
    rename_button.disabled =
        content->read_only || !content->rename_length || entry->scope != 0;
    if (vkr_ui_button(ui, string8_lit("rename-asset"), string8_lit("Rename"),
                      &rename_button)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_RENAME);
      content_copy(content->action.name, sizeof(content->action.name),
                   (const char *)content->rename);
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
    VkrUiWidgetConfig retry = content_widget(0, 0);
    retry.icon = VKR_UI_ICON_REFRESH;
    retry.disabled = content->read_only;
    if (vkr_ui_button(ui, string8_lit("retry"), string8_lit("Retry preview"),
                      &retry)) {
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
    reimport.disabled = content->read_only || !entry->source[0];
    if (vkr_ui_button(ui, string8_lit("reimport"), string8_lit("Reimport"),
                      &reimport)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_REIMPORT);
    }
    VkrUiWidgetConfig rebuild = content_widget(1, 1);
    rebuild.disabled = content->read_only;
    if (vkr_ui_button(ui, string8_lit("rebuild"), string8_lit("Rebuild"),
                      &rebuild)) {
      content_action(content, VKR_EDITOR_CONTENT_ACTION_REBUILD);
    }
    (void)vkr_ui_panel_end(ui);
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
                                       {28, VKR_UI_TRACK_PX}};
  inspector.rows = inspector_rows;
  inspector.row_count = inspector_height > 30 ? 4 : 1;
  if (show_details &&
      vkr_ui_panel_begin(ui, string8_lit("inspect"), &inspector)) {
    VkrUiWidgetConfig text = content_widget(0, 0);
    if (content->selected < content->count) {
      ContentAsset *entry = &content->entries[content->selected];
      String8 summary =
          string8_create_formatted(ui->frame_allocator, "%s", entry->name);
      text.tooltip = summary;
      vkr_ui_label(ui, string8_lit("selection"), summary, &text);
      if (inspector_height > 30) {
        VkrUiWidgetConfig provenance = content_widget(0, 1);
        const char *diagnostic = entry->diagnostic;
        for (uint32_t i = 0; i < CONTENT_CACHE_COUNT; ++i) {
          if (content->cache[i].key &&
              content->cache[i].asset == content->selected &&
              content->cache[i].diagnostic[0]) {
            diagnostic = content->cache[i].diagnostic;
          }
        }
        String8 details = content_string(diagnostic[0]      ? diagnostic
                                         : entry->source[0] ? entry->source
                                                            : entry->path);
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
      }
    } else {
      vkr_ui_label(
          ui, string8_lit("hint"),
          content_string(
              content->diagnostic[0]
                  ? content->diagnostic
                  : "Select an asset to inspect its source and build status"),
          &text);
    }
    (void)vkr_ui_panel_end(ui);
  }
}

void vkr_editor_content_build(VkrEditorContent *content, VkrUiSystem *ui,
                              VkrUiRect rect, VkrFontHandle heading) {
  if (!content || !ui || rect.width <= 0 || rect.height <= 0) {
    return;
  }
  const float32_t width = rect.width / ui->content_scale;
  const float32_t height = rect.height / ui->content_scale;
  const float32_t card_width = content->size == 256 ? 164.0f : 112.0f;
  const float32_t card_height = card_width + 43.0f;
  const bool8_t show_sources = width >= 620;
  const bool8_t show_details =
      width >= 1040 && height >= 240 && !content->details_hidden;
  const float32_t source_width = show_sources ? 170.0f : 0.0f;
  const float32_t detail_width = show_details ? 260.0f : 0.0f;
  const float32_t asset_width = Max(1.0f, width - source_width - detail_width);
  const float32_t inspector_height = show_details ? height - 86.0f : 0.0f;
  const float32_t tools_height = 64;
  const float32_t grid_height = Max(1.0f, height - tools_height - 22);
  const uint32_t columns =
      Max(1u, Min(16u, (uint32_t)(Max(0.0f, asset_width - 16) / card_width)));
  const uint32_t visible_rows = Max(
      1u, Min(8u, (uint32_t)(Max(0.0f, grid_height - 8) / (card_height + 6))));
  const uint32_t rows = Min(8u, visible_rows + 1);
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
  const VkrUiTrack body_columns[] = {{source_width, VKR_UI_TRACK_PX},
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
  if (show_sources) {
    content_sources(content, ui,
                    (VkrUiRect){rect.x,
                                rect.y + tools_height * ui->content_scale,
                                source_width * ui->content_scale,
                                grid_height * ui->content_scale});
  }
  content_filter(content);
  uint32_t total_rows = (content->filtered_count + columns - 1) / columns;
  uint32_t max_first =
      total_rows > visible_rows ? total_rows - visible_rows : 0;
  bool8_t over =
      !ui->mouse_captured && ui->input_layer == ui->mouse_input_layer &&
      ui->mouse_x >= rect.x + source_width * ui->content_scale &&
      ui->mouse_x < rect.x + (width - detail_width) * ui->content_scale &&
      ui->mouse_y >= rect.y + tools_height * ui->content_scale &&
      ui->mouse_y < rect.y + (tools_height + grid_height) * ui->content_scale;
  if (over && ui->mouse_wheel) {
    int64_t next = (int64_t)content->first_row - ui->mouse_wheel;
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
  content_build_grid(content, ui, columns, rows, card_width, card_height,
                     grid_id);
  content_scrollbar(ui, string8_lit("grid-scroll"), 1,
                    (VkrUiRect){rect.x + source_width * ui->content_scale,
                                rect.y + tools_height * ui->content_scale,
                                asset_width * ui->content_scale,
                                grid_height * ui->content_scale},
                    total_rows, visible_rows, &content->first_row,
                    &content->grid_scrollbar);
  content_build_inspector(content, ui, inspector_height, show_details);
  (void)vkr_ui_panel_end(ui);
  VkrUiWidgetConfig footer = content_widget(0, 2);
  footer.style.font_size_pt = vkr_ui_theme()->font_caption;
  footer.style.text_color = vkr_ui_theme()->text_secondary;
  String8 count = string8_create_formatted(
      ui->frame_allocator, "%u assets%s%s", content->filtered_count,
      content->selected < content->count ? "  \xc2\xb7  Selected: " : "",
      content->selected < content->count
          ? content->entries[content->selected].name
          : "");
  vkr_ui_label(ui, string8_lit("asset-count"), count, &footer);
  (void)vkr_ui_panel_end(ui);
  content_start_preview(content);
}

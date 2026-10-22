#include "editor_material.h"

#include "editor_agent.h"
#include "editor_graph.h"
#include "editor_internal.h"
#include "editor_scripts.h"

#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_asset_path.h"
#include "filesystem/vkr_vfs.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_edit.h"

#define MATERIAL_TAG VKR_ALLOCATOR_MEMORY_TAG_STRING
/* Directories the index walks below assets/ at most. */
#define MATERIAL_INDEX_DEPTH 12u

/* One write: the document's text before and after. A NULL text means the
   file did not exist. */
typedef struct MaterialStep {
  uint64_t sequence;
  uint64_t group;
  char path[VKR_EDITOR_MATERIAL_PATH];
  char author[64];
  char *before;
  uint64_t before_length;
  char *after;
  uint64_t after_length;
} MaterialStep;

typedef struct MaterialIndexEntry {
  char path[VKR_EDITOR_MATERIAL_PATH];
  char graph[VKR_EDITOR_MATERIAL_PATH];
} MaterialIndexEntry;

/* The document the Material panel shows: a graph, an instance with its
   graph, or a plain definition shown as the graph it would become. */
typedef struct MaterialPanel {
  char path[VKR_EDITOR_MATERIAL_PATH];
  VkrEditorMaterialKind kind;
  /* The graph shown and its file; an instance's or a definition's graph is
     read-only on the canvas. */
  VkrMaterialGraph *graph;
  char graph_path[VKR_EDITOR_MATERIAL_PATH];
  bool8_t read_only;
  VkrMaterialInstance *instance;
  /* A shown surface theme. */
  VkrSurfaceTheme *theme;
  bool8_t lowered;
  VkrMaterialLowering lowering;
  char problem[VKR_MATERIAL_GRAPH_ERROR_CAPACITY];
  /* The last change that did not save (a graph that does not validate, an
     undo that did not write), shown over the status until the next change
     saves or another document opens. The shown document stays editable. */
  char notice[VKR_MATERIAL_GRAPH_ERROR_CAPACITY + 64];
  /* A journal step rewrote the shown file; reopen before the next build. */
  bool8_t reload;
  VkrEditorGraphView view;
  bool8_t framed;
  VkrEditorDetails details;
  /* The canvas rect (window points) the dock panel left this build, built
     at the root by vkr_editor_material_canvas_build. */
  VkrUiRect canvas;
  bool8_t canvas_ready;
  /* A dropped material waiting for the Scene pick under it. */
  char drop[VKR_EDITOR_MATERIAL_PATH];
  /* Per-build strings the canvas borrows. */
  char details_text[VKR_MATERIAL_GRAPH_NODE_MAX][64];
  char badge_text[VKR_MATERIAL_GRAPH_NODE_MAX][40];
} MaterialPanel;

struct VkrEditorMaterials {
  VkrAllocator *allocator;
  /* Steps [0, cursor) are applied; [cursor, count) redo. */
  MaterialStep *steps;
  uint32_t count;
  uint32_t cursor;
  uint64_t bytes;
  MaterialIndexEntry *index;
  uint32_t index_count;
  bool8_t index_ready;
  MaterialPanel panel;
};

VkrEditorMaterials *vkr_editor_material_create(VkrAllocator *allocator) {
  VkrEditorMaterials *materials = vkr_allocator_alloc(
      allocator, sizeof(*materials), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!materials) {
    return NULL;
  }
  MemZero(materials, sizeof(*materials));
  materials->allocator = allocator;
  materials->steps = vkr_allocator_alloc(
      allocator, VKR_EDITOR_MATERIAL_STEP_MAX * sizeof(*materials->steps),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  materials->index = vkr_allocator_alloc(
      allocator, VKR_EDITOR_MATERIAL_INDEX_MAX * sizeof(*materials->index),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  materials->panel.graph =
      vkr_allocator_alloc(allocator, sizeof(*materials->panel.graph),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  materials->panel.instance =
      vkr_allocator_alloc(allocator, sizeof(*materials->panel.instance),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  materials->panel.theme =
      vkr_allocator_alloc(allocator, sizeof(*materials->panel.theme),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!materials->steps || !materials->index || !materials->panel.graph ||
      !materials->panel.instance || !materials->panel.theme) {
    return NULL;
  }
  vkr_editor_graph_view_init(&materials->panel.view);
  return materials;
}

static String8 material_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

static bool8_t material_ends_with(const char *text, const char *suffix) {
  const size_t length = string_length(text);
  const size_t suffix_length = string_length(suffix);
  return length >= suffix_length &&
         string_equals(text + length - suffix_length, suffix);
}

/* Rewrites `path` without "." segments and with each "name/.." removed,
   so two spellings of one file compare equal. */
static void material_normal_path(char *path) {
  char *segments[64];
  uint32_t count = 0u;
  char copy[VKR_EDITOR_MATERIAL_PATH];
  string_format(copy, sizeof(copy), "%s", path);
  /* Each non-empty run between '/' separators is a segment, as strtok
     splits; the copy is cut at the separators. */
  for (char *at = copy; *at;) {
    if (*at == '/') {
      ++at;
      continue;
    }
    char *segment = at;
    while (*at && *at != '/') {
      ++at;
    }
    if (*at) {
      *at++ = '\0';
    }
    if (string_equals(segment, ".")) {
      continue;
    }
    if (string_equals(segment, "..") && count > 0u &&
        !string_equals(segments[count - 1u], "..")) {
      count--;
      continue;
    }
    if (count < ArrayCount(segments)) {
      segments[count++] = segment;
    }
  }
  char out[VKR_EDITOR_MATERIAL_PATH] = {0};
  uint64_t length = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const int written = string_format(out + length, sizeof(out) - length,
                                      "%s%s", i ? "/" : "", segments[i]);
    if (written < 0 || (uint64_t)written >= sizeof(out) - length) {
      return;
    }
    length += (uint64_t)written;
  }
  string_format(path, VKR_EDITOR_MATERIAL_PATH, "%s", out);
}

bool8_t vkr_editor_material_read(VkrAllocator *scratch, const char *path,
                                 String8 *out) {
  FilePath file_path = vkr_asset_path_file(scratch, material_str(path));
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (!file_path.path.length ||
      file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  const FileError error = file_read_string(&file, scratch, out);
  file_close(&file);
  return error == FILE_ERROR_NONE &&
         out->length <= VKR_EDITOR_MATERIAL_TEXT_MAX;
}

VkrEditorMaterialKind vkr_editor_material_kind(VkrAllocator *scratch,
                                               const char *path) {
  if (material_ends_with(path, ".mtg")) {
    return VKR_EDITOR_MATERIAL_GRAPH;
  }
  if (material_ends_with(path, VKR_SURFACE_THEME_EXTENSION)) {
    return VKR_EDITOR_MATERIAL_THEME;
  }
  if (!material_ends_with(path, ".mt")) {
    return VKR_EDITOR_MATERIAL_NONE;
  }
  String8 text = {0};
  VkrMaterialInstance *instance = vkr_allocator_alloc(
      scratch, sizeof(*instance), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (instance && vkr_editor_material_read(scratch, path, &text) &&
      (vkr_material_instance_read(text, instance, error, sizeof(error)) ||
       error[0])) {
    return VKR_EDITOR_MATERIAL_INSTANCE;
  }
  return VKR_EDITOR_MATERIAL_DEFINITION;
}

/* Writes `text` to `path` through a temporary file and a rename, or
   removes the file. An open workspace with its own `assets/` (a shared
   project) receives the file, so the project carries it. */
static bool8_t material_store(VkrAllocator *scratch, const char *path,
                              String8 text, bool8_t remove, char *error,
                              uint32_t capacity) {
  FilePath target = vkr_asset_path_file(scratch, material_str(path));
  if (!target.path.length) {
    string_format(error, capacity, "'%s' is not a content path", path);
    return false_v;
  }
  char workspace_path[1100];
  FilePath workspace_target = {0};
  if (vkr_vfs_overlay_write_path(&target, &workspace_target, workspace_path,
                                 sizeof(workspace_path)) &&
      (!remove || file_exists(&workspace_target))) {
    target = workspace_target;
  }
  if (remove) {
    const FileError removed = file_remove(&target);
    if (removed != FILE_ERROR_NONE && removed != FILE_ERROR_NOT_FOUND) {
      string_format(error, capacity, "'%s' could not be removed", path);
      return false_v;
    }
    return true_v;
  }
  String8 directory = file_path_get_directory(scratch, target.path);
  if (directory.length && !file_ensure_directory(scratch, &directory)) {
    string_format(error, capacity, "The folder of '%s' could not be made",
                  path);
    return false_v;
  }
  char temp_text[1100];
  string_format(temp_text, sizeof(temp_text), "%.*s.tmp.%u",
                (int)target.path.length, (const char *)target.path.str,
                vkr_platform_get_process_id());
  FilePath temp = {.path = material_str(temp_text), .type = target.type};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&temp, mode, &file) != FILE_ERROR_NONE) {
    string_format(error, capacity, "'%s' could not be written", path);
    return false_v;
  }
  uint64_t written = 0u;
  const bool8_t ok =
      file_write(&file, text.length, text.str, &written) == FILE_ERROR_NONE &&
      written == text.length && file_sync(&file) == FILE_ERROR_NONE;
  file_close(&file);
  if (!ok || file_rename(&temp, &target, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temp);
    string_format(error, capacity, "'%s' could not be written", path);
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Index of material files
// =============================================================================

/* The graph `text` of the material at `path` names, made content-root
   relative and normal; empty for a plain definition. */
static void material_instance_graph(VkrAllocator *scratch, const char *path,
                                    String8 text, char *out) {
  out[0] = '\0';
  VkrMaterialInstance *instance = vkr_allocator_alloc(
      scratch, sizeof(*instance), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (!instance ||
      !vkr_material_instance_read(text, instance, error, sizeof(error))) {
    return;
  }
  const String8 resolved = vkr_asset_path_resolve(
      scratch, material_str(path), material_str(instance->graph));
  if (resolved.str && resolved.length < VKR_EDITOR_MATERIAL_PATH) {
    string_format(out, VKR_EDITOR_MATERIAL_PATH, "%s",
                  (const char *)resolved.str);
    material_normal_path(out);
  }
}

/* Records material file `path` (with `text`) in the index, or forgets it
   when `text` is NULL. */
static void material_index_set(VkrEditorMaterials *materials,
                               VkrAllocator *scratch, const char *path,
                               const String8 *text) {
  uint32_t at = 0u;
  while (at < materials->index_count &&
         !string_equals(materials->index[at].path, path)) {
    at++;
  }
  if (!text) {
    if (at < materials->index_count) {
      materials->index[at] = materials->index[--materials->index_count];
    }
    return;
  }
  if (at == materials->index_count) {
    if (materials->index_count == VKR_EDITOR_MATERIAL_INDEX_MAX) {
      return;
    }
    materials->index_count++;
  }
  MaterialIndexEntry *entry = &materials->index[at];
  string_format(entry->path, sizeof(entry->path), "%s", path);
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  material_instance_graph(scratch, path, *text, entry->graph);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

typedef struct MaterialScan {
  VkrEditorMaterials *materials;
  VkrAllocator *scratch;
  char absolute[1024];
  char relative[VKR_EDITOR_MATERIAL_PATH];
  uint32_t depth;
} MaterialScan;

static void material_scan_entry(void *context, const char *name,
                                bool8_t directory) {
  MaterialScan *scan = context;
  const size_t absolute_length = string_length(scan->absolute);
  const size_t relative_length = string_length(scan->relative);
  if (string_format(scan->absolute + absolute_length,
                    sizeof(scan->absolute) - absolute_length, "/%s",
                    name) >= (int)(sizeof(scan->absolute) - absolute_length) ||
      string_format(scan->relative + relative_length,
                    sizeof(scan->relative) - relative_length, "/%s",
                    name) >= (int)(sizeof(scan->relative) - relative_length)) {
    scan->absolute[absolute_length] = '\0';
    scan->relative[relative_length] = '\0';
    return;
  }
  if (directory && scan->depth < MATERIAL_INDEX_DEPTH) {
    scan->depth++;
    vkr_editor_directory_list(scan->absolute, material_scan_entry, scan);
    scan->depth--;
  } else if (!directory &&
             (material_ends_with(name, ".mt") ||
              material_ends_with(name, ".mtg") ||
              material_ends_with(name, VKR_SURFACE_THEME_EXTENSION))) {
    VkrAllocatorScope scope = vkr_allocator_begin_scope(scan->scratch);
    String8 text = {0};
    if (vkr_editor_material_read(scan->scratch, scan->relative, &text)) {
      material_index_set(scan->materials, scan->scratch, scan->relative, &text);
    }
    vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  }
  scan->absolute[absolute_length] = '\0';
  scan->relative[relative_length] = '\0';
}

static void material_index_build(VkrEditorMaterials *materials,
                                 VkrAllocator *scratch) {
  if (materials->index_ready) {
    return;
  }
  materials->index_ready = true_v;
  MaterialScan scan = {.materials = materials, .scratch = scratch};
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  const FilePath root = vkr_asset_path_file(scratch, material_str("assets"));
  if (root.path.length && root.path.length < sizeof(scan.absolute)) {
    string_format(scan.absolute, sizeof(scan.absolute), "%.*s",
                  (int)root.path.length, (const char *)root.path.str);
    string_format(scan.relative, sizeof(scan.relative), "assets");
    vkr_editor_directory_list(scan.absolute, material_scan_entry, &scan);
  }
  /* A shared project's workspace keeps its own materials. */
  const char *workspace = vkr_vfs_overlay_root();
  if (workspace &&
      string_format(scan.absolute, sizeof(scan.absolute), "%sassets",
                    workspace) < (int)sizeof(scan.absolute)) {
    string_format(scan.relative, sizeof(scan.relative), "assets");
    vkr_editor_directory_list(scan.absolute, material_scan_entry, &scan);
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

void vkr_editor_material_index_reset(VkrEditorMaterials *materials) {
  if (materials) {
    materials->index_ready = false_v;
    materials->index_count = 0u;
  }
}

uint32_t vkr_editor_material_index(VkrEditorMaterials *materials,
                                   VkrAllocator *scratch,
                                   const char **out_paths,
                                   const char **out_graphs, uint32_t capacity) {
  material_index_build(materials, scratch);
  const uint32_t count = Min(capacity, materials->index_count);
  for (uint32_t i = 0; i < count; ++i) {
    out_paths[i] = materials->index[i].path;
    out_graphs[i] = materials->index[i].graph;
  }
  return materials->index_count;
}

// =============================================================================
// Live materials
// =============================================================================

/* Hands material file `path`'s current definition to the live material
   that bears its name, when one is loaded. */
static void material_replace_live(const VkrSampleUiFrame *frame,
                                  const char *path) {
  if (!frame->assets) {
    return;
  }
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 text = {0};
  if (vkr_editor_material_read(scratch, path, &text)) {
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_material_loader_replace_live(&frame->assets->material_system,
                                          material_str(path), text, scratch,
                                          &error) &&
        error != VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
      log_warn("Materials: '%s' did not update the live material (%d)", path,
               (int)error);
    }
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

void vkr_editor_material_refresh(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame,
                                 const char *path) {
  /* Brushes read themes and the materials' world sizes once the frame's
     requests apply, after the live materials below changed. */
  const bool8_t look = material_ends_with(path, ".mt") ||
                       material_ends_with(path, ".mtg") ||
                       material_ends_with(path, VKR_SURFACE_THEME_EXTENSION);
  if (look && frame->looks_changed) {
    *frame->looks_changed = true_v;
  }
  if (material_ends_with(path, ".mt")) {
    material_replace_live(frame, path);
    return;
  }
  if (!material_ends_with(path, ".mtg")) {
    return;
  }
  material_index_build(materials, frame->ui->frame_allocator);
  char graph[VKR_EDITOR_MATERIAL_PATH];
  string_format(graph, sizeof(graph), "%s", path);
  material_normal_path(graph);
  for (uint32_t i = 0; i < materials->index_count; ++i) {
    if (string_equals(materials->index[i].graph, graph)) {
      material_replace_live(frame, materials->index[i].path);
    }
  }
}

// =============================================================================
// Journal
// =============================================================================

/* The panel reopens a shown file a write or an undo changed. */
static void material_panel_touched(VkrEditorMaterials *materials,
                                   const char *path) {
  MaterialPanel *panel = &materials->panel;
  if (panel->path[0] && (string_equals(panel->path, path) ||
                         string_equals(panel->graph_path, path))) {
    panel->reload = true_v;
  }
}

static void material_step_free(VkrEditorMaterials *materials,
                               MaterialStep *step) {
  if (step->before) {
    vkr_allocator_free(materials->allocator, step->before,
                       step->before_length + 1u, MATERIAL_TAG);
  }
  if (step->after) {
    vkr_allocator_free(materials->allocator, step->after,
                       step->after_length + 1u, MATERIAL_TAG);
  }
  materials->bytes -= step->before_length + step->after_length;
  MemZero(step, sizeof(*step));
}

/* Drops steps [first, first + count) and closes the gap. */
static void material_steps_remove(VkrEditorMaterials *materials, uint32_t first,
                                  uint32_t count) {
  for (uint32_t i = first; i < first + count; ++i) {
    material_step_free(materials, &materials->steps[i]);
  }
  MemCopy(&materials->steps[first], &materials->steps[first + count],
          (materials->count - first - count) * sizeof(*materials->steps));
  materials->count -= count;
  if (materials->cursor > first + count) {
    materials->cursor -= count;
  } else if (materials->cursor > first) {
    materials->cursor = first;
  }
}

void vkr_editor_material_reset(VkrEditorMaterials *materials) {
  if (!materials) {
    return;
  }
  /* The journal's paths name the old workspace's files: undoing one after
     the switch would write its text into the new workspace's file. */
  material_steps_remove(materials, 0u, materials->count);
  vkr_editor_material_index_reset(materials);
  MaterialPanel *panel = &materials->panel;
  panel->path[0] = '\0';
  panel->graph_path[0] = '\0';
  panel->kind = VKR_EDITOR_MATERIAL_NONE;
  panel->read_only = false_v;
  panel->lowered = false_v;
  panel->problem[0] = '\0';
  panel->notice[0] = '\0';
  panel->reload = false_v;
  panel->framed = false_v;
  panel->canvas_ready = false_v;
  panel->drop[0] = '\0';
  vkr_material_graph_init(panel->graph);
  MemZero(panel->instance, sizeof(*panel->instance));
  MemZero(panel->theme, sizeof(*panel->theme));
  vkr_editor_graph_view_init(&panel->view);
}

static char *material_copy(VkrEditorMaterials *materials, String8 text) {
  char *copy =
      vkr_allocator_alloc(materials->allocator, text.length + 1u, MATERIAL_TAG);
  if (copy) {
    MemCopy(copy, text.str, text.length);
    copy[text.length] = '\0';
  }
  return copy;
}

uint64_t vkr_editor_material_group(void) { return vkr_scene_edit_take_group(); }

bool8_t vkr_editor_material_write(VkrEditorMaterials *materials,
                                  const VkrSampleUiFrame *frame,
                                  const char *path, String8 text,
                                  bool8_t remove, uint64_t group,
                                  const char *author, char *error,
                                  uint32_t capacity) {
  if (string_length(path) >= VKR_EDITOR_MATERIAL_PATH ||
      text.length > VKR_EDITOR_MATERIAL_TEXT_MAX) {
    string_format(error, capacity,
                  "The document is too large or its path long");
    return false_v;
  }
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 before = {0};
  const bool8_t existed = vkr_editor_material_read(scratch, path, &before);
  if (!material_store(scratch, path, text, remove, error, capacity)) {
    vkr_allocator_end_scope(&scope, MATERIAL_TAG);
    return false_v;
  }
  /* The redo steps lose their place, then the oldest whole groups go while
     the journal is full. */
  material_steps_remove(materials, materials->cursor,
                        materials->count - materials->cursor);
  const uint64_t added = (existed ? before.length : 0u) + text.length;
  while (materials->count > 0u &&
         (materials->count == VKR_EDITOR_MATERIAL_STEP_MAX ||
          materials->bytes + added > VKR_EDITOR_MATERIAL_HISTORY_BYTES)) {
    const uint64_t oldest = materials->steps[0].group;
    uint32_t run = 0u;
    while (run < materials->count && materials->steps[run].group == oldest) {
      run++;
    }
    material_steps_remove(materials, 0u, run);
  }
  MaterialStep *step = &materials->steps[materials->count];
  MemZero(step, sizeof(*step));
  step->sequence = vkr_scene_edit_take_sequence();
  step->group = group ? group : vkr_editor_material_group();
  string_format(step->path, sizeof(step->path), "%s", path);
  string_format(step->author, sizeof(step->author), "%s", author ? author : "");
  step->before = existed ? material_copy(materials, before) : NULL;
  step->before_length = existed ? before.length : 0u;
  step->after = remove ? NULL : material_copy(materials, text);
  step->after_length = remove ? 0u : text.length;
  if ((existed && !step->before) || (!remove && !step->after)) {
    material_step_free(materials, step);
    log_warn("Materials: no memory to journal '%s'; it saved unjournaled",
             path);
  } else {
    materials->bytes += step->before_length + step->after_length;
    materials->count++;
    materials->cursor = materials->count;
  }
  if (materials->index_ready) {
    material_index_set(materials, scratch, path, remove ? NULL : &text);
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  material_panel_touched(materials, path);
  vkr_editor_material_refresh(materials, frame, path);
  return true_v;
}

uint64_t vkr_editor_material_next_sequence(const VkrEditorMaterials *materials,
                                           bool8_t redo) {
  if (!materials) {
    return 0u;
  }
  if (redo) {
    return materials->cursor < materials->count
               ? materials->steps[materials->cursor].sequence
               : 0u;
  }
  return materials->cursor ? materials->steps[materials->cursor - 1u].sequence
                           : 0u;
}

const char *vkr_editor_material_next_author(const VkrEditorMaterials *materials,
                                            bool8_t redo) {
  if (!materials) {
    return "";
  }
  if (redo) {
    return materials->cursor < materials->count
               ? materials->steps[materials->cursor].author
               : "";
  }
  return materials->cursor ? materials->steps[materials->cursor - 1u].author
                           : "";
}

/* Puts document `step` back to its text before (or after) the step. */
static bool8_t material_step_apply(VkrEditorMaterials *materials,
                                   const VkrSampleUiFrame *frame,
                                   const MaterialStep *step, bool8_t after) {
  const char *text = after ? step->after : step->before;
  const uint64_t length = after ? step->after_length : step->before_length;
  char error[160];
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  const String8 content = {.str = (uint8_t *)text, .length = length};
  const bool8_t ok = material_store(scratch, step->path, content, text == NULL,
                                    error, sizeof(error));
  if (ok && materials->index_ready) {
    material_index_set(materials, scratch, step->path, text ? &content : NULL);
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  if (!ok) {
    log_warn("Materials: %s", error);
    string_format(materials->panel.notice, sizeof(materials->panel.notice),
                  "%s did not apply: %s", after ? "Redo" : "Undo", error);
    return false_v;
  }
  material_panel_touched(materials, step->path);
  vkr_editor_material_refresh(materials, frame, step->path);
  return true_v;
}

bool8_t vkr_editor_material_undo(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame, bool8_t redo) {
  if (!materials) {
    return false_v;
  }
  if (redo) {
    if (materials->cursor == materials->count) {
      return false_v;
    }
    /* A step that does not write keeps the cursor before it, so the
       journal still matches the files; the panel shows why. */
    const uint64_t group = materials->steps[materials->cursor].group;
    while (materials->cursor < materials->count &&
           materials->steps[materials->cursor].group == group) {
      if (!material_step_apply(materials, frame,
                               &materials->steps[materials->cursor], true_v)) {
        return false_v;
      }
      materials->cursor++;
    }
    return true_v;
  }
  if (!materials->cursor) {
    return false_v;
  }
  const uint64_t group = materials->steps[materials->cursor - 1u].group;
  while (materials->cursor > 0u &&
         materials->steps[materials->cursor - 1u].group == group) {
    if (!material_step_apply(materials, frame,
                             &materials->steps[materials->cursor - 1u],
                             false_v)) {
      return false_v;
    }
    materials->cursor--;
  }
  return true_v;
}

bool8_t vkr_editor_material_group_present(const VkrEditorMaterials *materials,
                                          uint64_t group) {
  for (uint32_t i = 0; materials && i < materials->cursor; ++i) {
    if (materials->steps[i].group == group) {
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_editor_material_group_path(const VkrEditorMaterials *materials,
                                           uint64_t group) {
  for (uint32_t i = 0; materials && i < materials->count; ++i) {
    if (materials->steps[i].group == group) {
      return materials->steps[i].path;
    }
  }
  return NULL;
}

bool8_t vkr_editor_material_group_revert(VkrEditorMaterials *materials,
                                         const VkrSampleUiFrame *frame,
                                         uint64_t group, char *problem,
                                         uint32_t capacity) {
  uint32_t first = UINT32_MAX;
  for (uint32_t i = 0; i < materials->cursor; ++i) {
    if (materials->steps[i].group != group) {
      continue;
    }
    first = Min(first, i);
    for (uint32_t later = i + 1u; later < materials->cursor; ++later) {
      if (materials->steps[later].group != group &&
          string_equals(materials->steps[later].path,
                        materials->steps[i].path)) {
        string_format(problem, capacity, "A later change edited '%s'",
                      materials->steps[i].path);
        return false_v;
      }
    }
  }
  if (first == UINT32_MAX) {
    string_format(problem, capacity, "The change is no longer in the journal");
    return false_v;
  }
  /* The redo steps lose their place once history changes. */
  material_steps_remove(materials, materials->cursor,
                        materials->count - materials->cursor);
  for (uint32_t i = materials->cursor; i-- > first;) {
    if (materials->steps[i].group == group) {
      /* A step whose file did not write stays journaled. */
      if (!material_step_apply(materials, frame, &materials->steps[i],
                               false_v)) {
        string_format(problem, capacity, "'%s' could not be restored",
                      materials->steps[i].path);
        return false_v;
      }
      material_steps_remove(materials, i, 1u);
    }
  }
  return true_v;
}

// =============================================================================
// Material panel (Art workbench)
// =============================================================================

#define PANEL_INSPECTOR_PT 300.0f
#define PANEL_HEADER_PT 64.0f
/* The smallest zoom framing picks, where card text still shows. */
#define PANEL_FRAME_ZOOM_MIN 0.6f

/* The canvas's node ids: a hash of the node id text, stable while nodes
   move in the array. */
static uint64_t panel_node_key(const VkrMaterialNode *node) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (const char *c = node->id; *c; ++c) {
    hash = (hash ^ (uint8_t)*c) * UINT64_C(1099511628211);
  }
  return hash ? hash : 1u;
}

static void panel_lower(MaterialPanel *panel, VkrAllocator *scratch) {
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 definition = {0};
  const bool8_t instance = panel->kind == VKR_EDITOR_MATERIAL_INSTANCE;
  panel->lowered =
      vkr_material_graph_lower(panel->graph, material_str(panel->graph_path),
                               instance ? panel->instance->params : NULL,
                               instance ? panel->instance->param_count : 0u,
                               scratch, &definition, &panel->lowering);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

bool8_t vkr_editor_material_open(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame,
                                 const char *path) {
  MaterialPanel *panel = &materials->panel;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  const bool8_t same = string_equals(panel->path, path);
  if (!same) {
    string_format(panel->path, sizeof(panel->path), "%s", path);
    panel->notice[0] = '\0';
  }
  panel->kind = vkr_editor_material_kind(scratch, path);
  panel->problem[0] = '\0';
  panel->reload = false_v;
  panel->read_only = panel->kind != VKR_EDITOR_MATERIAL_GRAPH;
  vkr_material_graph_init(panel->graph);
  MemZero(panel->instance, sizeof(*panel->instance));
  MemZero(panel->theme, sizeof(*panel->theme));
  string_format(panel->graph_path, sizeof(panel->graph_path), "%s", path);
  String8 text = {0};
  char *problem = panel->problem;
  const uint32_t capacity = sizeof(panel->problem);
  if (!vkr_editor_material_read(scratch, path, &text)) {
    string_format(problem, capacity, "'%s' does not open", path);
  } else if (panel->kind == VKR_EDITOR_MATERIAL_GRAPH) {
    (void)vkr_material_graph_read(text, panel->graph, problem, capacity);
  } else if (panel->kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    String8 graph_text = {0};
    if (vkr_material_instance_read(text, panel->instance, problem, capacity)) {
      const String8 resolved = vkr_asset_path_resolve(
          scratch, material_str(path), material_str(panel->instance->graph));
      string_format(panel->graph_path, sizeof(panel->graph_path), "%s",
                    resolved.str ? (const char *)resolved.str : "");
      material_normal_path(panel->graph_path);
      if (!vkr_editor_material_read(scratch, panel->graph_path, &graph_text)) {
        string_format(problem, capacity, "Its graph '%s' does not open",
                      panel->graph_path);
      } else {
        (void)vkr_material_graph_read(graph_text, panel->graph, problem,
                                      capacity);
      }
    }
  } else if (panel->kind == VKR_EDITOR_MATERIAL_DEFINITION) {
    (void)vkr_material_graph_from_definition(text, panel->graph, problem,
                                             capacity);
  } else if (panel->kind == VKR_EDITOR_MATERIAL_THEME) {
    (void)vkr_surface_theme_read(text, panel->theme, problem, capacity);
  } else {
    string_format(problem, capacity, "'%s' is no material or graph", path);
  }
  if (!problem[0] && panel->kind != VKR_EDITOR_MATERIAL_THEME) {
    panel_lower(panel, scratch);
  } else {
    panel->lowered = false_v;
  }
  if (!same) {
    vkr_editor_graph_view_init(&panel->view);
    panel->framed = false_v;
  }
  vkr_editor_details_cancel(&panel->details, frame->ui);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  return !problem[0];
}

const char *vkr_editor_material_open_path(const VkrEditorMaterials *materials) {
  return materials ? materials->panel.path : "";
}

/* Shows a save that failed as a toast and the panel's notice; a save that
   wrote clears the notice. */
static void panel_saved(VkrEditorUi *editor, bool8_t saved, const char *error) {
  MaterialPanel *panel = &editor->materials->panel;
  if (saved) {
    panel->notice[0] = '\0';
    return;
  }
  const char *reason = error[0] ? error : "The document could not be written";
  string_format(panel->notice, sizeof(panel->notice), "Not saved: %s", reason);
  vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                   reason);
}

/* Writes the shown graph as one journal step by the designer. */
static void panel_save_graph(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  if (!vkr_material_graph_validate(panel->graph, error, sizeof(error))) {
    /* Reopening the file drops the edit; the notice, set after the reopen,
       says why. */
    (void)vkr_editor_material_open(materials, frame, panel->path);
    panel_saved(editor, false_v, error);
    return;
  }
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 text = {0};
  const bool8_t saved =
      vkr_material_graph_write(panel->graph, scratch, &text) &&
      vkr_editor_material_write(materials, frame, panel->graph_path, text,
                                false_v, 0u, NULL, error, sizeof(error));
  panel_saved(editor, saved, error);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

/* Writes the shown instance's overrides as one journal step. */
static void panel_save_instance(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 text = {0};
  char error[160] = {0};
  const bool8_t saved =
      vkr_material_instance_write(panel->instance, scratch, &text) &&
      vkr_editor_material_write(materials, frame, panel->path, text, false_v,
                                0u, NULL, error, sizeof(error));
  panel_saved(editor, saved, error);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

/* Makes `container` ("primary" or "world") take theme `path`, through the
   agent channel as the designer, so it is one scene edit with its undo. */
static void panel_select_theme(VkrEditorUi *editor, const char *path,
                               const char *container) {
  if (!editor->agent) {
    return;
  }
  char line[512];
  /* A slot number goes bare; "primary" and "world" are strings. */
  const bool8_t slot = container[0] >= '0' && container[0] <= '9';
  string_format(line, sizeof(line),
                "{\"v\":1,\"id\":\"theme\",\"op\":\"surface.theme.select\","
                "\"args\":{\"theme\":\"%s\",\"container\":%s%s%s,"
                "\"review\":false}}",
                path, slot ? "" : "\"", container, slot ? "" : "\"");
  (void)vkr_editor_agent_submit(editor->agent, line);
}

void vkr_editor_material_new_theme(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   const char *container) {
  VkrEditorMaterials *materials = editor->materials;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  char path[VKR_EDITOR_MATERIAL_PATH];
  String8 existing = {0};
  uint32_t n = 1u;
  do {
    string_format(path, sizeof(path), "assets/surfaces/theme_%u.surfaces", n++);
  } while (vkr_editor_material_read(scratch, path, &existing) && n < 1000u);
  const VkrSurfaceTheme empty = {0};
  String8 text = {0};
  char error[160] = {0};
  const bool8_t written =
      vkr_surface_theme_write(&empty, scratch, &text) &&
      vkr_editor_material_write(materials, frame, path, text, false_v, 0u, NULL,
                                error, sizeof(error));
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  if (!written) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     error[0] ? error : "The theme could not be written");
    return;
  }
  panel_select_theme(editor, path, container);
  (void)vkr_editor_material_open(materials, frame, path);
  vkr_editor_view_show(editor, frame, editor->workbenches.active,
                       VKR_EDITOR_VIEW_GRAPH);
}

/* Writes the shown theme as one journal step by the designer. */
static void panel_save_theme(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  String8 text = {0};
  char error[160] = {0};
  const bool8_t saved =
      vkr_surface_theme_write(panel->theme, scratch, &text) &&
      vkr_editor_material_write(materials, frame, panel->path, text, false_v,
                                0u, NULL, error, sizeof(error));
  panel_saved(editor, saved, error);
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
}

/* Turns the shown plain definition into a graph beside it, `<stem>.mtg` or
   the first free `<stem>_<n>.mtg`, and the definition into an instance of it
   with the same name: one undo step. An existing graph is never
   overwritten, and a failed instance write removes the new graph again. */
static void panel_convert(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  char path[VKR_EDITOR_MATERIAL_PATH];
  string_format(path, sizeof(path), "%s", panel->path);
  const int stem = (int)string_length(path) - (int)string_length(".mt");
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  char graph_path[VKR_EDITOR_MATERIAL_PATH];
  String8 existing = {0};
  string_format(graph_path, sizeof(graph_path), "%.*s.mtg", stem, path);
  for (uint32_t n = 1u;
       vkr_editor_material_read(scratch, graph_path, &existing) && n < 1000u;
       ++n) {
    string_format(graph_path, sizeof(graph_path), "%.*s_%u.mtg", stem, path, n);
  }
  if (vkr_editor_material_read(scratch, graph_path, &existing)) {
    vkr_allocator_end_scope(&scope, MATERIAL_TAG);
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     "No free graph name beside the material");
    return;
  }
  /* Texture paths were relative to the definition; the graph sits in the
     same folder, so they stay valid made content-root relative. */
  for (uint32_t i = 0; i < panel->graph->node_count; ++i) {
    VkrMaterialNode *node = &panel->graph->nodes[i];
    if (node->kind != VKR_MATERIAL_NODE_TEXTURE) {
      continue;
    }
    const String8 resolved = vkr_asset_path_resolve(scratch, material_str(path),
                                                    material_str(node->path));
    if (resolved.str && resolved.length < sizeof(node->path)) {
      string_format(node->path, sizeof(node->path), "%s",
                    (const char *)resolved.str);
    }
  }
  const char *slash = string_get_last_char_occurrence(graph_path, '/');
  char text[1024];
  String8 graph_text = {0};
  char error[160] = {0};
  const uint64_t group = vkr_editor_material_group();
  const int length = string_format(text, sizeof(text), "graph=./%s\n",
                                   slash ? slash + 1 : graph_path);
  bool8_t converted = false_v;
  if (!vkr_material_graph_write(panel->graph, scratch, &graph_text) ||
      !vkr_editor_material_write(materials, frame, graph_path, graph_text,
                                 false_v, group, NULL, error, sizeof(error))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     error[0] ? error : "The graph could not be written");
  } else if (!vkr_editor_material_write(
                 materials, frame, path,
                 (String8){.str = (uint8_t *)text, .length = (uint64_t)length},
                 false_v, group, NULL, error, sizeof(error))) {
    /* The material stays a plain definition: the new graph goes again. */
    char problem[160] = {0};
    if (!vkr_editor_material_group_revert(materials, frame, group, problem,
                                          sizeof(problem))) {
      log_warn("Materials: '%s' stays after a failed conversion: %s",
               graph_path, problem);
    }
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     error);
  } else {
    converted = true_v;
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  (void)vkr_editor_material_open(materials, frame,
                                 converted ? graph_path : path);
}

/* A new graph from the starting template at the first free
   assets/materials/graphs/material_<n>.mtg, opened. */
static void panel_new_graph(VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  char path[VKR_EDITOR_MATERIAL_PATH];
  String8 existing = {0};
  uint32_t n = 1u;
  do {
    string_format(path, sizeof(path), "assets/materials/graphs/material_%u.mtg",
                  n++);
  } while (vkr_editor_material_read(scratch, path, &existing) && n < 1000u);
  VkrMaterialGraph *graph = materials->panel.graph;
  vkr_material_graph_init(graph);
  const uint32_t base =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_COLOR, "base_color");
  const uint32_t roughness =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_SCALAR, "roughness");
  const uint32_t output =
      vkr_material_graph_add(graph, VKR_MATERIAL_NODE_SURFACE_OUTPUT, "output");
  graph->nodes[base].value = vec4_new(0.8f, 0.8f, 0.8f, 0.0f);
  graph->nodes[roughness].value.x = 0.5f;
  graph->nodes[roughness].position = vec2_new(0.0f, 140.0f);
  graph->nodes[output].position = vec2_new(320.0f, 0.0f);
  string_format(graph->nodes[base].parameter,
                sizeof(graph->nodes[base].parameter), "base_color");
  char error[160] = {0};
  (void)vkr_material_graph_connect(graph, base, 0u, output, 0u, error,
                                   sizeof(error));
  (void)vkr_material_graph_connect(graph, roughness, 0u, output, 3u, error,
                                   sizeof(error));
  String8 text = {0};
  if (!vkr_material_graph_write(graph, scratch, &text) ||
      !vkr_editor_material_write(materials, frame, path, text, false_v, 0u,
                                 NULL, error, sizeof(error))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     error[0] ? error : "The graph could not be written");
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  (void)vkr_editor_material_open(materials, frame, path);
}

/* A new instance of the shown graph beside it, opened. */
static void panel_new_instance(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  VkrAllocator *scratch = frame->ui->frame_allocator;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  char stem[VKR_EDITOR_MATERIAL_PATH];
  string_format(stem, sizeof(stem), "%.*s",
                (int)(string_length(panel->graph_path) - 4u),
                panel->graph_path);
  char path[VKR_EDITOR_MATERIAL_PATH];
  String8 existing = {0};
  uint32_t n = 1u;
  do {
    string_format(path, sizeof(path), "%.240s_%u.mt", stem, n++);
  } while (vkr_editor_material_read(scratch, path, &existing) && n < 1000u);
  const char *slash = string_get_last_char_occurrence(panel->graph_path, '/');
  char text[512];
  const int length = string_format(text, sizeof(text), "graph=./%s\n",
                                   slash ? slash + 1 : panel->graph_path);
  char error[160] = {0};
  if (!vkr_editor_material_write(
          materials, frame, path,
          (String8){.str = (uint8_t *)text, .length = (uint64_t)length},
          false_v, 0u, NULL, error, sizeof(error))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->error,
                     error);
  }
  vkr_allocator_end_scope(&scope, MATERIAL_TAG);
  (void)vkr_editor_material_open(materials, frame, path);
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

/* The fields of one node the inspector edits. */
typedef struct MaterialNodeEdit {
  char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  char parameter[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  float32_t value;
  Vec3 color;
  char path[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
  uint32_t color_space;
  uint32_t mask;
  Vec2 range;
  Vec4 tile;
} MaterialNodeEdit;

static const char *const s_mask_labels[] = {"Vertex colour", "Mask texture",
                                            "Slope", "Height", NULL};

static const char *const s_color_space_names[] = {"auto", "srgb", "linear",
                                                  NULL};
static const char *const s_color_space_labels[] = {"By use", "sRGB", "Linear",
                                                   NULL};

enum {
  NODE_EDIT_ID = 0,
  NODE_EDIT_PARAMETER,
  NODE_EDIT_VALUE,
  NODE_EDIT_COLOR,
  NODE_EDIT_PATH,
  NODE_EDIT_COLOR_SPACE,
  NODE_EDIT_MASK,
  NODE_EDIT_RANGE,
  NODE_EDIT_TILE,
};

static const VkrPropertyDesc s_node_edit_properties[] = {
    [NODE_EDIT_ID] = {.name = "id",
                      .label = "Id",
                      .tooltip = "The node's name in the graph: a-z, 0-9, _",
                      .offset = offsetof(MaterialNodeEdit, id),
                      .capacity = VKR_MATERIAL_GRAPH_ID_CAPACITY,
                      .kind = VKR_PROPERTY_STRING},
    [NODE_EDIT_PARAMETER] = {.name = "parameter",
                             .label = "Parameter",
                             .tooltip = "Exposed under this name, so "
                                        "instances override it; empty for "
                                        "none",
                             .offset = offsetof(MaterialNodeEdit, parameter),
                             .capacity = VKR_MATERIAL_GRAPH_ID_CAPACITY,
                             .kind = VKR_PROPERTY_STRING},
    [NODE_EDIT_VALUE] = {.name = "value",
                         .label = "Value",
                         .offset = offsetof(MaterialNodeEdit, value),
                         .kind = VKR_PROPERTY_F32,
                         .step = 0.01f},
    [NODE_EDIT_COLOR] = {.name = "color",
                         .label = "Colour",
                         .offset = offsetof(MaterialNodeEdit, color),
                         .kind = VKR_PROPERTY_COLOR},
    [NODE_EDIT_PATH] = {.name = "path",
                        .label = "File",
                        .tooltip = "Relative to the graph with ./ or ../, "
                                   "else to the content root",
                        .offset = offsetof(MaterialNodeEdit, path),
                        .capacity = VKR_MATERIAL_GRAPH_PATH_CAPACITY,
                        .kind = VKR_PROPERTY_STRING},
    [NODE_EDIT_COLOR_SPACE] = {.name = "color_space",
                               .label = "Colour space",
                               .names = s_color_space_names,
                               .labels = s_color_space_labels,
                               .offset =
                                   offsetof(MaterialNodeEdit, color_space),
                               .kind = VKR_PROPERTY_ENUM},
    [NODE_EDIT_MASK] = {.name = "mask",
                        .label = "Mask",
                        .tooltip = "What weighs the layers: the vertex "
                                   "colour's or the mask texture's RGBA "
                                   "(layers 0 to 3), or layer 1 over the "
                                   "surface by slope or world height",
                        .names = vkr_material_graph_mask_names,
                        .labels = s_mask_labels,
                        .offset = offsetof(MaterialNodeEdit, mask),
                        .kind = VKR_PROPERTY_ENUM},
    [NODE_EDIT_RANGE] = {.name = "range",
                         .label = "Range",
                         .tooltip = "Slope (the world normal's Y, 0 to 1) or "
                                    "height (meters) where layer 1 fades in, "
                                    "from x to y",
                         .offset = offsetof(MaterialNodeEdit, range),
                         .kind = VKR_PROPERTY_VEC2,
                         .step = 0.01f},
    [NODE_EDIT_TILE] = {.name = "tile",
                        .label = "Tile, offset",
                        .tooltip = "UVs times x and y, plus z and w",
                        .offset = offsetof(MaterialNodeEdit, tile),
                        .kind = VKR_PROPERTY_VEC4,
                        .step = 0.01f},
};

/* Only the fields a node kind has show; `context` is the kind. */
static VkrPropertyState node_edit_state(const void *value, uint32_t property,
                                        const void *context) {
  const VkrMaterialNodeKind kind = *(const VkrMaterialNodeKind *)context;
  const bool8_t constant =
      kind == VKR_MATERIAL_NODE_SCALAR || kind == VKR_MATERIAL_NODE_COLOR;
  bool8_t shown = true_v;
  const MaterialNodeEdit *edit = value;
  switch (property) {
  case NODE_EDIT_PARAMETER:
    shown = constant || kind == VKR_MATERIAL_NODE_TEXTURE ||
            kind == VKR_MATERIAL_NODE_LAYER;
    break;
  case NODE_EDIT_VALUE:
    shown = kind == VKR_MATERIAL_NODE_SCALAR ||
            kind == VKR_MATERIAL_NODE_WORLD_PLANAR;
    break;
  case NODE_EDIT_TILE:
    shown = kind == VKR_MATERIAL_NODE_TILE_OFFSET;
    break;
  case NODE_EDIT_COLOR:
    shown = kind == VKR_MATERIAL_NODE_COLOR;
    break;
  case NODE_EDIT_PATH:
    shown = kind == VKR_MATERIAL_NODE_TEXTURE ||
            kind == VKR_MATERIAL_NODE_LAYER ||
            (kind == VKR_MATERIAL_NODE_LAYER_BLEND &&
             edit->mask == VKR_MATERIAL_GRAPH_MASK_TEXTURE);
    break;
  case NODE_EDIT_COLOR_SPACE:
    shown = kind == VKR_MATERIAL_NODE_TEXTURE;
    break;
  case NODE_EDIT_MASK:
    shown = kind == VKR_MATERIAL_NODE_LAYER_BLEND;
    break;
  case NODE_EDIT_RANGE:
    shown = kind == VKR_MATERIAL_NODE_LAYER_BLEND &&
            (edit->mask == VKR_MATERIAL_GRAPH_MASK_SLOPE ||
             edit->mask == VKR_MATERIAL_GRAPH_MASK_HEIGHT);
    break;
  default:
    break;
  }
  return (VkrPropertyState){.flags = shown ? 0u : VKR_PROPERTY_STATE_HIDDEN};
}

static const VkrTypeDesc s_node_edit_type = {
    .name = "material_node",
    .label = "Node",
    .properties = s_node_edit_properties,
    .property_count = ArrayCount(s_node_edit_properties),
    .size = sizeof(MaterialNodeEdit),
    .align = _Alignof(MaterialNodeEdit),
    .state = node_edit_state,
};

/* The graph settings the inspector edits without a selection. */
typedef struct MaterialSettingsEdit {
  uint32_t alpha_mode;
  float32_t alpha_cutoff;
  bool8_t double_sided;
  uint32_t subsurface_profile;
} MaterialSettingsEdit;

static const char *const s_alpha_mode_names[] = {"infer", "opaque", "mask",
                                                 "blend", NULL};
static const char *const s_alpha_mode_labels[] = {"From base colour", "Opaque",
                                                  "Mask", "Blend", NULL};

static const VkrPropertyDesc s_settings_edit_properties[] = {
    {.name = "alpha_mode",
     .label = "Alpha",
     .names = s_alpha_mode_names,
     .labels = s_alpha_mode_labels,
     .offset = offsetof(MaterialSettingsEdit, alpha_mode),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "alpha_cutoff",
     .label = "Alpha cutoff",
     .tooltip = "Below zero keeps the loader's default",
     .offset = offsetof(MaterialSettingsEdit, alpha_cutoff),
     .kind = VKR_PROPERTY_F32,
     .min = -1.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "double_sided",
     .label = "Double sided",
     .offset = offsetof(MaterialSettingsEdit, double_sided),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "subsurface_profile",
     .label = "Subsurface profile",
     .offset = offsetof(MaterialSettingsEdit, subsurface_profile),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 7.0f},
};

static const VkrTypeDesc s_settings_edit_type = {
    .name = "material_settings",
    .label = "Graph",
    .properties = s_settings_edit_properties,
    .property_count = ArrayCount(s_settings_edit_properties),
    .size = sizeof(MaterialSettingsEdit),
    .align = _Alignof(MaterialSettingsEdit),
};

/* A graph's or instance's art-pass metadata (artist toolkit, part 6). */
typedef struct MaterialArtEdit {
  Vec2 world_size;
  uint32_t surface;
} MaterialArtEdit;

static const VkrPropertyDesc s_art_edit_properties[] = {
    {.name = "world_size",
     .label = "World size",
     .tooltip = "Meters one texture repeat covers on brush faces; zero is "
                "1 m. A face's UV scale counts repeats of it",
     .unit = "m",
     .offset = offsetof(MaterialArtEdit, world_size),
     .kind = VKR_PROPERTY_VEC2,
     .min = 0.0f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "surface",
     .label = "Surface",
     .tooltip = "The surface tag the material is made of",
     .names = vkr_surface_names,
     .labels = vkr_surface_labels,
     .offset = offsetof(MaterialArtEdit, surface),
     .kind = VKR_PROPERTY_ENUM},
};

static const VkrTypeDesc s_art_edit_type = {
    .name = "material_art",
    .label = "Art pass",
    .properties = s_art_edit_properties,
    .property_count = ArrayCount(s_art_edit_properties),
    .size = sizeof(MaterialArtEdit),
    .align = _Alignof(MaterialArtEdit),
};

/* One row of a shown theme: the material a tag binds. The whole theme is
   larger than a typed value may be, so each tag edits as its own row. */
typedef struct MaterialThemeRowEdit {
  char path[VKR_SURFACE_THEME_PATH_CAPACITY];
} MaterialThemeRowEdit;

/* One exposed parameter as an instance row: its effective value. */
typedef struct MaterialParamEdit {
  float32_t value;
  Vec3 color;
  char path[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
} MaterialParamEdit;

/* The node selected on the canvas, or UINT32_MAX. */
static uint32_t panel_selected(const MaterialPanel *panel) {
  for (uint32_t i = 0; i < panel->graph->node_count; ++i) {
    if (vkr_editor_graph_selected(&panel->view,
                                  panel_node_key(&panel->graph->nodes[i]))) {
      return i;
    }
  }
  return UINT32_MAX;
}

static void panel_label(VkrUiSystem *ui, String8 id, float32_t x, float32_t y,
                        float32_t width, const char *text, Vec4 color,
                        VkrFontHandle font) {
  VkrUiWidgetConfig label = vkr_editor_details_widget(x, y, width, 20.0f);
  label.style.text_color = color;
  label.text.font = font;
  vkr_ui_label(
      ui, id,
      string8_create_from_cstr((const uint8_t *)text, string_length(text)),
      &label);
}

/* An edited world size as the loader takes it: both sizes from 0.01 to
   1000 m, or zero (unset) when either is not positive. */
static Vec2 panel_world_size(Vec2 size) {
  if (!(size.x > 0.0f && size.y > 0.0f)) {
    return vec2_new(0.0f, 0.0f);
  }
  return vec2_new(vkr_clamp_f32(size.x, 0.01f, 1000.0f),
                  vkr_clamp_f32(size.y, 0.01f, 1000.0f));
}

/* Width an instance's parameter row leaves for its reset action. */
#define PANEL_RESET_PT (VKR_EDITOR_DETAILS_ACTION_PT + 4.0f)

/* A small icon button at (`x`, `y`) points, as the Details section actions
   are drawn. */
static bool8_t panel_icon_action(VkrUiSystem *ui, String8 id, float32_t x,
                                 float32_t y, VkrUiIcon icon, String8 tooltip) {
  const float32_t size = VKR_EDITOR_DETAILS_ACTION_PT;
  VkrUiWidgetConfig action = vkr_editor_details_widget(x, y, size, size);
  vkr_editor_ghost_style(&action);
  action.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  action.icon = icon;
  action.icon_size_pt = 13.0f;
  action.icon_color = vkr_ui_theme()->text_secondary;
  action.tooltip = tooltip;
  return vkr_ui_button(ui, id, (String8){0}, &action);
}

static void panel_inspector(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            float32_t x, float32_t top, float32_t width) {
  VkrEditorMaterials *materials = editor->materials;
  MaterialPanel *panel = &materials->panel;
  VkrUiSystem *ui = frame->ui;
  VkrEditorDetails *details = &panel->details;
  float32_t y = top;
  vkr_editor_details_begin(details);
  vkr_editor_details_error(details, ui, width, &y);
  const uint32_t selected = panel_selected(panel);
  VkrUiPanelConfig column = vkr_ui_panel_config_default();
  column.placement.column = 0u;
  column.placement.row = 0u;
  column.style.margin_pt = (VkrUiEdges){0.0f, 0.0f, 0.0f, x};
  column.style.min_size_pt = (Vec2){width, 10.0f};
  column.style.max_size_pt = (Vec2){width, 100000.0f};
  if (!vkr_ui_panel_begin(ui, string8_lit("material.inspector"), &column)) {
    vkr_editor_details_end(details);
    return;
  }
  if (panel->kind == VKR_EDITOR_MATERIAL_THEME) {
    panel_label(ui, string8_lit("material.theme"), VKR_EDITOR_DETAILS_PAD_PT, y,
                width, "Materials by surface tag",
                vkr_ui_theme()->text_secondary, editor->heading_font);
    y += 24.0f;
    for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
      MaterialThemeRowEdit edit = {0};
      string_format(edit.path, sizeof(edit.path), "%s",
                    panel->theme->materials[tag]);
      const VkrPropertyDesc property = {
          .name = vkr_surface_names[tag],
          .label = vkr_surface_labels[tag],
          .tooltip = "Material (.mt) this tag's faces show; empty leaves "
                     "them to the World's theme, then their greybox look",
          .offset = offsetof(MaterialThemeRowEdit, path),
          .capacity = sizeof(edit.path),
          .kind = VKR_PROPERTY_STRING,
      };
      const VkrTypeDesc type = {.name = "surface_theme_row",
                                .properties = &property,
                                .property_count = 1u,
                                .size = sizeof(MaterialThemeRowEdit),
                                .align = _Alignof(MaterialThemeRowEdit)};
      (void)vkr_ui_push_id_u64(ui, tag);
      const VkrEditorDetailsResult result = vkr_editor_details_type(
          details, ui, frame->input, width, &y, &type, &edit, NULL, false_v);
      (void)vkr_ui_pop_id(ui);
      if (result.changed && !result.gesture) {
        string_format(panel->theme->materials[tag],
                      sizeof(panel->theme->materials[tag]), "%s", edit.path);
        panel_save_theme(editor, frame);
      }
    }
  } else if (panel->kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    /* An instance edits its overrides; a row shows the override, else the
       graph's default. */
    panel_label(ui, string8_lit("material.params"), VKR_EDITOR_DETAILS_PAD_PT,
                y, width, "Parameters", vkr_ui_theme()->text_secondary,
                editor->heading_font);
    y += 24.0f;
    for (uint32_t i = 0; i < panel->graph->node_count; ++i) {
      const VkrMaterialNode *node = &panel->graph->nodes[i];
      if (!node->parameter[0]) {
        continue;
      }
      VkrMaterialInstance *instance = panel->instance;
      uint32_t at = 0u;
      while (at < instance->param_count &&
             !string_equals(instance->params[at].name, node->parameter)) {
        at++;
      }
      const char *override =
          at < instance->param_count ? instance->params[at].value : NULL;
      MaterialParamEdit edit = {
          .value = node->value.x,
          .color = vec3_new(node->value.x, node->value.y, node->value.z)};
      string_format(edit.path, sizeof(edit.path), "%s", node->path);
      if (override) {
        const String8 text = material_str(override);
        (void)string8_to_f32(&text, &edit.value);
        (void)string8_to_vec3(&text, &edit.color);
        string_format(edit.path, sizeof(edit.path), "%s", override);
      }
      VkrPropertyDesc property = {
          .name = node->parameter,
          .label = node->parameter,
          .tooltip =
              override ? "Overridden by this instance" : "The graph's default",
      };
      if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
        property.kind = VKR_PROPERTY_F32;
        property.offset = offsetof(MaterialParamEdit, value);
        property.step = 0.01f;
      } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
        property.kind = VKR_PROPERTY_COLOR;
        property.offset = offsetof(MaterialParamEdit, color);
      } else {
        property.kind = VKR_PROPERTY_STRING;
        property.offset = offsetof(MaterialParamEdit, path);
        property.capacity = sizeof(edit.path);
      }
      const VkrTypeDesc type = {.name = "material_param",
                                .properties = &property,
                                .property_count = 1u,
                                .size = sizeof(MaterialParamEdit),
                                .align = _Alignof(MaterialParamEdit)};
      /* Every row leaves room at its end for the reset action an override
         shows, so the rows' label columns align. */
      const float32_t row_y = y;
      (void)vkr_ui_push_id_u64(ui, i);
      const VkrEditorDetailsResult result = vkr_editor_details_type(
          details, ui, frame->input, width - PANEL_RESET_PT, &y, &type, &edit,
          NULL, false_v);
      const bool8_t reset =
          override &&
          panel_icon_action(
              ui, string8_lit("reset"),
              width - VKR_EDITOR_DETAILS_PAD_PT - VKR_EDITOR_DETAILS_ACTION_PT,
              row_y +
                  (VKR_EDITOR_DETAILS_ROW_PT - VKR_EDITOR_DETAILS_ACTION_PT) *
                      0.5f,
              VKR_UI_ICON_RESET,
              string8_lit("Reset to the graph's default: this instance "
                          "stops overriding it"));
      (void)vkr_ui_pop_id(ui);
      if (reset) {
        MemCopy(&instance->params[at], &instance->params[at + 1u],
                (instance->param_count - at - 1u) *
                    sizeof(instance->params[0]));
        instance->param_count--;
        panel_save_instance(editor, frame);
        continue;
      }
      if (!result.changed || result.gesture) {
        continue;
      }
      if (at == instance->param_count) {
        if (instance->param_count == VKR_MATERIAL_GRAPH_PARAM_MAX) {
          continue;
        }
        instance->param_count++;
        string_format(instance->params[at].name,
                      sizeof(instance->params[at].name), "%s", node->parameter);
      }
      char *value = instance->params[at].value;
      const size_t capacity = sizeof(instance->params[at].value);
      if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
        string_format(value, capacity, "%.9g", (double)edit.value);
      } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
        string_format(value, capacity, "%.9g,%.9g,%.9g", (double)edit.color.x,
                      (double)edit.color.y, (double)edit.color.z);
      } else {
        string_format(value, capacity, "%s", edit.path);
      }
      panel_save_instance(editor, frame);
    }
    /* Its own world size and surface, else the graph's. */
    VkrMaterialInstance *instance = panel->instance;
    MaterialArtEdit art = {.world_size = panel->graph->settings.world_size};
    VkrSurface surface = VKR_SURFACE_NONE;
    (void)vkr_surface_find(instance->surface[0]
                               ? instance->surface
                               : panel->graph->settings.surface,
                           &surface);
    art.surface = (uint32_t)surface;
    if (instance->world_size[0]) {
      const String8 text = material_str(instance->world_size);
      if (!string8_to_vec2(&text, &art.world_size) &&
          string8_to_f32(&text, &art.world_size.x)) {
        art.world_size.y = art.world_size.x;
      }
    }
    y += 8.0f;
    const VkrEditorDetailsResult art_result =
        vkr_editor_details_type(details, ui, frame->input, width, &y,
                                &s_art_edit_type, &art, NULL, false_v);
    if (art_result.changed && !art_result.gesture) {
      art.world_size = panel_world_size(art.world_size);
      if (art.world_size.x > 0.0f) {
        string_format(instance->world_size, sizeof(instance->world_size),
                      "%g,%g", (double)art.world_size.x,
                      (double)art.world_size.y);
      } else {
        instance->world_size[0] = '\0';
      }
      string_format(instance->surface, sizeof(instance->surface), "%s",
                    art.surface ? vkr_surface_names[art.surface] : "");
      panel_save_instance(editor, frame);
    }
  } else if (selected != UINT32_MAX) {
    VkrMaterialNode *node = &panel->graph->nodes[selected];
    MaterialNodeEdit edit = {
        .value = node->value.x,
        .color = vec3_new(node->value.x, node->value.y, node->value.z),
        .color_space = (uint32_t)node->color_space,
        .mask = (uint32_t)node->mask,
        .range = vec2_new(node->value.x, node->value.y),
        .tile = node->value,
    };
    string_format(edit.id, sizeof(edit.id), "%s", node->id);
    string_format(edit.parameter, sizeof(edit.parameter), "%s",
                  node->parameter);
    string_format(edit.path, sizeof(edit.path), "%s", node->path);
    panel_label(ui, string8_lit("material.node"), VKR_EDITOR_DETAILS_PAD_PT, y,
                width, vkr_material_node_desc(node->kind)->label,
                vkr_ui_theme()->text_secondary, editor->heading_font);
    y += 24.0f;
    const VkrMaterialNodeKind kind = node->kind;
    const VkrEditorDetailsResult result = vkr_editor_details_type(
        details, ui, frame->input, width, &y, &s_node_edit_type, &edit, &kind,
        panel->read_only);
    if (result.changed && !result.gesture && !panel->read_only) {
      string_format(node->id, sizeof(node->id), "%s", edit.id);
      string_format(node->parameter, sizeof(node->parameter), "%s",
                    edit.parameter);
      string_format(node->path, sizeof(node->path), "%s", edit.path);
      node->color_space = (VkrMaterialColorSpace)edit.color_space;
      node->mask = (VkrMaterialGraphMask)edit.mask;
      node->value =
          node->kind == VKR_MATERIAL_NODE_COLOR
              ? vec4_new(edit.color.x, edit.color.y, edit.color.z, 0.0f)
          : node->kind == VKR_MATERIAL_NODE_LAYER_BLEND
              ? vec4_new(edit.range.x, edit.range.y, 0.0f, 0.0f)
          : node->kind == VKR_MATERIAL_NODE_TILE_OFFSET
              ? edit.tile
              : vec4_new(edit.value, 0.0f, 0.0f, 0.0f);
      panel_save_graph(editor, frame);
    }
  } else {
    VkrMaterialGraphSettings *settings = &panel->graph->settings;
    MaterialSettingsEdit edit = {
        .alpha_mode = (uint32_t)settings->alpha_mode,
        .alpha_cutoff = settings->alpha_cutoff,
        .double_sided = settings->double_sided,
        .subsurface_profile = settings->subsurface_profile,
    };
    panel_label(ui, string8_lit("material.settings"), VKR_EDITOR_DETAILS_PAD_PT,
                y, width, "Graph settings", vkr_ui_theme()->text_secondary,
                editor->heading_font);
    y += 24.0f;
    const VkrEditorDetailsResult result = vkr_editor_details_type(
        details, ui, frame->input, width, &y, &s_settings_edit_type, &edit,
        NULL, panel->read_only);
    if (result.changed && !result.gesture && !panel->read_only) {
      settings->alpha_mode = (VkrMaterialGraphAlpha)edit.alpha_mode;
      settings->alpha_cutoff = edit.alpha_cutoff;
      settings->double_sided = edit.double_sided;
      settings->subsurface_profile = edit.subsurface_profile;
      panel_save_graph(editor, frame);
    }
    VkrSurface surface = VKR_SURFACE_NONE;
    (void)vkr_surface_find(settings->surface, &surface);
    MaterialArtEdit art = {.world_size = settings->world_size,
                           .surface = (uint32_t)surface};
    y += 8.0f;
    const VkrEditorDetailsResult art_result =
        vkr_editor_details_type(details, ui, frame->input, width, &y,
                                &s_art_edit_type, &art, NULL, panel->read_only);
    if (art_result.changed && !art_result.gesture && !panel->read_only) {
      settings->world_size = panel_world_size(art.world_size);
      string_format(settings->surface, sizeof(settings->surface), "%s",
                    art.surface ? vkr_surface_names[art.surface] : "");
      panel_save_graph(editor, frame);
    }
  }
  vkr_editor_details_end(details);
  (void)vkr_ui_panel_end(ui);
  vkr_editor_context_open_choice(editor, details);
  vkr_editor_color_picker_open(editor, details);
}

// -----------------------------------------------------------------------------
// Panel and canvas
// -----------------------------------------------------------------------------

/* The header's actions; each document kind shows some of them. */
typedef enum PanelAction {
  PANEL_ACTION_NEW_GRAPH = 0,
  PANEL_ACTION_NEW_INSTANCE,
  PANEL_ACTION_OPEN_GRAPH,
  PANEL_ACTION_CONVERT,
  PANEL_ACTION_THEME_SCENE,
  PANEL_ACTION_THEME_WORLD,
  PANEL_ACTION_FRAME,
  PANEL_ACTION_COUNT,
} PanelAction;

static const struct {
  const char *id;
  const char *label;
  VkrUiIcon icon;
} s_panel_actions[PANEL_ACTION_COUNT] = {
    [PANEL_ACTION_NEW_GRAPH] = {"material.new_graph", "New graph",
                                VKR_UI_ICON_ADD},
    [PANEL_ACTION_NEW_INSTANCE] = {"material.new_instance", "New instance",
                                   VKR_UI_ICON_DUPLICATE},
    [PANEL_ACTION_OPEN_GRAPH] = {"material.open_graph", "Open graph",
                                 VKR_UI_ICON_GRAPH},
    [PANEL_ACTION_CONVERT] = {"material.convert", "Convert to graph",
                              VKR_UI_ICON_GRAPH},
    [PANEL_ACTION_THEME_SCENE] = {"material.theme_scene", "Use in scene",
                                  VKR_UI_ICON_SCENE},
    [PANEL_ACTION_THEME_WORLD] = {"material.theme_world", "Use for World",
                                  VKR_UI_ICON_WORLD},
    [PANEL_ACTION_FRAME] = {"material.frame", "Frame", VKR_UI_ICON_ZOOM_IN},
};

/* One header cell of the title row. */
static VkrUiPlacement panel_header_cell(uint32_t column) {
  return (VkrUiPlacement){.column = column,
                          .row = 0u,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_START,
                          .align = VKR_UI_ALIGN_CENTER};
}

/* The header's title row: the document's title, then its action buttons
   sized to their labels. Returns the clicked action, or PANEL_ACTION_COUNT
   for none. */
static PanelAction panel_header(VkrEditorUi *editor, VkrUiSystem *ui,
                                float32_t width, const char *title,
                                const PanelAction *actions,
                                uint32_t action_count) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  VkrUiTrack columns[1u + PANEL_ACTION_COUNT];
  columns[0] = fill;
  for (uint32_t i = 0; i < action_count; ++i) {
    columns[1u + i] = auto_track;
  }
  const float32_t row_height = theme->control_height + 4.0f;
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = panel_header_cell(0u);
  header.placement.align = VKR_UI_ALIGN_START;
  header.columns = columns;
  header.column_count = 1u + action_count;
  header.rows = &fill;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  header.style.gap_pt = theme->space_sm;
  header.style.margin_pt = (VkrUiEdges){4.0f, 0.0f, 0.0f, 10.0f};
  header.style.min_size_pt = (Vec2){Max(40.0f, width - 20.0f), row_height};
  header.style.max_size_pt = header.style.min_size_pt;
  header.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("material.header"), &header)) {
    return PANEL_ACTION_COUNT;
  }
  VkrUiWidgetConfig label =
      vkr_editor_text_config(theme->font_body, theme->text);
  label.placement = panel_header_cell(0u);
  label.text.font = editor->heading_font;
  vkr_ui_label(
      ui, string8_lit("material.title"),
      string8_create_from_cstr((const uint8_t *)title, string_length(title)),
      &label);
  PanelAction clicked = PANEL_ACTION_COUNT;
  for (uint32_t i = 0; i < action_count; ++i) {
    const PanelAction action = actions[i];
    VkrUiWidgetConfig button = vkr_ui_widget_config_default();
    button.placement = panel_header_cell(1u + i);
    vkr_editor_action_style(&button, editor->heading_font);
    button.icon = s_panel_actions[action].icon;
    vkr_editor_button_size(&button);
    const char *id = s_panel_actions[action].id;
    const char *text = s_panel_actions[action].label;
    if (vkr_ui_button(
            ui,
            string8_create_from_cstr((const uint8_t *)id, string_length(id)),
            string8_create_from_cstr((const uint8_t *)text,
                                     string_length(text)),
            &button)) {
      clicked = action;
    }
  }
  (void)vkr_ui_panel_end(ui);
  return clicked;
}

void vkr_editor_material_panel_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect rect) {
  VkrEditorMaterials *materials = editor->materials;
  if (!materials) {
    return;
  }
  MaterialPanel *panel = &materials->panel;
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = Max(ui->content_scale, 0.001f);
  const float32_t width = rect.width / scale;
  const float32_t height = rect.height / scale;
  if (width < 160.0f || height < 120.0f) {
    return;
  }
  if (panel->reload && panel->path[0]) {
    (void)vkr_editor_material_open(materials, frame, panel->path);
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  /* Header: the document, its tier and cost, and the panel's actions. */
  char title[VKR_EDITOR_MATERIAL_PATH + 32];
  static const char *const kinds[] = {"", "Graph", "Instance", "Material",
                                      "Theme"};
  string_format(title, sizeof(title), "%s%s%s",
                panel->path[0] ? kinds[panel->kind] : "No material open",
                panel->path[0] ? "  " : "", panel->path);
  PanelAction actions[PANEL_ACTION_COUNT];
  uint32_t action_count = 0u;
  actions[action_count++] = PANEL_ACTION_NEW_GRAPH;
  if (panel->kind == VKR_EDITOR_MATERIAL_GRAPH && !panel->problem[0]) {
    actions[action_count++] = PANEL_ACTION_NEW_INSTANCE;
  } else if (panel->kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    actions[action_count++] = PANEL_ACTION_OPEN_GRAPH;
  } else if (panel->kind == VKR_EDITOR_MATERIAL_DEFINITION &&
             !panel->problem[0]) {
    actions[action_count++] = PANEL_ACTION_CONVERT;
  }
  if (panel->kind == VKR_EDITOR_MATERIAL_THEME && !panel->problem[0]) {
    /* The scene's own theme, or the World's beneath every scene. */
    actions[action_count++] = PANEL_ACTION_THEME_SCENE;
    actions[action_count++] = PANEL_ACTION_THEME_WORLD;
  } else {
    actions[action_count++] = PANEL_ACTION_FRAME;
  }
  const PanelAction clicked =
      panel_header(editor, ui, width, title, actions, action_count);
  bool8_t frame_requested = false_v;
  switch (clicked) {
  case PANEL_ACTION_NEW_GRAPH:
    panel_new_graph(editor, frame);
    break;
  case PANEL_ACTION_NEW_INSTANCE:
    panel_new_instance(editor, frame);
    break;
  case PANEL_ACTION_OPEN_GRAPH: {
    char graph[VKR_EDITOR_MATERIAL_PATH];
    string_format(graph, sizeof(graph), "%s", panel->graph_path);
    (void)vkr_editor_material_open(materials, frame, graph);
    break;
  }
  case PANEL_ACTION_CONVERT:
    panel_convert(editor, frame);
    break;
  case PANEL_ACTION_THEME_SCENE:
    panel_select_theme(editor, panel->path, "primary");
    break;
  case PANEL_ACTION_THEME_WORLD:
    panel_select_theme(editor, panel->path, "world");
    break;
  case PANEL_ACTION_FRAME:
    frame_requested = true_v;
    break;
  default:
    break;
  }
  char status[VKR_MATERIAL_GRAPH_ERROR_CAPACITY + 64];
  Vec4 status_color = theme->text_secondary;
  if (panel->problem[0]) {
    string_format(status, sizeof(status), "%s", panel->problem);
    status_color = theme->error;
  } else if (panel->notice[0]) {
    string_format(status, sizeof(status), "%s", panel->notice);
    status_color = theme->error;
  } else if (!panel->path[0]) {
    string_format(status, sizeof(status),
                  "Open a material from Content, or make a new graph");
  } else if (panel->kind == VKR_EDITOR_MATERIAL_THEME) {
    uint32_t bound = 0u;
    for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
      bound += panel->theme->materials[tag][0] ? 1u : 0u;
    }
    string_format(status, sizeof(status),
                  "Binds %u of %u surface tags \xc2\xb7 faces of a bound tag "
                  "show its material unless they have their own",
                  bound, VKR_SURFACE_COUNT - 1u);
  } else if (panel->lowered &&
             panel->lowering.tier == VKR_MATERIAL_TIER_CUSTOM) {
    string_format(status, sizeof(status),
                  "Custom \xc2\xb7 %u sample%s \xc2\xb7 %s \xc2\xb7 %s",
                  panel->lowering.samples,
                  panel->lowering.samples == 1u ? "" : "s",
                  panel->lowering.function, panel->lowering.reason);
    status_color = theme->warning;
  } else if (panel->lowered) {
    string_format(
        status, sizeof(status),
        "Standard \xc2\xb7 %u sample%s \xc2\xb7 %u layer%s \xc2\xb7 +0 "
        "pipelines%s",
        panel->lowering.samples, panel->lowering.samples == 1u ? "" : "s",
        panel->lowering.layers, panel->lowering.layers == 1u ? "" : "s",
        panel->read_only && panel->kind != VKR_EDITOR_MATERIAL_INSTANCE
            ? " \xc2\xb7 read only until converted"
            : "");
  } else {
    string_format(status, sizeof(status), "Does not lower: %s%s%s%s",
                  panel->lowering.reason,
                  panel->lowering.node[0] ? " (node " : "",
                  panel->lowering.node[0] ? panel->lowering.node : "",
                  panel->lowering.node[0] ? ")" : "");
    status_color = theme->warning;
  }
  panel_label(ui, string8_lit("material.status"), 10.0f, 34.0f, width - 20.0f,
              status, status_color, VKR_FONT_HANDLE_INVALID);

  /* The canvas fills the body left of the inspector; it builds at the root
     after the dock (vkr_editor_material_canvas_build). */
  /* A theme has no graph; its table takes the body. */
  const bool8_t theme_shown = panel->kind == VKR_EDITOR_MATERIAL_THEME;
  const float32_t inspector = theme_shown
                                  ? Min(width - 20.0f, 640.0f)
                                  : Min(PANEL_INSPECTOR_PT, width * 0.45f);
  panel->canvas = (VkrUiRect){rect.x / scale, rect.y / scale + PANEL_HEADER_PT,
                              width - inspector, height - PANEL_HEADER_PT};
  panel->canvas_ready = panel->path[0] && !panel->problem[0] && !theme_shown;
  if (panel->canvas_ready && (frame_requested || !panel->framed)) {
    panel->framed = true_v;
    panel->view.selected_count =
        frame_requested ? panel->view.selected_count : 0u;
    /* Framing needs the cards' sizes, which the next canvas build
       gives; one with no nodes frames the origin. */
    VkrEditorGraphNode nodes[VKR_MATERIAL_GRAPH_NODE_MAX];
    for (uint32_t i = 0; i < panel->graph->node_count; ++i) {
      nodes[i] = (VkrEditorGraphNode){
          .id = panel_node_key(&panel->graph->nodes[i]),
          .position = panel->graph->nodes[i].position,
          .input_count =
              vkr_material_node_desc(panel->graph->nodes[i].kind)->input_count,
          .output_count = 1u};
    }
    vkr_editor_graph_frame(&panel->view, panel->canvas, nodes,
                           panel->graph->node_count, frame_requested);
    /* Card text hides below about half size; a framed graph keeps it
       readable around the same centre and pans for the rest. */
    if (panel->view.zoom < PANEL_FRAME_ZOOM_MIN) {
      const Vec2 center = {
          panel->view.pan.x + panel->canvas.width * 0.5f / panel->view.zoom,
          panel->view.pan.y + panel->canvas.height * 0.5f / panel->view.zoom};
      panel->view.zoom = PANEL_FRAME_ZOOM_MIN;
      panel->view.pan =
          (Vec2){center.x - panel->canvas.width * 0.5f / panel->view.zoom,
                 center.y - panel->canvas.height * 0.5f / panel->view.zoom};
    }
  }
  if (panel->path[0] && !panel->problem[0]) {
    panel_inspector(editor, frame, theme_shown ? 10.0f : width - inspector,
                    PANEL_HEADER_PT, inspector);
  }
}

/* The add-node menu; the Custom groups hold the nodes only Custom graphs
   evaluate. */
static const VkrEditorGraphChoice s_choices[] = {
    {"Scalar", "Inputs"},
    {"Colour", "Inputs"},
    {"Texture", "Inputs"},
    {"Layer", "Inputs"},
    {"Multiply", "Math"},
    {"Normal map", "Math"},
    {"Layer blend", "Layers"},
    {"Surface output", "Output"},
    {"UV", "Custom inputs"},
    {"Vertex colour", "Custom inputs"},
    {"World position", "Custom inputs"},
    {"World normal", "Custom inputs"},
    {"Time", "Custom inputs"},
    {"Camera distance", "Custom inputs"},
    {"Add", "Custom math"},
    {"Subtract", "Custom math"},
    {"Divide", "Custom math"},
    {"Min", "Custom math"},
    {"Max", "Custom math"},
    {"Lerp", "Custom math"},
    {"Power", "Custom math"},
    {"One minus", "Custom math"},
    {"Saturate", "Custom math"},
    {"Abs", "Custom math"},
    {"Sine", "Custom math"},
    {"Dot", "Custom math"},
    {"Split", "Custom math"},
    {"Combine", "Custom math"},
    {"Tile and offset", "Custom coordinates"},
    {"World planar", "Custom coordinates"},
};
static const VkrMaterialNodeKind s_choice_kinds[] = {
    VKR_MATERIAL_NODE_SCALAR,
    VKR_MATERIAL_NODE_COLOR,
    VKR_MATERIAL_NODE_TEXTURE,
    VKR_MATERIAL_NODE_LAYER,
    VKR_MATERIAL_NODE_MULTIPLY,
    VKR_MATERIAL_NODE_NORMAL_MAP,
    VKR_MATERIAL_NODE_LAYER_BLEND,
    VKR_MATERIAL_NODE_SURFACE_OUTPUT,
    VKR_MATERIAL_NODE_UV,
    VKR_MATERIAL_NODE_VERTEX_COLOR,
    VKR_MATERIAL_NODE_WORLD_POSITION,
    VKR_MATERIAL_NODE_WORLD_NORMAL,
    VKR_MATERIAL_NODE_TIME,
    VKR_MATERIAL_NODE_CAMERA_DISTANCE,
    VKR_MATERIAL_NODE_ADD,
    VKR_MATERIAL_NODE_SUBTRACT,
    VKR_MATERIAL_NODE_DIVIDE,
    VKR_MATERIAL_NODE_MIN,
    VKR_MATERIAL_NODE_MAX,
    VKR_MATERIAL_NODE_LERP,
    VKR_MATERIAL_NODE_POWER,
    VKR_MATERIAL_NODE_ONE_MINUS,
    VKR_MATERIAL_NODE_SATURATE,
    VKR_MATERIAL_NODE_ABS,
    VKR_MATERIAL_NODE_SINE,
    VKR_MATERIAL_NODE_DOT,
    VKR_MATERIAL_NODE_SPLIT,
    VKR_MATERIAL_NODE_COMBINE,
    VKR_MATERIAL_NODE_TILE_OFFSET,
    VKR_MATERIAL_NODE_WORLD_PLANAR,
};
_Static_assert(ArrayCount(s_choices) == ArrayCount(s_choice_kinds),
               "one node kind per add-node choice");

/* A card's header colour by its node's category: inputs blue, textures
   green, math violet, layers teal and the output amber. */
static Vec4 panel_node_accent(VkrMaterialNodeKind kind) {
  switch (kind) {
  case VKR_MATERIAL_NODE_SCALAR:
  case VKR_MATERIAL_NODE_COLOR:
  case VKR_MATERIAL_NODE_UV:
  case VKR_MATERIAL_NODE_VERTEX_COLOR:
  case VKR_MATERIAL_NODE_WORLD_POSITION:
  case VKR_MATERIAL_NODE_WORLD_NORMAL:
  case VKR_MATERIAL_NODE_TIME:
  case VKR_MATERIAL_NODE_CAMERA_DISTANCE:
  case VKR_MATERIAL_NODE_WORLD_PLANAR:
    return (Vec4){0.36f, 0.56f, 0.86f, 1.0f};
  case VKR_MATERIAL_NODE_TEXTURE:
    return (Vec4){0.38f, 0.72f, 0.46f, 1.0f};
  case VKR_MATERIAL_NODE_LAYER:
  case VKR_MATERIAL_NODE_LAYER_BLEND:
    return (Vec4){0.30f, 0.68f, 0.70f, 1.0f};
  case VKR_MATERIAL_NODE_SURFACE_OUTPUT:
    return (Vec4){0.92f, 0.66f, 0.30f, 1.0f};
  default:
    /* Multiply, normal map, the Custom math and tile and offset. */
    return (Vec4){0.62f, 0.46f, 0.86f, 1.0f};
  }
}

/* The canvas port type of a value type; a multiply's inputs take any. */
static uint32_t panel_port_type(VkrMaterialNodeKind kind, bool8_t input,
                                VkrMaterialValueType type) {
  /* A generic node's colour inputs take scalars, UVs and colours. */
  if (input && vkr_material_node_desc(kind)->generic &&
      type == VKR_MATERIAL_VALUE_COLOR) {
    return VKR_EDITOR_GRAPH_PORT_ANY;
  }
  return (uint32_t)type;
}

/* Applies one canvas event to the shown graph; true when it changed. */
static bool8_t panel_event(VkrEditorUi *editor, MaterialPanel *panel,
                           const VkrEditorGraphEvent *event) {
  VkrMaterialGraph *graph = panel->graph;
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  switch (event->kind) {
  case VKR_EDITOR_GRAPH_EVENT_MOVE: {
    for (uint32_t i = 0; i < graph->node_count; ++i) {
      VkrMaterialNode *node = &graph->nodes[i];
      if (vkr_editor_graph_selected(&panel->view, panel_node_key(node))) {
        node->position = vec2_add(node->position, event->delta);
      }
    }
    return true_v;
  }
  case VKR_EDITOR_GRAPH_EVENT_CONNECT:
    if (!vkr_material_graph_connect(graph, event->node, event->port,
                                    event->other_node, event->other_port, error,
                                    sizeof(error))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, error);
      return false_v;
    }
    return true_v;
  case VKR_EDITOR_GRAPH_EVENT_DISCONNECT:
    if (event->node < graph->node_count &&
        event->port < VKR_MATERIAL_GRAPH_INPUT_MAX) {
      graph->nodes[event->node].inputs[event->port] = (VkrMaterialLink){0};
      return true_v;
    }
    return false_v;
  case VKR_EDITOR_GRAPH_EVENT_ADD: {
    if (event->choice >= ArrayCount(s_choice_kinds)) {
      return false_v;
    }
    const VkrMaterialNodeKind kind = s_choice_kinds[event->choice];
    const uint32_t added =
        vkr_material_graph_add(graph, kind, vkr_material_node_desc(kind)->name);
    if (added == UINT32_MAX) {
      return false_v;
    }
    graph->nodes[added].position = event->position;
    /* A dropped wire connects to the new node's first port that fits. */
    if (event->node != VKR_EDITOR_GRAPH_NONE &&
        event->node < graph->node_count) {
      const VkrMaterialNodeDesc *desc = vkr_material_node_desc(kind);
      if (event->from_output) {
        for (uint32_t port = 0; port < desc->input_count; ++port) {
          if (vkr_material_graph_connect(graph, event->node, event->port, added,
                                         port, error, sizeof(error))) {
            break;
          }
        }
      } else if (desc->output_count) {
        for (uint32_t port = 0; port < desc->output_count; ++port) {
          if (vkr_material_graph_connect(graph, added, port, event->node,
                                         event->port, error, sizeof(error))) {
            break;
          }
        }
      }
    }
    return true_v;
  }
  case VKR_EDITOR_GRAPH_EVENT_DELETE: {
    bool8_t removed = false_v;
    for (uint32_t i = graph->node_count; i-- > 0u;) {
      if (vkr_editor_graph_selected(&panel->view,
                                    panel_node_key(&graph->nodes[i]))) {
        vkr_material_graph_remove(graph, i);
        removed = true_v;
      }
    }
    panel->view.selected_count = 0u;
    return removed;
  }
  case VKR_EDITOR_GRAPH_EVENT_DUPLICATE: {
    const uint32_t count = graph->node_count;
    for (uint32_t i = 0; i < count; ++i) {
      const VkrMaterialNode source = graph->nodes[i];
      if (source.kind == VKR_MATERIAL_NODE_SURFACE_OUTPUT ||
          !vkr_editor_graph_selected(&panel->view, panel_node_key(&source))) {
        continue;
      }
      const uint32_t copy =
          vkr_material_graph_add(graph, source.kind, source.id);
      if (copy == UINT32_MAX) {
        break;
      }
      char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
      string_format(id, sizeof(id), "%s", graph->nodes[copy].id);
      graph->nodes[copy] = source;
      string_format(graph->nodes[copy].id, sizeof(graph->nodes[copy].id), "%s",
                    id);
      graph->nodes[copy].parameter[0] = '\0';
      graph->nodes[copy].position =
          vec2_add(source.position, vec2_new(40.0f, 40.0f));
    }
    return true_v;
  }
  default:
    return false_v;
  }
}

/* The read-only canvas's banner at its top-left corner: what the graph
   belongs to and where it is edited. */
static void panel_locked_banner(VkrEditorUi *editor, VkrUiSystem *ui,
                                const MaterialPanel *panel) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[VKR_EDITOR_MATERIAL_PATH + 96];
  if (panel->kind == VKR_EDITOR_MATERIAL_INSTANCE) {
    string_format(text, sizeof(text),
                  "Read-only: instance of %s; edit parameters on the right",
                  panel->graph_path);
  } else {
    string_format(text, sizeof(text),
                  "Read-only: a plain material shown as a graph; Convert to "
                  "graph to edit it");
  }
  const float32_t inset = theme->space_md;
  VkrUiWidgetConfig banner = vkr_editor_details_widget(
      panel->canvas.x + inset, panel->canvas.y + inset,
      Max(40.0f, Min(panel->canvas.width - inset * 2.0f, 520.0f)), 26.0f);
  banner.style.background_color = vkr_ui_color_alpha(theme->popup, 0.92f);
  banner.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  banner.style.padding_pt = (VkrUiEdges){4.0f, 8.0f, 4.0f, 8.0f};
  banner.style.text_color = theme->text_secondary;
  banner.style.font_size_pt = theme->font_caption;
  banner.icon = VKR_UI_ICON_LOCK;
  banner.icon_size_pt = 12.0f;
  banner.text.font = editor->heading_font;
  vkr_ui_label(
      ui, string8_lit("material.locked"),
      string8_create_from_cstr((const uint8_t *)text, string_length(text)),
      &banner);
}

void vkr_editor_material_canvas_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  if (!materials || !materials->panel.canvas_ready) {
    return;
  }
  MaterialPanel *panel = &materials->panel;
  /* The dock sets the rect again each build it shows the panel. */
  panel->canvas_ready = false_v;
  VkrMaterialGraph *graph = panel->graph;
  VkrEditorGraphNode nodes[VKR_MATERIAL_GRAPH_NODE_MAX];
  VkrEditorGraphWire wires[VKR_MATERIAL_GRAPH_NODE_MAX * 4u];
  VkrEditorGraphPort inputs[VKR_MATERIAL_NODE_KIND_COUNT]
                           [VKR_MATERIAL_GRAPH_INPUT_MAX];
  VkrEditorGraphPort outputs[VKR_MATERIAL_NODE_KIND_COUNT]
                            [VKR_MATERIAL_GRAPH_OUTPUT_MAX];
  for (uint32_t kind = 0; kind < VKR_MATERIAL_NODE_KIND_COUNT; ++kind) {
    const VkrMaterialNodeDesc *desc =
        vkr_material_node_desc((VkrMaterialNodeKind)kind);
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      inputs[kind][port] = (VkrEditorGraphPort){
          .name = desc->inputs[port].name,
          .label = desc->inputs[port].label,
          .type = panel_port_type((VkrMaterialNodeKind)kind, true_v,
                                  desc->inputs[port].type)};
    }
    for (uint32_t port = 0; port < desc->output_count; ++port) {
      outputs[kind][port] = (VkrEditorGraphPort){
          .name = desc->outputs[port].name,
          .label = desc->outputs[port].label,
          .type = panel_port_type((VkrMaterialNodeKind)kind, false_v,
                                  desc->outputs[port].type)};
    }
  }
  uint32_t wire_count = 0u;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    char *detail = panel->details_text[i];
    detail[0] = '\0';
    if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
      string_format(detail, 64, "%.3g", (double)node->value.x);
    } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
      string_format(detail, 64, "%.2f, %.2f, %.2f", (double)node->value.x,
                    (double)node->value.y, (double)node->value.z);
    } else if (node->kind == VKR_MATERIAL_NODE_TEXTURE) {
      const char *slash = string_get_last_char_occurrence(node->path, '/');
      string_format(detail, 64, "%s",
                    node->path[0] ? (slash ? slash + 1 : node->path)
                                  : "(no file)");
    }
    char *badge = panel->badge_text[i];
    badge[0] = '\0';
    const bool8_t error =
        !panel->lowered && string_equals(panel->lowering.node, node->id);
    if (error) {
      string_format(badge, 40, "not Standard");
    } else if (node->parameter[0]) {
      string_format(badge, 40, "%s", node->parameter);
    }
    nodes[i] = (VkrEditorGraphNode){
        .id = panel_node_key(node),
        .position = node->position,
        .title = node->id,
        .detail = detail[0] ? detail : NULL,
        .accent = panel_node_accent(node->kind),
        .inputs = inputs[node->kind],
        .input_count = desc->input_count,
        .outputs = outputs[node->kind],
        .output_count = desc->output_count,
        .badge = badge[0] ? badge : NULL,
        .error = error,
    };
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      const VkrMaterialLink link = node->inputs[port];
      if (link.node && wire_count < ArrayCount(wires)) {
        wires[wire_count++] = (VkrEditorGraphWire){.from_node = link.node - 1u,
                                                   .from_port = link.port,
                                                   .to_node = i,
                                                   .to_port = port};
      }
    }
  }
  VkrEditorGraphEvent events[VKR_EDITOR_GRAPH_EVENT_MAX];
  (void)vkr_ui_input_layer_set(frame->ui, 0u);
  /* A read-only graph offers no nodes to add; its other events apply to
     nothing, and a banner says where it is edited. */
  const uint32_t event_count = vkr_editor_graph_build(
      frame, &panel->view, panel->canvas, 0u, nodes, graph->node_count, wires,
      wire_count, s_choices, panel->read_only ? 0u : ArrayCount(s_choices),
      events, ArrayCount(events));
  if (panel->read_only) {
    panel_locked_banner(editor, frame->ui, panel);
    return;
  }
  bool8_t changed = false_v;
  for (uint32_t i = 0; i < event_count; ++i) {
    changed |= panel_event(editor, panel, &events[i]);
  }
  if (changed) {
    panel_save_graph(editor, frame);
  }
}

// -----------------------------------------------------------------------------
// Assignment
// -----------------------------------------------------------------------------

void vkr_editor_material_assign(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEntityId entity, const char *material) {
  if (!editor->agent || !entity.u64) {
    return;
  }
  char line[640];
  string_format(line, sizeof(line),
                "{\"v\":1,\"id\":\"assign\",\"op\":\"material.assign\","
                "\"args\":{\"entity\":\"%u:%u:%u\",\"material\":\"%s\","
                "\"review\":false}}",
                (unsigned)entity.parts.world, (unsigned)entity.parts.index,
                (unsigned)entity.parts.generation, material);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

void vkr_editor_material_assign_slot(VkrEditorUi *editor, VkrEntityId entity,
                                     uint32_t slot, const char *material) {
  if (!editor->agent || !entity.u64) {
    return;
  }
  char line[640];
  string_format(line, sizeof(line),
                "{\"v\":1,\"id\":\"assign\",\"op\":\"material.assign\","
                "\"args\":{\"entity\":\"%u:%u:%u\",\"slot\":%u,"
                "\"material\":\"%s\",\"review\":false}}",
                (unsigned)entity.parts.world, (unsigned)entity.parts.index,
                (unsigned)entity.parts.generation, slot, material);
  (void)vkr_editor_agent_submit(editor->agent, line);
}

void vkr_editor_material_drop(VkrEditorMaterials *materials,
                              const VkrSampleUiFrame *frame, const char *path,
                              Vec2 drop_px) {
  const bool8_t reference = vkr_asset_ref_is(
      string8_create_from_cstr((const uint8_t *)path, string_length(path)));
  if (!materials || !frame->pick_request ||
      (!reference && !material_ends_with(path, ".mt"))) {
    return;
  }
  string_format(materials->panel.drop, sizeof(materials->panel.drop), "%s",
                path);
  *frame->pick_request =
      (VkrSamplePickRequest){.request = true_v,
                             .position_px = drop_px,
                             .purpose = VKR_SAMPLE_PICK_MATERIAL_DROP};
}

void vkr_editor_material_finish_drop(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  VkrEditorMaterials *materials = editor->materials;
  if (!materials || !materials->panel.drop[0]) {
    return;
  }
  if (!frame->context_entity.u64) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "Drop a material on a brush or a mesh");
  } else {
    vkr_editor_material_assign(editor, frame, frame->context_entity,
                               materials->panel.drop);
  }
  materials->panel.drop[0] = '\0';
}

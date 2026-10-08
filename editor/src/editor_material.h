#pragma once

#include "assets/vkr_material_graph.h"
#include "vkr_sample_runtime.h"

/*
 * Material documents (docs/proposals/artist-toolkit.md, parts 2 and 5): the
 * material graphs (`.mtg`) and material files (`.mt`) that artists and
 * agents edit. Every change writes its file at once, through a temporary
 * file and a rename, then updates the live materials that read the file:
 * the material itself, or every instance of a changed graph. A document
 * journal keeps each change's text before and after, numbered from the
 * scene journals' counters (vkr_scene_edit_take_sequence), so one Undo
 * walks scene and document steps in the order they happened, and agents'
 * document changes are reviewed and reverted like their scene changes.
 * Paths are content-root relative, as `.mt` files name each other.
 */

#define VKR_EDITOR_MATERIAL_PATH 256u
/* Journal steps and history bytes; older steps go first. */
#define VKR_EDITOR_MATERIAL_STEP_MAX 512u
#define VKR_EDITOR_MATERIAL_HISTORY_BYTES (64ull * 1024ull * 1024ull)
/* Material files the instance index holds. */
#define VKR_EDITOR_MATERIAL_INDEX_MAX 4096u
/* Bytes a material document holds at most. */
#define VKR_EDITOR_MATERIAL_TEXT_MAX (1024u * 1024u)

typedef struct VkrEditorMaterials VkrEditorMaterials;

typedef enum VkrEditorMaterialKind {
  VKR_EDITOR_MATERIAL_NONE = 0,
  /* A `.mtg` graph. */
  VKR_EDITOR_MATERIAL_GRAPH,
  /* A `.mt` that names a graph and overrides its parameters. */
  VKR_EDITOR_MATERIAL_INSTANCE,
  /* A plain `.mt` definition. */
  VKR_EDITOR_MATERIAL_DEFINITION,
} VkrEditorMaterialKind;

VkrEditorMaterials *vkr_editor_material_create(VkrAllocator *allocator);

/* The kind of document `path` names, from its extension and content. */
VkrEditorMaterialKind vkr_editor_material_kind(VkrAllocator *scratch,
                                               const char *path);

/* Reads document `path` into `scratch`; false when it does not open. */
bool8_t vkr_editor_material_read(VkrAllocator *scratch, const char *path,
                                 String8 *out);

/* A new journal group for the writes of one change. */
uint64_t vkr_editor_material_group(void);

/* Writes `text` to document `path` (empty text with `remove` deletes it) as
   one step of journal group `group`, by `author` (NULL for the designer),
   then updates the live materials that read it. */
bool8_t vkr_editor_material_write(VkrEditorMaterials *materials,
                                  const VkrSampleUiFrame *frame,
                                  const char *path, String8 text,
                                  bool8_t remove, uint64_t group,
                                  const char *author, char *error,
                                  uint32_t capacity);

/* Sequence of the step undo (or redo) would apply next, or zero; Undo
   compares it with the scene journals'. */
uint64_t vkr_editor_material_next_sequence(const VkrEditorMaterials *materials,
                                           bool8_t redo);
/* The author of the step undo (or redo) would apply next; empty for the
   designer. */
const char *vkr_editor_material_next_author(const VkrEditorMaterials *materials,
                                            bool8_t redo);
/* Undoes (or redoes) the newest whole group. */
bool8_t vkr_editor_material_undo(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame, bool8_t redo);

/* Whether journal group `group` is applied. */
bool8_t vkr_editor_material_group_present(const VkrEditorMaterials *materials,
                                          uint64_t group);
/* Reverts applied group `group` out of order, when no later step changed
   one of its documents; drops it from the journal. False with `problem`
   otherwise. */
bool8_t vkr_editor_material_group_revert(VkrEditorMaterials *materials,
                                         const VkrSampleUiFrame *frame,
                                         uint64_t group, char *problem,
                                         uint32_t capacity);
/* The first document group `group` wrote, or NULL. */
const char *vkr_editor_material_group_path(const VkrEditorMaterials *materials,
                                           uint64_t group);

/* Updates the live materials that read document `path`. */
void vkr_editor_material_refresh(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame,
                                 const char *path);

/* Material files and graphs under the content root's assets, with the
   graph each material names (empty for a plain definition or a graph);
   scanned once with reads in
   `scratch` (a scoped allocator), then kept current by the writes above. */
uint32_t vkr_editor_material_index(VkrEditorMaterials *materials,
                                   VkrAllocator *scratch,
                                   const char **out_paths,
                                   const char **out_graphs,
                                   uint32_t capacity);

/* The Material panel (Art workbench): the open document's graph on a node
   canvas, how it lowers, and an inspector for the selected node, the graph's
   settings or an instance's parameters. */
struct VkrEditorUi;
/* Shows document `path` in the panel; false with the reason shown when it
   does not read. */
bool8_t vkr_editor_material_open(VkrEditorMaterials *materials,
                                 const VkrSampleUiFrame *frame,
                                 const char *path);
const char *vkr_editor_material_open_path(const VkrEditorMaterials *materials);
/* The panel's header and inspector in dock rect `rect` (pixels). */
void vkr_editor_material_panel_build(struct VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect rect);
/* Assigns material `path` (a `.mt`), dropped at `drop_px`, to the brush
   or mesh the Scene picks there once the pick answers
   (vkr_editor_material_finish_drop). */
void vkr_editor_material_drop(VkrEditorMaterials *materials,
                              const VkrSampleUiFrame *frame, const char *path,
                              Vec2 drop_px);
void vkr_editor_material_finish_drop(struct VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame);
/* `material.assign` of `material` to `entity`: every face of a brush, the
   face itself, or every submesh of a mesh; an empty material clears. */
void vkr_editor_material_assign(struct VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEntityId entity, const char *material);
/* The panel's node canvas, built at the root after the dock: the canvas
   places itself from the window origin. */
void vkr_editor_material_canvas_build(struct VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);

#pragma once
#include "core/vkr_json_writer.h"
#include "editor_bakery_service.h"
#include "editor_project_store.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_ui_system.h"

typedef struct VkrRenderAssets VkrRenderAssets;
typedef struct VkrEditorContent VkrEditorContent;

typedef enum VkrEditorContentActionKind {
  VKR_EDITOR_CONTENT_ACTION_NONE,
  VKR_EDITOR_CONTENT_ACTION_IMPORT,
  VKR_EDITOR_CONTENT_ACTION_REIMPORT,
  VKR_EDITOR_CONTENT_ACTION_REBUILD,
  VKR_EDITOR_CONTENT_ACTION_RENAME,
  /* Double-clicked, or dropped on the viewport (ADR-076). */
  VKR_EDITOR_CONTENT_ACTION_OPEN_SCENE,
  /* The World item: show it in a document. */
  VKR_EDITOR_CONTENT_ACTION_OPEN_WORLD,
  /* A built-in object item: add `object` (an object kind) to the scene. */
  VKR_EDITOR_CONTENT_ACTION_CREATE_OBJECT,
  /* A mesh dropped on the viewport: place an instance where it landed. */
  VKR_EDITOR_CONTENT_ACTION_PLACE_ASSET,
  /* A loaded object (`entity`): select it, frame it, rename it to `name` or
     delete it (ADR-076). */
  VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY,
  VKR_EDITOR_CONTENT_ACTION_FRAME_ENTITY,
  VKR_EDITOR_CONTENT_ACTION_RENAME_ENTITY,
  VKR_EDITOR_CONTENT_ACTION_DELETE_ENTITY,
  /* A project scene (`asset_id`): load it beside the open one, rename it to
     `name`, or delete it after confirmation. */
  VKR_EDITOR_CONTENT_ACTION_ADD_SCENE,
  VKR_EDITOR_CONTENT_ACTION_RENAME_SCENE,
  VKR_EDITOR_CONTENT_ACTION_DELETE_SCENE,
  /* Remove an unreferenced asset (`asset_id`, `scope`) from its inventory. */
  VKR_EDITOR_CONTENT_ACTION_DELETE_ASSET,
} VkrEditorContentActionKind;

/* A Content context menu's commands (ADR-076). */
typedef enum VkrEditorContentCommand {
  /* Scenes only: load the scene into the viewport. */
  VKR_EDITOR_CONTENT_COMMAND_LOAD,
  VKR_EDITOR_CONTENT_COMMAND_OPEN,
  VKR_EDITOR_CONTENT_COMMAND_PLACE,
  VKR_EDITOR_CONTENT_COMMAND_RENAME,
  VKR_EDITOR_CONTENT_COMMAND_DELETE,
  VKR_EDITOR_CONTENT_COMMAND_COUNT
} VkrEditorContentCommand;

typedef struct VkrEditorContentAction {
  VkrEditorContentActionKind kind;
  char asset_id[37];
  char source[1024];
  char name[513];
  uint32_t object;
  /* Owner of asset_id: "scene", "project" or "editor". */
  char scope[8];
  /* Pointer position of a drop, in pixels; the viewport's centre when the
     item was put into the viewport from its menu. */
  Vec2 drop_px;
  /* A created object goes where drop_px meets the scene. */
  bool8_t dropped;
  VkrEntityId entity;
  /* A REBUILD the browser queued because the asset's source changed on disk,
     not a click; it runs only while the scene has no unsaved edits. */
  bool8_t automatic;
} VkrEditorContentAction;

/** UI/render-thread owner. Freeable allocator and assets must outlive it.
 * Destruction cancels/joins the CPU process worker and unloads retained texture
 * requests through the resource system's GPU completion retirement. */
VkrEditorContent *vkr_editor_content_create(VkrAllocator *allocator,
                                            VkrRenderAssets *assets);
void vkr_editor_content_destroy(VkrEditorContent *content);
/** workspace_root is the selected .vkreditor directory. IDs may be empty.
 * Copies context, cancels old jobs, and refreshes the managed inventories. */
void vkr_editor_content_set_project(VkrEditorContent *content,
                                    const char *workspace_root,
                                    const char *project_id,
                                    const char *scene_id);
void vkr_editor_content_refresh(VkrEditorContent *content);
struct VkrSampleUiFrame;
/** Objects of the loaded containers (ADR-076): the World's at the Content
 * root and each project scene's in that scene's folder. `scene_ids[0]` names
 * the primary scene's project id and `scene_ids[i]` added scene i's; an empty
 * id lists that container's objects at the root. Call once per UI frame; only
 * a change rebuilds them. */
void vkr_editor_content_sync_objects(
    VkrEditorContent *content, const struct VkrSampleUiFrame *frame,
    const char *const scene_ids[1 + VKR_SCENE_ADDITIVE_MAX]);
/** Borrowed daemon that reports file changes under the open project; it must
 * outlive the browser. NULL disables change tracking. */
void vkr_editor_content_set_service(VkrEditorContent *content,
                                    EditorBakeryService *service);
/** Disable workspace mutations while retaining browsing and Reveal. */
void vkr_editor_content_set_read_only(VkrEditorContent *content,
                                      bool8_t read_only);
/** A background finalize (ADR-077) rebuilds the project's Content imports
 * (`project`) or the assets of scene `scene_id` (empty for none): their items
 * still at the deferred or preview tier show as cooking and are locked. */
void vkr_editor_content_set_cooking(VkrEditorContent *content, bool8_t project,
                                    const char *scene_id);
/** The World streams in the project meshes `ids`, or scene `scene_id` (empty
 * for none) streams in its meshes: their items show as loading and are
 * locked (ADR-076). */
void vkr_editor_content_set_loading(VkrEditorContent *content,
                                    const char (*ids)[37], uint32_t count,
                                    const char *scene_id);
/** Whether a listed mesh is loading, with the first one's name and how many
 * more in `label`. */
bool8_t vkr_editor_content_loading(const VkrEditorContent *content, char *label,
                                   uint32_t label_capacity);
/** Build revisions of the meshes cooking now, at most `capacity`, and in
 * `label` the first one's name and how many more; returns the count. */
uint32_t vkr_editor_content_cooking(const VkrEditorContent *content,
                                    char (*revisions)[37], uint32_t capacity,
                                    char *label, uint32_t label_capacity);
/** Poll once every UI frame even when hidden. Suspend while scene work or GPU
 * bakes own the renderer; late process results are discarded by generation. */
void vkr_editor_content_update(VkrEditorContent *content);
void vkr_editor_content_suspend_previews(VkrEditorContent *content,
                                         bool8_t suspended);
void vkr_editor_content_build(VkrEditorContent *content, VkrUiSystem *ui,
                              VkrUiRect rect, VkrFontHandle heading);
/** The dragged item under the pointer and its drop; call once per UI build
 * after every panel so it draws on top. */
void vkr_editor_content_build_drag(VkrEditorContent *content, VkrUiSystem *ui);
/** Where a dragged Content item may drop this frame, in pixels; an empty
 * rect disables dropping. The viewport sets it. */
void vkr_editor_content_set_drop_target(VkrEditorContent *content,
                                        VkrUiRect rect);
/** Replace the search query; it matches names, types, folders and tags of
 * items below the current folder. */
void vkr_editor_content_search(VkrEditorContent *content, String8 query);
/** Show an existing Content folder; the empty path is the root. */
bool8_t vkr_editor_content_open_folder(VkrEditorContent *content, String8 path);
/** Add a project folder and its ancestors. */
bool8_t vkr_editor_content_new_folder(VkrEditorContent *content, String8 path);
/** Move an item, named by id or display name, or a folder, named by path,
 * into an existing folder. Built-in items and system folders stay. */
bool8_t vkr_editor_content_move(VkrEditorContent *content, String8 item,
                                String8 folder);
/** Act as if the item, named by id or display name, were dropped on the
 * viewport at `drop_px`: a mesh is placed, others activate. */
bool8_t vkr_editor_content_place(VkrEditorContent *content, String8 item,
                                 Vec2 drop_px);
/** The current folder path; the root is empty. Borrowed until the next
 * navigation. */
const char *vkr_editor_content_folder(const VkrEditorContent *content);
/** An OS file drop landed at `drop_px` this frame. Arm before the Content
 * build so its folders can claim it, then read the target folder after. */
void vkr_editor_content_arm_file_drop(VkrEditorContent *content, Vec2 drop_px);
/** The folder an armed file drop lands in: the folder under it, else the
 * current folder; never Objects. Disarms the drop. */
void vkr_editor_content_file_drop_folder(
    VkrEditorContent *content, char out[VKR_EDITOR_FOLDER_PATH_CAPACITY]);
/** File an item, by id, into `folder`, keeping its tags. */
bool8_t vkr_editor_content_file_into(VkrEditorContent *content, const char *id,
                                     const char *folder);
/** Presets of the project (ADR-076), borrowed until the next refresh or
 * preset change. */
uint32_t vkr_editor_content_preset_count(const VkrEditorContent *content);
const VkrEditorPreset *
vkr_editor_content_preset(const VkrEditorContent *content, uint32_t index);
/** Save a valid value of a preset type as a new preset named after the type;
 * it appears in Content's Presets folder. */
bool8_t vkr_editor_content_save_preset(VkrEditorContent *content,
                                       const VkrTypeDesc *type,
                                       const void *value);
/** Show items as a list with columns, or as tiles. */
void vkr_editor_content_set_list(VkrEditorContent *content, bool8_t list);
bool8_t vkr_editor_content_take_action(VkrEditorContent *content,
                                       VkrEditorContentAction *action);
/** The pending action when it acts on a loaded object: select, frame,
 * rename or delete it. Other actions stay for their owner. */
bool8_t vkr_editor_content_take_object_action(VkrEditorContent *content,
                                              VkrEditorContentAction *action);
/** A right click on an item selected it and asks for its context menu; true
 * once, with the pointer position in points. */
bool8_t vkr_editor_content_take_menu(VkrEditorContent *content,
                                     Vec2 *position_pt);
/** The menu item's label for `command` ("Open scene", "Frame in viewport",
 * ...) and whether it applies; running it acts on the menu's item. */
const char *vkr_editor_content_menu_label(const VkrEditorContent *content,
                                          VkrEditorContentCommand command);
bool8_t vkr_editor_content_menu_available(const VkrEditorContent *content,
                                          VkrEditorContentCommand command);
void vkr_editor_content_menu_command(VkrEditorContent *content,
                                     VkrEditorContentCommand command);
/** Run a context menu command on an item named by id or name, or a folder
 * named by path or shown name; false when it does not apply. */
bool8_t vkr_editor_content_command(VkrEditorContent *content, String8 item,
                                   VkrEditorContentCommand command);
/** Presentation preferences are project-owned JSON, copied on restore. */
String8 vkr_editor_content_settings(VkrEditorContent *content,
                                    VkrAllocator *allocator);
void vkr_editor_content_restore_settings(VkrEditorContent *content,
                                         String8 settings);

bool8_t vkr_editor_content_write_settings(VkrEditorContent *content,
                                          VkrJsonWriter *writer);

/** Cancel and join child writers before releasing a workspace lease. */
bool8_t vkr_editor_content_stop_previews(VkrEditorContent *content);

#pragma once
#include "core/vkr_json_writer.h"
#include "renderer/systems/vkr_ui_system.h"

typedef struct VkrRenderAssets VkrRenderAssets;
typedef struct VkrEditorContent VkrEditorContent;

typedef enum VkrEditorContentActionKind {
  VKR_EDITOR_CONTENT_ACTION_NONE,
  VKR_EDITOR_CONTENT_ACTION_IMPORT,
  VKR_EDITOR_CONTENT_ACTION_REIMPORT,
  VKR_EDITOR_CONTENT_ACTION_REBUILD,
  VKR_EDITOR_CONTENT_ACTION_RENAME,
} VkrEditorContentActionKind;

typedef struct VkrEditorContentAction {
  VkrEditorContentActionKind kind;
  char asset_id[37];
  char source[1024];
  char name[513];
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
/** Disable workspace mutations while retaining browsing and Reveal. */
void vkr_editor_content_set_read_only(VkrEditorContent *content,
                                      bool8_t read_only);
/** Poll once every UI frame even when hidden. Suspend while scene work or GPU
 * bakes own the renderer; late process results are discarded by generation. */
void vkr_editor_content_update(VkrEditorContent *content);
void vkr_editor_content_suspend_previews(VkrEditorContent *content,
                                         bool8_t suspended);
void vkr_editor_content_build(VkrEditorContent *content, VkrUiSystem *ui,
                              VkrUiRect rect, VkrFontHandle heading);
bool8_t vkr_editor_content_take_action(VkrEditorContent *content,
                                       VkrEditorContentAction *action);
/** Presentation preferences are project-owned JSON, copied on restore. */
String8 vkr_editor_content_settings(VkrEditorContent *content,
                                    VkrAllocator *allocator);
void vkr_editor_content_restore_settings(VkrEditorContent *content,
                                         String8 settings);

bool8_t vkr_editor_content_write_settings(VkrEditorContent *content,
                                          VkrJsonWriter *writer);

/** Cancel and join child writers before releasing a workspace lease. */
bool8_t vkr_editor_content_stop_previews(VkrEditorContent *content);

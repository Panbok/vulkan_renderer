#pragma once

#include "renderer/systems/vkr_ui_system.h"
#include "vkr_sample_runtime.h"

/* The Script editor window (ADR-079): tabs of project script sources with C
 * syntax highlighting, completion from the script SDK headers and the open
 * file, compiler diagnostics in the gutter, and save-to-rebuild. UI-thread
 * owner. */

typedef struct VkrEditorUi VkrEditorUi;
typedef struct VkrEditorCode VkrEditorCode;

/** The allocator must support independent frees and outlive the editor. */
VkrEditorCode *vkr_editor_code_create(VkrAllocator *allocator);
void vkr_editor_code_destroy(VkrEditorCode *code);

/** Opens `path` in a tab, or shows its tab, and shows the Script editor. */
bool8_t vkr_editor_code_open(VkrEditorCode *code, VkrEditorUi *editor,
                             const char *path);
/** Shows the Script editor asking for a new module's name. */
void vkr_editor_code_new_script(VkrEditorCode *code, VkrEditorUi *editor);
/** Closes every saved tab, as when the project changes; tabs with unsaved
 * text stay open so nothing is lost. */
void vkr_editor_code_close_saved(VkrEditorCode *code);
/** Cmd hooks: move the active tab's caret to the start of a 1-based line's
 * text, or type ASCII text there as the keyboard would, completion
 * included. False without a tab. */
bool8_t vkr_editor_code_goto(VkrEditorCode *code, uint32_t line);
bool8_t vkr_editor_code_type(VkrEditorCode *code, const char *text);
/** Saves the active tab, which rebuilds its module; false without one. */
bool8_t vkr_editor_code_save_active(VkrEditorCode *code, VkrEditorUi *editor);
/** Some tab holds unsaved text. */
bool8_t vkr_editor_code_dirty(const VkrEditorCode *code);

/** Builds the window body inside `bounds`, in pixels. */
void vkr_editor_code_build(VkrEditorCode *code, VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame, VkrUiRect bounds);

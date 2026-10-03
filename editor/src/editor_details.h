#pragma once

/* Details: property rows generated from type descriptors (ADR-076). One
 * descriptor draws the same rows wherever a value of its type is edited:
 * scene components in the Details panel and machine preferences in the
 * Preferences window. Callers own the value and decide how a commit applies. */

#include "core/input.h"
#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_ui_system.h"

#define VKR_EDITOR_DETAILS_ROW_PT 26.0f
#define VKR_EDITOR_DETAILS_PAD_PT 10.0f
/* Section headers: a gap above, then the header bar. */
#define VKR_EDITOR_DETAILS_SECTION_GAP_PT 4.0f
#define VKR_EDITOR_DETAILS_SECTION_PT 28.0f
/* Icon actions on a section header. */
#define VKR_EDITOR_DETAILS_ACTION_PT 22.0f
/* Holds the longest STRING property, so a text area keeps its in-field undo
 * (VKR_UI_TEXT_UNDO_BYTES). */
#define VKR_EDITOR_DETAILS_TEXT_CAPACITY 256u

/* Caller-owned editing state, reused across frames by one panel. At most one
 * text field is being typed into at a time. */
typedef struct VkrEditorDetails {
  VkrUiId edit_field;
  char edit_text[VKR_EDITOR_DETAILS_TEXT_CAPACITY];
  /* Text the entry started from; leaving it unchanged writes nothing, so a
   * focus pass never rounds the stored value. */
  char edit_original[VKR_EDITOR_DETAILS_TEXT_CAPACITY];
  /* Continuous edits (scrubs and slider drags) share one gesture id. */
  uint64_t gesture_counter;
  uint64_t gesture;
  VkrUiId gesture_owner;
  bool8_t gesture_active;
  bool8_t gesture_held;
  /* Validation message for the last rejected change, or empty. */
  char error[160];
} VkrEditorDetails;

typedef struct VkrEditorDetailsResult {
  /* The value changed during this build and passed validation. Text entry
   * changes the value only when it finishes (Enter or focus loss), so every
   * change is ready to apply; a multi-line text area applies each keystroke
   * within one gesture. */
  bool8_t changed;
  /* Nonzero while a continuous gesture owns the change. */
  uint64_t gesture;
  /* A text field of this build holds keyboard focus. */
  bool8_t focused;
} VkrEditorDetailsResult;

/* Per-panel frame bracket: call begin before the first row and end after the
 * last. End closes a gesture whose drag was released this frame. */
void vkr_editor_details_begin(VkrEditorDetails *details);
void vkr_editor_details_end(VkrEditorDetails *details);

/* Drop any text entry and release its focus, e.g. when the selection
 * changes. */
void vkr_editor_details_cancel(VkrEditorDetails *details, VkrUiSystem *ui);

/* Rows for every visible property of `type` over `value`, starting at *y in
 * points and advancing it. `context` reaches the type's state hook. A change
 * is normalized by the type and validated; a rejected change restores the
 * previous value and leaves its message in `details->error`. */
VkrEditorDetailsResult
vkr_editor_details_type(VkrEditorDetails *details, VkrUiSystem *ui,
                        InputState *input, float32_t width, float32_t *y,
                        const VkrTypeDesc *type, void *value,
                        const void *context, bool8_t read_only);

/* Collapsible section header; returns true while expanded. */
/* Width of the label column every row shares, for a panel `width` wide. */
float32_t vkr_editor_details_label_width(float32_t width);
/* An icon action on the header of a section that started at `section_y`,
   its right edge at `right` points; true when clicked. */
bool8_t vkr_editor_details_section_action(VkrUiSystem *ui, String8 id,
                                          float32_t right, float32_t section_y,
                                          VkrUiIcon icon, String8 tooltip);
bool8_t vkr_editor_details_section(VkrUiSystem *ui, String8 id, float32_t width,
                                   float32_t *y, VkrUiIcon icon,
                                   Vec4 icon_color, String8 title,
                                   VkrFontHandle heading, bool8_t *collapsed);

/* Error banner for `details->error`; no-op when empty. */
void vkr_editor_details_error(VkrEditorDetails *details, VkrUiSystem *ui,
                              float32_t width, float32_t *y);

/* Absolutely placed widget config used by Details rows. */
VkrUiWidgetConfig vkr_editor_details_widget(float32_t x, float32_t y,
                                            float32_t width, float32_t height);

#pragma once

#include "renderer/systems/vkr_ui_system.h"

/*
 * Editor tables and split bars (docs/proposals/recordings.md, "UI
 * primitives"): a column table with resizable, sortable columns, row
 * selection and virtual rows, and a bar that resizes the panes beside it.
 * Both are built from the UI system's panels and buttons; their state is
 * the caller's, so a window keeps its column widths and split between
 * builds.
 */

#define VKR_EDITOR_TABLE_COLUMNS_MAX 12u
/* The row height every table uses, in points. */
#define VKR_EDITOR_TABLE_ROW_PT 20.0f
#define VKR_EDITOR_TABLE_NONE UINT32_MAX

typedef struct VkrEditorTableColumn {
  const char *title;
  /* Starting width; zero makes the column take what the others leave. */
  float32_t width_pt;
  float32_t min_pt;
  /* Right-aligned; its first sort is descending, largest first. */
  bool8_t numeric;
  /* Hover text of the header, such as what the column measures. */
  const char *tooltip;
} VkrEditorTableColumn;

typedef struct VkrEditorTable {
  bool8_t initialized;
  float32_t width_pt[VKR_EDITOR_TABLE_COLUMNS_MAX];
  /* VKR_EDITOR_TABLE_NONE keeps the caller's order. */
  uint32_t sort_column;
  bool8_t sort_descending;
  /* The selected row's caller key, or UINT64_MAX. */
  uint64_t selected_key;
  /* A column edge being dragged: the column, the pointer and the width
     where the drag began. */
  uint32_t drag_column;
  float32_t drag_from_x;
  float32_t drag_from_width;
  float32_t scroll_pt;
  /* Room kept clear at the right of the header and rows for the scrollbar
     while the rows overflow, so it never covers a right-aligned value. */
  float32_t gutter_pt;
  /* The rows' scroll area, for its last rectangle. */
  VkrUiId body_id;
} VkrEditorTable;

/* A row's cells: text, and optionally a colour dot before the first. */
typedef struct VkrEditorTableRow {
  uint64_t key;
  const String8 *cells;
  Vec4 dot;
  bool8_t has_dot;
  Vec4 text_color;
  String8 tooltip;
} VkrEditorTableRow;

/** Draws the header (a click sorts, a column's right edge resizes it) and
 * opens the rows in a virtual scroll area at `placement`. `out_sorted` says
 * the sort changed this build, so the caller reorders its rows by
 * `table->sort_column` and `sort_descending`. Rows [*out_first, *out_end)
 * are the ones in view; build each with vkr_editor_table_row, then call
 * vkr_editor_table_end. False when nothing opened. */
bool8_t vkr_editor_table_begin(VkrUiSystem *ui, const char *id,
                               VkrEditorTable *table,
                               const VkrEditorTableColumn *columns,
                               uint32_t column_count, uint32_t row_count,
                               VkrUiPlacement placement, bool8_t *out_sorted,
                               uint32_t *out_first, uint32_t *out_end);

/** Row `index` of the open table. True when it was clicked, which selects
 * it. */
bool8_t vkr_editor_table_row(VkrUiSystem *ui, VkrEditorTable *table,
                             const VkrEditorTableColumn *columns,
                             uint32_t column_count, uint32_t index,
                             const VkrEditorTableRow *row);

void vkr_editor_table_end(VkrUiSystem *ui);

/* A split between two panes: the first pane's share of `extent`. */
typedef struct VkrEditorSplit {
  float32_t ratio;
  float32_t min_ratio;
  float32_t max_ratio;
  bool8_t dragging;
  float32_t drag_from;
  float32_t drag_from_ratio;
} VkrEditorSplit;

/** The bar between two panes at `placement`: a drag moves the split.
 * `vertical` stacks the panes, so the bar is horizontal and drags up and
 * down; `extent_pt` is the panes' joint length along the drag. */
void vkr_editor_split_bar(VkrUiSystem *ui, const char *id,
                          VkrEditorSplit *split, VkrUiPlacement placement,
                          bool8_t vertical, float32_t extent_pt);

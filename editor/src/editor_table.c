#include "editor_table.h"

#include "editor_internal.h"

/* Header height, and the grab width of a column edge and of a split bar. */
#define EDITOR_TABLE_HEADER_PT 22.0f
#define EDITOR_TABLE_EDGE_PT 7.0f
#define EDITOR_SPLIT_BAR_PT 6.0f
/* Rows a wheel notch scrolls, and rows built before the body has a size. */
#define EDITOR_TABLE_WHEEL_ROWS 3.0f
#define EDITOR_TABLE_FIRST_PAGE 24u
#define EDITOR_TABLE_CELL_PAD_PT 6.0f

vkr_internal VkrUiPlacement editor_table_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_STRETCH};
}

/* The column tracks: fixed widths, and the rest for zero-width columns. */
vkr_internal void editor_table_tracks(const VkrEditorTable *table,
                                      uint32_t column_count, VkrUiTrack *out) {
  for (uint32_t c = 0u; c < column_count; ++c) {
    out[c] =
        table->width_pt[c] > 0.0f
            ? (VkrUiTrack){.value = table->width_pt[c], .unit = VKR_UI_TRACK_PX}
            : (VkrUiTrack){.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  }
}

/* A grid panel of the table's columns and one row, at `placement`. */
vkr_internal VkrUiPanelConfig editor_table_row_panel(VkrUiPlacement placement,
                                                     const VkrUiTrack *columns,
                                                     uint32_t column_count,
                                                     const VkrUiTrack *row) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = placement;
  panel.columns = columns;
  panel.column_count = column_count;
  panel.rows = row;
  panel.row_count = 1u;
  panel.style.gap_pt = 0.0f;
  return panel;
}

/* Text in a clipped cell, so a long value stops at its column's edge. */
vkr_internal void editor_table_text(VkrUiSystem *ui, uint32_t column,
                                    String8 text, bool8_t numeric,
                                    VkrUiWidgetConfig label) {
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig cell =
      editor_table_row_panel(editor_table_cell(column, 0u), &fill, 1u, &fill);
  cell.clip_children = true_v;
  (void)vkr_ui_push_id_u64(ui, column);
  if (vkr_ui_panel_begin(ui, string8_lit("cell"), &cell)) {
    label.placement = editor_table_cell(0u, 0u);
    label.placement.justify = numeric ? VKR_UI_ALIGN_END : VKR_UI_ALIGN_START;
    label.placement.align = VKR_UI_ALIGN_CENTER;
    label.style.padding_pt = (VkrUiEdges){0.0f, EDITOR_TABLE_CELL_PAD_PT, 0.0f,
                                          EDITOR_TABLE_CELL_PAD_PT};
    vkr_ui_label(ui, string8_lit("text"), text, &label);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

/* ------------------------------------------------------------------------ */
/* Header                                                                    */

/* Drags column `c`'s right edge: while its handle is held, the width
   follows the pointer from where the drag began. */
vkr_internal void editor_table_drag_edge(VkrUiSystem *ui, VkrEditorTable *table,
                                         const VkrEditorTableColumn *column,
                                         uint32_t c, VkrUiId edge_id) {
  const float32_t pointer = (float32_t)ui->mouse_x / ui->content_scale;
  if (ui->active_id == edge_id) {
    if (table->drag_column != c) {
      table->drag_column = c;
      table->drag_from_x = pointer;
      table->drag_from_width = table->width_pt[c];
    }
    table->width_pt[c] =
        Max(Max(column->min_pt, 24.0f),
            table->drag_from_width + pointer - table->drag_from_x);
  } else if (table->drag_column == c) {
    table->drag_column = VKR_EDITOR_TABLE_NONE;
  }
}

/* The header: a button per column that sorts by it, and a handle on each
   fixed column's right edge that resizes it. True when the sort changed. */
vkr_internal bool8_t editor_table_header(VkrUiSystem *ui, VkrEditorTable *table,
                                         const VkrEditorTableColumn *columns,
                                         uint32_t column_count) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack tracks[VKR_EDITOR_TABLE_COLUMNS_MAX];
  editor_table_tracks(table, column_count, tracks);
  const VkrUiTrack height = {.value = EDITOR_TABLE_HEADER_PT,
                             .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig header = editor_table_row_panel(
      editor_table_cell(0u, 0u), tracks, column_count, &height);
  header.style.background_color = theme->header;
  header.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  header.style.border_color = theme->separator;
  header.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 0.0f, 0.0f};
  header.clip_children = true_v;
  bool8_t sorted = false_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    return false_v;
  }
  for (uint32_t c = 0u; c < column_count; ++c) {
    const VkrEditorTableColumn *column = &columns[c];
    const bool8_t sorting = table->sort_column == c;
    (void)vkr_ui_push_id_u64(ui, c);
    VkrUiWidgetConfig title = vkr_ui_widget_config_default();
    title.placement = editor_table_cell(c, 0u);
    title.fill = true_v;
    vkr_editor_ghost_style(&title);
    title.style.font_size_pt = theme->font_caption;
    title.style.text_color = sorting ? theme->text : theme->text_secondary;
    title.style.padding_pt = (VkrUiEdges){0.0f, EDITOR_TABLE_CELL_PAD_PT, 0.0f,
                                          EDITOR_TABLE_CELL_PAD_PT};
    title.style.corner_radius_pt = (Vec4){0};
    if (column->tooltip) {
      title.tooltip = string8_create_from_cstr((const uint8_t *)column->tooltip,
                                               string_length(column->tooltip));
    }
    /* The whole cell sorts; its text lines up with the column's values,
       numbers on the right. */
    if (vkr_ui_button(ui, string8_lit("title"), (String8){0}, &title)) {
      table->sort_descending =
          sorting ? !table->sort_descending : column->numeric;
      table->sort_column = c;
      sorted = true_v;
    }
    VkrUiWidgetConfig text = vkr_editor_text_config(
        theme->font_caption, sorting ? theme->text : theme->text_secondary);
    if (sorting) {
      text.trailing_icon = table->sort_descending ? VKR_UI_ICON_SORT_DESCENDING
                                                  : VKR_UI_ICON_SORT_ASCENDING;
      text.trailing_icon_size_pt = 11.0f;
    }
    editor_table_text(ui, c,
                      string8_create_from_cstr((const uint8_t *)column->title,
                                               string_length(column->title)),
                      column->numeric, text);
    /* The right edge of a fixed column resizes it. */
    if (table->width_pt[c] > 0.0f) {
      const VkrUiId edge_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("edge"));
      editor_table_drag_edge(ui, table, column, c, edge_id);
      const bool8_t held = ui->active_id == edge_id;
      VkrUiWidgetConfig edge = vkr_ui_widget_config_default();
      edge.placement = editor_table_cell(c, 0u);
      edge.placement.justify = VKR_UI_ALIGN_END;
      edge.style.min_size_pt = edge.style.max_size_pt =
          (Vec2){EDITOR_TABLE_EDGE_PT, EDITOR_TABLE_HEADER_PT};
      edge.style.background_color = held ? theme->accent : theme->separator;
      edge.style.hover_background_color =
          vkr_ui_color_alpha(theme->accent, 0.55f);
      /* A one-point line inside the grab area marks the edge. */
      edge.style.border_pt = (VkrUiEdges){4.0f, 3.0f, 4.0f, 3.0f};
      edge.style.border_color = theme->header;
      edge.cursor = VKR_WINDOW_CURSOR_RESIZE_EW;
      edge.tooltip = string8_lit("Drag to resize the column");
      (void)vkr_ui_button(ui, string8_lit("edge"), (String8){0}, &edge);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  return sorted;
}

/* ------------------------------------------------------------------------ */
/* Table                                                                     */

vkr_internal void editor_table_init(VkrEditorTable *table,
                                    const VkrEditorTableColumn *columns,
                                    uint32_t column_count) {
  if (table->initialized) {
    return;
  }
  for (uint32_t c = 0u; c < column_count; ++c) {
    table->width_pt[c] = columns[c].width_pt;
  }
  table->sort_column = VKR_EDITOR_TABLE_NONE;
  table->selected_key = UINT64_MAX;
  table->drag_column = VKR_EDITOR_TABLE_NONE;
  table->initialized = true_v;
}

bool8_t vkr_editor_table_begin(VkrUiSystem *ui, const char *id,
                               VkrEditorTable *table,
                               const VkrEditorTableColumn *columns,
                               uint32_t column_count, uint32_t row_count,
                               VkrUiPlacement placement, bool8_t *out_sorted,
                               uint32_t *out_first, uint32_t *out_end) {
  column_count = Min(column_count, VKR_EDITOR_TABLE_COLUMNS_MAX);
  editor_table_init(table, columns, column_count);
  *out_sorted = false_v;
  *out_first = *out_end = 0u;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = EDITOR_TABLE_HEADER_PT, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig outer = vkr_ui_panel_config_default();
  outer.placement = placement;
  outer.columns = &fill;
  outer.column_count = 1u;
  outer.rows = rows;
  outer.row_count = ArrayCount(rows);
  outer.style.gap_pt = 0.0f;
  outer.style.background_color = vkr_ui_color_alpha(theme->window, 0.6f);
  outer.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  outer.clip_children = true_v;
  if (!vkr_ui_panel_begin(
          ui, string8_create_from_cstr((const uint8_t *)id, string_length(id)),
          &outer)) {
    return false_v;
  }
  *out_sorted = editor_table_header(ui, table, columns, column_count);

  /* The rows scroll by the wheel over them and by the bar; only the rows
     in view are built. */
  const float32_t row_pt = VKR_EDITOR_TABLE_ROW_PT;
  const float32_t content_pt = (float32_t)row_count * row_pt;
  VkrUiRect body = {0};
  float32_t page_pt = 0.0f;
  if (table->body_id && vkr_ui_widget_rect(ui, table->body_id, &body)) {
    page_pt = body.height / ui->content_scale;
    if (ui->mouse_wheel != 0 && !ui->mouse_captured &&
        (float32_t)ui->mouse_x >= body.x &&
        (float32_t)ui->mouse_x < body.x + body.width &&
        (float32_t)ui->mouse_y >= body.y &&
        (float32_t)ui->mouse_y < body.y + body.height) {
      table->scroll_pt -=
          (float32_t)ui->mouse_wheel * row_pt * EDITOR_TABLE_WHEEL_ROWS;
    }
  }
  table->scroll_pt =
      vkr_clamp_f32(table->scroll_pt, 0.0f, Max(0.0f, content_pt - page_pt));
  const VkrUiTrack content = {.value = Max(content_pt, 1.0f),
                              .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  list.placement = editor_table_cell(0u, 1u);
  list.columns = &fill;
  list.column_count = 1u;
  list.rows = &content;
  list.row_count = 1u;
  list.clip_children = true_v;
  table->body_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("rows"));
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("rows"), &list)) {
    (void)vkr_ui_panel_end(ui);
    return false_v;
  }
  (void)vkr_ui_scroll_area_offset(ui, &table->scroll_pt);
  const uint32_t first = (uint32_t)(table->scroll_pt / row_pt);
  const uint32_t page = page_pt > 0.0f ? (uint32_t)(page_pt / row_pt) + 2u
                                       : EDITOR_TABLE_FIRST_PAGE;
  *out_first = Min(first, row_count);
  *out_end = Min(row_count, first + page);
  return true_v;
}

bool8_t vkr_editor_table_row(VkrUiSystem *ui, VkrEditorTable *table,
                             const VkrEditorTableColumn *columns,
                             uint32_t column_count, uint32_t index,
                             const VkrEditorTableRow *row) {
  column_count = Min(column_count, VKR_EDITOR_TABLE_COLUMNS_MAX);
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack tracks[VKR_EDITOR_TABLE_COLUMNS_MAX];
  editor_table_tracks(table, column_count, tracks);
  const VkrUiTrack height = {.value = VKR_EDITOR_TABLE_ROW_PT,
                             .unit = VKR_UI_TRACK_PX};
  VkrUiPlacement placement = editor_table_cell(0u, 0u);
  placement.align = VKR_UI_ALIGN_START;
  placement.margin_pt.top = (float32_t)index * VKR_EDITOR_TABLE_ROW_PT;
  VkrUiPanelConfig panel =
      editor_table_row_panel(placement, tracks, column_count, &height);
  panel.style.min_size_pt.y = panel.style.max_size_pt.y =
      VKR_EDITOR_TABLE_ROW_PT;
  bool8_t clicked = false_v;
  (void)vkr_ui_push_id_u64(ui, row->key);
  if (vkr_ui_panel_begin(ui, string8_lit("row"), &panel)) {
    /* The whole row is one button under its cells. */
    VkrUiWidgetConfig hit = vkr_ui_widget_config_default();
    hit.placement = editor_table_cell(0u, 0u);
    hit.placement.column_span = column_count;
    hit.fill = true_v;
    vkr_editor_ghost_style(&hit);
    hit.style.corner_radius_pt = (Vec4){3.0f, 3.0f, 3.0f, 3.0f};
    if (table->selected_key == row->key) {
      hit.style.background_color = theme->selection;
    }
    hit.tooltip = row->tooltip;
    if (vkr_ui_button(ui, string8_lit("hit"), (String8){0}, &hit)) {
      table->selected_key = row->key;
      clicked = true_v;
    }
    VkrUiWidgetConfig label = vkr_editor_text_config(
        theme->font_caption,
        row->text_color.w > 0.0f ? row->text_color : theme->text);
    for (uint32_t c = 0u; c < column_count; ++c) {
      VkrUiWidgetConfig cell = label;
      if (c == 0u && row->has_dot) {
        cell.icon = VKR_UI_ICON_DOT;
        cell.icon_size_pt = 10.0f;
        cell.icon_color = row->dot;
      }
      editor_table_text(ui, c, row->cells[c], columns[c].numeric, cell);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
  return clicked;
}

void vkr_editor_table_end(VkrUiSystem *ui) {
  (void)vkr_ui_scroll_area_end(ui);
  (void)vkr_ui_panel_end(ui);
}

/* ------------------------------------------------------------------------ */
/* Split bar                                                                 */

void vkr_editor_split_bar(VkrUiSystem *ui, const char *id,
                          VkrEditorSplit *split, VkrUiPlacement placement,
                          bool8_t vertical, float32_t extent_pt) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const String8 label =
      string8_create_from_cstr((const uint8_t *)id, string_length(id));
  const VkrUiId bar_id = vkr_ui_id_stack_widget_label(&ui->id_stack, label);
  const float32_t pointer =
      (float32_t)(vertical ? ui->mouse_y : ui->mouse_x) / ui->content_scale;
  const bool8_t held = ui->active_id == bar_id;
  if (held && extent_pt > 0.0f) {
    if (!split->dragging) {
      split->dragging = true_v;
      split->drag_from = pointer;
      split->drag_from_ratio = split->ratio;
    }
    split->ratio =
        split->drag_from_ratio + (pointer - split->drag_from) / extent_pt;
  } else {
    split->dragging = false_v;
  }
  split->ratio =
      vkr_clamp_f32(split->ratio, split->min_ratio, split->max_ratio);
  VkrUiWidgetConfig bar = vkr_ui_widget_config_default();
  bar.placement = placement;
  bar.placement.justify = VKR_UI_ALIGN_STRETCH;
  bar.placement.align = VKR_UI_ALIGN_STRETCH;
  bar.fill = true_v;
  if (vertical) {
    bar.style.min_size_pt.y = bar.style.max_size_pt.y = EDITOR_SPLIT_BAR_PT;
  } else {
    bar.style.min_size_pt.x = bar.style.max_size_pt.x = EDITOR_SPLIT_BAR_PT;
  }
  /* A one-point line in the middle of the grab area, the accent while
     hovered or held. */
  bar.style.background_color = held ? theme->accent : theme->separator;
  bar.style.hover_background_color = vkr_ui_color_alpha(theme->accent, 0.55f);
  bar.style.border_pt = vertical ? (VkrUiEdges){2.5f, 0.0f, 2.5f, 0.0f}
                                 : (VkrUiEdges){0.0f, 2.5f, 0.0f, 2.5f};
  bar.style.border_color = theme->panel;
  bar.cursor =
      vertical ? VKR_WINDOW_CURSOR_RESIZE_NS : VKR_WINDOW_CURSOR_RESIZE_EW;
  bar.tooltip = string8_lit("Drag to resize");
  (void)vkr_ui_button(ui, label, (String8){0}, &bar);
}

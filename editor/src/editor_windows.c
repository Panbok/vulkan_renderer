#include "editor_internal.h"

static bool8_t editor_menu_button(VkrUiSystem *ui, String8 id, String8 content,
                                  uint32_t column, bool8_t active) {
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = (VkrUiPlacement){
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  button.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 10.0f};
  button.style.corner_radius_pt = (Vec4){4.0f, 4.0f, 4.0f, 4.0f};
  button.style.font_size_pt = 11.0f;
  button.style.background_color = active ? (Vec4){0.12f, 0.30f, 0.46f, 0.82f}
                                         : (Vec4){0.07f, 0.09f, 0.13f, 0.30f};
  button.style.text_color = active ? (Vec4){0.82f, 0.94f, 1.0f, 1.0f}
                                   : (Vec4){0.72f, 0.77f, 0.84f, 1.0f};
  return vkr_ui_button(ui, id, content, &button);
}

static void editor_window_raise(VkrEditorUi *editor, VkrEditorWindowKind kind) {
  if (kind >= VKR_EDITOR_WINDOW_COUNT)
    return;
  VkrEditorWindowState *window = &editor->windows[kind];
  const uint32_t old_z = window->z_order;
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    if (editor->windows[i].z_order > old_z)
      editor->windows[i].z_order--;
  }
  window->z_order = VKR_EDITOR_WINDOW_COUNT;
  window->visible = true_v;
}

void vkr_editor_windows_build_navigation(VkrEditorUi *editor, VkrUiSystem *ui) {
  const VkrUiTrack nav_columns[] = {
      {.unit = VKR_UI_TRACK_AUTO}, {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO}, {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_START,
  };
  bar.columns = nav_columns;
  bar.column_count = ArrayCount(nav_columns);
  bar.rows = &one_track;
  bar.row_count = 1u;
  bar.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 10.0f};
  bar.style.gap_pt = 3.0f;
  bar.style.min_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.max_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  bar.style.background_color = (Vec4){0.025f, 0.034f, 0.050f, 0.92f};
  bar.style.border_color = (Vec4){0.28f, 0.42f, 0.56f, 0.52f};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.navigation"), &bar))
    return;

  VkrUiWidgetConfig brand =
      vkr_editor_text_config(12.0f, (Vec4){0.43f, 0.80f, 1.0f, 1.0f});
  brand.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 10.0f, 0.0f, 2.0f},
  };
  vkr_ui_label(ui, string8_lit("brand"), string8_lit("VKR / EDITOR"), &brand);

  if (editor_menu_button(ui, string8_lit("menu.metrics"),
                         string8_lit("Metrics"), 1u,
                         editor->menu == VKR_EDITOR_MENU_METRICS)) {
    editor->menu = editor->menu == VKR_EDITOR_MENU_METRICS
                       ? VKR_EDITOR_MENU_NONE
                       : VKR_EDITOR_MENU_METRICS;
  }
  if (editor_menu_button(ui, string8_lit("menu.help"), string8_lit("Help"), 2u,
                         editor->windows[VKR_EDITOR_WINDOW_HELP].visible)) {
    editor_window_raise(editor, VKR_EDITOR_WINDOW_HELP);
    editor->menu = VKR_EDITOR_MENU_NONE;
  }

  VkrUiWidgetConfig status =
      vkr_editor_text_config(10.0f, (Vec4){0.46f, 0.84f, 0.66f, 1.0f});
  status.placement = (VkrUiPlacement){
      .column = 4u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_END,
      .align = VKR_UI_ALIGN_CENTER,
  };
  vkr_ui_label(ui, string8_lit("status"), string8_lit("LIVE  /  EDIT MODE"),
               &status);
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui) {
  if (editor->menu != VKR_EDITOR_MENU_METRICS)
    return;

  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_WINDOW_COUNT + 1u);
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {VKR_EDITOR_NAVIGATION_HEIGHT_PT, 0.0f, 0.0f,
                    VKR_EDITOR_METRICS_MENU_X_PT},
  };
  popup.columns = &one_track;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = ArrayCount(rows);
  popup.style = vkr_editor_glass_style();
  popup.style.min_size_pt = (Vec2){210.0f, 94.0f};
  popup.style.max_size_pt = popup.style.min_size_pt;
  popup.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.menu.popup"), &popup))
    return;

  VkrUiWidgetConfig heading =
      vkr_editor_text_config(10.0f, (Vec4){0.43f, 0.80f, 1.0f, 1.0f});
  heading.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
  };
  vkr_ui_label(ui, string8_lit("heading"), string8_lit("METRICS WINDOWS"),
               &heading);

  VkrUiWidgetConfig item =
      vkr_editor_text_config(12.0f, (Vec4){0.80f, 0.84f, 0.90f, 1.0f});
  item.style.padding_pt = (VkrUiEdges){4.0f, 3.0f, 4.0f, 3.0f};
  item.placement = heading.placement;
  item.placement.row = 1u;
  VkrEditorWindowState *draws = &editor->windows[VKR_EDITOR_WINDOW_DRAWS];
  if (vkr_ui_checkbox(ui, string8_lit("draws"), string8_lit("Draws"),
                      &draws->visible, &item) &&
      draws->visible)
    editor_window_raise(editor, VKR_EDITOR_WINDOW_DRAWS);
  item.placement.row = 2u;
  VkrEditorWindowState *memory = &editor->windows[VKR_EDITOR_WINDOW_MEMORY];
  if (vkr_ui_checkbox(ui, string8_lit("memory"), string8_lit("Memory"),
                      &memory->visible, &item) &&
      memory->visible)
    editor_window_raise(editor, VKR_EDITOR_WINDOW_MEMORY);
  (void)vkr_ui_panel_end(ui);
}

static VkrUiRect editor_window_rect(const VkrUiSystem *ui,
                                    const VkrEditorWindowState *window) {
  const float32_t scale = ui->content_scale;
  return (VkrUiRect){
      .x = window->position_pt.x * scale,
      .y = window->position_pt.y * scale,
      .width = window->size_pt.x * scale,
      .height = window->size_pt.y * scale,
  };
}

static VkrUiRect editor_menu_popup_rect(const VkrUiSystem *ui) {
  return (VkrUiRect){
      .x = VKR_EDITOR_METRICS_MENU_X_PT * ui->content_scale,
      .y = VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale,
      .width = 210.0f * ui->content_scale,
      .height = 94.0f * ui->content_scale,
  };
}

static bool8_t editor_point_in_rect(int32_t x, int32_t y, VkrUiRect rect) {
  return (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.width &&
         (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.height;
}

static void editor_window_clamp(VkrUiSystem *ui, VkrEditorWindowState *window) {
  const float32_t width_pt = (float32_t)ui->target_width / ui->content_scale;
  const float32_t height_pt = (float32_t)ui->target_height / ui->content_scale;
  const float32_t visible_title_pt = 72.0f;
  window->position_pt.x =
      vkr_clamp_f32(window->position_pt.x, visible_title_pt - window->size_pt.x,
                    vkr_max_f32(0.0f, width_pt - visible_title_pt));
  window->position_pt.y = vkr_clamp_f32(window->position_pt.y, 35.0f,
                                        vkr_max_f32(35.0f, height_pt - 28.0f));
}

void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui) {
  const bool8_t popup_contains_pointer =
      editor->menu == VKR_EDITOR_MENU_METRICS &&
      editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                           editor_menu_popup_rect(ui));
  if (ui->mouse_pressed && !popup_contains_pointer) {
    uint32_t top_z = 0u;
    VkrEditorWindowKind top_kind = VKR_EDITOR_WINDOW_COUNT;
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      VkrEditorWindowState *window = &editor->windows[i];
      editor_window_clamp(ui, window);
      if (window->visible && window->z_order > top_z &&
          editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                               editor_window_rect(ui, window))) {
        top_z = window->z_order;
        top_kind = (VkrEditorWindowKind)i;
      }
    }
    if (top_kind < VKR_EDITOR_WINDOW_COUNT)
      editor_window_raise(editor, top_kind);
  }

  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    VkrEditorWindowState *window = &editor->windows[i];
    editor_window_clamp(ui, window);
    if (window->visible)
      (void)vkr_ui_input_layer_register(ui, window->z_order,
                                        editor_window_rect(ui, window));
  }
  if (editor->menu == VKR_EDITOR_MENU_METRICS)
    (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_WINDOW_COUNT + 1u,
                                      editor_menu_popup_rect(ui));
}

static void editor_build_window(VkrEditorUi *editor, VkrUiSystem *ui,
                                InputState *input, VkrEditorWindowKind kind,
                                const VkrSampleUiText *text) {
  VkrEditorWindowState *window = &editor->windows[kind];
  if (!window->visible)
    return;

  String8 title_text = {0};
  String8 body_text = {0};
  float32_t font_size_pt = 10.0f;
  switch (kind) {
  case VKR_EDITOR_WINDOW_DRAWS:
    title_text = string8_lit("DRAWS / RENDER GRAPH");
    body_text = text->metrics;
    break;
  case VKR_EDITOR_WINDOW_MEMORY:
    title_text = string8_lit("MEMORY / LIVE");
    body_text = text->memory;
    break;
  case VKR_EDITOR_WINDOW_HELP:
    title_text = string8_lit("EDITOR CONTROLS");
    body_text = string8_lit("Tab  Toggle free camera     F8  Cycle IBL mode\n"
                            "F9 / F10  IBL intensity     G   Camera snapshot\n"
                            "Click a title bar to focus and drag a window.");
    font_size_pt = 11.0f;
    break;
  default:
    return;
  }

  editor_window_clamp(ui, window);
  (void)vkr_ui_input_layer_set(ui, window->z_order);
  (void)vkr_ui_push_id_u64(ui, kind);
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = 28.0f, .unit = VKR_UI_TRACK_PX},
      one_track,
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt =
          {
              .top = window->position_pt.y,
              .left = window->position_pt.x,
          },
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = vkr_editor_glass_style();
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.min_size_pt = window->size_pt;
  panel.style.max_size_pt = window->size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("window"), &panel)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }

  const VkrUiTrack header_columns[] = {
      one_track,
      {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  header.columns = header_columns;
  header.column_count = ArrayCount(header_columns);
  header.rows = &one_track;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  header.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  header.style.background_color = (Vec4){0.055f, 0.075f, 0.105f, 0.91f};
  header.style.border_color = (Vec4){0.24f, 0.42f, 0.58f, 0.50f};
  if (vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    VkrUiWidgetConfig drag = vkr_ui_widget_config_default();
    drag.placement = (VkrUiPlacement){
        .column = 0u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    drag.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 10.0f};
    drag.style.corner_radius_pt = (Vec4){0};
    drag.style.background_color = (Vec4){0};
    drag.style.text_color = (Vec4){0.43f, 0.80f, 1.0f, 1.0f};
    drag.style.font_size_pt = 10.0f;
    const VkrUiId drag_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("drag"));
    (void)vkr_ui_button(ui, string8_lit("drag"), title_text, &drag);
    if (ui->active_id == drag_id && input_is_button_down(input, BUTTON_LEFT)) {
      int32_t dx = 0;
      int32_t dy = 0;
      input_get_mouse_delta(input, &dx, &dy);
      window->position_pt.x += (float32_t)dx / ui->content_scale;
      window->position_pt.y += (float32_t)dy / ui->content_scale;
      editor_window_clamp(ui, window);
    }

    VkrUiWidgetConfig close = vkr_ui_widget_config_default();
    close.placement = (VkrUiPlacement){
        .column = 1u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    close.style.padding_pt = (VkrUiEdges){0};
    close.style.corner_radius_pt = (Vec4){0};
    close.style.background_color = (Vec4){0.10f, 0.12f, 0.16f, 0.50f};
    close.style.text_color = (Vec4){0.76f, 0.80f, 0.86f, 1.0f};
    close.style.font_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("close"), string8_lit("x"), &close))
      window->visible = false_v;
    (void)vkr_ui_panel_end(ui);
  }

  VkrUiWidgetConfig body =
      vkr_editor_text_config(font_size_pt, (Vec4){0.84f, 0.87f, 0.92f, 1.0f});
  body.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 1u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {10.0f, 12.0f, 10.0f, 12.0f},
  };
  vkr_ui_label(ui, string8_lit("body"), body_text, &body);
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiText *text) {
  for (uint32_t z = 1u; z <= VKR_EDITOR_WINDOW_COUNT; ++z) {
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      if (editor->windows[i].visible && editor->windows[i].z_order == z) {
        editor_build_window(editor, ui, input, (VkrEditorWindowKind)i, text);
        break;
      }
    }
  }
}

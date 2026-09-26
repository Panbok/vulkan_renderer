#include "editor_internal.h"

/* Dock layout owns these rectangles, including the Scene mapping. Present them
   as overlapping root-grid items so drawing never solves a second split tree.
 */
static VkrUiPanelConfig editor_dock_rect_panel(VkrUiSystem *ui,
                                               VkrUiRect rect) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  const float32_t scale = ui->content_scale;
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {rect.y / scale, 0.0f, 0.0f, rect.x / scale},
  };
  panel.style.min_size_pt = (Vec2){rect.width / scale, rect.height / scale};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  return panel;
}

/* Category hue on the tab icon only; tab surfaces stay neutral. */
static Vec4 editor_dock_icon_color(VkrUiDockPanelKind kind) {
  static const Vec4 colors[VKR_UI_DOCK_PANEL_COUNT] = {
      {0.46f, 0.72f, 0.98f, 1.0f}, /* Scene */
      {0.52f, 0.82f, 0.58f, 1.0f}, /* Hierarchy */
      {0.96f, 0.74f, 0.42f, 1.0f}, /* Inspector */
      {0.72f, 0.64f, 0.96f, 1.0f}, /* Console */
      {0.62f, 0.66f, 0.72f, 1.0f}, {0.62f, 0.66f, 0.72f, 1.0f},
      {0.98f, 0.60f, 0.42f, 1.0f}, /* Bakery */
      {0.42f, 0.84f, 0.86f, 1.0f}, /* Content */
  };
  return colors[kind];
}

static VkrUiIcon editor_dock_panel_icon(VkrUiDockPanelKind kind) {
  static const VkrUiIcon icons[VKR_UI_DOCK_PANEL_COUNT] = {
      VKR_UI_ICON_SCENE,   VKR_UI_ICON_HIERARCHY, VKR_UI_ICON_INSPECTOR,
      VKR_UI_ICON_CONSOLE, VKR_UI_ICON_NONE,      VKR_UI_ICON_NONE,
      VKR_UI_ICON_BAKERY,  VKR_UI_ICON_CONTENT,
  };
  return icons[kind];
}

void vkr_editor_dock_show(VkrUiDockTree *dock, VkrUiDockPanelKind kind) {
  for (uint32_t i = 0; i < dock->node_high_water; i++) {
    VkrUiDockNode *n = &dock->nodes[i];
    if (!n->used || n->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    for (uint32_t j = 0; j < n->as.leaf.tab_count; j++) {
      if (n->as.leaf.tabs[j].panel_kind != kind)
        continue;
      n->as.leaf.active_tab = j;
      dock->focused_tab_id = n->as.leaf.tabs[j].id;
      dock->revision++;
      return;
    }
  }
  uint32_t target = VKR_UI_DOCK_NODE_NONE;
  for (uint32_t i = 0; i < dock->node_high_water; i++) {
    VkrUiDockNode *n = &dock->nodes[i];
    if (!n->used || n->kind != VKR_UI_DOCK_NODE_TABS ||
        n->as.leaf.tabs[0].panel_kind == VKR_UI_DOCK_PANEL_TOOLBAR ||
        n->as.leaf.tab_count == VKR_UI_DOCK_TAB_CAPACITY)
      continue;
    target = i;
    for (uint32_t j = 0; j < n->as.leaf.tab_count; j++)
      if (n->as.leaf.tabs[j].panel_kind == VKR_UI_DOCK_PANEL_CONSOLE)
        goto found;
  }
found:
  if (target == VKR_UI_DOCK_NODE_NONE)
    return;
  VkrUiDockNode *n = &dock->nodes[target];
  n->as.leaf.active_tab = n->as.leaf.tab_count;
  n->as.leaf.tabs[n->as.leaf.tab_count++] =
      (VkrUiDockTab){.id = (uint64_t)kind + 1, .panel_kind = kind};
  dock->focused_tab_id = (uint64_t)kind + 1;
  dock->revision++;
}

void vkr_editor_dock_toggle(VkrUiDockTree *dock, VkrUiDockPanelKind kind) {
  if (!dock)
    return;
  for (uint32_t leaf = 0u; leaf < dock->node_high_water; ++leaf) {
    VkrUiDockNode *node = &dock->nodes[leaf];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
      if (node->as.leaf.tabs[tab].panel_kind == kind) {
        if (node->as.leaf.active_tab != tab) {
          node->as.leaf.active_tab = tab;
          dock->focused_tab_id = node->as.leaf.tabs[tab].id;
          dock->revision++;
          return;
        }
        (void)vkr_ui_dock_close_tab(dock, leaf, tab);
        return;
      }
    }
  }
  vkr_editor_dock_show(dock, kind);
}

void vkr_editor_dock_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  VkrUiDockTree *dock = frame->dock;
  VkrFontHandle heading_font = editor->heading_font;
  const VkrUiTheme *theme = vkr_ui_theme();
  /* Cover only the dock separators; the Scene content remains transparent to
     the renderer composite beneath the editor UI. A hovered or dragged
     separator shows the accent so its grab target is discoverable. */
  for (uint32_t split = 0u; split < dock->node_high_water; ++split) {
    const VkrUiDockNode *node = &dock->nodes[split];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_SPLIT)
      continue;
    const VkrUiRect bar_rect = vkr_ui_dock_split_bar_rect(dock, split);
    const float32_t grab = 3.0f * ui->content_scale;
    const bool8_t hovered =
        !frame->mouse_captured && ui->mouse_input_layer == 0u &&
        (float32_t)ui->mouse_x >= bar_rect.x - grab &&
        (float32_t)ui->mouse_x < bar_rect.x + bar_rect.width + grab &&
        (float32_t)ui->mouse_y >= bar_rect.y - grab &&
        (float32_t)ui->mouse_y < bar_rect.y + bar_rect.height + grab;
    const bool8_t resizing = dock->interaction.resize_split == split;
    if (hovered || resizing)
      ui->cursor = node->as.split.axis == VKR_UI_DOCK_SPLIT_X
                       ? VKR_WINDOW_CURSOR_RESIZE_EW
                       : VKR_WINDOW_CURSOR_RESIZE_NS;
    VkrUiPanelConfig bar = editor_dock_rect_panel(ui, bar_rect);
    bar.style.background_color = resizing ? theme->accent
                                 : hovered
                                     ? vkr_ui_color_alpha(theme->accent, 0.55f)
                                     : theme->window;
    if (vkr_ui_push_id_u64(ui, split)) {
      if (vkr_ui_panel_begin(ui, string8_lit("dock.splitter"), &bar))
        (void)vkr_ui_panel_end(ui);
      (void)vkr_ui_pop_id(ui);
    }
  }
  for (uint32_t leaf = 0u; leaf < dock->node_high_water; ++leaf) {
    VkrUiDockNode *node = &dock->nodes[leaf];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    const VkrUiDockTab active = node->as.leaf.tabs[node->as.leaf.active_tab];
    if (active.panel_kind == VKR_UI_DOCK_PANEL_TOOLBAR)
      continue;

    VkrUiRect bar_rect = node->rect_px;
    bar_rect.height = Min(bar_rect.height, dock->tab_bar_px);
    VkrUiPanelConfig bar = editor_dock_rect_panel(ui, bar_rect);
    bar.style.background_color = theme->header;
    bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
    bar.style.border_color = theme->separator;
    if (vkr_ui_push_id_u64(ui, leaf)) {
      if (vkr_ui_panel_begin(ui, string8_lit("dock.bar"), &bar))
        (void)vkr_ui_panel_end(ui);
      (void)vkr_ui_pop_id(ui);
    }

    uint32_t close_tab = UINT32_MAX;
    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
      const VkrUiDockTab dock_tab = node->as.leaf.tabs[tab];
      const bool8_t selected = tab == node->as.leaf.active_tab;
      const bool8_t focused = selected && dock->focused_tab_id == dock_tab.id;
      const bool8_t closable =
          dock_tab.panel_kind != VKR_UI_DOCK_PANEL_SCENE_VIEWPORT;
      const VkrUiRect rect = vkr_ui_dock_tab_rect(dock, leaf, tab);
      if (!vkr_ui_rect_has_area(rect) || !vkr_ui_push_id_u64(ui, dock_tab.id))
        continue;
      const VkrUiTrack tab_columns[] = {
          {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
          {.value = closable ? 20.0f : 0.0f, .unit = VKR_UI_TRACK_PX},
      };
      VkrUiPanelConfig tab_panel = editor_dock_rect_panel(ui, rect);
      tab_panel.columns = tab_columns;
      tab_panel.column_count = ArrayCount(tab_columns);
      tab_panel.style.corner_radius_pt = (Vec4){5.0f, 5.0f, 0.0f, 0.0f};
      tab_panel.style.background_color = selected ? theme->panel : (Vec4){0};
      tab_panel.style.border_pt =
          focused ? (VkrUiEdges){2.0f, 0.0f, 0.0f, 0.0f} : (VkrUiEdges){0};
      tab_panel.style.border_color = theme->accent;
      if (vkr_ui_panel_begin(ui, string8_lit("dock.tab"), &tab_panel)) {
        VkrUiWidgetConfig button = vkr_ui_widget_config_default();
        button.placement.column_span = 1u;
        button.fill = true_v;
        button.style.font_size_pt = theme->font_body;
        button.text.font = selected ? heading_font : VKR_FONT_HANDLE_INVALID;
        button.style.padding_pt = (VkrUiEdges){3.0f, 8.0f, 3.0f, 10.0f};
        button.style.corner_radius_pt = (Vec4){5.0f, 5.0f, 0.0f, 0.0f};
        button.style.background_color = (Vec4){0};
        button.style.hover_background_color =
            selected ? (Vec4){0} : theme->row_hover;
        button.style.text_color =
            selected ? theme->text : theme->text_secondary;
        button.icon = editor_dock_panel_icon(dock_tab.panel_kind);
        button.icon_size_pt = 14.0f;
        button.icon_color = editor_dock_icon_color(dock_tab.panel_kind);
        if (!selected)
          button.icon_color.w = 0.7f;
        button.tooltip = vkr_ui_dock_panel_label(dock_tab.panel_kind);
        const VkrUiId select_id =
            vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("select"));
        if (vkr_ui_button(ui, string8_lit("select"),
                          vkr_ui_dock_panel_label(dock_tab.panel_kind),
                          &button)) {
          if (node->as.leaf.active_tab != tab)
            dock->revision++;
          node->as.leaf.active_tab = tab;
          dock->focused_tab_id = dock_tab.id;
        }
        /* Right click opens the tab's menu. */
        if (ui->hot_id == select_id && !ui->mouse_captured &&
            input_button_just_pressed(frame->input, BUTTON_RIGHT)) {
          editor->context_open = true_v;
          editor->context_kind = VKR_EDITOR_CONTEXT_DOCK_TAB;
          editor->context_panel = (uint32_t)dock_tab.panel_kind;
          editor->context_position_pt =
              (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                     (float32_t)ui->mouse_y / ui->content_scale};
        }
        if (closable) {
          VkrUiWidgetConfig close = vkr_editor_icon_button_config(
              1u, 0u, VKR_UI_ICON_CLOSE, string8_lit("Close panel"));
          close.style.min_size_pt = close.style.max_size_pt =
              (Vec2){18.0f, 18.0f};
          close.style.padding_pt = (VkrUiEdges){3.0f, 3.0f, 3.0f, 3.0f};
          close.icon_size_pt = 11.0f;
          close.placement.margin_pt.right = 4.0f;
          close.icon_color =
              selected ? theme->text_secondary : theme->text_disabled;
          if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close))
            close_tab = tab;
        }
        (void)vkr_ui_panel_end(ui);
      }
      (void)vkr_ui_pop_id(ui);
    }

    const VkrUiDockTab content_tab =
        node->as.leaf.tabs[node->as.leaf.active_tab];
    VkrUiRect rect = node->rect_px;
    rect.y += bar_rect.height;
    rect.height -= bar_rect.height;
    if (!vkr_ui_rect_has_area(rect) || !vkr_ui_push_id_u64(ui, content_tab.id))
      continue;
    VkrUiPanelConfig content = editor_dock_rect_panel(ui, rect);
    if (content_tab.panel_kind != VKR_UI_DOCK_PANEL_SCENE_VIEWPORT)
      content.style.background_color = theme->panel;
    if (vkr_ui_panel_begin(ui, string8_lit("dock.content"), &content)) {
      switch (content_tab.panel_kind) {
      case VKR_UI_DOCK_PANEL_HIERARCHY:
        vkr_editor_hierarchy_build(editor, frame, rect, heading_font);
        break;
      case VKR_UI_DOCK_PANEL_INSPECTOR:
        vkr_editor_inspector_build(editor->scene_panels, frame, rect,
                                   heading_font);
        break;
      case VKR_UI_DOCK_PANEL_CONSOLE:
        vkr_editor_console_build(&editor->console, ui, rect, heading_font,
                                 editor->mono_font);
        if (editor->console.context_requested) {
          editor->console.context_requested = false_v;
          editor->context_open = true_v;
          editor->context_kind = VKR_EDITOR_CONTEXT_CONSOLE;
          editor->context_position_pt = editor->console.context_position_pt;
        }
        break;
      case VKR_UI_DOCK_PANEL_CONTENT:
        vkr_editor_content_build(editor->content, ui, rect, heading_font);
        break;
      case VKR_UI_DOCK_PANEL_BAKERY:
        vkr_editor_bakery_build(editor->bakery, ui, rect, heading_font);
        break;
      default:
        break;
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
    /* Close after drawing so this frame's tab indices stay consistent. */
    if (close_tab != UINT32_MAX)
      (void)vkr_ui_dock_close_tab(dock, leaf, close_tab);
  }

  if (dock->interaction.dragging_tab)
    ui->cursor = VKR_WINDOW_CURSOR_GRABBING;
  if (dock->interaction.dragging_tab &&
      dock->interaction.drop_leaf != VKR_UI_DOCK_NODE_NONE) {
    VkrUiPanelConfig preview =
        editor_dock_rect_panel(ui, dock->interaction.drop_rect_px);
    preview.style.background_color = vkr_ui_color_alpha(theme->accent, 0.18f);
    preview.style.border_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
    preview.style.border_color = theme->accent;
    preview.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
    if (vkr_ui_panel_begin(ui, string8_lit("dock.drop.preview"), &preview))
      (void)vkr_ui_panel_end(ui);
  }
}

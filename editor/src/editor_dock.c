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

static Vec4 editor_dock_tab_color(VkrUiDockPanelKind kind, bool8_t selected) {
  static const Vec4 colors[VKR_UI_DOCK_PANEL_COUNT] = {
      {0.19f, 0.29f, 0.36f, 1.0f}, /* Scene */
      {0.25f, 0.31f, 0.25f, 1.0f}, /* Hierarchy */
      {0.32f, 0.28f, 0.22f, 1.0f}, /* Inspector */
      {0.27f, 0.24f, 0.33f, 1.0f}, /* Console */
      {0.18f, 0.21f, 0.26f, 1.0f}, {0.22f, 0.25f, 0.30f, 1.0f},
      {0.36f, 0.24f, 0.17f, 1.0f},
  };
  Vec4 color = colors[kind];
  if (!selected) {
    color.x *= 0.65f;
    color.y *= 0.65f;
    color.z *= 0.65f;
  }
  return color;
}

static VkrUiIcon editor_dock_panel_icon(VkrUiDockPanelKind kind) {
  static const VkrUiIcon icons[VKR_UI_DOCK_PANEL_COUNT] = {
      VKR_UI_ICON_SCENE,   VKR_UI_ICON_HIERARCHY, VKR_UI_ICON_INSPECTOR,
      VKR_UI_ICON_CONSOLE, VKR_UI_ICON_NONE,      VKR_UI_ICON_NONE,
      VKR_UI_ICON_BAKERY,
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
  const Vec4 amber = {0.82f, 0.62f, 0.34f, 1.0f};
  /* Cover only the dock separators; the Scene content remains transparent to
     the renderer composite beneath the editor UI. */
  for (uint32_t split = 0u; split < dock->node_high_water; ++split) {
    const VkrUiDockNode *node = &dock->nodes[split];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_SPLIT)
      continue;
    VkrUiPanelConfig bar =
        editor_dock_rect_panel(ui, vkr_ui_dock_split_bar_rect(dock, split));
    bar.style.background_color = (Vec4){0.09f, 0.105f, 0.125f, 1.0f};
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
    bar.style.background_color = (Vec4){0.075f, 0.084f, 0.098f, 1.0f};
    if (vkr_ui_push_id_u64(ui, leaf)) {
      if (vkr_ui_panel_begin(ui, string8_lit("dock.bar"), &bar))
        (void)vkr_ui_panel_end(ui);
      (void)vkr_ui_pop_id(ui);
    }

    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
      const VkrUiDockTab dock_tab = node->as.leaf.tabs[tab];
      const bool8_t selected = tab == node->as.leaf.active_tab;
      const bool8_t focused = selected && dock->focused_tab_id == dock_tab.id;
      const VkrUiRect rect = vkr_ui_dock_tab_rect(dock, leaf, tab);
      if (!vkr_ui_rect_has_area(rect) || !vkr_ui_push_id_u64(ui, dock_tab.id))
        continue;
      VkrUiPanelConfig tab_panel = editor_dock_rect_panel(ui, rect);
      if (vkr_ui_panel_begin(ui, string8_lit("dock.tab"), &tab_panel)) {
        VkrUiWidgetConfig button = vkr_ui_widget_config_default();
        button.style.font_size_pt = 12.0f;
        button.text.font = heading_font;
        button.style.padding_pt = (VkrUiEdges){3.0f, 7.0f, 3.0f, 7.0f};
        button.style.border_pt = (VkrUiEdges){2.0f, 1.0f, 0.0f, 0.0f};
        button.style.border_color = focused ? amber
                                    : selected
                                        ? (Vec4){0.42f, 0.48f, 0.54f, 1.0f}
                                        : (Vec4){0.13f, 0.15f, 0.18f, 1.0f};
        button.style.background_color =
            editor_dock_tab_color(dock_tab.panel_kind, selected);
        button.style.text_color = selected ? (Vec4){0.96f, 0.94f, 0.88f, 1.0f}
                                           : (Vec4){0.73f, 0.76f, 0.80f, 1.0f};
        button.icon = editor_dock_panel_icon(dock_tab.panel_kind);
        button.icon_size_pt = 13.0f;
        button.tooltip = vkr_ui_dock_panel_label(dock_tab.panel_kind);
        if (vkr_ui_button(ui, string8_lit("select"),
                          vkr_ui_dock_panel_label(dock_tab.panel_kind),
                          &button)) {
          if (node->as.leaf.active_tab != tab)
            dock->revision++;
          node->as.leaf.active_tab = tab;
          dock->focused_tab_id = dock_tab.id;
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
      content.style.background_color = (Vec4){0.055f, 0.064f, 0.078f, 1.0f};
    if (vkr_ui_panel_begin(ui, string8_lit("dock.content"), &content)) {
      switch (content_tab.panel_kind) {
      case VKR_UI_DOCK_PANEL_HIERARCHY:
        vkr_editor_hierarchy_build(editor->scene_panels, frame, rect,
                                   heading_font);
        break;
      case VKR_UI_DOCK_PANEL_INSPECTOR:
        vkr_editor_inspector_build(editor->scene_panels, frame, rect,
                                   heading_font);
        break;
      case VKR_UI_DOCK_PANEL_CONSOLE:
        vkr_editor_console_build(&editor->console, ui, rect, heading_font);
        break;
      case VKR_UI_DOCK_PANEL_BAKERY:
        vkr_editor_bakery_build(editor->bakery, ui, heading_font);
        break;
      default:
        break;
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
  }

  if (dock->interaction.dragging_tab &&
      dock->interaction.drop_leaf != VKR_UI_DOCK_NODE_NONE) {
    VkrUiPanelConfig preview =
        editor_dock_rect_panel(ui, dock->interaction.drop_rect_px);
    preview.style.background_color = (Vec4){0.62f, 0.43f, 0.18f, 0.30f};
    preview.style.border_pt = (VkrUiEdges){2.0f, 2.0f, 2.0f, 2.0f};
    preview.style.border_color = amber;
    if (vkr_ui_panel_begin(ui, string8_lit("dock.drop.preview"), &preview))
      (void)vkr_ui_panel_end(ui);
  }
}

#include "editor_internal.h"

static void editor_dock_build_panel_content(VkrUiSystem *ui,
                                            VkrUiDockPanelKind panel_kind) {
  if (panel_kind == VKR_UI_DOCK_PANEL_SCENE_VIEWPORT ||
      panel_kind == VKR_UI_DOCK_PANEL_TOOLBAR)
    return;

  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 1u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  panel.style.background_color = (Vec4){0.025f, 0.032f, 0.048f, 0.90f};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.panel.content"), &panel))
    return;

  VkrUiWidgetConfig label =
      vkr_editor_text_config(12.0f, (Vec4){0.52f, 0.58f, 0.68f, 1.0f});
  label.placement.margin_pt = (VkrUiEdges){12.0f, 0.0f, 0.0f, 12.0f};
  vkr_ui_label(ui, string8_lit("label"), vkr_ui_dock_panel_label(panel_kind),
               &label);
  (void)vkr_ui_panel_end(ui);
}

static void editor_dock_build_node(VkrUiSystem *ui, VkrUiDockTree *dock,
                                   uint32_t node_index,
                                   VkrUiPlacement placement) {
  if (node_index >= dock->node_high_water || !dock->nodes[node_index].used)
    return;
  VkrUiDockNode *node = &dock->nodes[node_index];
  if (!vkr_ui_push_id_u64(ui, node_index))
    return;

  if (node->kind == VKR_UI_DOCK_NODE_SPLIT) {
    const VkrUiTrack split_tracks[] = {
        {.value = node->as.split.ratio, .unit = VKR_UI_TRACK_FR},
        {.value = 1.0f - node->as.split.ratio, .unit = VKR_UI_TRACK_FR},
    };
    const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
    VkrUiPanelConfig panel = vkr_ui_panel_config_default();
    panel.placement = placement;
    panel.style.gap_pt = 8.0f;
    if (node->as.split.axis == VKR_UI_DOCK_SPLIT_X) {
      panel.columns = split_tracks;
      panel.column_count = ArrayCount(split_tracks);
      panel.rows = &one_track;
      panel.row_count = 1u;
    } else {
      panel.columns = &one_track;
      panel.column_count = 1u;
      panel.rows = split_tracks;
      panel.row_count = ArrayCount(split_tracks);
    }
    if (vkr_ui_panel_begin(ui, string8_lit("dock.split"), &panel)) {
      VkrUiPlacement first = VKR_UI_PLACEMENT_DEFAULT;
      first.column = 0u;
      first.row = 0u;
      VkrUiPlacement second = first;
      if (node->as.split.axis == VKR_UI_DOCK_SPLIT_X)
        second.column = 1u;
      else
        second.row = 1u;
      editor_dock_build_node(ui, dock, node->as.split.first, first);
      editor_dock_build_node(ui, dock, node->as.split.second, second);
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
    return;
  }

  VkrUiDockPanelKind active_kind =
      node->as.leaf.tabs[node->as.leaf.active_tab].panel_kind;
  if (active_kind == VKR_UI_DOCK_PANEL_TOOLBAR) {
    VkrUiPanelConfig toolbar_backdrop = vkr_ui_panel_config_default();
    toolbar_backdrop.placement = placement;
    toolbar_backdrop.style.background_color =
        (Vec4){0.025f, 0.034f, 0.050f, 0.94f};
    if (vkr_ui_panel_begin(ui, string8_lit("dock.toolbar.backdrop"),
                           &toolbar_backdrop))
      (void)vkr_ui_panel_end(ui);
    (void)vkr_ui_pop_id(ui);
    return;
  }

  const VkrUiTrack rows[] = {
      {.value = 28.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
  };
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig leaf = vkr_ui_panel_config_default();
  leaf.placement = placement;
  leaf.columns = &one_track;
  leaf.column_count = 1u;
  leaf.rows = rows;
  leaf.row_count = ArrayCount(rows);
  leaf.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("dock.leaf"), &leaf)) {
    VkrUiTrack tab_tracks[VKR_UI_DOCK_TAB_CAPACITY] = {0};
    for (uint32_t i = 0u; i < node->as.leaf.tab_count; ++i)
      tab_tracks[i] = (VkrUiTrack){.unit = VKR_UI_TRACK_AUTO};

    VkrUiPanelConfig tab_bar = vkr_ui_panel_config_default();
    tab_bar.placement = (VkrUiPlacement){
        .column = 0u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    tab_bar.columns = tab_tracks;
    tab_bar.column_count = node->as.leaf.tab_count;
    tab_bar.rows = &one_track;
    tab_bar.row_count = 1u;
    tab_bar.style.gap_pt = 2.0f;
    tab_bar.style.background_color = (Vec4){0.075f, 0.085f, 0.105f, 1.0f};
    if (vkr_ui_panel_begin(ui, string8_lit("tabs"), &tab_bar)) {
      for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
        const VkrUiDockTab *dock_tab = &node->as.leaf.tabs[tab];
        if (!vkr_ui_push_id_u64(ui, dock_tab->id))
          continue;
        VkrUiWidgetConfig button = vkr_ui_widget_config_default();
        button.placement = (VkrUiPlacement){
            .column = tab,
            .row = 0u,
            .column_span = 1u,
            .row_span = 1u,
            .justify = VKR_UI_ALIGN_STRETCH,
            .align = VKR_UI_ALIGN_STRETCH,
        };
        button.style.font_size_pt = 12.0f;
        button.style.padding_pt = (VkrUiEdges){3.0f, 8.0f, 3.0f, 8.0f};
        button.style.background_color =
            tab == node->as.leaf.active_tab
                ? (Vec4){0.16f, 0.18f, 0.22f, 1.0f}
                : (Vec4){0.095f, 0.105f, 0.125f, 1.0f};
        if (vkr_ui_button(ui, string8_lit("tab"),
                          vkr_ui_dock_panel_label(dock_tab->panel_kind),
                          &button)) {
          node->as.leaf.active_tab = tab;
          dock->revision++;
        }
        (void)vkr_ui_pop_id(ui);
      }
      (void)vkr_ui_panel_end(ui);
    }
    active_kind = node->as.leaf.tabs[node->as.leaf.active_tab].panel_kind;
    editor_dock_build_panel_content(ui, active_kind);
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_dock_build(VkrUiSystem *ui, VkrUiDockTree *dock) {
  const VkrUiPlacement placement = {
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  editor_dock_build_node(ui, dock, dock->root, placement);
}

#include "core/ui/vkr_ui_dock.h"

#include "core/vkr_json.h"
#include "math/vkr_math.h"

#include <math.h>

static bool8_t vkr_ui_dock_point_in_rect(int32_t x, int32_t y, VkrUiRect rect) {
  return (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.width &&
         (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.height;
}

static VkrUiDockTab vkr_ui_dock_tab(VkrUiDockPanelKind kind) {
  return (VkrUiDockTab){.id = (uint64_t)kind + 1u, .panel_kind = kind};
}

static void vkr_ui_dock_leaf(VkrUiDockTree *tree, uint32_t index,
                             uint32_t parent, VkrUiDockPanelKind kind) {
  tree->nodes[index] = (VkrUiDockNode){
      .kind = VKR_UI_DOCK_NODE_TABS,
      .parent = parent,
      .used = true_v,
      .as.leaf = {.tabs = {vkr_ui_dock_tab(kind)},
                  .tab_count = 1u,
                  .active_tab = 0u},
  };
}

static void vkr_ui_dock_split(VkrUiDockTree *tree, uint32_t index,
                              uint32_t parent, VkrUiDockSplitAxis axis,
                              float32_t ratio, uint32_t first,
                              uint32_t second) {
  tree->nodes[index] = (VkrUiDockNode){
      .kind = VKR_UI_DOCK_NODE_SPLIT,
      .parent = parent,
      .used = true_v,
      .as.split = {.axis = axis,
                   .ratio = ratio,
                   .first = first,
                   .second = second},
  };
}

void vkr_ui_dock_default_editor_layout(VkrUiDockTree *tree) {
  if (!tree)
    return;
  MemZero(tree, sizeof(*tree));
  tree->root = 0u;
  tree->node_high_water = 9u;
  tree->revision = 1u;
  tree->focused_tab_id = (uint64_t)VKR_UI_DOCK_PANEL_SCENE_VIEWPORT + 1u;
  tree->splitter_px = VKR_UI_DOCK_SPLITTER_PT;
  tree->tab_bar_px = VKR_UI_DOCK_TAB_BAR_PT;
  tree->interaction = (VkrUiDockInteraction){
      .tab_leaf = VKR_UI_DOCK_NODE_NONE,
      .resize_split = VKR_UI_DOCK_NODE_NONE,
      .drop_leaf = VKR_UI_DOCK_NODE_NONE,
  };
  vkr_ui_dock_split(tree, 0u, VKR_UI_DOCK_NODE_NONE, VKR_UI_DOCK_SPLIT_Y, 0.06f,
                    1u, 2u);
  vkr_ui_dock_leaf(tree, 1u, 0u, VKR_UI_DOCK_PANEL_TOOLBAR);
  vkr_ui_dock_split(tree, 2u, 0u, VKR_UI_DOCK_SPLIT_Y, 0.74468085f, 3u, 4u);
  vkr_ui_dock_split(tree, 3u, 2u, VKR_UI_DOCK_SPLIT_X, 0.18f, 5u, 6u);
  vkr_ui_dock_leaf(tree, 4u, 2u, VKR_UI_DOCK_PANEL_CONTENT);
  tree->nodes[4u].as.leaf.tabs[1u] = (VkrUiDockTab){
      .id = (uint64_t)VKR_UI_DOCK_PANEL_CONSOLE + 1u,
      .panel_kind = VKR_UI_DOCK_PANEL_CONSOLE};
  tree->nodes[4u].as.leaf.tab_count = 2u;
  vkr_ui_dock_leaf(tree, 5u, 3u, VKR_UI_DOCK_PANEL_HIERARCHY);
  vkr_ui_dock_split(tree, 6u, 3u, VKR_UI_DOCK_SPLIT_X, 0.7317073f, 7u, 8u);
  vkr_ui_dock_leaf(tree, 7u, 6u, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT);
  vkr_ui_dock_leaf(tree, 8u, 6u, VKR_UI_DOCK_PANEL_INSPECTOR);
}

static bool8_t vkr_ui_dock_validate_node(const VkrUiDockTree *tree,
                                         uint32_t index, uint32_t parent,
                                         uint32_t *visited,
                                         uint32_t *scene_count) {
  if (index >= tree->node_high_water || !tree->nodes[index].used ||
      (*visited & (1u << index)) != 0u)
    return false_v;
  *visited |= 1u << index;
  const VkrUiDockNode *node = &tree->nodes[index];
  if (node->parent != parent)
    return false_v;
  if (node->kind == VKR_UI_DOCK_NODE_SPLIT) {
    return node->as.split.axis <= VKR_UI_DOCK_SPLIT_Y &&
           isfinite(node->as.split.ratio) && node->as.split.ratio >= 0.05f &&
           node->as.split.ratio <= 0.95f &&
           vkr_ui_dock_validate_node(tree, node->as.split.first, index, visited,
                                     scene_count) &&
           vkr_ui_dock_validate_node(tree, node->as.split.second, index,
                                     visited, scene_count);
  }
  if (node->kind != VKR_UI_DOCK_NODE_TABS || node->as.leaf.tab_count == 0u ||
      node->as.leaf.tab_count > VKR_UI_DOCK_TAB_CAPACITY ||
      node->as.leaf.active_tab >= node->as.leaf.tab_count)
    return false_v;
  for (uint32_t i = 0u; i < node->as.leaf.tab_count; ++i) {
    const VkrUiDockTab tab = node->as.leaf.tabs[i];
    if (tab.id == 0u || tab.panel_kind >= VKR_UI_DOCK_PANEL_COUNT)
      return false_v;
    *scene_count += tab.panel_kind == VKR_UI_DOCK_PANEL_SCENE_VIEWPORT;
  }
  return true_v;
}

bool8_t vkr_ui_dock_validate(const VkrUiDockTree *tree) {
  if (!tree || tree->node_high_water == 0u ||
      tree->node_high_water > VKR_UI_DOCK_NODE_CAPACITY ||
      tree->root >= tree->node_high_water)
    return false_v;
  uint32_t visited = 0u;
  uint32_t scene_count = 0u;
  if (!vkr_ui_dock_validate_node(tree, tree->root, VKR_UI_DOCK_NODE_NONE,
                                 &visited, &scene_count) ||
      scene_count != 1u)
    return false_v;
  for (uint32_t i = 0u; i < tree->node_high_water; ++i)
    if (tree->nodes[i].used && (visited & (1u << i)) == 0u)
      return false_v;
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
      const uint64_t id = node->as.leaf.tabs[tab].id;
      for (uint32_t previous_node = 0u; previous_node <= i; ++previous_node) {
        const VkrUiDockNode *previous = &tree->nodes[previous_node];
        if (!previous->used || previous->kind != VKR_UI_DOCK_NODE_TABS)
          continue;
        const uint32_t previous_tab_count =
            previous_node == i ? tab : previous->as.leaf.tab_count;
        for (uint32_t previous_tab = 0u; previous_tab < previous_tab_count;
             ++previous_tab)
          if (previous->as.leaf.tabs[previous_tab].id == id)
            return false_v;
      }
    }
  }
  return true_v;
}

static bool8_t vkr_ui_dock_node_is_toolbar(const VkrUiDockNode *node) {
  return node->kind == VKR_UI_DOCK_NODE_TABS && node->as.leaf.tab_count == 1u &&
         node->as.leaf.tabs[0].panel_kind == VKR_UI_DOCK_PANEL_TOOLBAR;
}

static bool8_t vkr_ui_dock_split_has_toolbar(const VkrUiDockTree *tree,
                                             const VkrUiDockNode *split) {
  return vkr_ui_dock_node_is_toolbar(&tree->nodes[split->as.split.first]) ||
         vkr_ui_dock_node_is_toolbar(&tree->nodes[split->as.split.second]);
}

static Vec2 vkr_ui_dock_measure_minimum(const VkrUiDockTree *tree,
                                        uint32_t index, Vec2 *minimum) {
  const VkrUiDockNode *node = &tree->nodes[index];
  const float32_t scale = tree->tab_bar_px / VKR_UI_DOCK_TAB_BAR_PT;
  Vec2 size;
  if (node->kind == VKR_UI_DOCK_NODE_TABS) {
    size = (Vec2){
        ceilf(VKR_UI_DOCK_PANEL_MIN_WIDTH_PT * scale),
        vkr_ui_dock_node_is_toolbar(node)
            ? roundf(VKR_UI_DOCK_TOOLBAR_PT * scale)
            : tree->tab_bar_px +
                  ceilf(VKR_UI_DOCK_PANEL_MIN_CONTENT_HEIGHT_PT * scale)};
  } else {
    const Vec2 first =
        vkr_ui_dock_measure_minimum(tree, node->as.split.first, minimum);
    const Vec2 second =
        vkr_ui_dock_measure_minimum(tree, node->as.split.second, minimum);
    size = node->as.split.axis == VKR_UI_DOCK_SPLIT_X
               ? (Vec2){first.x + tree->splitter_px + second.x,
                        Max(first.y, second.y)}
               : (Vec2){Max(first.x, second.x),
                        first.y + tree->splitter_px + second.y};
  }
  minimum[index] = size;
  return size;
}

static float32_t vkr_ui_dock_first_extent(const VkrUiDockTree *tree,
                                          uint32_t index, float32_t available,
                                          float32_t ratio,
                                          const Vec2 *minimum) {
  const VkrUiDockNode *node = &tree->nodes[index];
  const Vec2 first = minimum[node->as.split.first];
  const Vec2 second = minimum[node->as.split.second];
  const bool8_t horizontal = node->as.split.axis == VKR_UI_DOCK_SPLIT_X;
  const float32_t first_min = horizontal ? first.x : first.y;
  const float32_t second_min = horizontal ? second.x : second.y;
  const float32_t minimum_sum = first_min + second_min;
  /* Undersized windows share the shortage; neither side disappears first. */
  if (available < minimum_sum)
    return Min(available, roundf(available * first_min / minimum_sum));
  if (!horizontal && vkr_ui_dock_split_has_toolbar(tree, node))
    return vkr_ui_dock_node_is_toolbar(&tree->nodes[node->as.split.first])
               ? first_min
               : available - second_min;
  return vkr_clamp_f32(roundf(available * ratio), first_min,
                       available - second_min);
}

static void vkr_ui_dock_layout_node(VkrUiDockTree *tree, uint32_t index,
                                    VkrUiRect rect, const Vec2 *minimum) {
  VkrUiDockNode *node = &tree->nodes[index];
  node->rect_px = rect;
  if (node->kind != VKR_UI_DOCK_NODE_SPLIT)
    return;
  const float32_t extent =
      node->as.split.axis == VKR_UI_DOCK_SPLIT_X ? rect.width : rect.height;
  const float32_t gap = Min(extent, tree->splitter_px);
  const float32_t available = Max(0.0f, extent - gap);
  const float32_t first_extent = vkr_ui_dock_first_extent(
      tree, index, available, node->as.split.ratio, minimum);
  VkrUiRect first = rect;
  VkrUiRect second = rect;
  if (node->as.split.axis == VKR_UI_DOCK_SPLIT_X) {
    first.width = first_extent;
    second.x += first_extent + gap;
    second.width = Max(0.0f, available - first_extent);
  } else {
    first.height = first_extent;
    second.y += first_extent + gap;
    second.height = Max(0.0f, available - first_extent);
  }
  vkr_ui_dock_layout_node(tree, node->as.split.first, first, minimum);
  vkr_ui_dock_layout_node(tree, node->as.split.second, second, minimum);
}

bool8_t vkr_ui_dock_layout(VkrUiDockTree *tree, VkrUiRect target_rect_px,
                           float32_t splitter_px, float32_t tab_bar_px) {
  if (!vkr_ui_dock_validate(tree) || !vkr_ui_rect_has_area(target_rect_px) ||
      !isfinite(splitter_px) || splitter_px < 0.0f || !isfinite(tab_bar_px) ||
      tab_bar_px < 0.0f)
    return false_v;
  tree->splitter_px = splitter_px;
  tree->tab_bar_px = tab_bar_px;
  Vec2 minimum[VKR_UI_DOCK_NODE_CAPACITY];
  (void)vkr_ui_dock_measure_minimum(tree, tree->root, minimum);
  vkr_ui_dock_layout_node(tree, tree->root, target_rect_px, minimum);
  return true_v;
}

bool8_t vkr_ui_dock_find_panel(const VkrUiDockTree *tree,
                               VkrUiDockPanelKind panel_kind,
                               uint32_t *out_leaf,
                               VkrUiRect *out_content_rect) {
  if (!tree || panel_kind >= VKR_UI_DOCK_PANEL_COUNT)
    return false_v;
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    const uint32_t tab = node->as.leaf.active_tab;
    if (node->as.leaf.tabs[tab].panel_kind == panel_kind) {
      if (out_leaf)
        *out_leaf = i;
      if (out_content_rect) {
        *out_content_rect = node->rect_px;
        const float32_t tab_height =
            Min(tree->tab_bar_px, out_content_rect->height);
        out_content_rect->y += tab_height;
        out_content_rect->height -= tab_height;
      }
      return true_v;
    }
  }
  return false_v;
}

String8 vkr_ui_dock_panel_label(VkrUiDockPanelKind panel_kind) {
  switch (panel_kind) {
  case VKR_UI_DOCK_PANEL_SCENE_VIEWPORT:
    return string8_lit("Scene");
  case VKR_UI_DOCK_PANEL_HIERARCHY:
    return string8_lit("Hierarchy");
  case VKR_UI_DOCK_PANEL_INSPECTOR:
    return string8_lit("Inspector");
  case VKR_UI_DOCK_PANEL_CONSOLE:
    return string8_lit("Console");
  case VKR_UI_DOCK_PANEL_TOOLBAR:
    return string8_lit("Toolbar");
  case VKR_UI_DOCK_PANEL_CONTENT:
    return string8_lit("Content");
  case VKR_UI_DOCK_PANEL_BAKERY:
    return string8_lit("Bakery");
  default:
    return string8_lit("Panel");
  }
}

bool8_t vkr_ui_dock_set_split_ratio(VkrUiDockTree *tree, uint32_t split,
                                    float32_t ratio) {
  if (!tree || split >= tree->node_high_water || !tree->nodes[split].used ||
      tree->nodes[split].kind != VKR_UI_DOCK_NODE_SPLIT || !isfinite(ratio) ||
      vkr_ui_dock_split_has_toolbar(tree, &tree->nodes[split]))
    return false_v;
  float32_t normalized = vkr_clamp_f32(ratio, 0.05f, 0.95f);
  const VkrUiDockNode *node = &tree->nodes[split];
  const float32_t available =
      (node->as.split.axis == VKR_UI_DOCK_SPLIT_X ? node->rect_px.width
                                                  : node->rect_px.height) -
      tree->splitter_px;
  if (available > 0.0f) {
    Vec2 minimum[VKR_UI_DOCK_NODE_CAPACITY];
    (void)vkr_ui_dock_measure_minimum(tree, split, minimum);
    normalized = vkr_clamp_f32(
        vkr_ui_dock_first_extent(tree, split, available, normalized, minimum) /
            available,
        0.05f, 0.95f);
  }
  if (tree->nodes[split].as.split.ratio == normalized)
    return true_v;
  tree->nodes[split].as.split.ratio = normalized;
  tree->revision++;
  return true_v;
}

static uint32_t vkr_ui_dock_allocate_node(VkrUiDockTree *tree) {
  for (uint32_t i = 0u; i < tree->node_high_water; ++i)
    if (!tree->nodes[i].used) {
      tree->nodes[i] = (VkrUiDockNode){.used = true_v};
      return i;
    }
  if (tree->node_high_water == VKR_UI_DOCK_NODE_CAPACITY)
    return VKR_UI_DOCK_NODE_NONE;
  const uint32_t index = tree->node_high_water++;
  tree->nodes[index] = (VkrUiDockNode){.used = true_v};
  return index;
}

static VkrUiDockTab vkr_ui_dock_remove_tab(VkrUiDockNode *leaf,
                                           uint32_t index) {
  const VkrUiDockTab tab = leaf->as.leaf.tabs[index];
  for (uint32_t i = index + 1u; i < leaf->as.leaf.tab_count; ++i)
    leaf->as.leaf.tabs[i - 1u] = leaf->as.leaf.tabs[i];
  leaf->as.leaf.tab_count--;
  if (leaf->as.leaf.tab_count == 0u)
    leaf->as.leaf.active_tab = 0u;
  else if (index < leaf->as.leaf.active_tab)
    leaf->as.leaf.active_tab--;
  else if (leaf->as.leaf.active_tab >= leaf->as.leaf.tab_count)
    leaf->as.leaf.active_tab = leaf->as.leaf.tab_count - 1u;
  return tab;
}

static void vkr_ui_dock_insert_tab(VkrUiDockNode *leaf, uint32_t index,
                                   VkrUiDockTab tab) {
  index = Min(index, leaf->as.leaf.tab_count);
  for (uint32_t i = leaf->as.leaf.tab_count; i > index; --i)
    leaf->as.leaf.tabs[i] = leaf->as.leaf.tabs[i - 1u];
  leaf->as.leaf.tabs[index] = tab;
  leaf->as.leaf.tab_count++;
  leaf->as.leaf.active_tab = index;
}

static void vkr_ui_dock_rewire_parent(VkrUiDockTree *tree, uint32_t parent,
                                      uint32_t old_child, uint32_t new_child) {
  if (parent == VKR_UI_DOCK_NODE_NONE) {
    tree->root = new_child;
    tree->nodes[new_child].parent = VKR_UI_DOCK_NODE_NONE;
    return;
  }
  VkrUiDockNode *node = &tree->nodes[parent];
  if (node->as.split.first == old_child)
    node->as.split.first = new_child;
  else
    node->as.split.second = new_child;
  tree->nodes[new_child].parent = parent;
}

static void vkr_ui_dock_collapse_empty_leaf(VkrUiDockTree *tree,
                                            uint32_t leaf_index) {
  VkrUiDockNode *leaf = &tree->nodes[leaf_index];
  if (leaf->as.leaf.tab_count != 0u || leaf_index == tree->root)
    return;
  const uint32_t parent = leaf->parent;
  VkrUiDockNode *split = &tree->nodes[parent];
  const uint32_t sibling = split->as.split.first == leaf_index
                               ? split->as.split.second
                               : split->as.split.first;
  const uint32_t grandparent = split->parent;
  vkr_ui_dock_rewire_parent(tree, grandparent, parent, sibling);
  *leaf = (VkrUiDockNode){0};
  *split = (VkrUiDockNode){0};
}

static uint32_t vkr_ui_dock_first_tab_leaf(const VkrUiDockTree *tree,
                                           uint32_t index) {
  while (tree->nodes[index].kind == VKR_UI_DOCK_NODE_SPLIT)
    index = tree->nodes[index].as.split.first;
  return index;
}

bool8_t vkr_ui_dock_move_tab(VkrUiDockTree *tree, uint32_t source_leaf,
                             uint32_t source_tab, uint32_t target_leaf,
                             uint32_t target_index,
                             VkrUiDockDropZone drop_zone) {
  if (!tree || source_leaf >= tree->node_high_water ||
      target_leaf >= tree->node_high_water || !tree->nodes[source_leaf].used ||
      !tree->nodes[target_leaf].used ||
      tree->nodes[source_leaf].kind != VKR_UI_DOCK_NODE_TABS ||
      tree->nodes[target_leaf].kind != VKR_UI_DOCK_NODE_TABS ||
      source_tab >= tree->nodes[source_leaf].as.leaf.tab_count ||
      drop_zone > VKR_UI_DOCK_DROP_BOTTOM)
    return false_v;
  if (drop_zone == VKR_UI_DOCK_DROP_CENTER) {
    if (source_leaf != target_leaf &&
        tree->nodes[target_leaf].as.leaf.tab_count == VKR_UI_DOCK_TAB_CAPACITY)
      return false_v;
    VkrUiDockTab tab =
        vkr_ui_dock_remove_tab(&tree->nodes[source_leaf], source_tab);
    if (source_leaf == target_leaf && target_index > source_tab)
      target_index--;
    vkr_ui_dock_insert_tab(&tree->nodes[target_leaf], target_index, tab);
    if (source_leaf != target_leaf)
      vkr_ui_dock_collapse_empty_leaf(tree, source_leaf);
    tree->focused_tab_id = tab.id;
    tree->revision++;
    return vkr_ui_dock_validate(tree);
  }
  if (source_leaf == target_leaf &&
      tree->nodes[source_leaf].as.leaf.tab_count == 1u)
    return false_v;
  const uint32_t new_leaf = vkr_ui_dock_allocate_node(tree);
  const uint32_t new_split = vkr_ui_dock_allocate_node(tree);
  if (new_leaf == VKR_UI_DOCK_NODE_NONE || new_split == VKR_UI_DOCK_NODE_NONE) {
    if (new_leaf != VKR_UI_DOCK_NODE_NONE)
      tree->nodes[new_leaf] = (VkrUiDockNode){0};
    return false_v;
  }
  const VkrUiDockTab tab =
      vkr_ui_dock_remove_tab(&tree->nodes[source_leaf], source_tab);
  vkr_ui_dock_collapse_empty_leaf(tree, source_leaf);
  const uint32_t target_parent = tree->nodes[target_leaf].parent;
  const bool8_t new_first =
      drop_zone == VKR_UI_DOCK_DROP_LEFT || drop_zone == VKR_UI_DOCK_DROP_TOP;
  const VkrUiDockSplitAxis axis =
      drop_zone == VKR_UI_DOCK_DROP_LEFT || drop_zone == VKR_UI_DOCK_DROP_RIGHT
          ? VKR_UI_DOCK_SPLIT_X
          : VKR_UI_DOCK_SPLIT_Y;
  vkr_ui_dock_leaf(tree, new_leaf, new_split, tab.panel_kind);
  tree->nodes[new_leaf].as.leaf.tabs[0] = tab;
  vkr_ui_dock_split(tree, new_split, target_parent, axis, 0.5f,
                    new_first ? new_leaf : target_leaf,
                    new_first ? target_leaf : new_leaf);
  tree->nodes[target_leaf].parent = new_split;
  vkr_ui_dock_rewire_parent(tree, target_parent, target_leaf, new_split);
  tree->focused_tab_id = tab.id;
  tree->revision++;
  return vkr_ui_dock_validate(tree);
}

bool8_t vkr_ui_dock_close_tab(VkrUiDockTree *tree, uint32_t leaf,
                              uint32_t tab) {
  if (!vkr_ui_dock_validate(tree) || leaf >= tree->node_high_water ||
      !tree->nodes[leaf].used ||
      tree->nodes[leaf].kind != VKR_UI_DOCK_NODE_TABS ||
      tab >= tree->nodes[leaf].as.leaf.tab_count ||
      tree->nodes[leaf].as.leaf.tabs[tab].panel_kind ==
          VKR_UI_DOCK_PANEL_SCENE_VIEWPORT ||
      (leaf == tree->root && tree->nodes[leaf].as.leaf.tab_count == 1u))
    return false_v;

  const VkrUiDockTab closed = vkr_ui_dock_remove_tab(&tree->nodes[leaf], tab);
  uint32_t focus_leaf = leaf;
  if (tree->nodes[leaf].as.leaf.tab_count == 0u) {
    const VkrUiDockNode *parent = &tree->nodes[tree->nodes[leaf].parent];
    focus_leaf = parent->as.split.first == leaf ? parent->as.split.second
                                                : parent->as.split.first;
    vkr_ui_dock_collapse_empty_leaf(tree, leaf);
  }
  if (tree->focused_tab_id == closed.id) {
    focus_leaf = vkr_ui_dock_first_tab_leaf(tree, focus_leaf);
    const VkrUiDockNode *focused = &tree->nodes[focus_leaf];
    tree->focused_tab_id =
        focused->as.leaf.tabs[focused->as.leaf.active_tab].id;
  }
  tree->revision++;
  return vkr_ui_dock_validate(tree);
}

VkrUiRect vkr_ui_dock_split_bar_rect(const VkrUiDockTree *tree,
                                     uint32_t split) {
  const VkrUiDockNode *node = &tree->nodes[split];
  const VkrUiRect first = tree->nodes[node->as.split.first].rect_px;
  const VkrUiRect second = tree->nodes[node->as.split.second].rect_px;
  if (node->as.split.axis == VKR_UI_DOCK_SPLIT_X)
    return (VkrUiRect){first.x + first.width, node->rect_px.y,
                       second.x - first.x - first.width, node->rect_px.height};
  return (VkrUiRect){node->rect_px.x, first.y + first.height,
                     node->rect_px.width, second.y - first.y - first.height};
}

static VkrUiRect vkr_ui_dock_splitter_rect(const VkrUiDockTree *tree,
                                           const VkrUiDockNode *node) {
  VkrUiRect rect =
      vkr_ui_dock_split_bar_rect(tree, (uint32_t)(node - tree->nodes));
  const float32_t hit_width =
      Max(tree->splitter_px, 8.0f * tree->tab_bar_px / VKR_UI_DOCK_TAB_BAR_PT);
  if (node->as.split.axis == VKR_UI_DOCK_SPLIT_X) {
    rect.x -= (hit_width - rect.width) * 0.5f;
    rect.width = hit_width;
  } else {
    rect.y -= (hit_width - rect.height) * 0.5f;
    rect.height = hit_width;
  }
  return rect;
}

VkrUiRect vkr_ui_dock_tab_rect(const VkrUiDockTree *tree, uint32_t leaf,
                               uint32_t tab) {
  const VkrUiDockNode *node = &tree->nodes[leaf];
  /* Compact widths include the category icon and label. A crowded stack
     divides available width proportionally; its clipped labels retain tooltips.
   */
  static const float32_t widths_pt[VKR_UI_DOCK_PANEL_COUNT] = {
      [VKR_UI_DOCK_PANEL_SCENE_VIEWPORT] = 88.0f,
      [VKR_UI_DOCK_PANEL_HIERARCHY] = 116.0f,
      [VKR_UI_DOCK_PANEL_INSPECTOR] = 116.0f,
      [VKR_UI_DOCK_PANEL_CONSOLE] = 102.0f,
      [VKR_UI_DOCK_PANEL_TOOLBAR] = 98.0f,
      [VKR_UI_DOCK_PANEL_CUSTOM] = 88.0f,
      [VKR_UI_DOCK_PANEL_BAKERY] = 104.0f,
      [VKR_UI_DOCK_PANEL_CONTENT] = 102.0f,
  };
  float32_t total = 0.0f;
  float32_t preceding = 0.0f;
  for (uint32_t i = 0u; i < node->as.leaf.tab_count; ++i) {
    const float32_t width = widths_pt[node->as.leaf.tabs[i].panel_kind];
    total += width;
    if (i < tab)
      preceding += width;
  }
  const float32_t scale = Min(tree->tab_bar_px / VKR_UI_DOCK_TAB_BAR_PT,
                              node->rect_px.width / total);
  const float32_t width = widths_pt[node->as.leaf.tabs[tab].panel_kind];
  const float32_t left = roundf(preceding * scale);
  const float32_t right = roundf((preceding + width) * scale);
  return (VkrUiRect){node->rect_px.x + left, node->rect_px.y, right - left,
                     Min(tree->tab_bar_px, node->rect_px.height)};
}

static bool8_t vkr_ui_dock_tab_at(const VkrUiDockTree *tree, int32_t x,
                                  int32_t y, uint32_t *out_leaf,
                                  uint32_t *out_tab) {
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (!node->used || node->kind != VKR_UI_DOCK_NODE_TABS)
      continue;
    for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
      if (node->as.leaf.tabs[tab].panel_kind == VKR_UI_DOCK_PANEL_TOOLBAR)
        continue;
      if (vkr_ui_dock_point_in_rect(x, y, vkr_ui_dock_tab_rect(tree, i, tab))) {
        *out_leaf = i;
        *out_tab = tab;
        return true_v;
      }
    }
  }
  return false_v;
}

static bool8_t vkr_ui_dock_leaf_at(const VkrUiDockTree *tree, int32_t x,
                                   int32_t y, uint32_t *out_leaf) {
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (node->used && node->kind == VKR_UI_DOCK_NODE_TABS &&
        node->as.leaf.tabs[node->as.leaf.active_tab].panel_kind !=
            VKR_UI_DOCK_PANEL_TOOLBAR &&
        vkr_ui_dock_point_in_rect(x, y, node->rect_px)) {
      *out_leaf = i;
      return true_v;
    }
  }
  return false_v;
}

static VkrUiDockDropZone vkr_ui_dock_drop_zone(VkrUiRect rect, int32_t x,
                                               int32_t y) {
  const float32_t nx = ((float32_t)x - rect.x) / Max(rect.width, 1.0f);
  const float32_t ny = ((float32_t)y - rect.y) / Max(rect.height, 1.0f);
  if (nx < 0.25f)
    return VKR_UI_DOCK_DROP_LEFT;
  if (nx > 0.75f)
    return VKR_UI_DOCK_DROP_RIGHT;
  if (ny < 0.25f)
    return VKR_UI_DOCK_DROP_TOP;
  if (ny > 0.75f)
    return VKR_UI_DOCK_DROP_BOTTOM;
  return VKR_UI_DOCK_DROP_CENTER;
}

static bool8_t vkr_ui_dock_point_over_chrome(const VkrUiDockTree *tree,
                                             int32_t x, int32_t y) {
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (!node->used)
      continue;
    if (node->kind == VKR_UI_DOCK_NODE_SPLIT &&
        vkr_ui_dock_point_in_rect(x, y, vkr_ui_dock_splitter_rect(tree, node)))
      return true_v;
    if (node->kind != VKR_UI_DOCK_NODE_TABS ||
        !vkr_ui_dock_point_in_rect(x, y, node->rect_px))
      continue;
    const VkrUiDockPanelKind active =
        node->as.leaf.tabs[node->as.leaf.active_tab].panel_kind;
    if (active != VKR_UI_DOCK_PANEL_SCENE_VIEWPORT ||
        (float32_t)y < node->rect_px.y + tree->tab_bar_px)
      return true_v;
  }
  return false_v;
}

VkrUiDockInputCapture vkr_ui_dock_update_input(VkrUiDockTree *tree,
                                               const InputState *input,
                                               bool8_t mouse_captured) {
  VkrUiDockInputCapture capture = {0};
  if (!tree || !input)
    return capture;
  if (mouse_captured || input_key_just_pressed(input, KEY_ESCAPE)) {
    tree->interaction = (VkrUiDockInteraction){
        .tab_leaf = VKR_UI_DOCK_NODE_NONE,
        .resize_split = VKR_UI_DOCK_NODE_NONE,
        .drop_leaf = VKR_UI_DOCK_NODE_NONE,
    };
    return capture;
  }
  const VkrUiRect root_rect = tree->nodes[tree->root].rect_px;
  const uint64_t revision = tree->revision;
  int32_t x = 0;
  int32_t y = 0;
  input_get_mouse_position((InputState *)input, &x, &y);
  capture.mouse = vkr_ui_dock_point_over_chrome(tree, x, y);
  const bool8_t pressed = input_button_just_pressed(input, BUTTON_LEFT);
  const bool8_t down = input_is_button_down((InputState *)input, BUTTON_LEFT);
  const bool8_t released = input_button_just_released(input, BUTTON_LEFT);
  const bool8_t had_interaction =
      tree->interaction.tab_leaf != VKR_UI_DOCK_NODE_NONE ||
      tree->interaction.resize_split != VKR_UI_DOCK_NODE_NONE;

  if (pressed) {
    int32_t press_x, press_y;
    input_get_button_press_position(input, BUTTON_LEFT, &press_x, &press_y);
    uint32_t focused_leaf = 0u;
    if (vkr_ui_dock_leaf_at(tree, press_x, press_y, &focused_leaf)) {
      const VkrUiDockNode *node = &tree->nodes[focused_leaf];
      tree->focused_tab_id = node->as.leaf.tabs[node->as.leaf.active_tab].id;
    }
    for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
      const VkrUiDockNode *node = &tree->nodes[i];
      if (node->used && node->kind == VKR_UI_DOCK_NODE_SPLIT &&
          !vkr_ui_dock_split_has_toolbar(tree, node) &&
          vkr_ui_dock_point_in_rect(press_x, press_y,
                                    vkr_ui_dock_splitter_rect(tree, node))) {
        tree->interaction.resize_split = i;
        tree->interaction.press_x = press_x;
        tree->interaction.press_y = press_y;
        const VkrUiRect first = tree->nodes[node->as.split.first].rect_px;
        const bool8_t horizontal = node->as.split.axis == VKR_UI_DOCK_SPLIT_X;
        const float32_t available =
            (horizontal ? node->rect_px.width : node->rect_px.height) -
            tree->splitter_px;
        tree->interaction.resize_press_ratio =
            (horizontal ? first.width : first.height) / Max(1.0f, available);
        capture.mouse = true_v;
        break;
      }
    }
    if (tree->interaction.resize_split == VKR_UI_DOCK_NODE_NONE) {
      uint32_t leaf = 0u;
      uint32_t tab = 0u;
      if (vkr_ui_dock_tab_at(tree, press_x, press_y, &leaf, &tab)) {
        tree->interaction.tab_leaf = leaf;
        tree->interaction.tab_index = tab;
        tree->interaction.press_x = press_x;
        tree->interaction.press_y = press_y;
        if (tree->nodes[leaf].as.leaf.active_tab != tab)
          tree->revision++;
        tree->nodes[leaf].as.leaf.active_tab = tab;
        tree->focused_tab_id = tree->nodes[leaf].as.leaf.tabs[tab].id;
        capture.mouse = true_v;
      }
    }
  }
  /* The release event can carry the last movement, including a whole gesture
     drained before this frame. Use its endpoint before clearing interaction. */
  if ((down || released) &&
      tree->interaction.resize_split != VKR_UI_DOCK_NODE_NONE) {
    VkrUiDockNode *split = &tree->nodes[tree->interaction.resize_split];
    const float32_t delta = split->as.split.axis == VKR_UI_DOCK_SPLIT_X
                                ? (float32_t)x - tree->interaction.press_x
                                : (float32_t)y - tree->interaction.press_y;
    const float32_t extent = split->as.split.axis == VKR_UI_DOCK_SPLIT_X
                                 ? split->rect_px.width
                                 : split->rect_px.height;
    if (delta != 0.0f)
      (void)vkr_ui_dock_set_split_ratio(
          tree, tree->interaction.resize_split,
          tree->interaction.resize_press_ratio +
              delta / Max(1.0f, extent - tree->splitter_px));
    capture.mouse = true_v;
    capture.resizing_split = true_v;
  }
  if ((down || released) &&
      tree->interaction.tab_leaf != VKR_UI_DOCK_NODE_NONE) {
    const int64_t dx = (int64_t)x - tree->interaction.press_x;
    const int64_t dy = (int64_t)y - tree->interaction.press_y;
    const float32_t threshold =
        4.0f * tree->tab_bar_px / VKR_UI_DOCK_TAB_BAR_PT;
    tree->interaction.dragging_tab |=
        (float64_t)dx * dx + (float64_t)dy * dy >= threshold * threshold;
    capture.mouse = true_v;
    capture.dragging_tab = tree->interaction.dragging_tab;
  }
  tree->interaction.drop_leaf = VKR_UI_DOCK_NODE_NONE;
  if (tree->interaction.dragging_tab) {
    uint32_t target = 0u;
    if (vkr_ui_dock_leaf_at(tree, x, y, &target)) {
      const VkrUiDockNode *node = &tree->nodes[target];
      VkrUiDockDropZone zone = vkr_ui_dock_drop_zone(node->rect_px, x, y);
      uint32_t insertion = node->as.leaf.tab_count;
      VkrUiRect preview = node->rect_px;
      if ((float32_t)y < node->rect_px.y + tree->tab_bar_px) {
        zone = VKR_UI_DOCK_DROP_CENTER;
        for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab) {
          const VkrUiRect rect = vkr_ui_dock_tab_rect(tree, target, tab);
          if ((float32_t)x < rect.x + rect.width * 0.5f) {
            insertion = tab;
            break;
          }
        }
        const uint32_t at = Min(insertion, node->as.leaf.tab_count - 1u);
        preview = vkr_ui_dock_tab_rect(tree, target, at);
        if (insertion == node->as.leaf.tab_count)
          preview.x += preview.width;
        preview.width =
            Max(2.0f, 2.0f * tree->tab_bar_px / VKR_UI_DOCK_TAB_BAR_PT);
      } else if (zone == VKR_UI_DOCK_DROP_LEFT ||
                 zone == VKR_UI_DOCK_DROP_RIGHT) {
        preview.width *= 0.5f;
        if (zone == VKR_UI_DOCK_DROP_RIGHT)
          preview.x += preview.width;
      } else if (zone == VKR_UI_DOCK_DROP_TOP ||
                 zone == VKR_UI_DOCK_DROP_BOTTOM) {
        preview.height *= 0.5f;
        if (zone == VKR_UI_DOCK_DROP_BOTTOM)
          preview.y += preview.height;
      }
      const bool8_t same_leaf = target == tree->interaction.tab_leaf;
      const bool8_t can_drop =
          zone == VKR_UI_DOCK_DROP_CENTER
              ? same_leaf || node->as.leaf.tab_count < VKR_UI_DOCK_TAB_CAPACITY
              : !same_leaf || node->as.leaf.tab_count > 1u;
      if (can_drop) {
        tree->interaction.drop_leaf = target;
        tree->interaction.drop_index = insertion;
        tree->interaction.drop_zone = zone;
        tree->interaction.drop_rect_px = preview;
      }
    }
  }
  if (released && tree->interaction.drop_leaf != VKR_UI_DOCK_NODE_NONE) {
    (void)vkr_ui_dock_move_tab(
        tree, tree->interaction.tab_leaf, tree->interaction.tab_index,
        tree->interaction.drop_leaf, tree->interaction.drop_index,
        tree->interaction.drop_zone);
  }
  if (released) {
    tree->interaction = (VkrUiDockInteraction){
        .tab_leaf = VKR_UI_DOCK_NODE_NONE,
        .resize_split = VKR_UI_DOCK_NODE_NONE,
        .drop_leaf = VKR_UI_DOCK_NODE_NONE,
    };
    capture.mouse |= had_interaction;
  }
  /* Keep presentation and picking on the mutation's current rectangles, even
     when dropping the last tab collapses and replaces the root node. */
  if (tree->revision != revision && vkr_ui_rect_has_area(root_rect))
    (void)vkr_ui_dock_layout(tree, root_rect, tree->splitter_px,
                             tree->tab_bar_px);
  return capture;
}

static bool8_t vkr_ui_dock_json_name(VkrJsonWriter *writer, const char *name) {
  return vkr_json_writer_name(
      writer,
      string8_create_from_cstr((const uint8_t *)name, string_length(name)));
}

static String8 vkr_ui_dock_kind_name(VkrUiDockNodeKind kind) {
  return kind == VKR_UI_DOCK_NODE_SPLIT ? string8_lit("split")
                                        : string8_lit("tabs");
}

static String8 vkr_ui_dock_panel_name(VkrUiDockPanelKind kind) {
  static const char *const names[VKR_UI_DOCK_PANEL_COUNT] = {
      "scene_viewport", "hierarchy", "inspector", "console",
      "toolbar",        "custom",    "bakery",    "content",
  };
  return string8_create_from_cstr((const uint8_t *)names[kind],
                                  string_length(names[kind]));
}

static bool8_t vkr_ui_dock_write_tab(VkrJsonWriter *writer, VkrUiDockTab tab) {
  char id[17];
  const int length =
      snprintf(id, sizeof(id), "%016llx", (unsigned long long)tab.id);
  return length == 16 && vkr_json_writer_begin_object(writer) &&
         vkr_ui_dock_json_name(writer, "id") &&
         vkr_json_writer_string(
             writer, string8_create((uint8_t *)id, (uint64_t)length)) &&
         vkr_ui_dock_json_name(writer, "panel") &&
         vkr_json_writer_string(writer,
                                vkr_ui_dock_panel_name(tab.panel_kind)) &&
         vkr_json_writer_end_object(writer);
}

bool8_t vkr_ui_dock_write_json(VkrJsonWriter *writer,
                               const VkrUiDockTree *tree) {
  if (!writer || !vkr_ui_dock_validate(tree))
    return false_v;
  if (!vkr_json_writer_begin_object(writer) ||
      !vkr_ui_dock_json_name(writer, "version") ||
      !vkr_json_writer_u64(writer, 1u) ||
      !vkr_ui_dock_json_name(writer, "root") ||
      !vkr_json_writer_u64(writer, tree->root) ||
      !vkr_ui_dock_json_name(writer, "nodes") ||
      !vkr_json_writer_begin_array(writer))
    return false_v;
  for (uint32_t i = 0u; i < tree->node_high_water; ++i) {
    const VkrUiDockNode *node = &tree->nodes[i];
    if (!vkr_json_writer_begin_object(writer) ||
        !vkr_ui_dock_json_name(writer, "used") ||
        !vkr_json_writer_bool(writer, node->used))
      return false_v;
    if (node->used) {
      if (!vkr_ui_dock_json_name(writer, "kind") ||
          !vkr_json_writer_string(writer, vkr_ui_dock_kind_name(node->kind)) ||
          !vkr_ui_dock_json_name(writer, "parent") ||
          !vkr_json_writer_i64(writer, node->parent == VKR_UI_DOCK_NODE_NONE
                                           ? -1
                                           : (int64_t)node->parent))
        return false_v;
      if (node->kind == VKR_UI_DOCK_NODE_SPLIT) {
        if (!vkr_ui_dock_json_name(writer, "axis") ||
            !vkr_json_writer_string(writer,
                                    node->as.split.axis == VKR_UI_DOCK_SPLIT_X
                                        ? string8_lit("x")
                                        : string8_lit("y")) ||
            !vkr_ui_dock_json_name(writer, "ratio") ||
            !vkr_json_writer_f64(writer, node->as.split.ratio) ||
            !vkr_ui_dock_json_name(writer, "first") ||
            !vkr_json_writer_u64(writer, node->as.split.first) ||
            !vkr_ui_dock_json_name(writer, "second") ||
            !vkr_json_writer_u64(writer, node->as.split.second))
          return false_v;
      } else {
        if (!vkr_ui_dock_json_name(writer, "active") ||
            !vkr_json_writer_u64(writer, node->as.leaf.active_tab) ||
            !vkr_ui_dock_json_name(writer, "tabs") ||
            !vkr_json_writer_begin_array(writer))
          return false_v;
        for (uint32_t tab = 0u; tab < node->as.leaf.tab_count; ++tab)
          if (!vkr_ui_dock_write_tab(writer, node->as.leaf.tabs[tab]))
            return false_v;
        if (!vkr_json_writer_end_array(writer))
          return false_v;
      }
    }
    if (!vkr_json_writer_end_object(writer))
      return false_v;
  }
  return vkr_json_writer_end_array(writer) &&
         vkr_json_writer_end_object(writer);
}

static bool8_t vkr_ui_dock_string_equals(String8 value, const char *expected) {
  const uint64_t length = string_length(expected);
  return value.length == length && MemCompare(value.str, expected, length) == 0;
}

static bool8_t vkr_ui_dock_parse_hex_id(String8 value, uint64_t *out_id) {
  if (!out_id || value.length != 16u)
    return false_v;
  uint64_t id = 0u;
  for (uint64_t i = 0u; i < value.length; ++i) {
    const uint8_t c = value.str[i];
    const uint8_t digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10u
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10u
                                                 : 0xffu;
    if (digit == 0xffu)
      return false_v;
    id = (id << 4u) | digit;
  }
  *out_id = id;
  return id != 0u;
}

static bool8_t vkr_ui_dock_parse_panel(String8 value,
                                       VkrUiDockPanelKind *out_kind) {
  for (uint32_t kind = 0u; kind < VKR_UI_DOCK_PANEL_COUNT; ++kind) {
    const String8 name = vkr_ui_dock_panel_name(kind);
    if (value.length == name.length &&
        MemCompare(value.str, name.str, name.length) == 0) {
      *out_kind = (VkrUiDockPanelKind)kind;
      return true_v;
    }
  }
  return false_v;
}

static bool8_t vkr_ui_dock_read_tabs(VkrJsonReader *node_reader,
                                     VkrUiDockNode *node) {
  VkrJsonReader tabs = *node_reader;
  if (!vkr_json_find_array(&tabs, "tabs"))
    return false_v;
  while (vkr_json_next_array_element(&tabs)) {
    if (node->as.leaf.tab_count == VKR_UI_DOCK_TAB_CAPACITY)
      return false_v;
    VkrJsonReader object = {0};
    if (!vkr_json_enter_object(&tabs, &object))
      return false_v;
    String8 id = {0};
    String8 panel = {0};
    VkrUiDockTab *tab = &node->as.leaf.tabs[node->as.leaf.tab_count++];
    if (!vkr_json_get_string(&object, "id", &id) ||
        !vkr_json_get_string(&object, "panel", &panel) ||
        !vkr_ui_dock_parse_hex_id(id, &tab->id) ||
        !vkr_ui_dock_parse_panel(panel, &tab->panel_kind))
      return false_v;
  }
  return node->as.leaf.tab_count > 0u;
}

bool8_t vkr_ui_dock_read_json(String8 json, VkrUiDockTree *out_tree) {
  if (!json.str || json.length == 0u || !out_tree)
    return false_v;
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  int32_t version = 0;
  int32_t root = -1;
  if (!vkr_json_get_int(&reader, "version", &version) || version != 1 ||
      !vkr_json_get_int(&reader, "root", &root) || root < 0)
    return false_v;
  VkrUiDockTree parsed = {0};
  parsed.root = (uint32_t)root;
  parsed.revision = 1u;
  parsed.splitter_px = VKR_UI_DOCK_SPLITTER_PT;
  parsed.tab_bar_px = VKR_UI_DOCK_TAB_BAR_PT;
  parsed.interaction = (VkrUiDockInteraction){
      .tab_leaf = VKR_UI_DOCK_NODE_NONE,
      .resize_split = VKR_UI_DOCK_NODE_NONE,
      .drop_leaf = VKR_UI_DOCK_NODE_NONE,
  };
  VkrJsonReader nodes = reader;
  if (!vkr_json_find_array(&nodes, "nodes"))
    return false_v;
  while (vkr_json_next_array_element(&nodes)) {
    if (parsed.node_high_water == VKR_UI_DOCK_NODE_CAPACITY)
      return false_v;
    VkrJsonReader object = {0};
    if (!vkr_json_enter_object(&nodes, &object))
      return false_v;
    VkrUiDockNode *node = &parsed.nodes[parsed.node_high_water++];
    if (!vkr_json_get_bool(&object, "used", &node->used))
      return false_v;
    if (!node->used)
      continue;
    String8 kind = {0};
    int32_t parent = -1;
    if (!vkr_json_get_string(&object, "kind", &kind) ||
        !vkr_json_get_int(&object, "parent", &parent) || parent < -1)
      return false_v;
    node->parent = parent < 0 ? VKR_UI_DOCK_NODE_NONE : (uint32_t)parent;
    if (vkr_ui_dock_string_equals(kind, "split")) {
      String8 axis = {0};
      int32_t first = -1;
      int32_t second = -1;
      node->kind = VKR_UI_DOCK_NODE_SPLIT;
      if (!vkr_json_get_string(&object, "axis", &axis) ||
          !vkr_json_get_float(&object, "ratio", &node->as.split.ratio) ||
          !vkr_json_get_int(&object, "first", &first) || first < 0 ||
          !vkr_json_get_int(&object, "second", &second) || second < 0)
        return false_v;
      node->as.split.axis = vkr_ui_dock_string_equals(axis, "x")
                                ? VKR_UI_DOCK_SPLIT_X
                            : vkr_ui_dock_string_equals(axis, "y")
                                ? VKR_UI_DOCK_SPLIT_Y
                                : (VkrUiDockSplitAxis)UINT32_MAX;
      node->as.split.first = (uint32_t)first;
      node->as.split.second = (uint32_t)second;
    } else if (vkr_ui_dock_string_equals(kind, "tabs")) {
      int32_t active = -1;
      node->kind = VKR_UI_DOCK_NODE_TABS;
      if (!vkr_json_get_int(&object, "active", &active) || active < 0 ||
          !vkr_ui_dock_read_tabs(&object, node))
        return false_v;
      node->as.leaf.active_tab = (uint32_t)active;
    } else {
      return false_v;
    }
  }
  if (!vkr_ui_dock_validate(&parsed))
    return false_v;
  *out_tree = parsed;
  return true_v;
}

bool8_t vkr_ui_dock_save_file(const VkrUiDockTree *tree, String8 path) {
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(&file, path))
    return false_v;
  if (vkr_ui_dock_write_json(&file.writer, tree) &&
      vkr_json_file_writer_commit(&file))
    return true_v;
  vkr_json_file_writer_abort(&file);
  return false_v;
}

bool8_t vkr_ui_dock_load_file(VkrUiDockTree *tree, String8 path) {
  if (!tree || !path.str || path.length == 0u ||
      path.length > VKR_JSON_WRITER_PATH_MAX ||
      memchr(path.str, '\0', path.length))
    return false_v;
  char cpath[VKR_JSON_WRITER_PATH_MAX + 1u];
  MemCopy(cpath, path.str, path.length);
  cpath[path.length] = '\0';
  FILE *file = fopen(cpath, "rb");
  if (!file)
    return false_v;
  uint8_t json[VKR_UI_DOCK_JSON_CAPACITY];
  const size_t length = fread(json, 1u, sizeof(json), file);
  const bool8_t complete = !ferror(file) && feof(file);
  fclose(file);
  return complete && length > 0u &&
         vkr_ui_dock_read_json(string8_create(json, length), tree);
}

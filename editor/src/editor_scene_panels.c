#include "editor_scene_panels.h"
#include "editor_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
#define NO_ROW UINT32_MAX
#define TREE_VISIBLE_SLOTS 96u

typedef struct EditorTreeNode {
  VkrEntityId entity;
  uint32_t parent;
  uint32_t child;
  uint32_t next;
  uint32_t depth;
  bool8_t expanded;
  bool8_t match;
} EditorTreeNode;

struct VkrEditorScenePanels {
  VkrAllocator *allocator;
  EditorTreeNode *nodes;
  uint32_t *rows;
  uint32_t capacity;
  uint32_t row_count;
  uint32_t live_count;
  uint32_t selected_row;
  uint64_t generation;
  uint64_t structure_revision;
  bool8_t rebuild;
  char search[192];
  float32_t hierarchy_scroll;
  VkrEntityId revealed;
  VkrEntityId inspecting;
  uint64_t edit_revision;
  uint64_t inspector_generation;
  VkrEntityId row_entities[TREE_VISIBLE_SLOTS];
  VkrEntityId pending_focus;
  uint8_t *long_name;
  uint32_t long_name_capacity;
  VkrSceneEditValues values;
  VkrSceneEditValues original_values;
  char numbers[18][48];
  char original_numbers[18][48];
  char error[128];
  bool8_t changed;
  float32_t inspector_scroll;
};

static VkrUiWidgetConfig widget_at(float32_t x, float32_t y, float32_t width,
                                   float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column = 0,
                                 .row = 0,
                                 .column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_START,
                                 .margin_pt = {y, 0, 0, x}};
  c.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  c.style.max_size_pt = c.style.min_size_pt;
  c.style.font_size_pt = 12.0f;
  c.style.padding_pt = (VkrUiEdges){3, 5, 3, 5};
  return c;
}

static bool8_t in_rect(VkrUiSystem *ui, VkrUiRect r) {
  return !ui->mouse_captured && ui->mouse_input_layer == ui->input_layer &&
         ui->mouse_x >= r.x && ui->mouse_x < r.x + r.width &&
         ui->mouse_y >= r.y && ui->mouse_y < r.y + r.height;
}

static bool8_t pressed(const InputState *input, Keys key) {
  return input_key_just_pressed(input, key);
}

static bool8_t contains(String8 text, const char *needle) {
  size_t n = strlen(needle);
  if (!n)
    return true_v;
  for (uint64_t i = 0; i + n <= text.length; ++i) {
    size_t j = 0;
    while (j < n && tolower((unsigned char)text.str[i + j]) ==
                        tolower((unsigned char)needle[j]))
      ++j;
    if (j == n)
      return true_v;
  }
  return false_v;
}

VkrEditorScenePanels *vkr_editor_scene_panels_create(VkrAllocator *allocator) {
  VkrEditorScenePanels *p =
      vkr_allocator_alloc(allocator, sizeof(*p), PANEL_TAG);
  if (p) {
    MemZero(p, sizeof(*p));
    p->allocator = allocator;
    p->rebuild = true_v;
  }
  return p;
}

void vkr_editor_scene_panels_destroy(VkrEditorScenePanels *p) {
  if (!p)
    return;
  if (p->nodes)
    vkr_allocator_free(p->allocator, p->nodes, p->capacity * sizeof(*p->nodes),
                       PANEL_TAG);
  if (p->rows)
    vkr_allocator_free(p->allocator, p->rows, p->capacity * sizeof(*p->rows),
                       PANEL_TAG);
  if (p->long_name)
    vkr_allocator_free(p->allocator, p->long_name, p->long_name_capacity,
                       PANEL_TAG);
  vkr_allocator_free(p->allocator, p, sizeof(*p), PANEL_TAG);
}

static bool8_t rebuild_tree(VkrEditorScenePanels *p,
                            const VkrSampleUiFrame *frame) {
  const VkrScene *s = frame->scene;
  uint32_t capacity = s->world->dir.capacity;
  bool8_t new_scene = p->generation != frame->scene_generation;
  if (capacity > p->capacity) {
    EditorTreeNode *nodes =
        vkr_allocator_alloc(p->allocator, capacity * sizeof(*nodes), PANEL_TAG);
    uint32_t *rows =
        vkr_allocator_alloc(p->allocator, capacity * sizeof(*rows), PANEL_TAG);
    if (!nodes || !rows) {
      if (nodes)
        vkr_allocator_free(p->allocator, nodes, capacity * sizeof(*nodes),
                           PANEL_TAG);
      if (rows)
        vkr_allocator_free(p->allocator, rows, capacity * sizeof(*rows),
                           PANEL_TAG);
      return false_v;
    }
    MemZero(nodes, capacity * sizeof(*nodes));
    if (p->nodes) {
      MemCopy(nodes, p->nodes, p->capacity * sizeof(*nodes));
      vkr_allocator_free(p->allocator, p->nodes, p->capacity * sizeof(*nodes),
                         PANEL_TAG);
      vkr_allocator_free(p->allocator, p->rows, p->capacity * sizeof(*rows),
                         PANEL_TAG);
    }
    p->nodes = nodes;
    p->rows = rows;
    p->capacity = capacity;
  }
  if (new_scene) {
    p->hierarchy_scroll = 0;
    p->revealed = VKR_ENTITY_ID_INVALID;
    p->pending_focus = VKR_ENTITY_ID_INVALID;
    MemZero(p->row_entities, sizeof(p->row_entities));
  }
  p->live_count = 0;
  uint32_t first_root = NO_ROW;
  /* Reverse insertion preserves the source/entity order within each sibling
   * list. */
  for (uint32_t i = capacity; i-- > 0;) {
    EditorTreeNode *n = &p->nodes[i];
    VkrEntityId id = vkr_entity_id_from_index(s->world, i);
    bool8_t expanded =
        !new_scene && n->entity.u64 == id.u64 ? n->expanded : false_v;
    *n = (EditorTreeNode){.parent = NO_ROW,
                          .child = NO_ROW,
                          .next = NO_ROW,
                          .expanded = expanded};
    // Directory capacity includes vacant slots. A reconstructed generation
    // alone does not prove liveness; occupied records own the live entities.
    if (!s->world->dir.records[i].chunk)
      continue;
    n->entity = id;
    ++p->live_count;
    const SceneTransform *tr =
        vkr_entity_get_component(s->world, id, s->comp_transform);
    if (tr && tr->parent.u64)
      n->parent = tr->parent.parts.index;
    n->match = contains(vkr_scene_get_name(s, id), p->search);
  }
  for (uint32_t i = capacity; i-- > 0;) {
    EditorTreeNode *n = &p->nodes[i];
    if (!n->entity.u64)
      continue;
    if (n->parent != NO_ROW) {
      n->next = p->nodes[n->parent].child;
      p->nodes[n->parent].child = i;
    } else {
      n->next = first_root;
      first_root = i;
    }
  }
  /* Source hierarchy was validated by the scene owner. Ancestors of matches
     remain visible; expanding filtered paths doesn't change saved expansion. */
  if (p->search[0]) {
    for (uint32_t i = 0; i < capacity; ++i) {
      if (!p->nodes[i].entity.u64 || !p->nodes[i].match)
        continue;
      uint32_t parent = p->nodes[i].parent;
      while (parent != NO_ROW && !p->nodes[parent].match) {
        p->nodes[parent].match = true_v;
        parent = p->nodes[parent].parent;
      }
    }
  }
  if (frame->selected_entity.u64 != p->revealed.u64 &&
      vkr_scene_entity_alive(s, frame->selected_entity)) {
    uint32_t parent = p->nodes[frame->selected_entity.parts.index].parent;
    while (parent != NO_ROW) {
      p->nodes[parent].expanded = true_v;
      parent = p->nodes[parent].parent;
    }
  }
  p->row_count = 0;
  uint32_t index = first_root, depth = 0;
  while (index != NO_ROW) {
    EditorTreeNode *n = &p->nodes[index];
    n->depth = depth;
    bool8_t visible = !p->search[0] || n->match;
    if (visible)
      p->rows[p->row_count++] = index;
    if (visible && n->child != NO_ROW && (n->expanded || p->search[0])) {
      index = n->child;
      depth++;
      continue;
    }
    while (n->next == NO_ROW && n->parent != NO_ROW) {
      index = n->parent;
      n = &p->nodes[index];
      depth--;
    }
    index = n->next;
  }
  p->generation = frame->scene_generation;
  p->structure_revision = s->structure_revision;
  p->rebuild = false_v;
  return true_v;
}

void vkr_editor_hierarchy_build(VkrEditorScenePanels *p,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading) {
  VkrUiSystem *ui = frame->ui;
  float32_t w = rect.width / ui->content_scale,
            h = rect.height / ui->content_scale;
  if (w < 32 || h < 60)
    return;
  VkrUiWidgetConfig c = widget_at(6, 5, w - 12, 24);
  c.tooltip = string8_lit("Search scene nodes (matches keep their ancestors)");
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer search = {(uint8_t *)p->search,
                                (uint32_t)strlen(p->search), sizeof(p->search)};
  p->rebuild |=
      vkr_ui_text_field(ui, string8_lit("hierarchy.search"), &search, &c);
  if (!p->search[0] &&
      ui->focused_id != vkr_ui_id_stack_widget_label(
                            &ui->id_stack, string8_lit("hierarchy.search"))) {
    c.style.background_color = (Vec4){0};
    c.style.border_color = (Vec4){0};
    c.style.text_color = (Vec4){0.67f, 0.72f, 0.77f, 1};
    vkr_ui_label(ui, string8_lit("hierarchy.search.hint"),
                 string8_lit("Search nodes"), &c);
  }
  if (!frame->scene) {
    c = widget_at(6, 36, w - 12, 40);
    vkr_ui_label(ui, string8_lit("no.scene"), string8_lit("No scene loaded."),
                 &c);
    c = widget_at(6, 80, w - 12, 28);
    vkr_editor_action_style(&c, heading);
    if (vkr_ui_button(ui, string8_lit("load.scene"), string8_lit("Load scene"),
                      &c))
      *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_LOAD};

    return;
  }
  if (p->rebuild || p->generation != frame->scene_generation ||
      p->structure_revision != frame->scene->structure_revision ||
      frame->selected_entity.u64 != p->revealed.u64) {
    if (!rebuild_tree(p, frame))
      return;
    p->selected_row = NO_ROW;
    for (uint32_t row = 0; row < p->row_count; ++row)
      if (p->nodes[p->rows[row]].entity.u64 == frame->selected_entity.u64) {
        p->selected_row = row;
        break;
      }
  }
  float32_t page = Max(0.0f, h - 58.0f);
  const uint32_t selected_row = p->selected_row;
  if (frame->selected_entity.u64 != p->revealed.u64) {
    if (selected_row != NO_ROW) {
      float32_t y = selected_row * 23.0f;
      if (y < p->hierarchy_scroll)
        p->hierarchy_scroll = y;
      if (y + 23 > p->hierarchy_scroll + page)
        p->hierarchy_scroll = Max(0.0f, y + 23 - page);
    }
    p->revealed = frame->selected_entity;
  }
  if (in_rect(ui, rect))
    p->hierarchy_scroll -= ui->mouse_wheel * 46.0f;
  p->hierarchy_scroll = vkr_clamp_f32(p->hierarchy_scroll, 0,
                                      Max(0.0f, p->row_count * 23.0f - page));
  uint32_t first = (uint32_t)(p->hierarchy_scroll / 23.0f);
  uint32_t end = Min(p->row_count, first + Min(TREE_VISIBLE_SLOTS,
                                               (uint32_t)(page / 23.0f) + 2u));
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  c = widget_at(0, 34, w, page);
  list.placement = c.placement;
  list.style.min_size_pt = c.style.min_size_pt;
  list.style.max_size_pt = c.style.max_size_pt;
  list.clip_children = true_v;
  const VkrUiTrack track = {.value = 1, .unit = VKR_UI_TRACK_FR};
  list.columns = &track;
  list.column_count = 1;
  const VkrUiTrack content_track = {.value = p->row_count * 23.0f,
                                    .unit = VKR_UI_TRACK_PX};
  list.rows = &content_track;
  list.row_count = 1;
  bool8_t navigated = false_v;
  VkrEntityId next_focus = VKR_ENTITY_ID_INVALID;
  if (page > 0 &&
      vkr_ui_scroll_area_begin(ui, string8_lit("hierarchy.rows"), &list)) {
    (void)vkr_ui_scroll_area_offset_set(ui, p->hierarchy_scroll);
    for (uint32_t row = first; row < end; ++row) {
      EditorTreeNode *n = &p->nodes[p->rows[row]];
      const uint32_t slot = row - first;
      const float32_t y = row * 23.0f;
      const float32_t visible_y = y - p->hierarchy_scroll;
      const float32_t indent = Min(n->depth * 13.0f, Max(0.0f, w - 75));
      (void)vkr_ui_push_id_u64(ui, slot);
      const VkrUiId node_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("node"));
      const VkrUiId expand_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("expand"));
      if (p->row_entities[slot].u64 != n->entity.u64) {
        if (ui->focused_id == node_id || ui->focused_id == expand_id)
          ui->focused_id = 0;
        if (ui->active_id == node_id || ui->active_id == expand_id)
          ui->active_id = 0;
        p->row_entities[slot] = n->entity;
      }
      if (p->pending_focus.u64 == n->entity.u64) {
        ui->focused_id = node_id;
        p->pending_focus = VKR_ENTITY_ID_INVALID;
      }
      c = widget_at(4 + indent, y, 20, 22);
      c.disabled = n->child == NO_ROW;
      c.tooltip = string8_lit("Expand or collapse node");
      if (n->child != NO_ROW &&
          vkr_ui_button(ui, string8_lit("expand"),
                        n->expanded ? string8_lit("-") : string8_lit("+"),
                        &c)) {
        n->expanded = !n->expanded;
        p->rebuild = true_v;
      }
      c = widget_at(25 + indent, y, w - 30 - indent, 22);
      c.style.text_color = (Vec4){0.88f, 0.9f, 0.93f, 1};
      if (selected_row == row)
        c.style.background_color = (Vec4){0.23f, 0.31f, 0.39f, 1};
      String8 name = vkr_scene_get_name(frame->scene, n->entity);
      if (!name.length)
        name = string8_lit("(unnamed)");
      c.tooltip = name;
      if (vkr_ui_button(ui, string8_lit("node"), (String8){0}, &c))
        *frame->scene_edit = (VkrSceneEditRequest){
            .action = VKR_SCENE_EDIT_SELECT, .entity = n->entity};
      uint64_t preview_length = Min(name.length, 160u);
      while (preview_length < name.length && preview_length &&
             (name.str[preview_length] & 0xc0u) == 0x80u)
        --preview_length;
      c.style.background_color = (Vec4){0};
      vkr_ui_label(ui, string8_lit("node.label"),
                   (String8){.str = name.str, .length = preview_length}, &c);
      if (!navigated && !ui->mouse_captured && visible_y + 22 > 0 &&
          visible_y < page && ui->focused_id == node_id &&
          ui->keyboard_input_layer == ui->input_layer) {
        VkrEntityId target = VKR_ENTITY_ID_INVALID;
        const int32_t direction = (int32_t)pressed(frame->input, KEY_DOWN) -
                                  (int32_t)pressed(frame->input, KEY_UP);
        if (direction && (int32_t)row + direction >= 0 &&
            (int32_t)row + direction < (int32_t)p->row_count)
          target = p->nodes[p->rows[(int32_t)row + direction]].entity;
        if (pressed(frame->input, KEY_RIGHT) && n->child != NO_ROW) {
          if (n->expanded || p->search[0]) {
            if (row + 1u < p->row_count &&
                p->nodes[p->rows[row + 1u]].depth > n->depth)
              target = p->nodes[p->rows[row + 1u]].entity;
          } else {
            n->expanded = true_v;
            p->rebuild = true_v;
          }
        }
        if (pressed(frame->input, KEY_LEFT)) {
          if (n->expanded && !p->search[0]) {
            n->expanded = false_v;
            p->rebuild = true_v;
          } else if (n->parent != NO_ROW)
            target = p->nodes[n->parent].entity;
        }
        if (target.u64) {
          *frame->scene_edit = (VkrSceneEditRequest){
              .action = VKR_SCENE_EDIT_SELECT, .entity = target};
          next_focus = target;
          navigated = true_v;
        }
      }
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
  if (next_focus.u64)
    p->pending_focus = next_focus;
  c = widget_at(5, h - 23, w - 10, 22);
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("count"),
               string8_create_formatted(ui->frame_allocator,
                                        "%u visible / %u nodes", p->row_count,
                                        p->live_count),
               &c);
}

static void inspector_read(VkrEditorScenePanels *p, const VkrSampleUiFrame *f) {
  (void)vkr_scene_edit_read(f->scene, f->selected_entity, &p->values);
  float32_t rotation[3];
  vkr_quat_to_euler(p->values.rotation, &rotation[0], &rotation[1],
                    &rotation[2]);
  float32_t values[18] = {p->values.position.x,      p->values.position.y,
                          p->values.position.z,      rotation[0] * 57.2957795f,
                          rotation[1] * 57.2957795f, rotation[2] * 57.2957795f,
                          p->values.scale.x,         p->values.scale.y,
                          p->values.scale.z};
  Vec3 color = {0};
  float32_t intensity = 0, range = 0;
  Vec3 direction = {0};
  if (p->values.fields & VKR_SCENE_EDIT_POINT_LIGHT) {
    color = p->values.point_light.color;
    intensity = p->values.point_light.intensity;
    range = p->values.point_light.range;
    direction = p->values.point_light.direction_local;
  } else if (p->values.fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) {
    color = p->values.directional_light.color;
    intensity = p->values.directional_light.intensity;
    direction = p->values.directional_light.direction_local;
  }
  values[9] = color.x;
  values[10] = color.y;
  values[11] = color.z;
  values[12] = intensity;
  values[13] = range;
  // Angles describe the local light ray: yaw zero faces -Z; elevation is +Y.
  const float32_t horizontal = hypotf(direction.x, direction.z);
  values[14] = atan2f(direction.x, -direction.z) * 57.2957795f;
  values[15] = atan2f(direction.y, horizontal) * 57.2957795f;
  values[16] = p->values.point_light.inner_cone_angle * 57.2957795f;
  values[17] = p->values.point_light.outer_cone_angle * 57.2957795f;
  for (uint32_t i = 0; i < 18; i++)
    snprintf(p->numbers[i], sizeof(p->numbers[i]), "%.7g", values[i]);
  const String8 full_name = vkr_scene_get_name(f->scene, f->selected_entity);
  const uint32_t name_capacity =
      !(p->values.fields & VKR_SCENE_EDIT_NAME) && full_name.length < UINT32_MAX
          ? (uint32_t)full_name.length + 1u
          : 0;
  if (p->long_name_capacity != name_capacity) {
    if (p->long_name)
      vkr_allocator_free(p->allocator, p->long_name, p->long_name_capacity,
                         PANEL_TAG);
    p->long_name = NULL;
    p->long_name_capacity = 0;
    if (name_capacity) {
      p->long_name =
          vkr_allocator_alloc(p->allocator, name_capacity, PANEL_TAG);
      if (p->long_name)
        p->long_name_capacity = name_capacity;
    }
  }
  if (p->long_name) {
    MemCopy(p->long_name, full_name.str, full_name.length);
    p->long_name[full_name.length] = 0;
  }
  p->original_values = p->values;
  MemCopy(p->original_numbers, p->numbers, sizeof(p->numbers));
  p->inspector_generation = f->scene_generation;
  p->inspecting = f->selected_entity;
  p->edit_revision = f->edits->revision;
  p->changed = false_v;
  p->error[0] = 0;
}

static bool8_t inspector_parse(VkrEditorScenePanels *p,
                               VkrSceneEditValues *out) {
  // Unedited values keep their exact authored representation. Displayed Euler
  // angles and rounded numeric strings are only an editing surface.
  *out = p->original_values;
  out->fields = 0;
  if ((p->original_values.fields & VKR_SCENE_EDIT_NAME) &&
      strcmp(p->values.name, p->original_values.name)) {
    MemCopy(out->name, p->values.name, sizeof(out->name));
    out->fields |= VKR_SCENE_EDIT_NAME;
  }
  if ((p->original_values.fields & VKR_SCENE_EDIT_VISIBILITY) &&
      (p->values.visibility.visible != p->original_values.visibility.visible ||
       p->values.visibility.inherit_parent !=
           p->original_values.visibility.inherit_parent)) {
    out->visibility = p->values.visibility;
    out->fields |= VKR_SCENE_EDIT_VISIBILITY;
  }
  float32_t v[18] = {0};
  bool8_t numeric_changed[18] = {0};
  for (uint32_t i = 0; i < 18; ++i) {
    if (!strcmp(p->numbers[i], p->original_numbers[i]))
      continue;
    char *end;
    v[i] = strtof(p->numbers[i], &end);
    if (end == p->numbers[i] || *end || !isfinite(v[i])) {
      snprintf(p->error, sizeof(p->error), "Enter finite numeric values.");
      return false_v;
    }
    numeric_changed[i] = v[i] != strtof(p->original_numbers[i], NULL);
  }
  if (p->original_values.fields & VKR_SCENE_EDIT_TRANSFORM) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (numeric_changed[axis])
        out->position.elements[axis] = v[axis];
      if (numeric_changed[6 + axis])
        out->scale.elements[axis] = v[6 + axis];
    }
    if (numeric_changed[3] || numeric_changed[4] || numeric_changed[5]) {
      for (uint32_t i = 3; i < 6; ++i)
        if (!numeric_changed[i])
          v[i] = strtof(p->original_numbers[i], NULL);
      out->rotation = vkr_quat_from_euler(
          v[3] * 0.0174532925f, v[4] * 0.0174532925f, v[5] * 0.0174532925f);
    }
    if (MemCompare(&out->position, &p->original_values.position,
                   sizeof(out->position)) ||
        MemCompare(&out->rotation, &p->original_values.rotation,
                   sizeof(out->rotation)) ||
        MemCompare(&out->scale, &p->original_values.scale, sizeof(out->scale)))
      out->fields |= VKR_SCENE_EDIT_TRANSFORM;
  }
  if (p->original_values.fields &
      (VKR_SCENE_EDIT_POINT_LIGHT | VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (numeric_changed[9 + axis]) {
        out->point_light.color.elements[axis] = v[9 + axis];
        out->directional_light.color.elements[axis] = v[9 + axis];
      }
    }
    if (numeric_changed[14] || numeric_changed[15]) {
      for (uint32_t i = 14; i < 16; ++i)
        if (!numeric_changed[i])
          v[i] = strtof(p->original_numbers[i], NULL);
      if (v[14] < -180 || v[14] > 180 || v[15] < -90 || v[15] > 90) {
        snprintf(p->error, sizeof(p->error),
                 "Yaw must be -180..180; elevation must be -90..90 degrees.");
        return false_v;
      }
      const float32_t yaw = v[14] * 0.0174532925f;
      const float32_t elevation = v[15] * 0.0174532925f;
      const Vec3 direction = {sinf(yaw) * cosf(elevation), sinf(elevation),
                              -cosf(yaw) * cosf(elevation)};
      out->point_light.direction_local = direction;
      out->directional_light.direction_local = direction;
    }
    if (numeric_changed[12])
      out->point_light.intensity = out->directional_light.intensity = v[12];
    if (numeric_changed[13])
      out->point_light.range = v[13];
    if (numeric_changed[16])
      out->point_light.inner_cone_angle = v[16] * 0.0174532925f;
    if (numeric_changed[17])
      out->point_light.outer_cone_angle = v[17] * 0.0174532925f;
    if ((numeric_changed[16] || numeric_changed[17]) &&
        (out->point_light.inner_cone_angle < 0 ||
         out->point_light.outer_cone_angle <
             out->point_light.inner_cone_angle ||
         out->point_light.outer_cone_angle > 1.5707964f ||
         !(cosf(out->point_light.inner_cone_angle) >
           cosf(out->point_light.outer_cone_angle)))) {
      snprintf(p->error, sizeof(p->error),
               "Use 0 <= inner < outer <= 90 degrees; increase the gap between "
               "cone angles.");
      return false_v;
    }
    out->point_light.casts_shadow = p->values.point_light.casts_shadow;
    if (out->point_light.casts_shadow &&
        (out->point_light.range <= 0.0f ||
         (out->point_light.kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT &&
          out->point_light.outer_cone_angle >= 1.57079632679f))) {
      snprintf(p->error, sizeof(p->error),
               "Shadows require positive range and a spot outer angle below 90 "
               "degrees.");
      return false_v;
    }
    out->point_light.enabled = p->values.point_light.enabled;
    out->directional_light.enabled = p->values.directional_light.enabled;
    if ((p->original_values.fields & VKR_SCENE_EDIT_POINT_LIGHT) &&
        MemCompare(&out->point_light, &p->original_values.point_light,
                   sizeof(out->point_light)))
      out->fields |= VKR_SCENE_EDIT_POINT_LIGHT;
    if ((p->original_values.fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) &&
        MemCompare(&out->directional_light,
                   &p->original_values.directional_light,
                   sizeof(out->directional_light)))
      out->fields |= VKR_SCENE_EDIT_DIRECTIONAL_LIGHT;
  }
  if (out->fields && !vkr_scene_edit_validate(out)) {
    snprintf(p->error, sizeof(p->error),
             "Scale must be nonzero; light values must be valid.");
    return false_v;
  }
  return true_v;
}

static void inspector_clear_focus(VkrUiSystem *ui) {
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.scroll"));
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.fields"));
  const char *labels[] = {"name",  "visibility", "inherit", "apply", "revert",
                          "frame", "undo",       "redo",    "save",
                          "light.point.enabled", "light.directional.enabled"};
  for (uint32_t i = 0; i < ArrayCount(labels); ++i) {
    VkrUiId id = vkr_ui_id_stack_widget_label(
        &ui->id_stack, string8_create((uint8_t *)labels[i], strlen(labels[i])));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
  }
  for (uint32_t i = 0; i < 18; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("value"));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
    id = vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("slider"));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
    id = vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("matrix.row"));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_pop_id(ui);
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_inspector_build(VkrEditorScenePanels *p,
                                const VkrSampleUiFrame *f, VkrUiRect rect,
                                VkrFontHandle heading) {
  VkrUiSystem *ui = f->ui;
  float32_t w = rect.width / ui->content_scale,
            h = rect.height / ui->content_scale;
  if (w < 32 || h < 24)
    return;
  VkrUiWidgetConfig c = widget_at(5, 5, w - 10, 25);
  if (!f->scene || !vkr_scene_entity_alive(f->scene, f->selected_entity)) {
    c.style.min_size_pt.y = 44;
    c.style.max_size_pt.y = 44;
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = Max(1.0f, w - 20.0f);
    vkr_ui_label(ui, string8_lit("select.prompt"),
                 string8_lit("Select a scene node."), &c);
    inspector_clear_focus(ui);
    if (p->long_name) {
      vkr_allocator_free(p->allocator, p->long_name, p->long_name_capacity,
                         PANEL_TAG);
      p->long_name = NULL;
      p->long_name_capacity = 0;
    }
    p->inspecting = VKR_ENTITY_ID_INVALID;
    return;
  }
  if (p->inspecting.u64 != f->selected_entity.u64 ||
      p->edit_revision != f->edits->revision ||
      p->inspector_generation != f->scene_generation) {
    const bool8_t selection_changed =
        p->inspecting.u64 != f->selected_entity.u64 ||
        p->inspector_generation != f->scene_generation;
    inspector_read(p, f);
    if (selection_changed) {
      p->inspector_scroll = 0;
      // Stable field IDs never transfer an active edit to the new selection.
      inspector_clear_focus(ui);
    }
  }
  const SceneTransform *tr = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_transform);
  if (!p->changed && tr && tr->trs_editable &&
      (MemCompare(&p->values.position, &tr->position, sizeof(Vec3)) ||
       MemCompare(&p->values.rotation, &tr->rotation, sizeof(VkrQuat)) ||
       MemCompare(&p->values.scale, &tr->scale, sizeof(Vec3))))
    inspector_read(p, f);
  float32_t content_height =
      5 + 27 + 29 + 27 + 30 + 26 + 9 * 26 + 29 + 30 + 44 + 88;
  const bool8_t point = (p->values.fields & VKR_SCENE_EDIT_POINT_LIGHT) != 0;
  const bool8_t directional =
      (p->values.fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) != 0;
  const bool8_t spot = point &&
      p->values.point_light.kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  const bool8_t light = point || directional;
  const bool8_t aimed = directional || spot;
  if (light)
    content_height += 26 + (point + directional) * 27 +
                      (4 + point + (aimed ? 4 : 0) + (spot ? 4 : 0)) * 26;
  if (!(p->values.fields & VKR_SCENE_EDIT_TRANSFORM))
    content_height -= 9 * 26;
  if (tr && !tr->trs_editable)
    content_height += 48 + 4 * 26;
  if (in_rect(ui, rect))
    p->inspector_scroll -= ui->mouse_wheel * 40.0f;
  p->inspector_scroll =
      vkr_clamp_f32(p->inspector_scroll, 0, Max(0.0f, content_height - h));
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  const VkrUiTrack content_track = {.value = content_height,
                                    .unit = VKR_UI_TRACK_PX};
  scroll.rows = &content_track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("inspector.scroll"), &scroll))
    return;
  (void)vkr_ui_scroll_area_offset_set(ui, p->inspector_scroll);
  float32_t y = 5;
  bool8_t field_focus = false_v;
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.fields"));
  c = widget_at(5, y, w - 10, 24);
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("entity"),
               string8_create_formatted(ui->frame_allocator,
                                        "Entity %u / generation %u",
                                        f->selected_entity.parts.index,
                                        f->selected_entity.parts.generation),
               &c);
  y += 27;
  c = widget_at(5, y, w - 10, 25);
  c.read_only = !(p->values.fields & VKR_SCENE_EDIT_NAME);
  vkr_editor_field_style(&c);
  c.tooltip = c.read_only
                  ? string8_lit("Original name exceeds editable capacity; "
                                "select and copy its full text")
                  : string8_lit("Node name (up to 511 UTF-8 bytes)");
  VkrUiTextEditBuffer name = {(uint8_t *)p->values.name,
                              (uint32_t)strlen(p->values.name),
                              sizeof(p->values.name)};
  if (p->long_name)
    name = (VkrUiTextEditBuffer){p->long_name, p->long_name_capacity - 1u,
                                 p->long_name_capacity};
  if (c.read_only && !p->long_name) {
    vkr_ui_label(ui, string8_lit("name.unavailable"),
                 string8_lit("Original name unavailable; editing disabled"),
                 &c);
  } else
    p->changed |= vkr_ui_text_field(ui, string8_lit("name"), &name, &c);
  field_focus |= y + 24 > p->inspector_scroll && y < h + p->inspector_scroll &&
                 ui->focused_id == vkr_ui_id_stack_widget_label(
                                       &ui->id_stack, string8_lit("name"));
  y += 29;
  c = widget_at(5, y, w - 10, 24);
  c.disabled = !(p->values.fields & VKR_SCENE_EDIT_VISIBILITY);
  p->changed |=
      vkr_ui_checkbox(ui, string8_lit("visibility"), string8_lit("Visible"),
                      &p->values.visibility.visible, &c);
  y += 27;
  c = widget_at(5, y, w - 10, 24);
  c.disabled = !(p->values.fields & VKR_SCENE_EDIT_VISIBILITY);
  p->changed |= vkr_ui_checkbox(ui, string8_lit("inherit"),
                                string8_lit("Inherit parent visibility"),
                                &p->values.visibility.inherit_parent, &c);
  y += 30;
  c = widget_at(5, y, w - 10, 24);
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("transform.title"),
               string8_lit("Local transform"), &c);
  y += 26;
  const char *labels[18] = {"Position X",       "Position Y",
                            "Position Z",       "Rotation X (deg)",
                            "Rotation Y (deg)", "Rotation Z (deg)",
                            "Scale X",          "Scale Y",
                            "Scale Z",          "Linear red",
                            "Linear green",     "Linear blue",
                            "Intensity",        "Range (0 = unlimited)",
                            "Local yaw (deg)",  "Local elevation (deg)",
                            "Inner cone (deg)", "Outer cone (deg)"};
  uint32_t total = light ? 18u : 9u;
  for (uint32_t i = 0; i < total; i++) {
    if (i < 9 && !(p->values.fields & VKR_SCENE_EDIT_TRANSFORM))
      continue;
    if (i == 9) {
      c = widget_at(5, y, w - 10, 24);
      c.text.font = heading;
      vkr_ui_label(ui, string8_lit("light.title"),
                   spot    ? string8_lit("Spot light")
                   : point ? string8_lit("Point light")
                           : string8_lit("Directional light"),
                   &c);
      y += 26;
      if (point) {
        c = widget_at(5, y, w - 10, 24);
        p->changed |= vkr_ui_checkbox(ui, string8_lit("light.point.enabled"),
                                      string8_lit("Light enabled"),
                                      &p->values.point_light.enabled, &c);
        y += 27;
        c = widget_at(5, y, w - 10, 24);
        p->changed |= vkr_ui_checkbox(ui, string8_lit("light.point.shadow"),
                                      string8_lit("Cast shadows"),
                                      &p->values.point_light.casts_shadow, &c);
        y += 27;
      }
      if (directional) {
        c = widget_at(5, y, w - 10, 24);
        p->changed |=
            vkr_ui_checkbox(ui, string8_lit("light.directional.enabled"),
                            string8_lit("Directional light enabled"),
                            &p->values.directional_light.enabled, &c);
        y += 27;
      }
    }
    if ((i == 13 && !point) || (i >= 14 && i < 16 && !aimed) ||
        (i >= 16 && !spot))
      continue;
    (void)vkr_ui_push_id_u64(ui, i);
    c = widget_at(5, y, w * 0.53f - 6, 24);
    vkr_ui_label(ui, string8_lit("label"),
                 string8_create((uint8_t *)labels[i], strlen(labels[i])), &c);
    c = widget_at(w * 0.53f, y, w * 0.47f - 6, 24);
    c.read_only = i < 9 && !(p->values.fields & VKR_SCENE_EDIT_TRANSFORM);
    vkr_editor_field_style(&c);
    VkrUiTextEditBuffer value = {(uint8_t *)p->numbers[i],
                                 (uint32_t)strlen(p->numbers[i]),
                                 sizeof(p->numbers[i])};
    p->changed |= vkr_ui_text_field(ui, string8_lit("value"), &value, &c);
    field_focus |=
        y + 24 > p->inspector_scroll && y < h + p->inspector_scroll &&
        ui->focused_id ==
            vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("value"));
    y += 26;
    if (i >= 14) {
      const float32_t minimum = i == 14 ? -180.0f : i == 15 ? -90.0f : 0.0f;
      const float32_t maximum = i == 14 ? 180.0f : 90.0f;
      char *end;
      float32_t angle = strtof(p->numbers[i], &end);
      const bool8_t valid = end != p->numbers[i] && !*end && isfinite(angle);
      c = widget_at(5, y, w - 11, 24);
      c.disabled = !valid;
      c.tooltip = i < 16
                      ? string8_lit("Local light direction; node rotation also applies")
                        : string8_lit("Cone half-angle; inner must be less than outer");
      // The slider owns only this draft string. Apply creates the undo entry.
      float32_t slider_angle = valid ? vkr_clamp_f32(angle, minimum, maximum) : 0;
      if (vkr_ui_slider_f32(ui, string8_lit("slider"), &slider_angle,
                            minimum, maximum, &c)) {
        snprintf(p->numbers[i], sizeof(p->numbers[i]), "%.7g", slider_angle);
        p->changed = true_v;
      }
      field_focus |=
          y + 24 > p->inspector_scroll && y < h + p->inspector_scroll &&
          ui->focused_id == vkr_ui_id_stack_widget_label(&ui->id_stack,
                                                         string8_lit("slider"));
      y += 26;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (tr && !tr->trs_editable) {
    c = widget_at(5, y, w - 10, 44);
    vkr_ui_label(
        ui, string8_lit("matrix.readonly"),
        string8_lit(
            "Authored local matrix (read-only).\nTRS editing is unavailable."),
        &c);
    y += 48;
    for (uint32_t row = 0; row < 4; ++row) {
      const Vec4 values = mat4_row(tr->local, (int32_t)row);
      String8 text = string8_create_formatted(
          ui->frame_allocator, "%.7g  %.7g  %.7g  %.7g", values.x, values.y,
          values.z, values.w);
      // Formatted String8 storage has a terminator; the field retains its own
      // copy.
      c = widget_at(5, y, w - 10, 24);
      c.read_only = true_v;
      vkr_editor_field_style(&c);
      VkrUiTextEditBuffer matrix = {text.str, (uint32_t)text.length,
                                    (uint32_t)text.length + 1u};
      (void)vkr_ui_push_id_u64(ui, row);
      (void)vkr_ui_text_field(ui, string8_lit("matrix.row"), &matrix, &c);
      (void)vkr_ui_pop_id(ui);
      y += 26;
    }
  }
  field_focus &=
      !ui->mouse_captured && ui->keyboard_input_layer == ui->input_layer;
  c = widget_at(5, y, w / 3 - 7, 25);
  vkr_editor_action_style(&c, heading);
  c.disabled = !p->changed;
  bool8_t apply =
      vkr_ui_button(ui, string8_lit("apply"), string8_lit("Apply"), &c);
  c = widget_at(w / 3, y, w / 3 - 5, 25);
  vkr_editor_action_style(&c, heading);
  if (vkr_ui_button(ui, string8_lit("revert"), string8_lit("Revert"), &c) ||
      (field_focus && pressed(f->input, KEY_ESCAPE)))
    inspector_read(p, f);
  c = widget_at(2 * w / 3, y, w / 3 - 5, 25);
  vkr_editor_action_style(&c, heading);
  if (vkr_ui_button(ui, string8_lit("frame"), string8_lit("Frame"), &c))
    *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_FRAME,
                                           .entity = f->selected_entity};
  if (apply || (field_focus && p->changed && pressed(f->input, KEY_ENTER))) {
    VkrSceneEditValues values;
    if (inspector_parse(p, &values)) {
      if (values.fields)
        *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                               .entity = f->selected_entity,
                                               .values = values};
      else
        inspector_read(p, f);
    }
  }
  y += 29;
  c = widget_at(5, y, w / 3 - 7, 25);
  vkr_editor_action_style(&c, heading);
  c.disabled = f->edits->undo_cursor == 0;
  if (vkr_ui_button(ui, string8_lit("undo"), string8_lit("Undo"), &c))
    f->scene_edit->action = VKR_SCENE_EDIT_UNDO;
  c = widget_at(w / 3, y, w / 3 - 5, 25);
  vkr_editor_action_style(&c, heading);
  c.disabled = f->edits->undo_cursor == f->edits->undo_count;
  if (vkr_ui_button(ui, string8_lit("redo"), string8_lit("Redo"), &c))
    f->scene_edit->action = VKR_SCENE_EDIT_REDO;
  c = widget_at(2 * w / 3, y, w / 3 - 5, 25);
  vkr_editor_action_style(&c, heading);
  if (vkr_ui_button(ui, string8_lit("save"), string8_lit("Save"), &c))
    f->scene_edit->action = VKR_SCENE_EDIT_SAVE;
  y += 30;
  c = widget_at(5, y, w - 10, 42);
  c.style.text_color = (Vec4){0.95f, 0.72f, 0.4f, 1};
  const char *status = p->error[0] ? p->error : f->edits->status;
  vkr_ui_label(ui, string8_lit("status"),
               (String8){.str = (uint8_t *)status, .length = strlen(status)},
               &c);
  y += 44;
  const SceneSourceIdentity *source = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_source_identity);
  if (source) {
    c = widget_at(5, y, w - 10, 84);
    const uint32_t indices[5] = {
        source->gltf_node_index, source->gltf_mesh_index,
        source->gltf_camera_index, source->gltf_skin_index,
        source->gltf_light_index};
    char index_text[5][16];
    for (uint32_t i = 0; i < 5; ++i) {
      if (indices[i] == UINT32_MAX)
        snprintf(index_text[i], sizeof(index_text[i]), "none");
      else
        snprintf(index_text[i], sizeof(index_text[i]), "%u", indices[i]);
    }
    vkr_ui_label(ui, string8_lit("source"),
                 string8_create_formatted(
                     ui->frame_allocator,
                     "Source entity: %u\nglTF node: %s / mesh: %s\nCamera: %s "
                     "/ skin: %s\nLight: %s",
                     source->scene_entity_index, index_text[0], index_text[1],
                     index_text[2], index_text[3], index_text[4]),
                 &c);
  }
  (void)vkr_ui_pop_id(ui);
  (void)vkr_ui_scroll_area_end(ui);
}

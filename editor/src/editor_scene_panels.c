#include "editor_scene_panels.h"
#include "core/vkr_json.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include "editor_projects.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_animation.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
#define PHYSICS_COLLIDER_STRIDE 14u
#define PHYSICS_ATTACHMENT_BASE                                                \
  (8u + VKR_SCENE_PHYSICS_MAX_COLLIDERS * PHYSICS_COLLIDER_STRIDE)
#define PHYSICS_JOINT_BASE (PHYSICS_ATTACHMENT_BASE + 6u)
#define PHYSICS_NUMBER_COUNT                                                   \
  (PHYSICS_JOINT_BASE + VKR_SCENE_PHYSICS_MAX_JOINTS * 22u)
#define NO_ROW UINT32_MAX
#define TREE_VISIBLE_SLOTS 96u
/** Transform, light and rectangle-size fields; 21 is the directional light's
 * colour temperature. */
#define INSPECTOR_NUMBER_COUNT 22u

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
  char numbers[INSPECTOR_NUMBER_COUNT][48];
  char original_numbers[INSPECTOR_NUMBER_COUNT][48];
  char physics_numbers[PHYSICS_NUMBER_COUNT][48];
  char physics_original_numbers[PHYSICS_NUMBER_COUNT][48];
  char impulse_numbers[6][48];
  bool8_t impulse_at_point;
  VkrUiId physics_focused_id;
  uint64_t open_collider;
  uint64_t open_joint;
  uint32_t preset_index;
  bool8_t show_collision_layers;
  bool8_t show_attachment;
  bool8_t show_joints;
  char bone_filter[64];
  char error[128];
  bool8_t changed;
  float32_t inspector_scroll;
  /* Measured content height of the previous Inspector build, in points. */
  float32_t inspector_height;
  /* Collapsed Inspector sections, indexed by InspectorSection. */
  bool8_t section_collapsed[8];
  bool8_t section_collapsed_init;
  /* Continuous edits (scrubs and slider drags) share one undo entry. */
  uint64_t gesture_counter;
  uint64_t gesture;
  VkrUiId gesture_owner;
  bool8_t gesture_active;
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

#define HIERARCHY_ROW_PT 24.0f
#define HIERARCHY_INDENT_PT 16.0f
#define HIERARCHY_LIST_TOP_PT 40.0f
#define HIERARCHY_FOOTER_PT 22.0f

/* Type icon and category hue for a hierarchy row, Godot-style: lights amber,
 * geometry blue, physics green, groups neutral. */
static VkrUiIcon hierarchy_entity_icon(const VkrScene *scene,
                                       VkrEntityId entity, bool8_t has_child,
                                       Vec4 *out_color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec4 light = {0.98f, 0.78f, 0.36f, 1.0f};
  const Vec4 geometry = {0.47f, 0.70f, 0.98f, 1.0f};
  const Vec4 physics = {0.45f, 0.84f, 0.56f, 1.0f};
  VkrWorld *world = scene->world;
  *out_color = light;
  if (vkr_entity_get_component(world, entity, scene->comp_directional_light))
    return VKR_UI_ICON_DIRECTIONAL_LIGHT;
  const ScenePointLight *point =
      vkr_entity_get_component(world, entity, scene->comp_point_light);
  if (point)
    return point->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
               ? VKR_UI_ICON_SPOT_LIGHT
               : VKR_UI_ICON_POINT_LIGHT;
  if (vkr_entity_get_component(world, entity, scene->comp_rectangle_light))
    return VKR_UI_ICON_RECT_LIGHT;
  *out_color = physics;
  if (scene->player_entity.u64 == entity.u64)
    return VKR_UI_ICON_PERSON_WALK;
  if (vkr_entity_get_component(world, entity, scene->comp_physics_body))
    return VKR_UI_ICON_RIGID_BODY;
  if (vkr_entity_get_component(world, entity, scene->comp_physics_collider))
    return VKR_UI_ICON_COLLIDER;
  *out_color = geometry;
  if (vkr_entity_get_component(world, entity, scene->comp_mesh_renderer))
    return VKR_UI_ICON_MESH;
  if (vkr_entity_get_component(world, entity, scene->comp_text3d))
    return VKR_UI_ICON_TEXT;
  if (vkr_entity_get_component(world, entity, scene->comp_shape))
    return VKR_UI_ICON_SHAPES;
  *out_color = theme->text_secondary;
  return has_child ? VKR_UI_ICON_FOLDER : VKR_UI_ICON_EMPTY;
}

/* Toggles one entity's own visibility through the undoable edit journal. */
void vkr_editor_toggle_visibility(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity) {
  VkrSceneEditValues values;
  if (!vkr_scene_edit_read(frame->scene, entity, &values) ||
      !(values.fields & VKR_SCENE_EDIT_VISIBILITY))
    return;
  values.fields = VKR_SCENE_EDIT_VISIBILITY;
  values.visibility.visible = !values.visibility.visible;
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
}

void vkr_editor_hierarchy_build(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorScenePanels *p = editor->scene_panels;
  VkrUiSystem *ui = frame->ui;
  float32_t w = rect.width / ui->content_scale,
            h = rect.height / ui->content_scale;
  if (w < 32 || h < 60)
    return;
  /* Toolbar: search with an Add action beside it. */
  const VkrUiTrack tool_columns[] = {{.value = 1, .unit = VKR_UI_TRACK_FR},
                                     {.value = 26, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig tools = vkr_ui_panel_config_default();
  tools.placement.column = tools.placement.row = 0u;
  tools.placement.justify = tools.placement.align = VKR_UI_ALIGN_START;
  tools.placement.margin_pt = (VkrUiEdges){7, 0, 0, 8};
  tools.columns = tool_columns;
  tools.column_count = ArrayCount(tool_columns);
  tools.style.gap_pt = theme->space_sm;
  tools.style.min_size_pt = tools.style.max_size_pt = (Vec2){w - 16, 26};
  if (vkr_ui_panel_begin(ui, string8_lit("hierarchy.tools"), &tools)) {
    VkrUiTextEditBuffer search = {
        (uint8_t *)p->search, (uint32_t)strlen(p->search), sizeof(p->search)};
    VkrUiPlacement placement = VKR_UI_PLACEMENT_DEFAULT;
    placement.column = placement.row = 0u;
    p->rebuild |= vkr_editor_search_field(
        ui, string8_lit("hierarchy.search"), &search, placement,
        string8_lit("Search"),
        string8_lit("Search scene nodes (matches keep their ancestors)"),
        VKR_FONT_HANDLE_INVALID);
    VkrUiWidgetConfig add = vkr_editor_icon_button_config(
        1u, 0u, VKR_UI_ICON_PLUS_CIRCLE,
        string8_lit("Add a model or light to the loaded, writable project "
                    "scene"));
    add.icon_color = theme->accent_hover;
    add.disabled =
        !vkr_editor_projects_can_add_entity(editor->projects, editor, frame);
    if (vkr_ui_button(ui, string8_lit("hierarchy.add"), (String8){0}, &add))
      vkr_editor_projects_add_entity(editor->projects, editor, frame);
    (void)vkr_ui_panel_end(ui);
  }
  VkrUiWidgetConfig c;
  if (!frame->scene) {
    c = widget_at(8, 64, w - 16, 40);
    c.style.text_color = theme->text_secondary;
    c.placement.justify = VKR_UI_ALIGN_CENTER;
    c.icon = VKR_UI_ICON_SCENE;
    c.icon_size_pt = 18.0f;
    c.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("no.scene"), string8_lit("No scene loaded"),
                 &c);
    c = widget_at(Max(8.0f, w * 0.5f - 60.0f), 108, 120, 28);
    vkr_editor_primary_style(&c, heading);
    c.icon = VKR_UI_ICON_SCENE_LOAD;
    c.icon_size_pt = 14.0f;
    c.disabled = frame->scene_loading;
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
  const float32_t row_h = HIERARCHY_ROW_PT;
  float32_t page =
      Max(0.0f, h - HIERARCHY_LIST_TOP_PT - HIERARCHY_FOOTER_PT - 2.0f);
  const uint32_t selected_row = p->selected_row;
  if (frame->selected_entity.u64 != p->revealed.u64) {
    if (selected_row != NO_ROW) {
      float32_t y = selected_row * row_h;
      if (y < p->hierarchy_scroll)
        p->hierarchy_scroll = y;
      if (y + row_h > p->hierarchy_scroll + page)
        p->hierarchy_scroll = Max(0.0f, y + row_h - page);
    }
    p->revealed = frame->selected_entity;
  }
  if (in_rect(ui, rect))
    p->hierarchy_scroll -= ui->mouse_wheel * row_h * 2.0f;
  p->hierarchy_scroll = vkr_clamp_f32(p->hierarchy_scroll, 0,
                                      Max(0.0f, p->row_count * row_h - page));
  uint32_t first = (uint32_t)(p->hierarchy_scroll / row_h);
  uint32_t end = Min(p->row_count, first + Min(TREE_VISIBLE_SLOTS,
                                               (uint32_t)(page / row_h) + 2u));
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  c = widget_at(0, HIERARCHY_LIST_TOP_PT, w, page);
  list.placement = c.placement;
  list.style.min_size_pt = c.style.min_size_pt;
  list.style.max_size_pt = c.style.max_size_pt;
  list.clip_children = true_v;
  const VkrUiTrack track = {.value = 1, .unit = VKR_UI_TRACK_FR};
  list.columns = &track;
  list.column_count = 1;
  const VkrUiTrack content_track = {.value = p->row_count * row_h,
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
      const float32_t y = row * row_h;
      const float32_t visible_y = y - p->hierarchy_scroll;
      const float32_t indent =
          Min(n->depth * HIERARCHY_INDENT_PT, Max(0.0f, w - 90));
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
      const bool8_t selected = selected_row == row;
      const SceneVisibility *visibility = vkr_entity_get_component(
          frame->scene->world, n->entity, frame->scene->comp_visibility);
      const bool8_t hidden = visibility && !visibility->visible;
      /* Full-width row: hover and selection fills. */
      c = widget_at(4, y, w - 8, row_h);
      c.style.corner_radius_pt =
          (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
      c.style.background_color = selected ? theme->selection : (Vec4){0};
      c.style.hover_background_color =
          selected ? theme->selection : theme->row_hover;
      String8 name = vkr_scene_get_name(frame->scene, n->entity);
      if (!name.length)
        name = string8_lit("(unnamed)");
      c.tooltip = name;
      if (vkr_ui_button(ui, string8_lit("node"), (String8){0}, &c))
        *frame->scene_edit = (VkrSceneEditRequest){
            .action = VKR_SCENE_EDIT_SELECT, .entity = n->entity};
      const bool8_t row_hot = ui->hot_id == node_id;
      /* Right click selects the row and opens its context menu. */
      if (row_hot && !ui->mouse_captured &&
          input_button_just_pressed(frame->input, BUTTON_RIGHT)) {
        *frame->scene_edit = (VkrSceneEditRequest){
            .action = VKR_SCENE_EDIT_SELECT, .entity = n->entity};
        editor->context_open = true_v;
        editor->context_kind = VKR_EDITOR_CONTEXT_ENTITY;
        editor->context_entity = n->entity;
        editor->context_position_pt =
            (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                   (float32_t)ui->mouse_y / ui->content_scale};
        editor->menu = VKR_EDITOR_MENU_NONE;
      }
      if (n->child != NO_ROW) {
        c = widget_at(6 + indent, y + 3, 18, 18);
        vkr_editor_ghost_style(&c);
        c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
        c.icon = n->expanded || p->search[0] ? VKR_UI_ICON_DISCLOSURE_OPEN
                                             : VKR_UI_ICON_DISCLOSURE_CLOSED;
        c.icon_size_pt = 10.0f;
        c.icon_color = selected ? theme->text : theme->text_secondary;
        c.tooltip = string8_lit("Expand or collapse (Left/Right arrows)");
        if (vkr_ui_button(ui, string8_lit("expand"), (String8){0}, &c)) {
          n->expanded = !n->expanded;
          p->rebuild = true_v;
        }
      }
      uint64_t preview_length = Min(name.length, 160u);
      while (preview_length < name.length && preview_length &&
             (name.str[preview_length] & 0xc0u) == 0x80u)
        --preview_length;
      Vec4 icon_color;
      /* Names use the eye button's space on rows that do not show it. */
      const bool8_t show_eye = visibility && (row_hot || hidden || selected);
      c = widget_at(26 + indent, y,
                    Max(10.0f, w - (show_eye ? 58.0f : 36.0f) - indent), row_h);
      c.placement.align = VKR_UI_ALIGN_START;
      c.style.padding_pt = (VkrUiEdges){4, 4, 4, 2};
      c.style.text_color = hidden     ? theme->text_disabled
                           : selected ? theme->text_on_accent
                                      : theme->text;
      c.icon = hierarchy_entity_icon(frame->scene, n->entity,
                                     n->child != NO_ROW, &icon_color);
      c.icon_size_pt = 14.0f;
      c.icon_color = selected ? theme->text_on_accent : icon_color;
      if (hidden)
        c.icon_color.w = 0.45f;
      c.tooltip = (String8){0};
      vkr_ui_label(ui, string8_lit("node.label"),
                   (String8){.str = name.str, .length = preview_length}, &c);
      if (show_eye) {
        c = widget_at(w - 32, y + 2, 20, 20);
        vkr_editor_ghost_style(&c);
        c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
        c.icon = hidden ? VKR_UI_ICON_EYE_SLASH : VKR_UI_ICON_EYE;
        c.icon_size_pt = 13.0f;
        c.icon_color = hidden     ? theme->text_disabled
                       : selected ? theme->text_on_accent
                                  : theme->text_secondary;
        c.tooltip = hidden ? string8_lit("Show (undoable)")
                           : string8_lit("Hide (undoable)");
        if (vkr_ui_button(ui, string8_lit("visibility"), (String8){0}, &c))
          vkr_editor_toggle_visibility(frame, n->entity);
      }
      if (!navigated && !ui->mouse_captured && visible_y + row_h > 0 &&
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
  c = widget_at(10, h - HIERARCHY_FOOTER_PT, w - 20, HIERARCHY_FOOTER_PT);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = theme->text_secondary;
  c.style.padding_pt = (VkrUiEdges){3, 0, 3, 0};
  vkr_ui_label(ui, string8_lit("count"),
               p->search[0]
                   ? string8_create_formatted(ui->frame_allocator,
                                              "%u matches \xc2\xb7 %u nodes",
                                              p->row_count, p->live_count)
                   : string8_create_formatted(ui->frame_allocator, "%u nodes",
                                              p->live_count),
               &c);
}

static void physics_numbers_read(VkrEditorScenePanels *p) {
  const VkrScenePhysicsSnapshot *body = &p->values.physics;
  float32_t numbers[PHYSICS_NUMBER_COUNT] = {body->mass,
                                             body->friction,
                                             body->restitution,
                                             body->gravity_factor,
                                             body->linear_damping,
                                             body->angular_damping,
                                             body->collision_layer,
                                             body->collision_mask};
  for (uint32_t i = 0; i < body->collider_count; ++i) {
    const VkrSceneColliderConfig *c = &body->colliders[i];
    float32_t *n = &numbers[8u + i * PHYSICS_COLLIDER_STRIDE];
    n[0] = c->position.x;
    n[1] = c->position.y;
    n[2] = c->position.z;
    vkr_quat_to_euler(c->rotation, &n[3], &n[4], &n[5]);
    for (uint32_t axis = 3; axis < 6; ++axis) {
      n[axis] *= 57.2957795f;
    }
    n[6] = c->half_extent.x;
    n[7] = c->half_extent.y;
    n[8] = c->half_extent.z;
    n[9] = c->radius;
    n[10] = c->half_height;
    n[11] = c->scale.x;
    n[12] = c->scale.y;
    n[13] = c->scale.z;
  }
  float32_t *attachment = &numbers[PHYSICS_ATTACHMENT_BASE];
  attachment[0] = body->attachment.position.x;
  attachment[1] = body->attachment.position.y;
  attachment[2] = body->attachment.position.z;
  vkr_quat_to_euler(body->attachment.rotation, &attachment[3], &attachment[4],
                    &attachment[5]);
  for (uint32_t i = 3; i < 6; ++i) {
    attachment[i] *= 57.2957795f;
  }
  for (uint32_t i = 0; i < body->joint_count; ++i) {
    const VkrSceneJointConfig *joint = &body->joints[i];
    const Vec3 vectors[] = {joint->anchor_a, joint->anchor_b, joint->axis_a,
                            joint->axis_b,   joint->normal_a, joint->normal_b};
    float32_t *values = &numbers[PHYSICS_JOINT_BASE + i * 22u];
    for (uint32_t j = 0; j < ArrayCount(vectors); ++j) {
      values[j * 3u] = vectors[j].x;
      values[j * 3u + 1u] = vectors[j].y;
      values[j * 3u + 2u] = vectors[j].z;
    }
    values[18] = joint->min_limit;
    values[19] = joint->max_limit;
    values[20] = joint->swing_normal_limit;
    values[21] = joint->swing_plane_limit;
  }
  for (uint32_t i = 0; i < PHYSICS_NUMBER_COUNT; ++i) {
    snprintf(p->physics_numbers[i], sizeof(p->physics_numbers[i]), "%.7g",
             numbers[i]);
  }
  MemCopy(p->physics_original_numbers, p->physics_numbers,
          sizeof(p->physics_numbers));
}

static bool8_t physics_numbers_parse(VkrEditorScenePanels *p,
                                     VkrScenePhysicsSnapshot *body) {
  for (uint32_t i = 0; i < 8u + body->collider_count * PHYSICS_COLLIDER_STRIDE;
       ++i) {
    if (!strcmp(p->physics_numbers[i], p->physics_original_numbers[i])) {
      continue;
    }
    char *end;
    float32_t value = strtof(p->physics_numbers[i], &end);
    if (end == p->physics_numbers[i] || *end || !isfinite(value)) {
      snprintf(p->error, sizeof(p->error),
               "Physics values must be finite numbers.");
      return false_v;
    }
    if (i < 6) {
      float32_t *fields[] = {&body->mass,           &body->friction,
                             &body->restitution,    &body->gravity_factor,
                             &body->linear_damping, &body->angular_damping};
      *fields[i] = value;
      continue;
    }
    if (i < 8) {
      if (value < 0 || value > UINT16_MAX || floorf(value) != value) {
        snprintf(p->error, sizeof(p->error),
                 "Collision bits must be integers from 0 to 65535.");
        return false_v;
      }
      if (i == 6) {
        body->collision_layer = (uint16_t)value;
      } else {
        body->collision_mask = (uint16_t)value;
      }
      continue;
    }
    VkrSceneColliderConfig *c =
        &body->colliders[(i - 8u) / PHYSICS_COLLIDER_STRIDE];
    const uint32_t field = (i - 8u) % PHYSICS_COLLIDER_STRIDE;
    if (field < 3) {
      (&c->position.x)[field] = value;
    } else if (field < 6) {
      /* Convert all edited Euler axes together below. */
    } else if (field < 9) {
      (&c->half_extent.x)[field - 6u] = value;
    } else if (field == 9) {
      c->radius = value;
    } else if (field == 10) {
      c->half_height = value;
    } else {
      (&c->scale.x)[field - 11u] = value;
    }
  }
  for (uint32_t i = 0; i < body->collider_count; ++i) {
    const uint32_t offset = 8u + i * PHYSICS_COLLIDER_STRIDE + 3u;
    bool8_t changed = false_v;
    float32_t angles[3];
    vkr_quat_to_euler(body->colliders[i].rotation, &angles[0], &angles[1],
                      &angles[2]);
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (strcmp(p->physics_numbers[offset + axis],
                 p->physics_original_numbers[offset + axis])) {
        angles[axis] =
            strtof(p->physics_numbers[offset + axis], NULL) * 0.0174532925f;
        changed = true_v;
      }
    }
    if (changed) {
      body->colliders[i].rotation =
          vkr_quat_from_euler(angles[0], angles[1], angles[2]);
    }
  }
  float32_t attachment_angles[3];
  vkr_quat_to_euler(body->attachment.rotation, &attachment_angles[0],
                    &attachment_angles[1], &attachment_angles[2]);
  bool8_t attachment_rotation_changed = false_v;
  for (uint32_t i = PHYSICS_ATTACHMENT_BASE;
       i < PHYSICS_JOINT_BASE + body->joint_count * 22u; ++i) {
    if (!strcmp(p->physics_numbers[i], p->physics_original_numbers[i])) {
      continue;
    }
    char *end;
    const float32_t value = strtof(p->physics_numbers[i], &end);
    if (end == p->physics_numbers[i] || *end || !isfinite(value)) {
      snprintf(p->error, sizeof(p->error),
               "Attachment and joint values must be finite.");
      return false_v;
    }
    if (i < PHYSICS_JOINT_BASE) {
      const uint32_t field = i - PHYSICS_ATTACHMENT_BASE;
      if (field < 3) {
        (&body->attachment.position.x)[field] = value;
      } else {
        attachment_angles[field - 3u] = value * 0.0174532925f;
        attachment_rotation_changed = true_v;
      }
      continue;
    }
    VkrSceneJointConfig *joint = &body->joints[(i - PHYSICS_JOINT_BASE) / 22u];
    const uint32_t field = (i - PHYSICS_JOINT_BASE) % 22u;
    if (field < 18) {
      Vec3 *vectors[] = {&joint->anchor_a, &joint->anchor_b, &joint->axis_a,
                         &joint->axis_b,   &joint->normal_a, &joint->normal_b};
      (&vectors[field / 3u]->x)[field % 3u] = value;
    } else {
      float32_t *limits[] = {&joint->min_limit, &joint->max_limit,
                             &joint->swing_normal_limit,
                             &joint->swing_plane_limit};
      *limits[field - 18u] = value;
    }
  }
  if (attachment_rotation_changed) {
    body->attachment.rotation = vkr_quat_from_euler(
        attachment_angles[0], attachment_angles[1], attachment_angles[2]);
  }
  return true_v;
}

static void inspector_read(VkrEditorScenePanels *p, const VkrSampleUiFrame *f) {
  if (p->physics_focused_id && f->ui->focused_id == p->physics_focused_id) {
    f->ui->focused_id = 0;
  }
  if (p->physics_focused_id && f->ui->active_id == p->physics_focused_id) {
    f->ui->active_id = 0;
  }
  p->physics_focused_id = 0;
  (void)vkr_scene_edit_read(f->scene, f->selected_entity, &p->values);
  physics_numbers_read(p);
  for (uint32_t i = 0; i < 6; ++i) {
    snprintf(p->impulse_numbers[i], sizeof(p->impulse_numbers[i]), "%u",
             i == 1 ? 5u : 0u);
  }
  float32_t rotation[3];
  vkr_quat_to_euler(p->values.rotation, &rotation[0], &rotation[1],
                    &rotation[2]);
  float32_t values[INSPECTOR_NUMBER_COUNT] = {
      p->values.position.x,      p->values.position.y,
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
  } else if (p->values.fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) {
    color = p->values.rectangle_light.color;
    intensity = p->values.rectangle_light.radiance;
    values[19] = p->values.rectangle_light.size.x;
    values[20] = p->values.rectangle_light.size.y;
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
  values[18] = p->values.directional_light.sun_angular_diameter_degrees;
  values[21] = p->values.directional_light.temperature_kelvin;
  for (uint32_t i = 0; i < INSPECTOR_NUMBER_COUNT; i++)
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
  float32_t v[INSPECTOR_NUMBER_COUNT] = {0};
  bool8_t numeric_changed[INSPECTOR_NUMBER_COUNT] = {0};
  for (uint32_t i = 0; i < INSPECTOR_NUMBER_COUNT; ++i) {
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
      (VKR_SCENE_EDIT_POINT_LIGHT | VKR_SCENE_EDIT_DIRECTIONAL_LIGHT |
       VKR_SCENE_EDIT_RECTANGLE_LIGHT)) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (numeric_changed[9 + axis]) {
        out->point_light.color.elements[axis] = v[9 + axis];
        out->directional_light.color.elements[axis] = v[9 + axis];
        out->rectangle_light.color.elements[axis] = v[9 + axis];
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
    if (numeric_changed[12])
      out->rectangle_light.radiance = v[12];
    if (numeric_changed[13])
      out->point_light.range = v[13];
    if (numeric_changed[16])
      out->point_light.inner_cone_angle = v[16] * 0.0174532925f;
    if (numeric_changed[17])
      out->point_light.outer_cone_angle = v[17] * 0.0174532925f;
    if (numeric_changed[18]) {
      if (v[18] < 0.0f || v[18] >= 180.0f) {
        snprintf(p->error, sizeof(p->error),
                 "Sun angular diameter must be in [0, 180) degrees.");
        return false_v;
      }
      out->directional_light.sun_angular_diameter_degrees = v[18];
    }
    if (numeric_changed[21]) {
      if (v[21] != 0.0f && (v[21] < VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K ||
                            v[21] > VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K)) {
        snprintf(p->error, sizeof(p->error),
                 "Temperature must be 0 or 1000..40000 K.");
        return false_v;
      }
      out->directional_light.temperature_kelvin = v[21];
    }
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
    out->directional_light.atmosphere_sun =
        p->values.directional_light.atmosphere_sun;
    out->rectangle_light.enabled = p->values.rectangle_light.enabled;
    if ((p->original_values.fields & VKR_SCENE_EDIT_POINT_LIGHT) &&
        MemCompare(&out->point_light, &p->original_values.point_light,
                   sizeof(out->point_light)))
      out->fields |= VKR_SCENE_EDIT_POINT_LIGHT;
    if ((p->original_values.fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) &&
        MemCompare(&out->directional_light,
                   &p->original_values.directional_light,
                   sizeof(out->directional_light)))
      out->fields |= VKR_SCENE_EDIT_DIRECTIONAL_LIGHT;
    if (numeric_changed[19])
      out->rectangle_light.size.x = v[19];
    if (numeric_changed[20])
      out->rectangle_light.size.y = v[20];
    if ((p->original_values.fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) &&
        MemCompare(&out->rectangle_light, &p->original_values.rectangle_light,
                   sizeof(out->rectangle_light)))
      out->fields |= VKR_SCENE_EDIT_RECTANGLE_LIGHT;
  }
  out->physics = p->values.physics;
  if (!physics_numbers_parse(p, &out->physics)) {
    return false_v;
  }
  if (MemCompare(&out->physics, &p->original_values.physics,
                 sizeof(out->physics))) {
    out->fields |= VKR_SCENE_EDIT_PHYSICS;
  }
  const char *physics_error = NULL;
  if ((out->fields & VKR_SCENE_EDIT_PHYSICS) &&
      !vkr_scene_physics_snapshot_validate(&out->physics, &physics_error)) {
    snprintf(p->error, sizeof(p->error), "%s",
             physics_error ? physics_error : "Invalid physics values.");
    return false_v;
  }
  if (out->fields && !vkr_scene_edit_validate(out)) {
    snprintf(p->error, sizeof(p->error),
             "Invalid transform, light or physics values.");
    return false_v;
  }
  return true_v;
}

static void inspector_clear_focus(VkrUiSystem *ui) {
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.scroll"));
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.fields"));
  const char *labels[] = {"name",
                          "visibility",
                          "inherit",
                          "apply",
                          "revert",
                          "frame",
                          "undo",
                          "redo",
                          "save",
                          "light.point.enabled",
                          "light.directional.enabled",
                          "light.directional.atmosphere_sun",
                          "light.rectangle.enabled"};
  for (uint32_t i = 0; i < ArrayCount(labels); ++i) {
    VkrUiId id = vkr_ui_id_stack_widget_label(
        &ui->id_stack, string8_create((uint8_t *)labels[i], strlen(labels[i])));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
  }
  for (uint32_t i = 0; i < INSPECTOR_NUMBER_COUNT; ++i) {
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

static bool8_t physics_number_widget(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                     float32_t w, float32_t *y,
                                     const char *label, uint32_t index,
                                     bool8_t disabled) {
  (void)vkr_ui_push_id_u64(ui, index);
  VkrUiWidgetConfig c = widget_at(5, *y, w * 0.53f - 6, 24);
  vkr_ui_label(ui, string8_lit("label"),
               string8_create((uint8_t *)label, strlen(label)), &c);
  c = widget_at(w * 0.53f, *y, w * 0.47f - 6, 24);
  c.disabled = disabled;
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer buffer = {(uint8_t *)p->physics_numbers[index],
                                (uint32_t)strlen(p->physics_numbers[index]),
                                sizeof(p->physics_numbers[index])};
  p->changed |= vkr_ui_text_field(ui, string8_lit("number"), &buffer, &c);
  bool8_t focused = ui->focused_id == vkr_ui_id_stack_widget_label(
                                          &ui->id_stack, string8_lit("number"));
  if (focused) {
    p->physics_focused_id = ui->focused_id;
  }
  (void)vkr_ui_pop_id(ui);
  *y += 26;
  return focused;
}

static uint64_t physics_next_collider_id(const VkrScenePhysicsSnapshot *body) {
  /* At most 32 IDs are live. Searching this bounded range avoids overflow
     when a loaded collider uses UINT64_MAX. */
  for (uint64_t candidate = 1;
       candidate <= VKR_SCENE_PHYSICS_MAX_COLLIDERS + 1u; ++candidate) {
    bool8_t used = false_v;
    for (uint32_t i = 0; i < body->collider_count; ++i) {
      used |= body->colliders[i].authored_id == candidate;
    }
    if (!used) {
      return candidate;
    }
  }
  return 0;
}

static bool8_t physics_fit_collider(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f,
                                    VkrSceneColliderConfig *collider) {
  const SceneTransform *root = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_transform);
  if (!root || !f->assets) {
    return false_v;
  }
  const Mat4 inverse = mat4_inverse_affine(root->world);
  Vec3 lower = {0}, upper = {0};
  bool8_t found = false_v;
  for (uint32_t i = 0; i < f->scene->topo_count; ++i) {
    VkrEntityId candidate = f->scene->topo_order[i];
    VkrEntityId ancestor = candidate;
    while (ancestor.u64 && ancestor.u64 != f->selected_entity.u64) {
      const SceneTransform *transform = vkr_entity_get_component(
          f->scene->world, ancestor, f->scene->comp_transform);
      ancestor = transform ? transform->parent : VKR_ENTITY_ID_INVALID;
    }
    if (!ancestor.u64) {
      continue;
    }
    const SceneMeshRenderer *mesh = vkr_entity_get_component(
        f->scene->world, candidate, f->scene->comp_mesh_renderer);
    const VkrMeshInstance *instance =
        mesh ? vkr_mesh_manager_get_instance(&f->assets->mesh_manager,
                                             mesh->instance)
             : NULL;
    if (!instance || !instance->bounds_valid ||
        !isfinite(instance->bounds_world_radius) ||
        instance->bounds_world_radius <= 0) {
      continue;
    }
    const Vec3 center_world = instance->bounds_world_center;
    const Vec4 center_local = mat4_mul_vec4(
        inverse, vec4_new(center_world.x, center_world.y, center_world.z, 1));
    const Vec3 center = {center_local.x, center_local.y, center_local.z};
    const float32_t r = instance->bounds_world_radius;
    const Vec3 radius = {
        r * sqrtf(inverse.elements[0] * inverse.elements[0] +
                  inverse.elements[4] * inverse.elements[4] +
                  inverse.elements[8] * inverse.elements[8]),
        r * sqrtf(inverse.elements[1] * inverse.elements[1] +
                  inverse.elements[5] * inverse.elements[5] +
                  inverse.elements[9] * inverse.elements[9]),
        r * sqrtf(inverse.elements[2] * inverse.elements[2] +
                  inverse.elements[6] * inverse.elements[6] +
                  inverse.elements[10] * inverse.elements[10])};
    const Vec3 lo = vec3_sub(center, radius), hi = vec3_add(center, radius);
    lower = found ? vec3_new(Min(lower.x, lo.x), Min(lower.y, lo.y),
                             Min(lower.z, lo.z))
                  : lo;
    upper = found ? vec3_new(Max(upper.x, hi.x), Max(upper.y, hi.y),
                             Max(upper.z, hi.z))
                  : hi;
    found = true_v;
  }
  if (!found) {
    snprintf(p->error, sizeof(p->error), "No loaded render bounds to fit.");
    return false_v;
  }
  collider->position = vec3_scale(vec3_add(lower, upper), 0.5f);
  collider->rotation = vkr_quat_identity();
  collider->scale = vec3_one();
  collider->half_extent = vec3_scale(vec3_sub(upper, lower), 0.5f);
  collider->radius =
      collider->shape == VKR_PHYSICS_CAPSULE
          ? hypotf(collider->half_extent.x, collider->half_extent.z)
          : vec3_length(collider->half_extent);
  collider->half_height = collider->half_extent.y;
  return true_v;
}

static bool8_t physics_source_equal(const SceneSourceIdentity *a,
                                    const SceneSourceIdentity *b) {
  return a->scene_entity_index == b->scene_entity_index &&
         a->gltf_node_index == b->gltf_node_index &&
         a->source_fingerprint == b->source_fingerprint;
}

static VkrEntityId physics_source_entity(const VkrScene *scene,
                                         const SceneSourceIdentity *source) {
  for (uint32_t i = 0; i < scene->topo_count; ++i) {
    const VkrEntityId entity = scene->topo_order[i];
    const SceneSourceIdentity *identity = vkr_entity_get_component(
        scene->world, entity, scene->comp_source_identity);
    if (identity && physics_source_equal(source, identity)) {
      return entity;
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

static VkrEntityId physics_next_reference(const VkrScene *scene,
                                          VkrEntityId current,
                                          VkrEntityId exclude,
                                          bool8_t animation) {
  uint32_t start = 0;
  for (uint32_t i = 0; i < scene->topo_count; ++i) {
    if (scene->topo_order[i].u64 == current.u64) {
      start = i + 1u;
      break;
    }
  }
  for (uint32_t i = 0; i < scene->topo_count; ++i) {
    const VkrEntityId entity =
        scene->topo_order[(start + i) % scene->topo_count];
    if (entity.u64 == exclude.u64 ||
        !vkr_entity_get_component(scene->world, entity,
                                  scene->comp_source_identity)) {
      continue;
    }
    VkrScenePhysicsSnapshot body;
    if (animation
            ? vkr_scene_animation_get_player(scene, entity) != NULL
            : vkr_scene_physics_read(scene, entity, &body) && body.present) {
      return entity;
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

/* Physics section buttons: unstyled callers get the secondary action style. */
static bool8_t inspector_button(VkrUiSystem *ui, String8 id, String8 text,
                                VkrUiWidgetConfig *config) {
  if (config->style.background_color.w <= 0.0f)
    vkr_editor_action_style(config, VKR_FONT_HANDLE_INVALID);
  return vkr_ui_button(ui, id, text, config);
}

static void physics_layer_widgets(VkrEditorScenePanels *p,
                                  const VkrSampleUiFrame *f, float32_t w,
                                  float32_t *y, bool8_t disabled) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  VkrSceneCollisionLayers settings;
  vkr_scene_collision_layers_read(f->scene, &settings);
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  if (inspector_button(ui, string8_lit("layers.expand"),
                       p->show_collision_layers
                           ? string8_lit("Hide layers and presets")
                           : string8_lit("Layers and presets"),
                       &c)) {
    p->show_collision_layers = !p->show_collision_layers;
  }
  *y += 26;
  if (!p->show_collision_layers) {
    return;
  }
  if (settings.preset_count) {
    p->preset_index = Min(p->preset_index, settings.preset_count - 1u);
    c = widget_at(5, *y, w - 10, 24);
    if (inspector_button(
            ui, string8_lit("preset.next"),
            string8_create_formatted(ui->frame_allocator, "Preset: %s (next)",
                                     settings.presets[p->preset_index].name),
            &c)) {
      p->preset_index = (p->preset_index + 1u) % settings.preset_count;
    }
    *y += 26;
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    if (inspector_button(ui, string8_lit("preset.apply"),
                         string8_lit("Copy preset to draft"), &c) &&
        physics_numbers_parse(p, body)) {
      const VkrSceneCollisionPreset *preset =
          &settings.presets[p->preset_index];
      body->collision_layer = preset->membership;
      body->collision_mask = preset->mask;
      body->sensor = preset->sensor;
      physics_numbers_read(p);
      p->changed = true_v;
    }
    *y += 26;
  }
  for (uint32_t i = 0; i < VKR_COLLISION_LAYER_COUNT; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    for (uint32_t mask = 0; mask < 2; ++mask) {
      (void)vkr_ui_push_id_u64(ui, mask);
      uint16_t *bits = mask ? &body->collision_mask : &body->collision_layer;
      bool8_t checked = (*bits & (1u << i)) != 0;
      c = widget_at(5 + mask * (w - 10) / 2, *y, (w - 10) / 2 - 2, 24);
      c.disabled = disabled;
      if (vkr_ui_checkbox(ui, string8_lit("layer.bit"),
                          string8_create_formatted(ui->frame_allocator, "%s %s",
                                                   mask ? "Hits" : "Is",
                                                   settings.names[i]),
                          &checked, &c)) {
        *bits = checked ? *bits | (uint16_t)(1u << i)
                        : *bits & (uint16_t)~(1u << i);
        p->changed = true_v;
      }
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 26;
  }
}

static bool8_t physics_attachment_widgets(VkrEditorScenePanels *p,
                                          const VkrSampleUiFrame *f,
                                          float32_t w, float32_t *y,
                                          bool8_t disabled) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsAttachment *attachment = &p->values.physics.attachment;
  bool8_t focused = false_v;
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  if (inspector_button(ui, string8_lit("attachment.expand"),
                       p->show_attachment ? string8_lit("Hide bone attachment")
                                          : string8_lit("Bone attachment"),
                       &c)) {
    p->show_attachment = !p->show_attachment;
  }
  *y += 26;
  if (!p->show_attachment) {
    return false_v;
  }
  (void)vkr_ui_push_id_label(ui, string8_lit("attachment"));
  VkrEntityId wrapper =
      physics_source_entity(f->scene, &attachment->animation_source);
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = disabled;
  if (vkr_ui_checkbox(ui, string8_lit("enabled"),
                      string8_lit("Attach to evaluated bone"),
                      &attachment->enabled, &c)) {
    if (attachment->enabled && !wrapper.u64) {
      wrapper = physics_next_reference(f->scene, VKR_ENTITY_ID_INVALID,
                                       VKR_ENTITY_ID_INVALID, true_v);
      const SceneSourceIdentity *source = vkr_entity_get_component(
          f->scene->world, wrapper, f->scene->comp_source_identity);
      if (source) {
        attachment->animation_source = *source;
      }
      attachment->position = vec3_zero();
      attachment->rotation = vkr_quat_identity();
      physics_numbers_read(p);
    }
    p->changed = true_v;
  }
  *y += 26;
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = disabled;
  const String8 wrapper_name = wrapper.u64
                                   ? vkr_scene_get_name(f->scene, wrapper)
                                   : string8_lit("Choose animation");
  if (inspector_button(
          ui, string8_lit("owner.next"),
          string8_create_formatted(ui->frame_allocator, "%.*s (next owner)",
                                   (int)wrapper_name.length, wrapper_name.str),
          &c)) {
    wrapper = physics_next_reference(f->scene, wrapper, VKR_ENTITY_ID_INVALID,
                                     true_v);
    const SceneSourceIdentity *source = vkr_entity_get_component(
        f->scene->world, wrapper, f->scene->comp_source_identity);
    if (source) {
      attachment->animation_source = *source;
      attachment->source_node = 0;
      p->changed = true_v;
    }
  }
  *y += 26;
  const VkrAnimationAsset *asset = vkr_animation_player_asset(
      vkr_scene_animation_get_player(f->scene, wrapper));
  const String8 name = asset && attachment->source_node < asset->node_count
                           ? asset->nodes[attachment->source_node].name
                           : string8_lit("Missing bone");
  c = widget_at(5, *y, w - 10, 24);
  vkr_ui_label(ui, string8_lit("bone.name"),
               string8_create_formatted(ui->frame_allocator, "Bone %u: %.*s",
                                        attachment->source_node,
                                        (int)name.length, name.str),
               &c);
  *y += 26;
  for (uint32_t next = 0; next < 2; ++next) {
    c = widget_at(5 + next * (w - 10) / 2, *y, (w - 10) / 2 - 2, 24);
    c.disabled = disabled || !asset || !asset->node_count;
    (void)vkr_ui_push_id_u64(ui, next);
    if (inspector_button(ui, string8_lit("bone.choose"),
                         next ? string8_lit("Next bone")
                              : string8_lit("Previous bone"),
                         &c) &&
        asset && asset->node_count) {
      attachment->source_node =
          (attachment->source_node + (next ? 1u : asset->node_count - 1u)) %
          asset->node_count;
      p->changed = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 26;
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = disabled || !asset;
  c.tooltip =
      string8_lit("Search bones by name; up to 16 matching nodes are shown.");
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer filter = {(uint8_t *)p->bone_filter,
                                (uint32_t)strlen(p->bone_filter),
                                sizeof(p->bone_filter)};
  (void)vkr_ui_text_field(ui, string8_lit("bone.filter"), &filter, &c);
  if (ui->focused_id ==
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("bone.filter"))) {
    p->physics_focused_id = ui->focused_id;
    focused = true_v;
  }
  *y += 26;
  uint32_t matches = 0;
  for (uint32_t i = 0;
       asset && p->bone_filter[0] && i < asset->node_count && matches < 16;
       ++i) {
    if (!contains(asset->nodes[i].name, p->bone_filter)) {
      continue;
    }
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    (void)vkr_ui_push_id_u64(ui, i);
    if (inspector_button(ui, string8_lit("bone.match"), asset->nodes[i].name,
                         &c)) {
      attachment->source_node = i;
      p->changed = true_v;
    }
    (void)vkr_ui_pop_id(ui);
    *y += 26;
    matches++;
  }
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = disabled;
  p->changed |=
      vkr_ui_checkbox(ui, string8_lit("drive"),
                      string8_lit("Drive bone from Dynamic body (ragdoll)"),
                      &attachment->drive_bone, &c);
  *y += 26;
  const char *labels[] = {"Bone offset X",         "Bone offset Y",
                          "Bone offset Z",         "Bone rotation X (deg)",
                          "Bone rotation Y (deg)", "Bone rotation Z (deg)"};
  for (uint32_t i = 0; i < ArrayCount(labels); ++i) {
    focused |= physics_number_widget(p, ui, w, y, labels[i],
                                     PHYSICS_ATTACHMENT_BASE + i, disabled);
  }
  (void)vkr_ui_pop_id(ui);
  return focused;
}

static bool8_t physics_joint_widgets(VkrEditorScenePanels *p,
                                     const VkrSampleUiFrame *f, float32_t w,
                                     float32_t *y, bool8_t disabled) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  bool8_t focused = false_v;
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  if (inspector_button(ui, string8_lit("joints.expand"),
                       string8_create_formatted(
                           ui->frame_allocator, "%s joints (%u)",
                           p->show_joints ? "Hide" : "Edit", body->joint_count),
                       &c)) {
    p->show_joints = !p->show_joints;
  }
  *y += 26;
  if (!p->show_joints) {
    return false_v;
  }
  const VkrEntityId next_target = physics_next_reference(
      f->scene, VKR_ENTITY_ID_INVALID, f->selected_entity, false_v);
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = disabled || body->joint_count == VKR_SCENE_PHYSICS_MAX_JOINTS ||
               !next_target.u64;
  if (inspector_button(ui, string8_lit("joint.add"),
                       string8_lit("Add joint to another body"), &c) &&
      physics_numbers_parse(p, body)) {
    uint64_t id = 1;
    for (;;) {
      bool8_t used = false_v;
      for (uint32_t i = 0; i < body->joint_count; ++i) {
        used |= body->joints[i].authored_id == id;
      }
      if (!used) {
        break;
      }
      id++;
    }
    const SceneSourceIdentity *target = vkr_entity_get_component(
        f->scene->world, next_target, f->scene->comp_source_identity);
    body->joints[body->joint_count++] =
        (VkrSceneJointConfig){.authored_id = id,
                              .target_source = *target,
                              .type = VKR_PHYSICS_JOINT_FIXED,
                              .axis_a = {1, 0, 0},
                              .axis_b = {1, 0, 0},
                              .normal_a = {0, 1, 0},
                              .normal_b = {0, 1, 0},
                              .min_limit = -0.7853982f,
                              .max_limit = 0.7853982f,
                              .swing_normal_limit = 0.7853982f,
                              .swing_plane_limit = 0.7853982f,
                              .enabled = true_v};
    p->open_joint = id;
    p->changed = true_v;
    physics_numbers_read(p);
  }
  *y += 26;
  const char *types[] = {"Fixed", "Hinge", "Distance", "Swing / twist"};
  for (uint32_t i = 0; i < body->joint_count; ++i) {
    VkrSceneJointConfig *joint = &body->joints[i];
    (void)vkr_ui_push_id_u64(ui, joint->authored_id);
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    p->changed |= vkr_ui_checkbox(
        ui, string8_lit("joint.enabled"),
        string8_create_formatted(ui->frame_allocator, "Joint %llu enabled",
                                 (unsigned long long)joint->authored_id),
        &joint->enabled, &c);
    *y += 26;
    const VkrEntityId resolved_target =
        physics_source_entity(f->scene, &joint->target_source);
    VkrScenePhysicsSnapshot target_body;
    bool8_t target_available =
        resolved_target.u64 &&
        vkr_scene_physics_read(f->scene, resolved_target, &target_body) &&
        target_body.present && target_body.enabled;
    bool8_t target_has_shape = false_v;
    if (target_available) {
      for (uint32_t shape = 0; shape < target_body.collider_count; ++shape) {
        target_has_shape |= target_body.colliders[shape].enabled;
      }
    }
    if (!target_available || !target_has_shape ||
        vkr_scene_physics_body_is_disabled(f->scene, resolved_target)) {
      c = widget_at(5, *y, w - 10, 24);
      vkr_ui_label(ui, string8_lit("joint.suspended"),
                   string8_lit("Suspended: target body unavailable"), &c);
      *y += 26;
    }
    c = widget_at(5, *y, w - 10, 24);
    if (inspector_button(ui, string8_lit("joint.open"),
                         p->open_joint == joint->authored_id
                             ? string8_lit("Hide joint settings")
                             : string8_lit("Edit joint settings"),
                         &c)) {
      p->open_joint =
          p->open_joint == joint->authored_id ? 0 : joint->authored_id;
    }
    *y += 26;
    if (p->open_joint != joint->authored_id) {
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    if (inspector_button(ui, string8_lit("joint.type"),
                         string8_create_formatted(ui->frame_allocator,
                                                  "%s (next type)",
                                                  types[joint->type]),
                         &c)) {
      joint->type =
          (VkrPhysicsJointType)((joint->type + 1u) % ArrayCount(types));
      if (joint->type == VKR_PHYSICS_JOINT_DISTANCE) {
        joint->min_limit = 0;
        joint->max_limit = 1;
      }
      physics_numbers_read(p);
      p->changed = true_v;
    }
    *y += 26;
    const VkrEntityId target_entity =
        physics_source_entity(f->scene, &joint->target_source);
    const String8 target_name =
        target_entity.u64 ? vkr_scene_get_name(f->scene, target_entity)
                          : string8_lit("Missing target");
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    if (inspector_button(
            ui, string8_lit("joint.target"),
            string8_create_formatted(ui->frame_allocator, "%.*s (next target)",
                                     (int)target_name.length, target_name.str),
            &c)) {
      const VkrEntityId target = physics_next_reference(
          f->scene, target_entity, f->selected_entity, false_v);
      const SceneSourceIdentity *source = vkr_entity_get_component(
          f->scene->world, target, f->scene->comp_source_identity);
      if (source) {
        joint->target_source = *source;
        p->changed = true_v;
      }
    }
    *y += 26;
    const char *labels[] = {"Anchor A X",
                            "Anchor A Y",
                            "Anchor A Z",
                            "Anchor B X",
                            "Anchor B Y",
                            "Anchor B Z",
                            "Axis A X",
                            "Axis A Y",
                            "Axis A Z",
                            "Axis B X",
                            "Axis B Y",
                            "Axis B Z",
                            "Normal A X",
                            "Normal A Y",
                            "Normal A Z",
                            "Normal B X",
                            "Normal B Y",
                            "Normal B Z",
                            "Min limit (rad / m)",
                            "Max limit (rad / m)",
                            "Swing normal (rad)",
                            "Swing plane (rad)"};
    for (uint32_t j = 0; j < ArrayCount(labels); ++j) {
      focused |= physics_number_widget(
          p, ui, w, y, labels[j], PHYSICS_JOINT_BASE + i * 22u + j, disabled);
    }
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    bool8_t removed = false_v;
    if (inspector_button(ui, string8_lit("joint.remove"),
                         string8_lit("Remove joint"), &c) &&
        physics_numbers_parse(p, body)) {
      MemCopy(joint, joint + 1, (body->joint_count - i - 1u) * sizeof(*joint));
      body->joint_count--;
      MemZero(&body->joints[body->joint_count], sizeof(*joint));
      physics_numbers_read(p);
      p->changed = true_v;
      removed = true_v;
    }
    *y += 26;
    (void)vkr_ui_pop_id(ui);
    if (removed) {
      break;
    }
  }
  return focused;
}

static void physics_ragdoll_widgets(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y) {
  if (!vkr_scene_animation_get_player(f->scene, f->selected_entity)) {
    return;
  }
  VkrUiSystem *ui = f->ui;
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  vkr_ui_label(ui, string8_lit("ragdoll.title"),
               string8_lit("Ragdoll from skin joints"), &c);
  *y += 26;
  const char *actions[] = {"Create ragdoll", "Enable ragdoll",
                           "Disable ragdoll", "Delete ragdoll"};
  for (uint32_t i = 0; i < ArrayCount(actions); ++i) {
    if (i == 2) {
      *y += 26;
    }
    c = widget_at(5 + (i % 2u) * (w - 10) / 2, *y, (w - 10) / 2 - 2, 24);
    c.disabled = p->changed || !vkr_scene_physics_is_paused(f->scene);
    (void)vkr_ui_push_id_u64(ui, i);
    if (inspector_button(
            ui, string8_lit("ragdoll.action"),
            string8_create((uint8_t *)actions[i], strlen(actions[i])), &c)) {
      uint32_t count = 0;
      const char *error = NULL;
      if (!vkr_scene_physics_ragdoll_plan(f->scene, f->selected_entity,
                                          (VkrSceneRagdollOperation)i, NULL, 0,
                                          &count, &error) ||
          !count || count > VKR_SCENE_PHYSICS_MAX_BODIES) {
        snprintf(p->error, sizeof(p->error), "%s",
                 error ? error : "No matching ragdoll bodies.");
      } else {
        VkrScenePhysicsChange *changes = vkr_allocator_alloc(
            ui->frame_allocator, count * sizeof(*changes), PANEL_TAG);
        if (changes &&
            vkr_scene_physics_ragdoll_plan(f->scene, f->selected_entity,
                                           (VkrSceneRagdollOperation)i, changes,
                                           count, &count, &error)) {
          *f->scene_edit = (VkrSceneEditRequest){
              .action = VKR_SCENE_EDIT_APPLY_PHYSICS_BATCH,
              .physics_batch = changes,
              .physics_batch_count = count};
        } else {
          snprintf(p->error, sizeof(p->error), "%s",
                   error ? error : "Ragdoll draft allocation failed.");
        }
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 26;
}

static bool8_t physics_body_widgets(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y, bool8_t disabled) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  bool8_t focused = false_v;
  const bool8_t muted =
      vkr_scene_physics_body_is_disabled(f->scene, f->selected_entity);
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  c.disabled = !f->physics_request;
  c.tooltip = string8_lit("Temporary simulation/query mute. Not saved; "
                          "authored Body enabled is unchanged.");
  if (inspector_button(ui, string8_lit("session.mute"),
                       muted ? string8_lit("Resume body (session)")
                             : string8_lit("Mute body (session)"),
                       &c) &&
      f->physics_request) {
    *f->physics_request =
        (VkrSamplePhysicsRequest){.entity = f->selected_entity,
                                  .set_body_disabled = true_v,
                                  .body_disabled = !muted};
  }
  *y += 26;
  const char *motions[] = {"Static", "Kinematic", "Dynamic"};
  for (uint32_t i = 0; i < ArrayCount(motions); ++i) {
    c = widget_at(5 + i * (w - 10) / 3, *y, (w - 10) / 3 - 3, 24);
    c.disabled = disabled || body->motion == (VkrPhysicsMotion)i;
    (void)vkr_ui_push_id_u64(ui, i);
    if (inspector_button(
            ui, string8_lit("motion"),
            string8_create((uint8_t *)motions[i], strlen(motions[i])), &c)) {
      body->motion = (VkrPhysicsMotion)i;
      p->changed = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 26;
  bool8_t *flags[] = {&body->enabled, &body->sensor, &body->allow_sleep,
                      &body->continuous};
  const char *labels[] = {"Body enabled", "Sensor (Static / Kinematic)",
                          "Allow sleep", "Continuous collision"};
  for (uint32_t i = 0; i < ArrayCount(flags); ++i) {
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    (void)vkr_ui_push_id_u64(ui, i);
    p->changed |= vkr_ui_checkbox(
        ui, string8_lit("flag"),
        string8_create((uint8_t *)labels[i], strlen(labels[i])), flags[i], &c);
    (void)vkr_ui_pop_id(ui);
    *y += 26;
  }
  const char *labels_numeric[] = {"Mass (kg)",
                                  "Friction",
                                  "Restitution",
                                  "Gravity factor",
                                  "Linear damping",
                                  "Angular damping",
                                  "Layer bits (0..65535)",
                                  "Mask bits (0..65535)"};
  for (uint32_t i = 0; i < 6; ++i) {
    focused |=
        physics_number_widget(p, ui, w, y, labels_numeric[i], i, disabled);
  }
  physics_layer_widgets(p, f, w, y, disabled);
  focused |= physics_attachment_widgets(p, f, w, y, disabled);
  focused |= physics_joint_widgets(p, f, w, y, disabled);
  return focused;
}

static bool8_t physics_collider_widgets(VkrEditorScenePanels *p,
                                        const VkrSampleUiFrame *f, float32_t w,
                                        float32_t *y, bool8_t disabled,
                                        const char *const *shapes) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  bool8_t focused = false_v;
  for (uint32_t i = 0; i < body->collider_count; ++i) {
    VkrSceneColliderConfig *shape = &body->colliders[i];
    (void)vkr_ui_push_id_u64(ui, shape->authored_id);
    VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    p->changed |= vkr_ui_checkbox(
        ui, string8_lit("enabled"),
        string8_create_formatted(ui->frame_allocator, "%s %llu enabled",
                                 shapes[shape->shape] + 2,
                                 (unsigned long long)shape->authored_id),
        &shape->enabled, &c);
    *y += 26;
    c = widget_at(5, *y, w - 10, 24);
    if (inspector_button(ui, string8_lit("expand"),
                         p->open_collider == shape->authored_id
                             ? string8_lit("Hide shape settings")
                             : string8_lit("Edit shape settings"),
                         &c)) {
      p->open_collider =
          p->open_collider == shape->authored_id ? 0 : shape->authored_id;
    }
    *y += 26;
    if (p->open_collider != shape->authored_id) {
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    if (shape->shape >= VKR_PHYSICS_CONVEX_HULL) {
      c = widget_at(5, *y, w - 10, 24);
      vkr_ui_label(ui, string8_lit("asset.label"),
                   string8_lit("Cooked collision asset (.vkc)"), &c);
      *y += 26;
      c = widget_at(5, *y, w - 10, 24);
      c.disabled = disabled;
      vkr_editor_field_style(&c);
      c.tooltip = string8_lit("Workspace-relative .vkc path (legacy projects: "
                              "repository-relative). "
                              "Triangle meshes require Static or Kinematic "
                              "bodies. Cook geometry in Bakery.");
      VkrUiTextEditBuffer path = {(uint8_t *)shape->asset_path,
                                  (uint32_t)strlen(shape->asset_path),
                                  sizeof(shape->asset_path)};
      p->changed |= vkr_ui_text_field(ui, string8_lit("asset.path"), &path, &c);
      if (ui->focused_id == vkr_ui_id_stack_widget_label(
                                &ui->id_stack, string8_lit("asset.path"))) {
        p->physics_focused_id = ui->focused_id;
        focused = true_v;
      }
      *y += 26;
    }
    const char *labels[] = {"Offset X",
                            "Offset Y",
                            "Offset Z",
                            "Rotation X (deg)",
                            "Rotation Y (deg)",
                            "Rotation Z (deg)",
                            "Half width",
                            "Half height",
                            "Half depth",
                            "Radius",
                            "Cylinder half height",
                            "Shape scale X",
                            "Shape scale Y",
                            "Shape scale Z"};
    for (uint32_t j = 0; j < ArrayCount(labels); ++j) {
      if ((j >= 6 && j <= 8 && shape->shape != VKR_PHYSICS_BOX) ||
          (j == 9 && shape->shape != VKR_PHYSICS_SPHERE &&
           shape->shape != VKR_PHYSICS_CAPSULE) ||
          (j == 10 && shape->shape != VKR_PHYSICS_CAPSULE)) {
        continue;
      }
      focused |=
          physics_number_widget(p, ui, w, y, labels[j],
                                8u + i * PHYSICS_COLLIDER_STRIDE + j, disabled);
    }
    if (shape->shape < VKR_PHYSICS_CONVEX_HULL) {
      c = widget_at(5, *y, w - 10, 24);
      c.disabled = disabled;
      if (inspector_button(ui, string8_lit("fit"),
                           string8_lit("Fit loaded bounds (approx.)"), &c) &&
          physics_numbers_parse(p, body) && physics_fit_collider(p, f, shape)) {
        physics_numbers_read(p);
        p->changed = true_v;
      }
      *y += 26;
    }
    c = widget_at(5, *y, (w - 10) / 2 - 3, 24);
    c.disabled =
        disabled || body->collider_count == VKR_SCENE_PHYSICS_MAX_COLLIDERS;
    if (inspector_button(ui, string8_lit("duplicate"), string8_lit("Duplicate"),
                         &c) &&
        physics_numbers_parse(p, body)) {
      const uint64_t id = physics_next_collider_id(body);
      body->colliders[body->collider_count] = *shape;
      body->colliders[body->collider_count++].authored_id = id;
      physics_numbers_read(p);
      p->changed = true_v;
    }
    c = widget_at(w / 2, *y, w / 2 - 5, 24);
    c.disabled = disabled;
    bool8_t removed = false_v;
    if (inspector_button(ui, string8_lit("remove"), string8_lit("Remove"),
                         &c) &&
        physics_numbers_parse(p, body)) {
      MemCopy(shape, shape + 1,
              (body->collider_count - i - 1u) * sizeof(*shape));
      body->collider_count--;
      MemZero(&body->colliders[body->collider_count], sizeof(*shape));
      physics_numbers_read(p);
      p->changed = true_v;
      removed = true_v;
    }
    *y += 28;
    (void)vkr_ui_pop_id(ui);
    if (removed) {
      break;
    }
  }
  return focused;
}

static void physics_impulse_widgets(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  vkr_ui_label(ui, string8_lit("impulse.title"),
               string8_lit("Test impulse (N s; unsaved)"), &c);
  *y += 26;
  c = widget_at(5, *y, w - 10, 24);
  (void)vkr_ui_checkbox(ui, string8_lit("impulse.at.point"),
                        string8_lit("Apply at world point"),
                        &p->impulse_at_point, &c);
  *y += 26;
  const char *labels[] = {"Impulse X",     "Impulse Y",     "Impulse Z",
                          "World point X", "World point Y", "World point Z"};
  for (uint32_t i = 0; i < (p->impulse_at_point ? 6u : 3u); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    c = widget_at(5, *y, w * 0.53f - 6, 24);
    vkr_ui_label(ui, string8_lit("impulse.label"),
                 string8_create((uint8_t *)labels[i], strlen(labels[i])), &c);
    c = widget_at(w * 0.53f, *y, w * 0.47f - 6, 24);
    vkr_editor_field_style(&c);
    VkrUiTextEditBuffer buffer = {(uint8_t *)p->impulse_numbers[i],
                                  (uint32_t)strlen(p->impulse_numbers[i]),
                                  sizeof(p->impulse_numbers[i])};
    (void)vkr_ui_text_field(ui, string8_lit("impulse.value"), &buffer, &c);
    if (ui->focused_id == vkr_ui_id_stack_widget_label(
                              &ui->id_stack, string8_lit("impulse.value"))) {
      p->physics_focused_id = ui->focused_id;
    }
    (void)vkr_ui_pop_id(ui);
    *y += 26;
  }
  c = widget_at(5, *y, w - 10, 24);
  c.disabled = p->changed || !body->enabled;
  if (inspector_button(ui, string8_lit("impulse.apply"),
                       string8_lit("Apply test impulse"), &c)) {
    float32_t values[6] = {0};
    bool8_t valid = true_v;
    for (uint32_t i = 0; i < (p->impulse_at_point ? 6u : 3u); ++i) {
      char *end;
      values[i] = strtof(p->impulse_numbers[i], &end);
      valid &= end != p->impulse_numbers[i] && !*end && isfinite(values[i]);
    }
    if (valid) {
      *f->physics_request = (VkrSamplePhysicsRequest){
          .entity = f->selected_entity,
          .impulse = {values[0], values[1], values[2]},
          .world_point = {values[3], values[4], values[5]},
          .apply_impulse = true_v,
          .at_point = p->impulse_at_point};
    } else {
      snprintf(p->error, sizeof(p->error),
               "Impulse and world point must be finite.");
    }
  }
  *y += 28;
}

static bool8_t physics_inspector_build(VkrEditorScenePanels *p,
                                       const VkrSampleUiFrame *f, float32_t w,
                                       float32_t *y, VkrFontHandle heading) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  const bool8_t paused = vkr_scene_physics_is_paused(f->scene);
  bool8_t focused = false_v;
  (void)vkr_ui_push_id_label(ui, string8_lit("physics"));
  VkrUiWidgetConfig c = widget_at(5, *y, w - 10, 24);
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("title"), string8_lit("Physics body (m, kg, s)"),
               &c);
  *y += 26;
  const SceneTransform *transform = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_transform);
  const bool8_t eligible = transform && transform->trs_editable;
  c = widget_at(5, *y, w - 10, 44);
  const char *error = NULL;
  (void)vkr_scene_physics_validate(f->scene, f->selected_entity, body, &error);
  uint32_t enabled_colliders = 0;
  for (uint32_t i = 0; i < body->collider_count; ++i) {
    enabled_colliders += body->colliders[i].enabled;
  }
  const char *status =
      !eligible            ? "Physics requires an editable TRS transform."
      : !paused            ? "Pause simulation to edit physics."
      : error              ? error
      : !body->present     ? "No body. Add a shape to create one."
      : !body->enabled     ? "Disabled: body excluded from simulation."
      : !enabled_colliders ? "No enabled colliders: body suspended."
                           : "Collider children form one compound body.";
  vkr_ui_label(ui, string8_lit("status"),
               string8_create((uint8_t *)status, strlen(status)), &c);
  *y += 46;
  const bool8_t disabled = !paused || !eligible;
  if (body->present) {
    focused |= physics_body_widgets(p, f, w, y, disabled);
  }
  physics_ragdoll_widgets(p, f, w, y);
  const char *shapes[] = {"+ Box", "+ Sphere", "+ Capsule", "+ Convex",
                          "+ Mesh"};
  for (uint32_t i = 0; i < ArrayCount(shapes); ++i) {
    if (i == 3) {
      *y += 26;
    }
    c = widget_at(5 + (i % 3u) * (w - 10) / 3, *y, (w - 10) / 3 - 3, 24);
    c.disabled =
        disabled || body->collider_count == VKR_SCENE_PHYSICS_MAX_COLLIDERS;
    (void)vkr_ui_push_id_u64(ui, i);
    if (inspector_button(
            ui, string8_lit("add"),
            string8_create((uint8_t *)shapes[i], strlen(shapes[i])), &c) &&
        physics_numbers_parse(p, body)) {
      if (!body->present) {
        *body = vkr_scene_physics_default();
        body->present = true_v;
        body->motion = VKR_PHYSICS_STATIC;
        body->collider_count = 0;
        MemZero(body->colliders, sizeof(body->colliders));
      }
      const uint64_t id = physics_next_collider_id(body);
      body->colliders[body->collider_count++] =
          (VkrSceneColliderConfig){.authored_id = id,
                                   .shape = (VkrPhysicsShape)i,
                                   .rotation = vkr_quat_identity(),
                                   .scale = {1, 1, 1},
                                   .half_extent = {0.5f, 0.5f, 0.5f},
                                   .radius = 0.5f,
                                   .half_height = 0.5f,
                                   .enabled = true_v};
      p->open_collider = id;
      physics_numbers_read(p);
      p->changed = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }
  *y += 26;
  focused |= physics_collider_widgets(p, f, w, y, disabled, shapes);
  if (body->present) {
    c = widget_at(5, *y, w - 10, 24);
    c.disabled = disabled;
    if (inspector_button(ui, string8_lit("remove.body"),
                         string8_lit("Remove body and colliders"), &c)) {
      *body = vkr_scene_physics_default();
      body->present = false_v;
      body->collider_count = 0;
      physics_numbers_read(p);
      p->changed = true_v;
    }
    *y += 28;
  }
  if (body->present && body->motion == VKR_PHYSICS_DYNAMIC &&
      f->physics_request) {
    physics_impulse_widgets(p, f, w, y);
  }
  (void)vkr_ui_pop_id(ui);
  return focused;
}

/* Light kinds on the inspected node. They choose the light rows the inspector
 * shows and the scroll height reserved for them. */
typedef struct InspectorLights {
  bool8_t point;
  bool8_t directional;
  bool8_t rectangle;
  bool8_t spot;
  bool8_t light;
  bool8_t aimed;
} InspectorLights;

static InspectorLights inspector_lights(const VkrSceneEditValues *values) {
  InspectorLights lights = {0};
  lights.point = (values->fields & VKR_SCENE_EDIT_POINT_LIGHT) != 0;
  lights.directional = (values->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT) != 0;
  lights.rectangle = (values->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT) != 0;
  lights.spot = lights.point &&
                values->point_light.kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  lights.light = lights.point || lights.directional || lights.rectangle;
  lights.aimed = lights.directional || lights.spot;
  return lights;
}

typedef enum InspectorSection {
  INSPECTOR_SECTION_TRANSFORM = 0,
  INSPECTOR_SECTION_LIGHT,
  INSPECTOR_SECTION_PHYSICS,
  INSPECTOR_SECTION_DEBUG,
} InspectorSection;

#define INSPECTOR_ROW_PT 26.0f
#define INSPECTOR_PAD_PT 10.0f

/* Collapsible section header; returns true while the section is expanded. */
static bool8_t inspector_section(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                 float32_t w, float32_t *y,
                                 InspectorSection section, VkrUiIcon icon,
                                 Vec4 icon_color, const char *title,
                                 VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t expanded = !p->section_collapsed[section];
  *y += 4;
  VkrUiWidgetConfig header = widget_at(0, *y, w, 28);
  header.style.background_color = theme->header;
  header.style.hover_background_color = theme->raised;
  header.style.border_pt = (VkrUiEdges){1, 0, 1, 0};
  header.style.border_color = theme->separator;
  header.tooltip = expanded ? string8_lit("Collapse section")
                            : string8_lit("Expand section");
  (void)vkr_ui_push_id_u64(ui, 0x5ec70000u + section);
  if (vkr_ui_button(ui, string8_lit("section"), (String8){0}, &header))
    p->section_collapsed[section] = expanded;
  VkrUiWidgetConfig caret = widget_at(INSPECTOR_PAD_PT - 2, *y, 14, 28);
  caret.placement.align = VKR_UI_ALIGN_START;
  caret.style.padding_pt = (VkrUiEdges){9, 0, 9, 0};
  caret.icon =
      expanded ? VKR_UI_ICON_DISCLOSURE_OPEN : VKR_UI_ICON_DISCLOSURE_CLOSED;
  caret.icon_size_pt = 10.0f;
  caret.icon_color = theme->text_secondary;
  vkr_ui_label(ui, string8_lit("caret"), (String8){0}, &caret);
  VkrUiWidgetConfig label =
      widget_at(INSPECTOR_PAD_PT + 14, *y, w - INSPECTOR_PAD_PT - 20, 28);
  label.placement.align = VKR_UI_ALIGN_START;
  label.style.padding_pt = (VkrUiEdges){6, 4, 6, 2};
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text;
  label.text.font = heading;
  label.icon = icon;
  label.icon_size_pt = 14.0f;
  label.icon_color = icon_color;
  vkr_ui_label(ui, string8_lit("title"),
               string8_create((uint8_t *)title, strlen(title)), &label);
  (void)vkr_ui_pop_id(ui);
  *y += 32;
  return expanded;
}

/* Row label in secondary text, vertically centered in a row. */
static void inspector_row_label(VkrUiSystem *ui, String8 id, const char *text,
                                float32_t y, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig label =
      widget_at(INSPECTOR_PAD_PT, y, Max(8.0f, width), INSPECTOR_ROW_PT - 2);
  label.placement.align = VKR_UI_ALIGN_START;
  label.style.padding_pt = (VkrUiEdges){5, 2, 5, 0};
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, id, string8_create((uint8_t *)text, strlen(text)), &label);
}

/* Starts or continues a continuous edit owned by `owner`; returns its id. */
static uint64_t inspector_gesture(VkrEditorScenePanels *p, VkrUiId owner) {
  if (!p->gesture_active || p->gesture_owner != owner) {
    p->gesture = ++p->gesture_counter;
    p->gesture_owner = owner;
  }
  p->gesture_active = true_v;
  return p->gesture;
}

/* Drag-to-change handle: a tag or label that scrubs numbers[index] while
 * held. Shift is 10x faster and Alt 10x finer. */
static void inspector_scrub(VkrEditorScenePanels *p, const VkrSampleUiFrame *f,
                            uint32_t index, float32_t step, VkrUiId handle) {
  VkrUiSystem *ui = f->ui;
  if (ui->active_id != handle)
    return;
  int32_t dx = 0, dy = 0;
  input_get_mouse_delta(f->input, &dx, &dy);
  (void)inspector_gesture(p, handle);
  if (!dx)
    return;
  float32_t scale = step / Max(ui->content_scale, 1.0f);
  if (input_is_key_down(f->input, KEY_SHIFT) ||
      input_is_key_down(f->input, KEY_LSHIFT) ||
      input_is_key_down(f->input, KEY_RSHIFT))
    scale *= 10.0f;
  if (input_is_key_down(f->input, KEY_LMENU) ||
      input_is_key_down(f->input, KEY_RMENU))
    scale *= 0.1f;
  char *end = NULL;
  const float32_t value = strtof(p->numbers[index], &end);
  if (end == p->numbers[index] || !isfinite(value))
    return;
  snprintf(p->numbers[index], sizeof(p->numbers[index]), "%.6g",
           value + (float32_t)dx * scale);
  p->changed = true_v;
}

/* Numeric text field bound to numbers[index]. Returns true while focused. */
static bool8_t inspector_number_field(VkrEditorScenePanels *p,
                                      const VkrSampleUiFrame *f, uint32_t index,
                                      float32_t x, float32_t y, float32_t width,
                                      bool8_t read_only) {
  VkrUiSystem *ui = f->ui;
  VkrUiWidgetConfig field = widget_at(x, y + 1, width, INSPECTOR_ROW_PT - 4);
  field.style.padding_pt = (VkrUiEdges){3, 6, 3, 6};
  field.read_only = read_only;
  vkr_editor_field_style(&field);
  VkrUiTextEditBuffer value = {(uint8_t *)p->numbers[index],
                               (uint32_t)strlen(p->numbers[index]),
                               sizeof(p->numbers[index])};
  (void)vkr_ui_push_id_u64(ui, 0x7a100000u + index);
  p->changed |= vkr_ui_text_field(ui, string8_lit("value"), &value, &field);
  const bool8_t focused =
      ui->focused_id ==
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("value"));
  (void)vkr_ui_pop_id(ui);
  return focused;
}

/* Three axis fields with colored, scrubbable X/Y/Z (or R/G/B) tags. */
static bool8_t inspector_vector_row(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y, const char *label,
                                    uint32_t first, float32_t step,
                                    bool8_t read_only, bool8_t color) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  static const char *axis_names[2][3] = {{"X", "Y", "Z"}, {"R", "G", "B"}};
  const Vec4 axis_colors[3] = {theme->axis_x, theme->axis_y, theme->axis_z};
  /* Narrow panels move the label above the fields so each axis keeps a
   * readable width. */
  const bool8_t stacked = w < 300.0f;
  const float32_t label_w =
      stacked ? w - INSPECTOR_PAD_PT * 2 : Min(92.0f, w * 0.3f);
  const float32_t left = INSPECTOR_PAD_PT + (stacked ? 0.0f : label_w);
  const float32_t available = Max(60.0f, w - left - INSPECTOR_PAD_PT);
  const float32_t cell = available / 3.0f;
  bool8_t focused = false_v;
  (void)vkr_ui_push_id_u64(ui, 0x7ec70000u + first);
  inspector_row_label(ui, string8_lit("label"), label, *y, label_w - 4);
  if (stacked)
    *y += INSPECTOR_ROW_PT - 6;
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const float32_t x = left + cell * (float32_t)axis;
    VkrUiWidgetConfig tag = widget_at(x, *y + 1, 18, INSPECTOR_ROW_PT - 4);
    tag.style.corner_radius_pt = (Vec4){theme->radius, 0, 0, theme->radius};
    tag.style.background_color =
        vkr_ui_color_alpha(axis_colors[axis], read_only ? 0.35f : 0.85f);
    tag.style.hover_background_color = axis_colors[axis];
    tag.style.text_color = theme->text_on_accent;
    tag.style.font_size_pt = theme->font_caption;
    tag.style.padding_pt = (VkrUiEdges){2, 0, 2, 0};
    tag.disabled = read_only;
    tag.tooltip = string8_lit("Drag to change (Shift faster, Alt finer)");
    tag.cursor = VKR_WINDOW_CURSOR_RESIZE_EW;
    (void)vkr_ui_push_id_u64(ui, axis);
    const VkrUiId tag_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("tag"));
    (void)vkr_ui_button(
        ui, string8_lit("tag"),
        string8_create((uint8_t *)axis_names[color ? 1 : 0][axis], 1u), &tag);
    (void)vkr_ui_pop_id(ui);
    if (!read_only)
      inspector_scrub(p, f, first + axis, step, tag_id);
    focused |= inspector_number_field(p, f, first + axis, x + 17, *y, cell - 20,
                                      read_only);
  }
  (void)vkr_ui_pop_id(ui);
  *y += INSPECTOR_ROW_PT + 2;
  return focused;
}

/* Label, optional inline slider and field for one scalar. */
static bool8_t inspector_scalar_row(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y, const char *label,
                                    uint32_t index, float32_t step,
                                    bool8_t slider, float32_t minimum,
                                    float32_t maximum, const char *tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  const float32_t label_w = Min(118.0f, w * 0.36f);
  const float32_t left = INSPECTOR_PAD_PT + label_w;
  const float32_t available = Max(60.0f, w - left - INSPECTOR_PAD_PT);
  const float32_t field_w = slider ? Min(72.0f, available * 0.4f) : available;
  bool8_t focused = false_v;
  (void)vkr_ui_push_id_u64(ui, 0x5ca10000u + index);
  /* The label itself scrubs, as in UE5 and Unity. */
  VkrUiWidgetConfig handle =
      widget_at(INSPECTOR_PAD_PT, *y, label_w - 4, INSPECTOR_ROW_PT - 2);
  vkr_editor_ghost_style(&handle);
  handle.style.padding_pt = (VkrUiEdges){5, 2, 5, 2};
  handle.style.text_color = theme->text_secondary;
  handle.style.font_size_pt = theme->font_body;
  handle.tooltip = tooltip ? string8_create((uint8_t *)tooltip, strlen(tooltip))
                           : string8_lit("Drag to change (Shift faster, Alt "
                                         "finer)");
  handle.cursor = VKR_WINDOW_CURSOR_RESIZE_EW;
  const VkrUiId handle_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("handle"));
  (void)vkr_ui_button(ui, string8_lit("handle"), (String8){0}, &handle);
  inspector_scrub(p, f, index, step, handle_id);
  VkrUiWidgetConfig text = handle;
  text.placement.justify = VKR_UI_ALIGN_START;
  text.style.background_color = (Vec4){0};
  text.tooltip = (String8){0};
  vkr_ui_label(ui, string8_lit("label"),
               string8_create((uint8_t *)label, strlen(label)), &text);
  if (slider) {
    char *end = NULL;
    const float32_t angle = strtof(p->numbers[index], &end);
    const bool8_t valid = end != p->numbers[index] && !*end && isfinite(angle);
    float32_t slider_value =
        valid ? vkr_clamp_f32(angle, minimum, maximum) : minimum;
    VkrUiWidgetConfig bar =
        widget_at(left, *y + 2, available - field_w - 8, INSPECTOR_ROW_PT - 6);
    bar.disabled = !valid;
    bar.tooltip = tooltip ? string8_create((uint8_t *)tooltip, strlen(tooltip))
                          : (String8){0};
    const VkrUiId bar_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("slider"));
    if (vkr_ui_slider_f32(ui, string8_lit("slider"), &slider_value, minimum,
                          maximum, &bar)) {
      snprintf(p->numbers[index], sizeof(p->numbers[index]), "%.5g",
               slider_value);
      p->changed = true_v;
    }
    if (ui->active_id == bar_id)
      (void)inspector_gesture(p, bar_id);
  }
  focused |= inspector_number_field(p, f, index, w - INSPECTOR_PAD_PT - field_w,
                                    *y, field_w, false_v);
  (void)vkr_ui_pop_id(ui);
  *y += INSPECTOR_ROW_PT + 2;
  return focused;
}

static void inspector_checkbox_row(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                   float32_t w, float32_t *y, String8 id,
                                   const char *label, bool8_t *value,
                                   bool8_t disabled, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig c = widget_at(
      INSPECTOR_PAD_PT, *y + 2, w - INSPECTOR_PAD_PT * 2, INSPECTOR_ROW_PT - 4);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){3, 2, 3, 0};
  c.style.text_color = theme->text;
  c.style.font_size_pt = theme->font_body;
  c.disabled = disabled;
  c.tooltip = tooltip;
  p->changed |= vkr_ui_checkbox(
      ui, id, string8_create((uint8_t *)label, strlen(label)), value, &c);
  *y += INSPECTOR_ROW_PT;
}

/* Entity card: type icon, editable name and visibility controls. */
static bool8_t inspector_header(VkrEditorScenePanels *p,
                                const VkrSampleUiFrame *f, float32_t w,
                                float32_t *y, VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  bool8_t field_focus = false_v;
  const EditorTreeNode *node = NULL;
  for (uint32_t i = 0; i < p->row_count && !node; ++i)
    if (p->nodes[p->rows[i]].entity.u64 == f->selected_entity.u64)
      node = &p->nodes[p->rows[i]];
  Vec4 icon_color;
  const VkrUiIcon icon = hierarchy_entity_icon(
      f->scene, f->selected_entity, node && node->child != NO_ROW, &icon_color);
  VkrUiWidgetConfig badge = widget_at(INSPECTOR_PAD_PT, *y + 2, 30, 30);
  badge.style.background_color = vkr_ui_color_alpha(icon_color, 0.16f);
  badge.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
  badge.style.padding_pt = (VkrUiEdges){6, 6, 6, 6};
  badge.icon = icon;
  badge.icon_size_pt = 18.0f;
  badge.icon_color = icon_color;
  vkr_ui_label(ui, string8_lit("badge"), (String8){0}, &badge);

  VkrUiWidgetConfig c = widget_at(INSPECTOR_PAD_PT + 38, *y + 3,
                                  w - INSPECTOR_PAD_PT * 2 - 70, 28);
  c.read_only = !(p->values.fields & VKR_SCENE_EDIT_NAME);
  c.style.font_size_pt = theme->font_emphasis;
  c.style.padding_pt = (VkrUiEdges){4, 7, 4, 7};
  /* Entity names are user content: the Regular face covers Latin Extended,
   * Greek and Cyrillic, while headings cover Latin-1 only. */
  vkr_editor_field_style(&c);
  c.tooltip = c.read_only
                  ? string8_lit("Original name exceeds editable capacity; "
                                "select and copy its full text")
                  : string8_lit("Rename (Enter applies, Escape reverts)");
  VkrUiTextEditBuffer name = {(uint8_t *)p->values.name,
                              (uint32_t)strlen(p->values.name),
                              sizeof(p->values.name)};
  if (p->long_name)
    name = (VkrUiTextEditBuffer){p->long_name, p->long_name_capacity - 1u,
                                 p->long_name_capacity};
  if (c.read_only && !p->long_name) {
    vkr_ui_label(ui, string8_lit("name.unavailable"),
                 string8_lit("Original name unavailable"), &c);
  } else {
    p->changed |= vkr_ui_text_field(ui, string8_lit("name"), &name, &c);
  }
  field_focus |= ui->focused_id == vkr_ui_id_stack_widget_label(
                                       &ui->id_stack, string8_lit("name"));
  const bool8_t can_hide = (p->values.fields & VKR_SCENE_EDIT_VISIBILITY) != 0;
  VkrUiWidgetConfig eye = vkr_editor_icon_button_config(
      0, 0,
      p->values.visibility.visible ? VKR_UI_ICON_EYE : VKR_UI_ICON_EYE_SLASH,
      p->values.visibility.visible ? string8_lit("Visible (click to hide)")
                                   : string8_lit("Hidden (click to show)"));
  eye.placement =
      widget_at(w - INSPECTOR_PAD_PT - 26, *y + 4, 26, 26).placement;
  eye.disabled = !can_hide;
  vkr_editor_toggle_style(&eye, p->values.visibility.visible);
  if (vkr_ui_button(ui, string8_lit("visibility"), (String8){0}, &eye)) {
    p->values.visibility.visible = !p->values.visibility.visible;
    p->changed = true_v;
  }
  *y += 38;
  VkrUiWidgetConfig subtitle =
      widget_at(INSPECTOR_PAD_PT + 38, *y - 4, w - INSPECTOR_PAD_PT * 2, 18);
  subtitle.placement.align = VKR_UI_ALIGN_START;
  subtitle.style.font_size_pt = theme->font_caption;
  subtitle.style.text_color = theme->text_secondary;
  subtitle.style.padding_pt = (VkrUiEdges){0, 2, 0, 2};
  const InspectorLights lights = inspector_lights(&p->values);
  const char *kind =
      lights.directional          ? "Directional light"
      : lights.spot               ? "Spot light"
      : lights.point              ? "Point light"
      : lights.rectangle          ? "Rectangle light"
      : p->values.physics.present ? "Physics body"
      : vkr_entity_get_component(f->scene->world, f->selected_entity,
                                 f->scene->comp_mesh_renderer)
          ? "Mesh"
          : "Node";
  vkr_ui_label(ui, string8_lit("kind"),
               string8_create_formatted(ui->frame_allocator, "%s", kind),
               &subtitle);
  *y += 16;
  inspector_checkbox_row(p, ui, w, y, string8_lit("inherit"),
                         "Inherit parent visibility",
                         &p->values.visibility.inherit_parent, !can_hide,
                         string8_lit("Hidden when any ancestor is hidden"));
  return field_focus;
}

static bool8_t inspector_transform_section(VkrEditorScenePanels *p,
                                           const VkrSampleUiFrame *f,
                                           float32_t w, float32_t *y,
                                           VkrFontHandle heading,
                                           const SceneTransform *tr) {
  const VkrUiTheme *theme = vkr_ui_theme();
  bool8_t field_focus = false_v;
  if (!inspector_section(p, f->ui, w, y, INSPECTOR_SECTION_TRANSFORM,
                         VKR_UI_ICON_MOVE, theme->accent_hover, "Transform",
                         heading))
    return false_v;
  if (tr && !tr->trs_editable) {
    VkrUiWidgetConfig c =
        widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 40);
    c.style.text_color = theme->text_secondary;
    c.style.font_size_pt = theme->font_caption;
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = Max(1.0f, w - INSPECTOR_PAD_PT * 2);
    c.icon = VKR_UI_ICON_LOCK;
    c.icon_size_pt = 13.0f;
    vkr_ui_label(f->ui, string8_lit("matrix.readonly"),
                 string8_lit("Authored matrix with shear: transform is "
                             "read-only. See Debug for its rows."),
                 &c);
    *y += 42;
    return false_v;
  }
  const bool8_t read_only = !(p->values.fields & VKR_SCENE_EDIT_TRANSFORM);
  field_focus |= inspector_vector_row(p, f, w, y, "Location", 0, 0.01f,
                                      read_only, false_v);
  field_focus |=
      inspector_vector_row(p, f, w, y, "Rotation", 3, 0.5f, read_only, false_v);
  field_focus |=
      inspector_vector_row(p, f, w, y, "Scale", 6, 0.005f, read_only, false_v);
  return field_focus;
}

/* Linear light color shown as an sRGB swatch. */
static Vec4 inspector_swatch(const VkrEditorScenePanels *p) {
  float32_t rgb[3];
  for (uint32_t i = 0; i < 3; ++i) {
    const float32_t linear =
        vkr_clamp_f32(strtof(p->numbers[9 + i], NULL), 0.0f, 1.0f);
    rgb[i] = linear <= 0.0031308f ? linear * 12.92f
                                  : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
  }
  return (Vec4){rgb[0], rgb[1], rgb[2], 1.0f};
}

static bool8_t inspector_light_section(VkrEditorScenePanels *p,
                                       const VkrSampleUiFrame *f, float32_t w,
                                       float32_t *y, VkrFontHandle heading,
                                       const InspectorLights *lights) {
  VkrUiSystem *ui = f->ui;
  bool8_t field_focus = false_v;
  const char *title = lights->rectangle ? "Rectangle light"
                      : lights->spot    ? "Spot light"
                      : lights->point   ? "Point light"
                                        : "Directional light";
  const VkrUiIcon icon = lights->rectangle ? VKR_UI_ICON_RECT_LIGHT
                         : lights->spot    ? VKR_UI_ICON_SPOT_LIGHT
                         : lights->point   ? VKR_UI_ICON_POINT_LIGHT
                                           : VKR_UI_ICON_DIRECTIONAL_LIGHT;
  if (!inspector_section(p, ui, w, y, INSPECTOR_SECTION_LIGHT, icon,
                         (Vec4){0.98f, 0.78f, 0.36f, 1.0f}, title, heading))
    return false_v;
  if (lights->point) {
    inspector_checkbox_row(p, ui, w, y, string8_lit("light.point.enabled"),
                           "Enabled", &p->values.point_light.enabled, false_v,
                           (String8){0});
    inspector_checkbox_row(p, ui, w, y, string8_lit("light.point.shadow"),
                           "Cast shadows", &p->values.point_light.casts_shadow,
                           false_v,
                           string8_lit("Requires a finite positive range"));
  }
  if (lights->directional) {
    inspector_checkbox_row(
        p, ui, w, y, string8_lit("light.directional.enabled"), "Enabled",
        &p->values.directional_light.enabled, false_v, (String8){0});
    inspector_checkbox_row(
        p, ui, w, y, string8_lit("light.directional.atmosphere_sun"),
        "Atmosphere sun", &p->values.directional_light.atmosphere_sun, false_v,
        string8_lit("Drives the atmosphere's sun direction and color"));
  }
  if (lights->rectangle)
    inspector_checkbox_row(p, ui, w, y, string8_lit("light.rectangle.enabled"),
                           "Enabled", &p->values.rectangle_light.enabled,
                           false_v, (String8){0});
  /* Color: swatch beside linear R/G/B fields. */
  VkrUiWidgetConfig swatch =
      widget_at(w - INSPECTOR_PAD_PT - 22, *y + 3, 22, INSPECTOR_ROW_PT - 6);
  swatch.style.background_color = inspector_swatch(p);
  swatch.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  swatch.style.border_color = vkr_ui_theme()->border_strong;
  swatch.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
  swatch.tooltip = string8_lit("Linear RGB color");
  vkr_ui_label(ui, string8_lit("color.swatch"), (String8){0}, &swatch);
  field_focus |= inspector_vector_row(p, f, w - 28, y, "Color", 9, 0.005f,
                                      false_v, true_v);
  field_focus |= inspector_scalar_row(
      p, f, w, y, lights->rectangle ? "Radiance" : "Intensity", 12, 0.05f,
      false_v, 0, 0, NULL);
  if (lights->point)
    field_focus |=
        inspector_scalar_row(p, f, w, y, "Range", 13, 0.05f, false_v, 0, 0,
                             "Attenuation range; 0 is unlimited");
  if (lights->aimed) {
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Yaw", 14, 0.5f, true_v, -180.0f, 180.0f,
        "Local light direction; node rotation also applies");
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Elevation", 15, 0.5f, true_v, -90.0f, 90.0f,
        "Local light direction; node rotation also applies");
  }
  if (lights->spot) {
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Inner cone", 16, 0.25f, true_v, 0.0f, 90.0f,
        "Cone half-angle in degrees; inner must be less than outer");
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Outer cone", 17, 0.25f, true_v, 0.0f, 90.0f,
        "Cone half-angle in degrees; inner must be less than outer");
  }
  if (lights->directional) {
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Sun diameter", 18, 0.01f, true_v, 0.0f, 179.999f,
        "Solar-disc diameter in degrees; zero keeps a hard PCF edge");
    field_focus |= inspector_scalar_row(
        p, f, w, y, "Temperature", 21, 10.0f, false_v, 0, 0,
        "Color temperature in Kelvin; 0 uses the authored color");
  }
  if (lights->rectangle) {
    field_focus |= inspector_scalar_row(p, f, w, y, "Width", 19, 0.01f, false_v,
                                        0, 0, NULL);
    field_focus |= inspector_scalar_row(p, f, w, y, "Height", 20, 0.01f,
                                        false_v, 0, 0, NULL);
  }
  return field_focus;
}

/* Internal identity for debugging imports, collapsed by default. */
static void inspector_debug_section(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y, VkrFontHandle heading,
                                    const SceneTransform *tr) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  if (!inspector_section(p, ui, w, y, INSPECTOR_SECTION_DEBUG,
                         VKR_UI_ICON_LOG_DEBUG, theme->text_secondary, "Debug",
                         heading))
    return;
  const SceneSourceIdentity *source = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_source_identity);
  char index_text[5][16] = {{0}};
  if (source) {
    const uint32_t indices[5] = {
        source->gltf_node_index, source->gltf_mesh_index,
        source->gltf_camera_index, source->gltf_skin_index,
        source->gltf_light_index};
    for (uint32_t i = 0; i < 5; ++i) {
      if (indices[i] == UINT32_MAX)
        snprintf(index_text[i], sizeof(index_text[i]), "none");
      else
        snprintf(index_text[i], sizeof(index_text[i]), "%u", indices[i]);
    }
  }
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 92);
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = theme->text_secondary;
  vkr_ui_label(
      ui, string8_lit("identity"),
      source ? string8_create_formatted(
                   ui->frame_allocator,
                   "Entity %u \xc2\xb7 generation %u\nSource entity %u\n"
                   "glTF node %s \xc2\xb7 mesh %s\ncamera %s \xc2\xb7 skin %s "
                   "\xc2\xb7 light %s",
                   f->selected_entity.parts.index,
                   f->selected_entity.parts.generation,
                   source->scene_entity_index, index_text[0], index_text[1],
                   index_text[2], index_text[3], index_text[4])
             : string8_create_formatted(ui->frame_allocator,
                                        "Entity %u \xc2\xb7 generation %u",
                                        f->selected_entity.parts.index,
                                        f->selected_entity.parts.generation),
      &c);
  *y += source ? 76 : 24;
  if (tr && !tr->trs_editable) {
    for (uint32_t row = 0; row < 4; ++row) {
      const Vec4 values = mat4_row(tr->local, (int32_t)row);
      String8 text = string8_create_formatted(
          ui->frame_allocator, "%.7g  %.7g  %.7g  %.7g", values.x, values.y,
          values.z, values.w);
      // Formatted String8 storage has a terminator; the field retains its own
      // copy.
      c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
      c.read_only = true_v;
      vkr_editor_field_style(&c);
      VkrUiTextEditBuffer matrix = {text.str, (uint32_t)text.length,
                                    (uint32_t)text.length + 1u};
      (void)vkr_ui_push_id_u64(ui, row);
      (void)vkr_ui_text_field(ui, string8_lit("matrix.row"), &matrix, &c);
      (void)vkr_ui_pop_id(ui);
      *y += 26;
    }
  }
}

void vkr_editor_inspector_build(VkrEditorScenePanels *p,
                                const VkrSampleUiFrame *f, VkrUiRect rect,
                                VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  float32_t w = rect.width / ui->content_scale,
            h = rect.height / ui->content_scale;
  if (w < 32 || h < 24)
    return;
  if (!p->section_collapsed_init) {
    p->section_collapsed[INSPECTOR_SECTION_DEBUG] = true_v;
    p->section_collapsed[INSPECTOR_SECTION_PHYSICS] = true_v;
    p->section_collapsed_init = true_v;
  }
  VkrUiWidgetConfig c;
  if (!f->scene || !vkr_scene_entity_alive(f->scene, f->selected_entity)) {
    const float32_t prompt_y = Max(16.0f, h * 0.3f);
    c = widget_at(INSPECTOR_PAD_PT, prompt_y, w - INSPECTOR_PAD_PT * 2, 28);
    c.center = true_v;
    c.icon = VKR_UI_ICON_SELECT;
    c.icon_size_pt = 24.0f;
    c.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("select.icon"), (String8){0}, &c);

    c = widget_at(INSPECTOR_PAD_PT, prompt_y + 32.0f, w - INSPECTOR_PAD_PT * 2,
                  48);
    c.center = true_v;
    c.style.text_color = theme->text_secondary;
    c.text.layout.word_wrap = true_v;
    /* Wrap inside the label's 5 pt side padding. */
    c.text.layout.max_width = Max(1.0f, w - INSPECTOR_PAD_PT * 2 - 10.0f);
    c.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("select.prompt"),
                 string8_lit("Select an entity in the Scene or Hierarchy"), &c);
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
      p->gesture_active = false_v;
      // Stable field IDs never transfer an active edit to the new selection.
      inspector_clear_focus(ui);
    }
  }
  VkrEntityId physics_owner =
      vkr_scene_physics_owner(f->scene, f->selected_entity);
  if (physics_owner.u64 && physics_owner.u64 != f->selected_entity.u64) {
    c = widget_at(INSPECTOR_PAD_PT, 12, w - INSPECTOR_PAD_PT * 2, 50);
    c.style.text_color = theme->text_secondary;
    c.icon = VKR_UI_ICON_COLLIDER;
    c.icon_size_pt = 16.0f;
    c.icon_color = (Vec4){0.45f, 0.84f, 0.56f, 1.0f};
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = Max(1.0f, w - 50.0f);
    vkr_ui_label(ui, string8_lit("collider.owner.info"),
                 string8_lit("Collider child. Edit its shape and placement in "
                             "the owning body's collider list."),
                 &c);
    c = widget_at(INSPECTOR_PAD_PT, 70, Min(180.0f, w - 20), 28);
    vkr_editor_action_style(&c, heading);
    c.icon = VKR_UI_ICON_ARROW_UP;
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("collider.owner.select"),
                      string8_lit("Edit owning body"), &c)) {
      *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT,
                                             .entity = physics_owner};
    }
    return;
  }
  const SceneTransform *tr = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_transform);
  if (!p->changed && tr && tr->trs_editable &&
      (MemCompare(&p->values.position, &tr->position, sizeof(Vec3)) ||
       MemCompare(&p->values.rotation, &tr->rotation, sizeof(VkrQuat)) ||
       MemCompare(&p->values.scale, &tr->scale, sizeof(Vec3))))
    inspector_read(p, f);
  const InspectorLights lights = inspector_lights(&p->values);
  if (in_rect(ui, rect))
    p->inspector_scroll -= ui->mouse_wheel * 40.0f;
  const float32_t content_height = Max(h, p->inspector_height);
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
  float32_t y = 8;
  bool8_t field_focus = false_v;
  const bool8_t gesture_held = p->gesture_active;
  p->gesture_active = false_v;
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.fields"));
  field_focus |= inspector_header(p, f, w, &y, heading);
  if (p->error[0]) {
    c = widget_at(INSPECTOR_PAD_PT, y, w - INSPECTOR_PAD_PT * 2, 36);
    c.style.background_color = vkr_ui_color_alpha(theme->error, 0.14f);
    c.style.corner_radius_pt = (Vec4){5, 5, 5, 5};
    c.style.padding_pt = (VkrUiEdges){5, 8, 5, 8};
    c.style.text_color = theme->text;
    c.style.font_size_pt = theme->font_caption;
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = Max(1.0f, w - 60.0f);
    c.icon = VKR_UI_ICON_WARNING_FILL;
    c.icon_size_pt = 14.0f;
    c.icon_color = theme->error;
    vkr_ui_label(
        ui, string8_lit("error"),
        (String8){.str = (uint8_t *)p->error, .length = strlen(p->error)}, &c);
    y += 42;
  }
  field_focus |= inspector_transform_section(p, f, w, &y, heading, tr);
  if (lights.light)
    field_focus |= inspector_light_section(p, f, w, &y, heading, &lights);
  if (inspector_section(p, ui, w, &y, INSPECTOR_SECTION_PHYSICS,
                        VKR_UI_ICON_PHYSICS, (Vec4){0.45f, 0.84f, 0.56f, 1.0f},
                        "Physics", heading))
    field_focus |= physics_inspector_build(p, f, w, &y, heading);
  inspector_debug_section(p, f, w, &y, heading, tr);
  field_focus &=
      !ui->mouse_captured && ui->keyboard_input_layer == ui->input_layer;
  (void)vkr_ui_pop_id(ui);
  (void)vkr_ui_scroll_area_end(ui);
  p->inspector_height = y + 16;

  /* Live editing: Escape reverts a draft; Enter or leaving the field commits;
   * toggles and continuous gestures apply at once, each gesture folding into
   * one undo entry. */
  if (field_focus && pressed(f->input, KEY_ESCAPE)) {
    inspector_read(p, f);
    return;
  }
  const bool8_t commit = p->changed && (p->gesture_active || !field_focus ||
                                        pressed(f->input, KEY_ENTER));
  if (!p->gesture_active && gesture_held)
    p->gesture = 0;
  if (commit) {
    VkrSceneEditValues values;
    if (inspector_parse(p, &values)) {
      if (values.fields)
        *f->scene_edit = (VkrSceneEditRequest){
            .action = VKR_SCENE_EDIT_APPLY,
            .entity = f->selected_entity,
            .values = values,
            .gesture = p->gesture_active ? p->gesture : 0u};
      else
        inspector_read(p, f);
      p->changed = false_v;
    }
  }
}

bool8_t vkr_editor_scene_panels_write_json(const VkrEditorScenePanels *panels,
                                           VkrJsonWriter *writer) {
  return panels && vkr_json_writer_begin_object(writer) &&
         vkr_json_writer_name(writer, string8_lit("version")) &&
         vkr_json_writer_u64(writer, 1) &&
         vkr_json_writer_name(writer, string8_lit("search")) &&
         vkr_json_writer_string(
             writer, string8_create_from_cstr((const uint8_t *)panels->search,
                                              strlen(panels->search))) &&
         vkr_json_writer_name(writer, string8_lit("inspector_scroll")) &&
         vkr_json_writer_f64(writer, panels->inspector_scroll) &&
         vkr_json_writer_end_object(writer);
}

bool8_t vkr_editor_scene_panels_read_json(VkrEditorScenePanels *panels,
                                          String8 json) {
  if (!panels) {
    return false_v;
  }
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  int32_t version;
  float32_t scroll;
  char search[sizeof(panels->search)];
  if (!vkr_json_get_int(&reader, "version", &version) || version != 1 ||
      !vkr_json_get_float(&reader, "inspector_scroll", &scroll) ||
      !isfinite(scroll) || scroll < 0 ||
      !vkr_editor_project_json_string(json, "search", search, sizeof(search),
                                      NULL)) {
    return false_v;
  }
  if (!vkr_string_copy_bounded(panels->search, sizeof(panels->search),
                               search)) {
    return false_v;
  }
  panels->inspector_scroll = scroll;
  panels->rebuild = true_v;
  return true_v;
}

bool8_t
vkr_editor_scene_panels_write_scene_json(const VkrEditorScenePanels *panels,
                                         const VkrSampleUiFrame *frame,
                                         VkrJsonWriter *writer) {
  if (!panels || !frame || !frame->scene ||
      panels->generation != frame->scene_generation ||
      !vkr_json_writer_begin_object(writer) ||
      !vkr_json_writer_name(writer, string8_lit("version")) ||
      !vkr_json_writer_u64(writer, 1) ||
      !vkr_json_writer_name(writer, string8_lit("hierarchy_scroll")) ||
      !vkr_json_writer_f64(writer, panels->hierarchy_scroll) ||
      !vkr_json_writer_name(writer, string8_lit("expanded")) ||
      !vkr_json_writer_begin_array(writer)) {
    return false_v;
  }
  for (uint32_t i = 0; i < panels->capacity; ++i) {
    const EditorTreeNode *node = &panels->nodes[i];
    VkrSampleEntityIdentity identity;
    if (node->expanded &&
        vkr_sample_entity_identity(frame->scene, node->entity, &identity) &&
        !vkr_sample_entity_identity_write_json(&identity, writer)) {
      return false_v;
    }
  }
  return vkr_json_writer_end_array(writer) &&
         vkr_json_writer_end_object(writer);
}

static bool8_t panels_restore_expansion(VkrEditorScenePanels *panels,
                                        const VkrSampleUiFrame *frame,
                                        String8 array, bool8_t apply) {
  VkrJsonReader reader = vkr_json_reader_from_string(array);
  vkr_json_skip_whitespace(&reader);
  if (reader.pos >= reader.length || reader.data[reader.pos++] != '[') {
    return false_v;
  }
  uint32_t count = 0;
  for (;;) {
    vkr_json_skip_whitespace(&reader);
    if (reader.pos >= reader.length) {
      return false_v;
    }
    if (reader.data[reader.pos] == ']') {
      return true_v;
    }
    if (count++ >= panels->capacity) {
      return false_v;
    }
    VkrJsonReader object;
    VkrSampleEntityIdentity identity;
    if (!vkr_json_enter_object(&reader, &object) ||
        !vkr_sample_entity_identity_read_json(
            (String8){.str = (uint8_t *)object.data, .length = object.length},
            &identity)) {
      return false_v;
    }
    if (apply) {
      VkrEntityId entity = vkr_sample_entity_find(frame->scene, &identity);
      if (entity.u64 && entity.parts.index < panels->capacity) {
        panels->nodes[entity.parts.index].expanded = true_v;
      }
    }
    vkr_json_skip_whitespace(&reader);
    if (reader.pos >= reader.length) {
      return false_v;
    }
    if (reader.data[reader.pos] == ']') {
      return true_v;
    }
    if (reader.data[reader.pos++] != ',') {
      return false_v;
    }
  }
}

bool8_t vkr_editor_scene_panels_read_scene_json(VkrEditorScenePanels *panels,
                                                const VkrSampleUiFrame *frame,
                                                String8 json) {
  if (!panels || !frame || !frame->scene) {
    return false_v;
  }
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  int32_t version;
  float32_t scroll;
  String8 expanded;
  if (!vkr_json_get_int(&reader, "version", &version) || version != 1 ||
      !vkr_json_get_float(&reader, "hierarchy_scroll", &scroll) ||
      !isfinite(scroll) || scroll < 0 ||
      !vkr_editor_project_json_member(json, "expanded", &expanded, NULL) ||
      !rebuild_tree(panels, frame) ||
      !panels_restore_expansion(panels, frame, expanded, false_v)) {
    return false_v;
  }
  for (uint32_t i = 0; i < panels->capacity; ++i) {
    panels->nodes[i].expanded = false_v;
  }
  if (!panels_restore_expansion(panels, frame, expanded, true_v)) {
    return false_v;
  }
  panels->hierarchy_scroll = scroll;
  panels->rebuild = true_v;
  return true_v;
}

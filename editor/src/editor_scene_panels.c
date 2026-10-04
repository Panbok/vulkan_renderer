#include "editor_scene_panels.h"
#include "editor_agent.h"

#include "core/vkr_json.h"
#include "editor_details.h"
#include "editor_internal.h"
#include "editor_project_store.h"
#include "editor_projects.h"
#include "editor_scripts.h"
#include "filesystem/filesystem.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_types.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Text drafts for attachment and joint numbers; body and collider rows come
   from their descriptors. */
#define PHYSICS_ATTACHMENT_BASE 0u
#define PHYSICS_JOINT_BASE (PHYSICS_ATTACHMENT_BASE + 6u)
#define PHYSICS_NUMBER_COUNT                                                   \
  (PHYSICS_JOINT_BASE + VKR_SCENE_PHYSICS_MAX_JOINTS * 22u)
#define NO_ROW UINT32_MAX
#define TREE_VISIBLE_SLOTS 96u
/* Rows of pinned objects and their expanded descendants, drawn above the
   tree. */
#define TREE_PINNED_ROW_MAX 64u

/* Hierarchy containers (ADR-076): the root World, the active scene, then
   additive scenes by slot. */
#define TREE_CONTAINER_WORLD 0u
#define TREE_CONTAINER_SCENE 1u
#define TREE_CONTAINER_ADDITIVE 2u
#define TREE_CONTAINER_COUNT (2u + VKR_SCENE_ADDITIVE_MAX)

typedef struct EditorTreeNode {
  VkrEntityId entity;
  uint32_t parent;
  uint32_t child;
  uint32_t next;
  uint32_t depth;
  /* TREE_CONTAINER_*; header rows name a container and own its roots. */
  uint8_t container;
  bool8_t header;
  bool8_t expanded;
  bool8_t match;
  /* Pinned to the Outliner's top with its subtree. */
  bool8_t pinned;
  /* The object and its descendants show no viewport icon; `icons_hidden`
     is the row's own toggle, `icons_off` includes an ancestor's. */
  bool8_t icons_hidden;
  bool8_t icons_off;
  /* Its model's textures encode in a background finalize (ADR-077). */
  bool8_t cooking;
} EditorTreeNode;

typedef struct TreeContainer {
  const VkrScene *scene;
  uint32_t kind;
  uint32_t base;
} TreeContainer;

struct VkrEditorScenePanels {
  /* A physics row drag is in progress; the draft commits when it ends. */
  bool8_t physics_dragging;
  /* Component type whose Presets button was pressed this build. */
  const VkrTypeDesc *preset_menu;
  /* The script slot's picker was pressed this build, below this point. */
  bool8_t script_menu;
  Vec2 script_menu_pt;
  /* The Details name field takes focus on its next build. */
  bool8_t rename_request;
  /* The physics settings' collision matrix button opens its window once
     the sections are built. */
  bool8_t open_collision_layers;
  /* Outliner row last clicked, for double-click framing. */
  VkrEntityId click_entity;
  float64_t click_time;
  VkrAllocator *allocator;
  EditorTreeNode *nodes;
  uint32_t *rows;
  uint32_t capacity;
  /* Nodes the last rebuild laid out: headers and directory slots. */
  uint32_t node_total;
  uint32_t row_count;
  uint32_t pinned_rows[TREE_PINNED_ROW_MAX];
  uint32_t pinned_depths[TREE_PINNED_ROW_MAX];
  uint32_t pinned_row_count;
  uint32_t live_count;
  uint32_t selected_row;
  uint64_t generation;
  uint64_t structure_revision;
  const VkrScene *tree_world;
  uint64_t world_structure_revision;
  /* Additive containers shown and the sum of their structure revisions. */
  const VkrScene *tree_additive[VKR_SCENE_ADDITIVE_MAX];
  uint64_t additive_structure_revision;
  bool8_t container_collapsed[TREE_CONTAINER_COUNT];
  bool8_t rebuild;
  char search[192];
  float32_t hierarchy_scroll;
  /* Cooking marks follow the cooking revisions and the tree they were
     computed for, and refresh while cooking as models finish loading. */
  uint64_t cooking_key;
  float64_t cooking_marked_at;
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
  /* Generated component rows; owns text entry and drag gestures. */
  VkrEditorDetails details;
  /* Collapsed world component sections, indexed like vkr_scene_world_type. */
  bool8_t world_collapsed[VKR_SCENE_TYPE_MAX];
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

/* Containers shown by the Hierarchy, each with a header node followed by its
   directory slots. */
static uint32_t tree_containers(const VkrSampleUiFrame *frame,
                                TreeContainer out[TREE_CONTAINER_COUNT],
                                uint32_t *out_total) {
  uint32_t count = 0u;
  uint32_t total = 0u;
  const VkrScene *scenes[TREE_CONTAINER_COUNT] = {frame->world, frame->scene};
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i)
    scenes[TREE_CONTAINER_ADDITIVE + i] = frame->additive[i];
  for (uint32_t kind = 0u; kind < TREE_CONTAINER_COUNT; ++kind) {
    if (!scenes[kind]) {
      continue;
    }
    out[count++] =
        (TreeContainer){.scene = scenes[kind], .kind = kind, .base = total};
    total += 1u + scenes[kind]->world->dir.capacity;
  }
  *out_total = total;
  return count;
}

/* Node index of an entity, or NO_ROW when no shown container owns it. */
static uint32_t tree_node_index(const TreeContainer *containers,
                                uint32_t container_count, VkrEntityId entity) {
  for (uint32_t c = 0u; c < container_count; ++c) {
    const VkrScene *scene = containers[c].scene;
    if (entity.u64 && entity.parts.world == scene->world_id &&
        entity.parts.index < scene->world->dir.capacity) {
      return containers[c].base + 1u + entity.parts.index;
    }
  }
  return NO_ROW;
}

static const VkrScene *tree_node_scene(const VkrSampleUiFrame *frame,
                                       const EditorTreeNode *node) {
  if (node->container == TREE_CONTAINER_WORLD)
    return frame->world;
  if (node->container >= TREE_CONTAINER_ADDITIVE)
    return frame->additive[node->container - TREE_CONTAINER_ADDITIVE];
  return frame->scene;
}

/* Search also finds objects by the types they carry, such as "fog" or
   "Point light" (ADR-076). */
static bool8_t tree_type_matches(const VkrScene *scene, VkrEntityId entity,
                                 const char *search) {
  if (!search[0])
    return false_v;
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    if (vkr_scene_get_typed(scene, entity, type) &&
        (contains(string8_create((uint8_t *)type->label, strlen(type->label)),
                  search) ||
         contains(string8_create((uint8_t *)type->name, strlen(type->name)),
                  search)))
      return true_v;
  }
  const struct {
    VkrComponentTypeId id;
    const VkrTypeDesc *type;
  } lights[] = {
      {scene->comp_point_light, &vkr_scene_point_light_type},
      {scene->comp_directional_light, &vkr_scene_directional_light_type},
      {scene->comp_rectangle_light, &vkr_scene_rectangle_light_type}};
  for (uint32_t i = 0; i < ArrayCount(lights); ++i) {
    if (vkr_entity_has_component(scene->world, entity, lights[i].id) &&
        contains(string8_create((uint8_t *)lights[i].type->label,
                                strlen(lights[i].type->label)),
                 search))
      return true_v;
  }
  return false_v;
}

static bool8_t rebuild_tree(VkrEditorScenePanels *p,
                            const VkrSampleUiFrame *frame) {
  TreeContainer containers[TREE_CONTAINER_COUNT];
  uint32_t capacity = 0u;
  const uint32_t container_count =
      tree_containers(frame, containers, &capacity);
  bool8_t new_scene =
      p->generation != frame->scene_generation || p->tree_world != frame->world;
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
  uint32_t last_header = NO_ROW;
  /* Scenes are children of the World they inherit from (ADR-076); without a
     World each container is a root. */
  const uint32_t world_header =
      container_count && containers[0].kind == TREE_CONTAINER_WORLD
          ? containers[0].base
          : NO_ROW;
  uint32_t nested[TREE_CONTAINER_COUNT];
  uint32_t nested_count = 0u;
  for (uint32_t c = 0u; c < container_count; ++c) {
    const VkrScene *s = containers[c].scene;
    const uint32_t base = containers[c].base;
    const uint32_t slots = s->world->dir.capacity;
    EditorTreeNode *header = &p->nodes[base];
    *header = (EditorTreeNode){.parent = NO_ROW,
                               .child = NO_ROW,
                               .next = NO_ROW,
                               .container = (uint8_t)containers[c].kind,
                               .header = true_v,
                               .expanded =
                                   !p->container_collapsed[containers[c].kind],
                               .match = !p->search[0]};
    if (world_header != NO_ROW && base != world_header) {
      header->parent = world_header;
      nested[nested_count++] = base;
    } else {
      if (last_header == NO_ROW)
        first_root = base;
      else
        p->nodes[last_header].next = base;
      last_header = base;
    }
    /* Reverse insertion preserves the source/entity order within each
     * sibling list. */
    for (uint32_t i = slots; i-- > 0;) {
      EditorTreeNode *n = &p->nodes[base + 1u + i];
      VkrEntityId id = vkr_entity_id_from_index(s->world, i);
      const bool8_t kept = !new_scene && n->entity.u64 == id.u64;
      *n = (EditorTreeNode){.parent = NO_ROW,
                            .child = NO_ROW,
                            .next = NO_ROW,
                            .container = (uint8_t)containers[c].kind,
                            .expanded = kept && n->expanded,
                            .pinned = kept && n->pinned,
                            .icons_hidden = kept && n->icons_hidden};
      // Directory capacity includes vacant slots. A reconstructed generation
      // alone does not prove liveness; occupied records own the live entities.
      if (!s->world->dir.records[i].chunk)
        continue;
      n->entity = id;
      ++p->live_count;
      const SceneTransform *tr =
          vkr_entity_get_component(s->world, id, s->comp_transform);
      n->parent =
          tr && tr->parent.u64 ? base + 1u + tr->parent.parts.index : base;
      n->match = contains(vkr_scene_get_name(s, id), p->search) ||
                 tree_type_matches(s, id, p->search);
    }
    for (uint32_t i = slots; i-- > 0;) {
      const uint32_t index = base + 1u + i;
      EditorTreeNode *n = &p->nodes[index];
      /* Brush faces and connections belong to their parent; Details and
         agents edit them (ADR-084). */
      if (!n->entity.u64 || vkr_scene_entity_is_part(s, n->entity))
        continue;
      n->next = p->nodes[n->parent].child;
      p->nodes[n->parent].child = index;
    }
  }
  /* Nested scenes follow the World's own objects, in container order. */
  if (nested_count) {
    uint32_t *link = &p->nodes[world_header].child;
    while (*link != NO_ROW)
      link = &p->nodes[*link].next;
    for (uint32_t i = 0u; i < nested_count; ++i) {
      *link = nested[i];
      link = &p->nodes[nested[i]].next;
    }
  }
  /* Source hierarchy was validated by the scene owner. Ancestors of matches,
     including their container header, remain visible; expanding filtered
     paths doesn't change saved expansion. */
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
  const uint32_t selected_node =
      tree_node_index(containers, container_count, frame->selected_entity);
  if (frame->selected_entity.u64 != p->revealed.u64 &&
      selected_node != NO_ROW &&
      p->nodes[selected_node].entity.u64 == frame->selected_entity.u64) {
    uint32_t parent = p->nodes[selected_node].parent;
    while (parent != NO_ROW) {
      p->nodes[parent].expanded = true_v;
      if (p->nodes[parent].header)
        p->container_collapsed[p->nodes[parent].container] = false_v;
      parent = p->nodes[parent].parent;
    }
  }
  /* An object's icons stay off while any ancestor turns them off. */
  for (uint32_t i = 0; i < capacity; ++i) {
    EditorTreeNode *n = &p->nodes[i];
    uint32_t at = i;
    n->icons_off = false_v;
    while (n->entity.u64 && at != NO_ROW && !p->nodes[at].header &&
           !n->icons_off) {
      n->icons_off = p->nodes[at].icons_hidden;
      at = p->nodes[at].parent;
    }
  }
  /* Pinned objects, in tree order, each followed by its expanded subtree. */
  p->pinned_row_count = 0;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (!p->nodes[i].pinned || !p->nodes[i].entity.u64)
      continue;
    uint32_t at = i;
    uint32_t level = 0;
    while (at != NO_ROW && p->pinned_row_count < TREE_PINNED_ROW_MAX) {
      p->pinned_rows[p->pinned_row_count] = at;
      p->pinned_depths[p->pinned_row_count++] = level;
      const EditorTreeNode *n = &p->nodes[at];
      if (n->child != NO_ROW && n->expanded) {
        at = n->child;
        ++level;
        continue;
      }
      while (at != i && p->nodes[at].next == NO_ROW) {
        at = p->nodes[at].parent;
        --level;
      }
      at = at == i ? NO_ROW : p->nodes[at].next;
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
  p->node_total = capacity;
  p->cooking_key = 0u;
  p->generation = frame->scene_generation;
  p->tree_world = frame->world;
  p->structure_revision = frame->scene ? frame->scene->structure_revision : 0u;
  p->world_structure_revision =
      frame->world ? frame->world->structure_revision : 0u;
  p->additive_structure_revision = 0u;
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    p->tree_additive[i] = frame->additive[i];
    if (frame->additive[i])
      p->additive_structure_revision += frame->additive[i]->structure_revision;
  }
  p->rebuild = false_v;
  return true_v;
}

bool8_t vkr_editor_entity_mesh_from(const VkrSampleUiFrame *frame,
                                    const VkrScene *scene, VkrEntityId entity,
                                    const char (*revisions)[37],
                                    uint32_t count) {
  VkrMeshManager *meshes = &frame->assets->mesh_manager;
  const SceneMeshRenderer *renderer =
      scene && count ? vkr_entity_get_component(scene->world, entity,
                                                scene->comp_mesh_renderer)
                     : NULL;
  VkrMeshInstance *instance =
      renderer ? vkr_mesh_manager_get_instance(meshes, renderer->instance)
               : NULL;
  const VkrMeshAsset *asset =
      instance ? vkr_mesh_manager_get_live_asset(meshes, instance->asset)
               : NULL;
  for (uint32_t r = 0u; asset && r < count; ++r) {
    if (contains(asset->mesh_path, revisions[r])) {
      return true_v;
    }
  }
  return false_v;
}

/* Marks nodes whose mesh loaded from a build revision a background finalize
   rebuilds (ADR-077), and their ancestors, so a collapsed model shows it. */
static void tree_mark_cooking(VkrEditorScenePanels *p,
                              const VkrSampleUiFrame *frame,
                              const char (*revisions)[37], uint32_t count) {
  for (uint32_t i = 0u; i < p->node_total; ++i) {
    p->nodes[i].cooking = false_v;
  }
  for (uint32_t i = 0u; count && i < p->node_total; ++i) {
    const EditorTreeNode *n = &p->nodes[i];
    if (n->header || !n->entity.u64 || n->cooking ||
        !vkr_editor_entity_mesh_from(frame, tree_node_scene(frame, n),
                                     n->entity, revisions, count)) {
      continue;
    }
    for (uint32_t node = i; node != NO_ROW && !p->nodes[node].header;
         node = p->nodes[node].parent) {
      p->nodes[node].cooking = true_v;
    }
  }
}

bool8_t vkr_editor_scene_panels_icons_off(const VkrEditorScenePanels *panels,
                                          const VkrSampleUiFrame *frame,
                                          VkrEntityId entity) {
  TreeContainer containers[TREE_CONTAINER_COUNT];
  uint32_t total = 0u;
  const uint32_t node = tree_node_index(
      containers, tree_containers(frame, containers, &total), entity);
  return node != NO_ROW && node < panels->capacity &&
         panels->nodes[node].entity.u64 == entity.u64 &&
         panels->nodes[node].icons_off;
}

bool8_t vkr_editor_scene_panels_cooking(const VkrEditorScenePanels *panels,
                                        VkrEntityId entity) {
  for (uint32_t i = 0u; panels && entity.u64 && i < panels->node_total; ++i) {
    if (panels->nodes[i].entity.u64 == entity.u64) {
      return panels->nodes[i].cooking;
    }
  }
  return false_v;
}

/* True when the additive containers or their structure changed. */
static bool8_t tree_additive_changed(const VkrEditorScenePanels *p,
                                     const VkrSampleUiFrame *frame) {
  uint64_t revision = 0u;
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (p->tree_additive[i] != frame->additive[i])
      return true_v;
    if (frame->additive[i])
      revision += frame->additive[i]->structure_revision;
  }
  return revision != p->additive_structure_revision;
}

#define INSPECTOR_ROW_PT VKR_EDITOR_DETAILS_ROW_PT
#define INSPECTOR_PAD_PT VKR_EDITOR_DETAILS_PAD_PT
#define HIERARCHY_ROW_PT 24.0f
#define HIERARCHY_INDENT_PT 14.0f
/* The search toolbar above the column header. */
#define HIERARCHY_TOOLS_PT 40.0f
#define HIERARCHY_HEADER_PT 24.0f
#define HIERARCHY_FOOTER_PT 22.0f
#define HIERARCHY_TOGGLE_PT 20.0f
/* Pinned rows shown before the pinned block scrolls no further. */
#define HIERARCHY_PINNED_ROWS_SHOWN 8u

/* Type icon and category hue for a hierarchy row, Godot-style: lights amber,
 * geometry blue, physics green, groups neutral. */
/* Icon for a world component type. */
VkrUiIcon vkr_editor_world_type_icon(const VkrTypeDesc *type) {
  if (type == &vkr_scene_environment_type)
    return VKR_UI_ICON_SKY;
  if (type == &vkr_scene_atmosphere_type)
    return VKR_UI_ICON_PLANET;
  if (type == &vkr_scene_clouds_type)
    return VKR_UI_ICON_CLOUD;
  if (type == &vkr_scene_fog_type || type == &vkr_scene_froxel_fog_type)
    return VKR_UI_ICON_FOG;
  if (type == &vkr_scene_fog_box_type)
    return VKR_UI_ICON_BOUNDING_BOX;
  if (type == &vkr_scene_post_process_type)
    return VKR_UI_ICON_PALETTE;
  if (type == &vkr_scene_reflection_probe_type)
    return VKR_UI_ICON_PROBE;
  if (type == &vkr_scene_subsurface_type)
    return VKR_UI_ICON_DROP;
  if (type == &vkr_scene_physics_settings_type)
    return VKR_UI_ICON_PHYSICS;
  if (type == &vkr_scene_animation_settings_type)
    return VKR_UI_ICON_ANIMATION;
  if (type == &vkr_scene_shape_type)
    return VKR_UI_ICON_SHAPES;
  if (type == &vkr_scene_text_type)
    return VKR_UI_ICON_TEXT;
  if (type == &vkr_scene_animation_type)
    return VKR_UI_ICON_ANIMATION;
  if (type == &vkr_scene_player_start_type)
    return VKR_UI_ICON_PERSON_WALK;
  if (vkr_scene_world_type_registered(type))
    return VKR_UI_ICON_CODE;
  return VKR_UI_ICON_LIGHT;
}

/* ---- Object creation (ADR-076) ---- */

typedef struct EditorObjectKind {
  const char *word;
  const char *label;
  VkrUiIcon icon;
  /* Light or live world component type; NULL for an empty object. */
  const VkrTypeDesc *type;
  bool8_t spot;
  /* Menu heading the kind is listed under. */
  const char *group;
  /* Brush and blockout kinds run an agent operation instead
     (docs/proposals/level-design-toolkit.md): 1 box, 2 wedge, 3 cylinder,
     4 room. */
  uint32_t brush;
} EditorObjectKind;

/* Labels match the names the loader gives legacy world blocks. Scripts are
   Content assets attached to objects, not objects of their own (ADR-079). */
static const EditorObjectKind s_object_kinds[] = {
    {"empty", "Empty", VKR_UI_ICON_EMPTY, NULL, false_v, "Basic"},
    {"cube", "Cube", VKR_UI_ICON_SHAPES, &vkr_scene_shape_type, false_v,
     "Basic"},
    {"text", "Text", VKR_UI_ICON_TEXT, &vkr_scene_text_type, false_v, "Basic"},
    {"point_light", "Point Light", VKR_UI_ICON_POINT_LIGHT,
     &vkr_scene_point_light_type, false_v, "Lights"},
    {"spot_light", "Spot Light", VKR_UI_ICON_SPOT_LIGHT,
     &vkr_scene_point_light_type, true_v, "Lights"},
    {"rect_light", "Rect Light", VKR_UI_ICON_RECT_LIGHT,
     &vkr_scene_rectangle_light_type, false_v, "Lights"},
    {"directional_light", "Directional Light", VKR_UI_ICON_DIRECTIONAL_LIGHT,
     &vkr_scene_directional_light_type, false_v, "Lights"},
    {"player_start", "Player Start", VKR_UI_ICON_PERSON_WALK,
     &vkr_scene_player_start_type, false_v, "Gameplay"},
    {"brush_box", "Brush Box", VKR_UI_ICON_SHAPES, &vkr_scene_brush_type,
     false_v, "Level", 1u},
    {"brush_wedge", "Brush Wedge", VKR_UI_ICON_SHAPES, &vkr_scene_brush_type,
     false_v, "Level", 2u},
    {"brush_cylinder", "Brush Cylinder", VKR_UI_ICON_SHAPES,
     &vkr_scene_brush_type, false_v, "Level", 3u},
    {"blockout_room", "Blockout Room", VKR_UI_ICON_BOUNDING_BOX,
     &vkr_scene_brush_type, false_v, "Level", 4u},
    {"atmosphere", "Sky Atmosphere", VKR_UI_ICON_PLANET,
     &vkr_scene_atmosphere_type, false_v, "Environment"},
    {"clouds", "Volumetric Clouds", VKR_UI_ICON_CLOUD, &vkr_scene_clouds_type,
     false_v, "Environment"},
    {"fog", "Height Fog", VKR_UI_ICON_FOG, &vkr_scene_fog_type, false_v,
     "Environment"},
    {"volumetric_fog", "Volumetric Fog", VKR_UI_ICON_FOG,
     &vkr_scene_froxel_fog_type, false_v, "Environment"},
    {"fog_density_box", "Fog Density Box", VKR_UI_ICON_BOUNDING_BOX,
     &vkr_scene_fog_box_type, false_v, "Environment"},
    {"post_process", "Post Process", VKR_UI_ICON_PALETTE,
     &vkr_scene_post_process_type, false_v, "Environment"},
    {"physics_settings", "Physics Settings", VKR_UI_ICON_PHYSICS,
     &vkr_scene_physics_settings_type, false_v, "Settings"},
    {"animation_settings", "Animation Settings", VKR_UI_ICON_ANIMATION,
     &vkr_scene_animation_settings_type, false_v, "Settings"},
};

/* Built-in kinds, then one unlisted kind per component type a script module
   registered (ADR-079), so Cmd can still create an entity carrying it. */
static bool8_t editor_object_kind(uint32_t kind, EditorObjectKind *out) {
  if (kind < ArrayCount(s_object_kinds)) {
    *out = s_object_kinds[kind];
    return true_v;
  }
  const VkrTypeDesc *type =
      vkr_scene_registered_type(kind - ArrayCount(s_object_kinds));
  if (!type) {
    return false_v;
  }
  *out = (EditorObjectKind){.word = type->name,
                            .label = type->label,
                            .icon = VKR_UI_ICON_CODE,
                            .type = type,
                            .group = "Scripts"};
  return true_v;
}

uint32_t vkr_editor_object_kind_count(void) {
  uint32_t count = ArrayCount(s_object_kinds);
  while (vkr_scene_registered_type(count - ArrayCount(s_object_kinds))) {
    ++count;
  }
  return count;
}

bool8_t vkr_editor_object_kind_listed(uint32_t kind) {
  return kind < ArrayCount(s_object_kinds);
}

const char *vkr_editor_object_kind_word(uint32_t kind) {
  EditorObjectKind object;
  return editor_object_kind(kind, &object) ? object.word : NULL;
}

const char *vkr_editor_object_kind_label(uint32_t kind) {
  EditorObjectKind object;
  return editor_object_kind(kind, &object) ? object.label : NULL;
}

const char *vkr_editor_object_kind_group(uint32_t kind) {
  EditorObjectKind object;
  return editor_object_kind(kind, &object) ? object.group : NULL;
}

VkrUiIcon vkr_editor_object_kind_icon(uint32_t kind) {
  EditorObjectKind object;
  return editor_object_kind(kind, &object) ? object.icon : VKR_UI_ICON_NONE;
}

// ---- Script slot (ADR-079) ----

const VkrTypeDesc *vkr_editor_entity_script(const VkrScene *scene,
                                            VkrEntityId entity) {
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; scene && (type = vkr_scene_registered_type(i)); ++i) {
    if (vkr_scene_get_typed(scene, entity, type)) {
      return type;
    }
  }
  return NULL;
}

uint32_t vkr_editor_script_types(const VkrSampleUiFrame *frame,
                                 const VkrTypeDesc **out, uint32_t capacity) {
  uint32_t count = 0u;
  const VkrScriptHost *host = frame->scripts;
  for (uint32_t m = 0; host && m < host->module_count; ++m) {
    const VkrScriptModule *module = &host->modules[m];
    for (uint32_t t = 0; !module->retired && t < module->type_count; ++t) {
      if (count < capacity) {
        out[count] = module->types[t];
      }
      ++count;
    }
  }
  return Min(count, capacity);
}

bool8_t vkr_editor_script_source(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 const VkrTypeDesc *type, char *out,
                                 uint32_t capacity) {
  const VkrScriptHost *host = frame->scripts;
  const char *module_name = NULL;
  for (uint32_t m = 0; host && type && !module_name && m < host->module_count;
       ++m) {
    for (uint32_t t = 0; t < host->modules[m].type_count; ++t) {
      if (host->modules[m].types[t] == type) {
        module_name = host->modules[m].name;
      }
    }
  }
  if (!module_name) {
    return false_v;
  }
  /* The module's `<Name>.c`, else its first C source. */
  const char *found = NULL;
  char wanted[VKR_EDITOR_SCRIPT_NAME + 2u];
  snprintf(wanted, sizeof(wanted), "%s.c", module_name);
  const uint32_t files =
      editor->scripts ? vkr_editor_scripts_file_count(editor->scripts) : 0u;
  for (uint32_t i = 0; i < files; ++i) {
    const VkrEditorScriptFile *file =
        vkr_editor_scripts_file(editor->scripts, i);
    const VkrEditorScriptModule *module =
        vkr_editor_scripts_module(editor->scripts, file->module);
    const size_t length = strlen(file->name);
    if (!module || strcmp(module->name, module_name) || length < 2u ||
        strcmp(file->name + length - 2u, ".c")) {
      continue;
    }
    if (!found || !strcmp(file->name, wanted)) {
      found = file->path;
    }
  }
  if (found) {
    snprintf(out, capacity, "%s", found);
    return true_v;
  }
  /* A module linked into the editor, such as the FPS sample, keeps its
     sources in this repository's scripts/<name>/src. */
#if defined(VKR_EDITOR_SCRIPT_SDK_ROOT)
  char linked[VKR_EDITOR_SCRIPT_PATH];
  const int32_t length =
      snprintf(linked, sizeof(linked), "%s/scripts/%s/src/%s_module.c",
               VKR_EDITOR_SCRIPT_SDK_ROOT, module_name, module_name);
  const FilePath path = {
      .path = string8_create_from_cstr((const uint8_t *)linked, strlen(linked)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  if (length > 0 && (uint32_t)length < sizeof(linked) && file_exists(&path)) {
    snprintf(out, capacity, "%s", linked);
    return true_v;
  }
#endif
  return false_v;
}

bool8_t vkr_editor_open_entity_script(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrEntityId entity) {
  char path[VKR_EDITOR_SCRIPT_PATH];
  const VkrTypeDesc *type =
      vkr_editor_entity_script(vkr_editor_entity_scene(frame, entity), entity);
  if (!type) {
    return false_v;
  }
  if (vkr_editor_script_source(editor, frame, type, path, sizeof(path)) &&
      vkr_editor_code_open(editor->code, editor, path)) {
    /* A linked module's edits apply once the editor is rebuilt. */
    if (!vkr_editor_scripts_module_of(editor->scripts, path)) {
      char text[160];
      snprintf(text, sizeof(text),
               "%s is built into the editor; saved changes apply after a "
               "rebuild",
               type->label);
      vkr_editor_toast(editor, VKR_UI_ICON_INFO_FILL, vkr_ui_theme()->accent,
                       text);
    }
    return true_v;
  }
  char text[160];
  snprintf(text, sizeof(text), "%s has no source to open", type->label);
  vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, vkr_ui_theme()->warning,
                   text);
  return false_v;
}

const VkrTypeDesc *vkr_editor_module_script(const VkrSampleUiFrame *frame,
                                            const char *module) {
  const VkrScriptModule *loaded =
      frame->scripts && module ? vkr_script_host_module(frame->scripts, module)
                               : NULL;
  return loaded && !loaded->retired && loaded->type_count ? loaded->types[0]
                                                          : NULL;
}

bool8_t vkr_editor_attach_script(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, const VkrTypeDesc *type) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[160];
  if (!type || !scene || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  const String8 name = vkr_scene_get_name(scene, entity);
  if (vkr_scene_get_typed(scene, entity, type)) {
    snprintf(text, sizeof(text), "%.*s already runs %s", (int)name.length,
             name.str, type->label);
    vkr_editor_toast(editor, VKR_UI_ICON_INFO_FILL, theme->accent, text);
    return false_v;
  }
  if (!vkr_scene_type_allowed(scene, type) ||
      !vkr_entity_get_component(scene->world, entity, scene->comp_transform)) {
    snprintf(text, sizeof(text), "%.*s cannot run scripts", (int)name.length,
             name.str);
    vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, theme->warning, text);
    return false_v;
  }
  VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_ADD_COMPONENT,
                                 .entity = entity};
  request.values.component_type = type;
  vkr_type_defaults(type, request.values.component);
  *frame->scene_edit = request;
  /* The object becomes the selection so Details shows its new script. */
  snprintf(text, sizeof(text), "Attached %s to %.*s", type->label,
           (int)name.length, name.str);
  vkr_editor_toast(editor, VKR_UI_ICON_CODE, theme->accent, text);
  return true_v;
}

void vkr_editor_drop_script(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            const char *module, Vec2 drop_px) {
  const VkrTypeDesc *type = vkr_editor_module_script(frame, module);
  if (!type) {
    char text[160];
    snprintf(text, sizeof(text), "%s has not built yet; see the Script editor",
             module ? module : "The script");
    vkr_editor_toast(editor, VKR_UI_ICON_LOG_WARNING, vkr_ui_theme()->warning,
                     text);
    return;
  }
  if (!frame->pick_request) {
    return;
  }
  editor->script_drop_type = type;
  *frame->pick_request =
      (VkrSamplePickRequest){.request = true_v,
                             .position_px = drop_px,
                             .purpose = VKR_SAMPLE_PICK_SCRIPT_DROP};
}

void vkr_editor_finish_script_drop(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  const VkrTypeDesc *type = editor->script_drop_type;
  editor->script_drop_type = NULL;
  if (!type) {
    return;
  }
  if (frame->context_entity.u64) {
    (void)vkr_editor_attach_script(editor, frame, frame->context_entity, type);
    return;
  }
  /* Empty space: a new object named after the script, running it. */
  for (uint32_t kind = 0; kind < vkr_editor_object_kind_count(); ++kind) {
    EditorObjectKind object;
    if (editor_object_kind(kind, &object) && object.type == type) {
      (void)vkr_editor_request_create(editor, frame, kind,
                                      vkr_editor_create_container(frame),
                                      &frame->context_position_px);
      return;
    }
  }
}

void vkr_editor_scene_panels_request_rename(VkrEditorScenePanels *panels) {
  if (panels) {
    panels->rename_request = true_v;
  }
}

void vkr_editor_request_script(const VkrSampleUiFrame *frame,
                               VkrEntityId entity, const VkrTypeDesc *type) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    return;
  }
  const VkrTypeDesc *current = vkr_editor_entity_script(scene, entity);
  if (current == type) {
    return;
  }
  VkrSceneEditRequest request = {.entity = entity};
  if (!type) {
    request.action = VKR_SCENE_EDIT_REMOVE_COMPONENT;
    request.values.component_type = current;
  } else {
    request.action = current ? VKR_SCENE_EDIT_REPLACE_COMPONENT
                             : VKR_SCENE_EDIT_ADD_COMPONENT;
    request.replaced_type = current;
    request.values.component_type = type;
    vkr_type_defaults(type, request.values.component);
  }
  *frame->scene_edit = request;
}

uint16_t vkr_editor_create_container(const VkrSampleUiFrame *frame) {
  const VkrSampleUiFrame container =
      vkr_editor_entity_frame(frame, frame->selected_entity);
  if (container.scene &&
      vkr_scene_entity_alive(container.scene, frame->selected_entity)) {
    return frame->selected_entity.parts.world;
  }
  if (frame->scene) {
    return 0u;
  }
  return frame->world ? VKR_SCENE_WORLD_ROOT_ID : UINT16_MAX;
}

/* A new text turns about its up axis to face the camera, and its origin, the
   corner of its one meter wide box, moves so the box center lands on the
   placed point. */
static void editor_place_text(VkrSceneEditValues *values,
                              const VkrEditorDropPose *pose) {
  const VkrQuat inverse = vkr_quat_conjugate(pose->rotation);
  const Vec3 to_eye =
      vkr_quat_rotate_vec3(inverse, vec3_sub(pose->eye, pose->position));
  if (to_eye.x * to_eye.x + to_eye.z * to_eye.z > 1e-6f) {
    const VkrQuat face = vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f),
                                                  atan2f(to_eye.x, to_eye.z));
    values->rotation = vkr_quat_normalize(vkr_quat_mul(pose->rotation, face));
  }
  const VkrSceneText3DConfig box = VKR_SCENE_TEXT3D_CONFIG_DEFAULT;
  const Vec3 center = vec3_new(
      0.5f, 0.5f * (float32_t)box.texture_height / (float32_t)box.texture_width,
      0.0f);
  values->position = vec3_sub(values->position,
                              vkr_quat_rotate_vec3(values->rotation, center));
}

/* A brush kind runs its agent operation at the placement point, snapped to
   the brush grid, without review: the designer made it. */
static bool8_t editor_request_brush(const VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t brush, uint16_t container,
                                    const Vec2 *drop_px) {
  if (container == UINT16_MAX || !editor->agent) {
    return false_v;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 pixel =
      drop_px ? *drop_px
              : (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f};
  VkrEditorDropPose pose;
  if (!vkr_editor_viewport_place(editor, frame, pixel, 0.0f, &pose)) {
    return false_v;
  }
  const Vec3 p = vec3_new(roundf(pose.position.x * 4.0f) * 0.25f,
                          roundf(pose.position.y * 4.0f) * 0.25f,
                          roundf(pose.position.z * 4.0f) * 0.25f);
  char target[16];
  if (container == VKR_SCENE_WORLD_ROOT_ID) {
    snprintf(target, sizeof(target), "\"world\"");
  } else if (container == 0u) {
    snprintf(target, sizeof(target), "\"primary\"");
  } else {
    snprintf(target, sizeof(target), "%u", (unsigned)container);
  }
  char line[512];
  if (brush == 4u) {
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"create\",\"op\":\"blockout.room\",\"args\":"
             "{\"min\":[%g,%g,%g],\"size\":[6,3,6],\"container\":%s,"
             "\"review\":false}}",
             p.x - 3.0f, p.y, p.z - 3.0f, target);
  } else if (brush == 3u) {
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"create\",\"op\":\"brush.cylinder\",\"args\":"
             "{\"center\":[%g,%g,%g],\"radius\":1,\"height\":2,"
             "\"sides\":12,\"container\":%s,\"review\":false}}",
             p.x, p.y, p.z, target);
  } else {
    snprintf(line, sizeof(line),
             "{\"v\":1,\"id\":\"create\",\"op\":\"%s\",\"args\":"
             "{\"min\":[%g,%g,%g],\"max\":[%g,%g,%g],\"container\":%s,"
             "\"review\":false}}",
             brush == 2u ? "brush.wedge" : "brush.box", p.x - 1.0f, p.y,
             p.z - 1.0f, p.x + 1.0f, p.y + 2.0f, p.z + 1.0f, target);
  }
  return vkr_editor_agent_submit(editor->agent, line);
}

bool8_t vkr_editor_request_create(const VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, uint32_t kind,
                                  uint16_t container, const Vec2 *drop_px) {
  EditorObjectKind kind_value;
  if (!editor_object_kind(kind, &kind_value)) {
    return false_v;
  }
  const EditorObjectKind *object = &kind_value;
  if (object->brush) {
    return editor_request_brush(editor, frame, object->brush, container,
                                drop_px);
  }
  /* World-only settings always go to the World. */
  if (object->type && (object->type->flags & VKR_TYPE_FLAG_WORLD_ONLY)) {
    container = frame->world ? VKR_SCENE_WORLD_ROOT_ID : UINT16_MAX;
  }
  if (container == UINT16_MAX) {
    return false_v;
  }
  VkrSceneEditValues values;
  MemZero(&values, sizeof(values));
  values.fields = VKR_SCENE_EDIT_NAME;
  snprintf(values.name, sizeof(values.name), "%s", object->label);
  if (object->type == &vkr_scene_point_light_type) {
    vkr_type_defaults(object->type, &values.point_light);
    if (object->spot) {
      values.point_light.kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT;
    }
    values.fields |= VKR_SCENE_EDIT_POINT_LIGHT;
  } else if (object->type == &vkr_scene_rectangle_light_type) {
    vkr_type_defaults(object->type, &values.rectangle_light);
    values.fields |= VKR_SCENE_EDIT_RECTANGLE_LIGHT;
  } else if (object->type == &vkr_scene_directional_light_type) {
    vkr_type_defaults(object->type, &values.directional_light);
    values.fields |= VKR_SCENE_EDIT_DIRECTIONAL_LIGHT;
  } else if (object->type) {
    values.component_type = object->type;
    vkr_type_defaults(object->type, values.component);
    values.fields |= VKR_SCENE_EDIT_COMPONENT;
  }
  /* A shape is centred on its origin, so it rests half its height above the
     snap point; a text's center rises an em above it. */
  const bool8_t text = object->type == &vkr_scene_text_type;
  const float32_t base =
      object->type == &vkr_scene_shape_type
          ? ((const SceneShapeSettings *)values.component)->dimensions.y * 0.5f
      : text ? ((const SceneTextSettings *)values.component)->size
             : 0.0f;
  const Vec4 image = frame->mapping.image_rect_px;
  const Vec2 pixel =
      drop_px ? *drop_px
              : (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f};
  VkrEditorDropPose pose;
  if (vkr_editor_viewport_place(editor, frame, pixel, base, &pose)) {
    values.fields |= VKR_SCENE_EDIT_TRANSFORM;
    values.position = pose.position;
    values.rotation = pose.rotation;
    values.scale = vec3_one();
    if (text) {
      editor_place_text(&values, &pose);
    }
  }
  *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_CREATE,
                                             .values = values,
                                             .container = container};
  return true_v;
}

void vkr_editor_apply_content_object(const VkrSampleUiFrame *frame,
                                     const VkrEditorContentAction *action) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, action->entity);
  if (!scene || !vkr_scene_entity_alive(scene, action->entity)) {
    return;
  }
  VkrSceneEditRequest request = {.entity = action->entity};
  switch (action->kind) {
  case VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY:
    request.action = VKR_SCENE_EDIT_SELECT;
    break;
  case VKR_EDITOR_CONTENT_ACTION_FRAME_ENTITY:
    request.action = VKR_SCENE_EDIT_FRAME;
    break;
  case VKR_EDITOR_CONTENT_ACTION_RENAME_ENTITY:
    if (!vkr_scene_edit_read(scene, action->entity, &request.values)) {
      return;
    }
    request.action = VKR_SCENE_EDIT_APPLY;
    request.values.fields = VKR_SCENE_EDIT_NAME;
    snprintf(request.values.name, sizeof(request.values.name), "%s",
             action->name);
    break;
  case VKR_EDITOR_CONTENT_ACTION_DELETE_ENTITY:
    /* The journal refuses what cannot be deleted; the reason shows in the
       status line as for the Outliner. */
    request.action = VKR_SCENE_EDIT_DELETE;
    break;
  default:
    return;
  }
  *frame->scene_edit = request;
}

bool8_t vkr_editor_component_read(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity, const VkrTypeDesc *type,
                                  void *out) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  if (!scene || !type || !out) {
    return false_v;
  }
  /* Lights live in edit values; world components in their typed storage. */
  if (vkr_scene_edit_component_field(type)) {
    VkrSceneEditValues values;
    return vkr_scene_edit_read(scene, entity, &values) &&
           vkr_scene_edit_component_get(&values, type, out);
  }
  const void *current = vkr_scene_get_typed(scene, entity, type);
  if (!current) {
    return false_v;
  }
  MemCopy(out, current, type->size);
  return true_v;
}

bool8_t vkr_editor_request_physics_body(const VkrSampleUiFrame *frame,
                                        VkrEntityId entity, bool8_t present) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  VkrSceneEditValues values;
  if (!scene || !vkr_scene_edit_read(scene, entity, &values) ||
      values.physics.present == present) {
    return false_v;
  }
  /* A new body is a static unit box collider; removal drops its colliders. */
  values.physics = vkr_scene_physics_default();
  values.physics.body.motion = VKR_PHYSICS_STATIC;
  if (!present) {
    values.physics.present = false_v;
    values.physics.collider_count = 0u;
    MemZero(values.physics.colliders, sizeof(values.physics.colliders));
  }
  values.fields = VKR_SCENE_EDIT_PHYSICS;
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
  return true_v;
}

void vkr_editor_request_component(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity, const VkrTypeDesc *type,
                                  const void *value) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  if (!scene || !type || !value) {
    return;
  }
  VkrSceneEditValues values;
  MemZero(&values, sizeof(values));
  if (vkr_scene_edit_component_field(type)) {
    if (!vkr_scene_edit_read(scene, entity, &values)) {
      return;
    }
    (void)vkr_scene_edit_component_set(&values, type, value);
    values.fields = vkr_scene_edit_component_field(type);
  } else {
    values.fields = VKR_SCENE_EDIT_COMPONENT;
    values.component_type = type;
    MemCopy(values.component, value, type->size);
  }
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
}

VkrUiIcon vkr_editor_entity_icon(const VkrScene *scene, VkrEntityId entity,
                                 bool8_t has_child, Vec4 *out_color) {
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
  /* Scripts are tags on an object, shown beside it, never its icon. */
  const VkrTypeDesc *world_type = NULL;
  for (uint32_t i = 0; (world_type = vkr_scene_world_type(i)); ++i) {
    if (!vkr_scene_world_type_registered(world_type) &&
        vkr_scene_get_typed(scene, entity, world_type)) {
      *out_color = (Vec4){0.62f, 0.78f, 0.98f, 1.0f};
      return vkr_editor_world_type_icon(world_type);
    }
  }
  *out_color = theme->text_secondary;
  return has_child ? VKR_UI_ICON_FOLDER : VKR_UI_ICON_EMPTY;
}

/* Toggles one entity's own visibility through the undoable edit journal. */
void vkr_editor_toggle_visibility(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity) {
  VkrSceneEditValues values;
  if (!vkr_scene_edit_read(vkr_editor_entity_scene(frame, entity), entity,
                           &values) ||
      !(values.fields & VKR_SCENE_EDIT_VISIBILITY))
    return;
  values.fields = VKR_SCENE_EDIT_VISIBILITY;
  values.visibility.visible = !values.visibility.visible;
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
}

/* Container display name: "World", the project's name for its open or added
   scene, or the scene document's file stem. */
static String8 hierarchy_container_name(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint32_t container) {
  if (container == TREE_CONTAINER_WORLD)
    return string8_lit("World");
  const String8 project_name = vkr_editor_projects_scene_name(editor->projects);
  if (container == TREE_CONTAINER_SCENE && project_name.length)
    return project_name;
  String8 path =
      container >= TREE_CONTAINER_ADDITIVE
          ? frame->additive_names[container - TREE_CONTAINER_ADDITIVE]
          : frame->scene_path;
  const String8 added = vkr_editor_projects_added_name(editor->projects, path);
  if (container >= TREE_CONTAINER_ADDITIVE && added.length)
    return added;
  uint64_t start = path.length;
  while (start > 0 && path.str[start - 1] != '/' && path.str[start - 1] != '\\')
    --start;
  String8 name = {.str = path.str + start, .length = path.length - start};
  const String8 suffix = string8_lit(".scene.json");
  if (name.length > suffix.length &&
      MemCompare(name.str + name.length - suffix.length, suffix.str,
                 suffix.length) == 0)
    name.length -= suffix.length;
  return name.length ? name : string8_lit("Scene");
}

/* Outliner table columns: three toggles (visibility, viewport icons, pin),
   the item label and, when the panel is wide enough, the type. */
typedef struct HierarchyColumns {
  float32_t width;
  float32_t name_x;
  /* Zero hides the Type column. */
  float32_t type_x;
  float32_t type_w;
} HierarchyColumns;

static HierarchyColumns hierarchy_columns(float32_t w) {
  HierarchyColumns cols = {.width = w,
                           .name_x = 6.0f + HIERARCHY_TOGGLE_PT * 3.0f + 4.0f};
  if (w >= 250.0f) {
    cols.type_w = vkr_clamp_f32(w * 0.3f, 72.0f, 150.0f);
    cols.type_x = w - 6.0f - cols.type_w;
  }
  return cols;
}

/* Right edge of the label column. */
static float32_t hierarchy_name_right(HierarchyColumns cols) {
  return cols.type_x > 0.0f ? cols.type_x - 6.0f : cols.width - 8.0f;
}

/* One toggle cell of a row: a ghost icon button centered in its column. */
static bool8_t hierarchy_toggle(VkrUiSystem *ui, String8 id, uint32_t column,
                                float32_t y, VkrUiIcon icon, Vec4 color,
                                String8 tooltip) {
  VkrUiWidgetConfig c =
      widget_at(6.0f + HIERARCHY_TOGGLE_PT * (float32_t)column,
                y + (HIERARCHY_ROW_PT - HIERARCHY_TOGGLE_PT) * 0.5f,
                HIERARCHY_TOGGLE_PT, HIERARCHY_TOGGLE_PT);
  vkr_editor_ghost_style(&c);
  c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
  c.icon = icon;
  c.icon_size_pt = 13.0f;
  c.icon_color = color;
  c.tooltip = tooltip;
  return vkr_ui_button(ui, id, (String8){0}, &c);
}

/* Type column text for an entity, matching its Outliner icon. */
static String8 hierarchy_type_label(const VkrScene *scene, VkrEntityId entity,
                                    bool8_t has_child) {
  VkrWorld *world = scene->world;
  if (vkr_entity_get_component(world, entity, scene->comp_directional_light))
    return string8_lit("Directional Light");
  const ScenePointLight *point =
      vkr_entity_get_component(world, entity, scene->comp_point_light);
  if (point)
    return point->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
               ? string8_lit("Spot Light")
               : string8_lit("Point Light");
  if (vkr_entity_get_component(world, entity, scene->comp_rectangle_light))
    return string8_lit("Rect Light");
  if (vkr_entity_get_component(world, entity, scene->comp_physics_body))
    return string8_lit("Rigid Body");
  if (vkr_entity_get_component(world, entity, scene->comp_physics_collider))
    return string8_lit("Collider");
  if (vkr_entity_get_component(world, entity, scene->comp_mesh_renderer))
    return vkr_scene_get_typed(scene, entity, &vkr_scene_animation_type)
               ? string8_lit("Animated Mesh")
               : string8_lit("Static Mesh");
  if (vkr_entity_get_component(world, entity, scene->comp_text3d))
    return string8_lit("Text");
  if (vkr_entity_get_component(world, entity, scene->comp_shape))
    return string8_lit("Shape");
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    if (!vkr_scene_world_type_registered(type) &&
        vkr_scene_get_typed(scene, entity, type))
      return string8_create_from_cstr((const uint8_t *)type->label,
                                      strlen(type->label));
  }
  return has_child ? string8_lit("Group") : string8_lit("Empty");
}

/* Type column cell: secondary text from the column's leading edge. */
static void hierarchy_type_cell(VkrUiSystem *ui, HierarchyColumns cols,
                                float32_t y, String8 text, bool8_t selected) {
  if (cols.type_x <= 0.0f || !text.length)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig c =
      widget_at(cols.type_x, y, cols.type_w, HIERARCHY_ROW_PT);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){4, 4, 4, 0};
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = selected ? theme->text_on_accent : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("node.type"), text, &c);
}

/* Container header: caret, icon and name; clicking folds the container. */
static void hierarchy_container_row(VkrEditorUi *editor,
                                    VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *frame,
                                    EditorTreeNode *n, HierarchyColumns cols,
                                    float32_t y, VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t w = cols.width;
  const float32_t row_h = HIERARCHY_ROW_PT;
  const float32_t indent =
      Min(n->depth * HIERARCHY_INDENT_PT, Max(0.0f, w - cols.name_x - 90.0f));
  const float32_t x = cols.name_x - 20.0f + indent;
  VkrUiWidgetConfig c = widget_at(4, y, w - 8, row_h);
  c.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  c.style.background_color = theme->header;
  c.style.hover_background_color = theme->row_hover;
  const bool8_t world = n->container == TREE_CONTAINER_WORLD;
  c.tooltip = world ? string8_lit("Project-wide objects every scene uses "
                                  "unless it has its own")
                    : string8_lit("The loaded scene");
  if (vkr_ui_button(ui, string8_lit("container"), (String8){0}, &c)) {
    p->container_collapsed[n->container] = n->expanded;
    p->rebuild = true_v;
  }
  c = widget_at(x, y + 3, 18, 18);
  c.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
  c.icon = n->expanded || p->search[0] ? VKR_UI_ICON_DISCLOSURE_OPEN
                                       : VKR_UI_ICON_DISCLOSURE_CLOSED;
  c.icon_size_pt = 10.0f;
  c.icon_color = theme->text_secondary;
  vkr_ui_label(ui, string8_lit("container.caret"), (String8){0}, &c);
  /* Buttons share the row's trailing edge; the name stops before them. */
  const uint32_t buttons = n->container >= TREE_CONTAINER_ADDITIVE ? 3u
                           : !world && frame->world                ? 1u
                                                                   : 0u;
  c = widget_at(x + 18.0f, y,
                Max(10.0f, w - x - 18.0f - 8.0f - 26.0f * (float32_t)buttons),
                row_h);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){4, 4, 4, 2};
  c.style.text_color = theme->text;
  c.text.font = heading;
  c.icon = world                                     ? VKR_UI_ICON_WORLD
           : n->container >= TREE_CONTAINER_ADDITIVE ? VKR_UI_ICON_LAYERS
                                                     : VKR_UI_ICON_SCENE;
  c.icon_size_pt = 14.0f;
  c.icon_color = world ? (Vec4){0.62f, 0.78f, 0.98f, 1.0f} : theme->accent;
  vkr_ui_label(ui, string8_lit("container.name"),
               hierarchy_container_name(editor, frame, n->container), &c);
  /* A scene inherits the World's objects unless scoped to its own. */
  const VkrScene *scene = tree_node_scene(frame, n);
  if (!world && frame->world && scene) {
    const bool8_t inherit = scene->settings.inherit_world;
    c = vkr_editor_icon_button_config(
        0, 0, VKR_UI_ICON_WORLD,
        inherit ? string8_lit("Inherits the World's objects where it has none; "
                              "click to use only its own")
                : string8_lit("Uses only its own objects; click to inherit "
                              "the World's"));
    c.placement = widget_at(w - 32, y + 2, 20, 20).placement;
    c.style.min_size_pt = c.style.max_size_pt = (Vec2){20, 20};
    c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
    c.icon_size_pt = 13.0f;
    c.icon_color = inherit ? theme->accent_hover : theme->text_disabled;
    if (vkr_ui_button(ui, string8_lit("container.inherit"), (String8){0}, &c)) {
      VkrSceneEditRequest request = {
          .action = VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS,
          .container =
              n->container >= TREE_CONTAINER_ADDITIVE
                  ? (uint16_t)(n->container - TREE_CONTAINER_ADDITIVE + 1u)
                  : 0u};
      request.scene_settings = scene->settings;
      request.scene_settings.inherit_world = !inherit;
      *frame->scene_edit = request;
    }
  }
  if (n->container >= TREE_CONTAINER_ADDITIVE) {
    const uint16_t container =
        (uint16_t)(n->container - TREE_CONTAINER_ADDITIVE + 1u);
    const String8 path =
        frame->additive_names[n->container - TREE_CONTAINER_ADDITIVE];
    if (vkr_editor_projects_added_name(editor->projects, path).length) {
      c = vkr_editor_icon_button_config(
          0, 0, VKR_UI_ICON_PIN,
          string8_lit("Set primary: edit this scene in the viewport and keep "
                      "the current one beside it"));
      c.placement = widget_at(w - 84, y + 2, 20, 20).placement;
      c.style.min_size_pt = c.style.max_size_pt = (Vec2){20, 20};
      c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
      c.icon_size_pt = 13.0f;
      c.disabled = !vkr_editor_projects_switch_ready(editor->projects);
      if (vkr_ui_button(ui, string8_lit("container.primary"), (String8){0}, &c))
        (void)vkr_editor_projects_set_primary(editor->projects, editor, frame,
                                              container);
    }
    c = vkr_editor_icon_button_config(0, 0, VKR_UI_ICON_CLOSE,
                                      string8_lit("Remove this scene from the "
                                                  "world (unsaved edits "
                                                  "block it)"));
    c.placement = widget_at(w - 58, y + 2, 20, 20).placement;
    c.style.min_size_pt = c.style.max_size_pt = (Vec2){20, 20};
    c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
    c.icon_size_pt = 13.0f;
    if (vkr_ui_button(ui, string8_lit("container.remove"), (String8){0}, &c))
      *frame->scene_request =
          (VkrSampleSceneRequest){.remove = true_v, .container = container};
  }
}

/* Toolbar: search with an Add action beside it. */
static void hierarchy_toolbar(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, float32_t w) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorScenePanels *p = editor->scene_panels;
  VkrUiSystem *ui = frame->ui;
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
        string8_lit("Create an object in the selection's scene, or the "
                    "primary scene"));
    add.icon_color = theme->accent_hover;
    const uint16_t container = vkr_editor_create_container(frame);
    add.disabled = container == UINT16_MAX;
    if (vkr_ui_button(ui, string8_lit("hierarchy.add"), (String8){0}, &add)) {
      vkr_editor_context_open(
          editor, VKR_EDITOR_CONTEXT_CREATE,
          (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                 (float32_t)ui->mouse_y / ui->content_scale});
      editor->context_container = container;
    }
    (void)vkr_ui_panel_end(ui);
  }
}

/* A click selects a row; a second one within 0.4 s frames the object in the
   Scene and opens its script when it has one. */
static void hierarchy_row_click(VkrEditorUi *editor, VkrEditorScenePanels *p,
                                const VkrSampleUiFrame *frame,
                                VkrEntityId entity) {
  const float64_t now = vkr_platform_get_absolute_time();
  const bool8_t twice =
      p->click_entity.u64 == entity.u64 && now - p->click_time < 0.4;
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = twice ? VKR_SCENE_EDIT_FRAME : VKR_SCENE_EDIT_SELECT,
      .entity = entity};
  if (twice) {
    (void)vkr_editor_open_entity_script(editor, frame, entity);
  }
  p->click_entity = twice ? VKR_ENTITY_ID_INVALID : entity;
  p->click_time = now;
}

/* A right click selects the row and opens its context menu. */
static void hierarchy_row_menu(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrEntityId entity) {
  VkrUiSystem *ui = frame->ui;
  *frame->scene_edit =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT, .entity = entity};
  vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_ENTITY,
                          (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                                 (float32_t)ui->mouse_y / ui->content_scale});
  editor->context_entity = entity;
}

/* A Script asset dragged from Content attaches to the row it is released
   over; true while it would, so the row lights. */
static bool8_t hierarchy_script_drop(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiId node_id, VkrEntityId entity,
                                     VkrUiRect list) {
  VkrUiSystem *ui = frame->ui;
  char module[VKR_EDITOR_SCRIPT_NAME];
  VkrUiRect row = {0};
  if (!vkr_editor_content_dragged_script(editor->content, module,
                                         sizeof(module)) ||
      !vkr_ui_widget_rect(ui, node_id, &row) || !in_rect(ui, row) ||
      !in_rect(ui, list)) {
    return false_v;
  }
  if (ui->mouse_released) {
    vkr_editor_content_end_drag(editor->content);
    (void)vkr_editor_attach_script(editor, frame, entity,
                                   vkr_editor_module_script(frame, module));
  }
  return true_v;
}

/* A scripted object names its script in a chip ending at `right`, when the
   row's `room` leaves the name 90 points; returns the width it took. */
static float32_t hierarchy_script_chip(VkrUiSystem *ui, const VkrScene *scene,
                                       VkrEntityId entity, float32_t right,
                                       float32_t y, float32_t height,
                                       float32_t room, bool8_t selected) {
  const VkrTypeDesc *script = vkr_editor_entity_script(scene, entity);
  const float32_t width =
      script ? Min(120.0f, (float32_t)strlen(script->label) * 6.4f + 28.0f)
             : 0.0f;
  if (!script || room - width <= 90.0f) {
    return 0.0f;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec4 script_color = {0.80f, 0.66f, 0.98f, 1.0f};
  VkrUiWidgetConfig c =
      widget_at(right - width + 8.0f, y + 4.0f, width - 4.0f, height - 8.0f);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.background_color = vkr_ui_color_alpha(
      selected ? theme->text_on_accent : script_color, 0.16f);
  c.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
  c.style.padding_pt = (VkrUiEdges){1, 6, 1, 5};
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = selected ? theme->text_on_accent : script_color;
  c.icon = VKR_UI_ICON_CODE;
  c.icon_size_pt = 10.0f;
  c.icon_color = c.style.text_color;
  c.tooltip = string8_lit("The script this object runs");
  vkr_ui_label(ui, string8_lit("node.script"),
               string8_create_from_cstr((const uint8_t *)script->label,
                                        strlen(script->label)),
               &c);
  return width + 4.0f;
}

/* One object row: the toggles, caret, icon and name, script chip and type.
   `node_id` is the row button's id; returns whether the pointer is on it. */
static bool8_t hierarchy_entity_row(
    VkrEditorUi *editor, VkrEditorScenePanels *p, const VkrSampleUiFrame *frame,
    EditorTreeNode *n, uint32_t depth, HierarchyColumns cols, float32_t y,
    bool8_t selected, VkrUiId node_id, VkrUiRect list, float64_t now) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t w = cols.width;
  const float32_t row_h = HIERARCHY_ROW_PT;
  const VkrScene *row_scene = tree_node_scene(frame, n);
  const SceneVisibility *visibility = vkr_entity_get_component(
      row_scene->world, n->entity, row_scene->comp_visibility);
  const bool8_t hidden = visibility && !visibility->visible;
  const bool8_t script_over =
      hierarchy_script_drop(editor, frame, node_id, n->entity, list);
  /* Full-width row: hover and selection fills. */
  VkrUiWidgetConfig c = widget_at(4, y, w - 8, row_h);
  c.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  c.style.background_color =
      script_over ? vkr_ui_color_alpha(theme->accent, 0.35f)
      : selected  ? theme->selection
      : n->pinned ? vkr_ui_color_alpha(theme->accent, 0.08f)
                  : (Vec4){0};
  c.style.hover_background_color =
      selected ? theme->selection : theme->row_hover;
  String8 name = vkr_scene_get_name(row_scene, n->entity);
  if (!name.length)
    name = string8_lit("(unnamed)");
  c.tooltip = name;
  if (vkr_ui_button(ui, string8_lit("node"), (String8){0}, &c))
    hierarchy_row_click(editor, p, frame, n->entity);
  const bool8_t row_hot = ui->hot_id == node_id;
  if (row_hot && !ui->mouse_captured &&
      input_button_just_pressed(frame->input, BUTTON_RIGHT))
    hierarchy_row_menu(editor, frame, n->entity);

  /* Toggle columns. Idle toggles stay faint until the row is hovered. */
  const Vec4 idle = vkr_ui_color_alpha(selected ? theme->text_on_accent
                                                : theme->text_secondary,
                                       row_hot || selected ? 0.9f : 0.4f);
  const Vec4 off = selected ? theme->text_on_accent : theme->text;
  if (visibility &&
      hierarchy_toggle(ui, string8_lit("visibility"), 0, y,
                       hidden ? VKR_UI_ICON_EYE_SLASH : VKR_UI_ICON_EYE,
                       hidden ? off : idle,
                       hidden ? string8_lit("Show in the Scene (undoable)")
                              : string8_lit("Hide in the Scene (undoable)")))
    vkr_editor_toggle_visibility(frame, n->entity);
  const bool8_t inherited_icons = n->icons_off && !n->icons_hidden;
  Vec4 icons_color = n->icons_hidden ? off : idle;
  if (inherited_icons)
    icons_color = vkr_ui_color_alpha(off, 0.35f);
  if (hierarchy_toggle(
          ui, string8_lit("icons"), 1, y,
          n->icons_off ? VKR_UI_ICON_SELECTION : VKR_UI_ICON_TAG, icons_color,
          inherited_icons ? string8_lit("Viewport icons are off for a parent")
          : n->icons_hidden
              ? string8_lit("Show viewport icons for this object and its "
                            "children")
              : string8_lit("Hide viewport icons for this object and its "
                            "children"))) {
    n->icons_hidden = !n->icons_hidden;
    p->rebuild = true_v;
  }
  if ((n->pinned || row_hot || selected) &&
      hierarchy_toggle(
          ui, string8_lit("pin"), 2, y, VKR_UI_ICON_PIN,
          n->pinned ? (selected ? theme->text_on_accent : theme->accent_hover)
                    : idle,
          n->pinned ? string8_lit("Unpin")
                    : string8_lit("Pin to the top of the "
                                  "Outliner with its children"))) {
    n->pinned = !n->pinned;
    p->rebuild = true_v;
  }

  /* Label column: caret, icon and name, then the script chip. */
  const float32_t right = hierarchy_name_right(cols);
  const float32_t indent =
      Min(depth * HIERARCHY_INDENT_PT, Max(0.0f, right - cols.name_x - 90.0f));
  const float32_t x = cols.name_x + indent;
  if (n->child != NO_ROW) {
    c = widget_at(x - 2.0f, y + 3, 18, 18);
    vkr_editor_ghost_style(&c);
    c.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
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
  const float32_t lock = n->cooking ? 22.0f : 0.0f;
  const float32_t chip =
      hierarchy_script_chip(ui, row_scene, n->entity, right - lock, y, row_h,
                            right - lock - x - 18.0f, selected);
  Vec4 icon_color;
  c = widget_at(x + 16.0f, y, Max(10.0f, right - lock - chip - x - 16.0f),
                row_h);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){4, 4, 4, 2};
  c.style.text_color = hidden     ? theme->text_disabled
                       : selected ? theme->text_on_accent
                                  : theme->text;
  c.icon = vkr_editor_entity_icon(row_scene, n->entity, n->child != NO_ROW,
                                  &icon_color);
  c.icon_size_pt = 14.0f;
  c.icon_color = selected ? theme->text_on_accent : icon_color;
  if (hidden)
    c.icon_color.w = 0.45f;
  c.tooltip = (String8){0};
  if (n->cooking) {
    /* A pulsing spinner: its textures are still encoding. */
    c.icon = VKR_UI_ICON_SPINNER;
    c.icon_color = vkr_ui_color_alpha(
        selected ? theme->text_on_accent : theme->accent_hover,
        0.55f + 0.45f * sinf((float32_t)now * 4.0f));
    c.tooltip = string8_lit(
        "Cooking full-quality textures in the background; locked until "
        "they are ready");
  }
  vkr_ui_label(ui, string8_lit("node.label"),
               (String8){.str = name.str, .length = preview_length}, &c);
  if (n->cooking) {
    c = widget_at(right - 20.0f, y + 2, 20, 20);
    c.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
    c.icon = VKR_UI_ICON_LOCK;
    c.icon_size_pt = 12.0f;
    c.icon_color = selected ? theme->text_on_accent : theme->text_secondary;
    vkr_ui_label(ui, string8_lit("cooking"), (String8){0}, &c);
  }
  hierarchy_type_cell(
      ui, cols, y,
      hierarchy_type_label(row_scene, n->entity, n->child != NO_ROW), selected);
  return row_hot;
}

/* Column header: toggle glyphs, then Item Label and Type. */
static void hierarchy_column_header(VkrUiSystem *ui, HierarchyColumns cols,
                                    float32_t y, VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig bar = widget_at(0, y, cols.width, HIERARCHY_HEADER_PT);
  bar.style.background_color = theme->header;
  bar.style.border_pt = (VkrUiEdges){0, 0, 1, 0};
  bar.style.border_color = theme->separator;
  vkr_ui_label(ui, string8_lit("columns"), (String8){0}, &bar);
  static const struct {
    VkrUiIcon icon;
    const char *tooltip;
  } toggles[] = {
      {VKR_UI_ICON_EYE, "Visibility in the Scene"},
      {VKR_UI_ICON_TAG, "Viewport icons of the object and its children"},
      {VKR_UI_ICON_PIN, "Pinned to the top of the Outliner"},
  };
  for (uint32_t i = 0; i < ArrayCount(toggles); ++i) {
    VkrUiWidgetConfig c =
        widget_at(6.0f + HIERARCHY_TOGGLE_PT * (float32_t)i,
                  y + (HIERARCHY_HEADER_PT - HIERARCHY_TOGGLE_PT) * 0.5f,
                  HIERARCHY_TOGGLE_PT, HIERARCHY_TOGGLE_PT);
    c.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
    c.icon = toggles[i].icon;
    c.icon_size_pt = 12.0f;
    c.icon_color = theme->text_secondary;
    c.tooltip = string8_create_from_cstr((const uint8_t *)toggles[i].tooltip,
                                         strlen(toggles[i].tooltip));
    (void)vkr_ui_push_id_u64(ui, i);
    vkr_ui_label(ui, string8_lit("column.toggle"), (String8){0}, &c);
    (void)vkr_ui_pop_id(ui);
  }
  const float32_t right = hierarchy_name_right(cols);
  VkrUiWidgetConfig c = widget_at(
      cols.name_x, y, Max(10.0f, right - cols.name_x), HIERARCHY_HEADER_PT);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){4, 4, 4, 2};
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = theme->text_secondary;
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("column.label"), string8_lit("Item Label"), &c);
  if (cols.type_x > 0.0f) {
    VkrUiWidgetConfig divider =
        widget_at(cols.type_x - 4.0f, y + 5.0f, 1.0f, HIERARCHY_HEADER_PT - 10);
    divider.style.background_color = theme->separator;
    vkr_ui_label(ui, string8_lit("column.divider"), (String8){0}, &divider);
    c = widget_at(cols.type_x, y, cols.type_w, HIERARCHY_HEADER_PT);
    c.placement.align = VKR_UI_ALIGN_START;
    c.style.padding_pt = (VkrUiEdges){4, 4, 4, 0};
    c.style.font_size_pt = theme->font_caption;
    c.style.text_color = theme->text_secondary;
    c.text.font = heading;
    vkr_ui_label(ui, string8_lit("column.type"), string8_lit("Type"), &c);
  }
}

/* Pinned objects above the tree, each with its expanded subtree; returns
   the height used. */
static float32_t hierarchy_pinned_build(VkrEditorUi *editor,
                                        VkrEditorScenePanels *p,
                                        const VkrSampleUiFrame *frame,
                                        HierarchyColumns cols, float32_t top,
                                        float32_t limit, VkrUiRect list,
                                        float64_t now) {
  if (!p->pinned_row_count || limit < HIERARCHY_ROW_PT * 2.0f)
    return 0.0f;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const uint32_t shown = Min(
      p->pinned_row_count, Min(HIERARCHY_PINNED_ROWS_SHOWN,
                               (uint32_t)((limit - 6.0f) / HIERARCHY_ROW_PT)));
  const float32_t height = (float32_t)shown * HIERARCHY_ROW_PT + 6.0f;
  VkrUiPanelConfig block = vkr_ui_panel_config_default();
  VkrUiWidgetConfig area = widget_at(0, top, cols.width, height);
  block.placement = area.placement;
  block.style.min_size_pt = block.style.max_size_pt = area.style.min_size_pt;
  block.style.background_color = vkr_ui_color_alpha(theme->accent, 0.05f);
  block.style.border_pt = (VkrUiEdges){0, 0, 1, 0};
  block.style.border_color = theme->separator;
  block.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("hierarchy.pinned"), &block))
    return height;
  for (uint32_t i = 0; i < shown; ++i) {
    EditorTreeNode *n = &p->nodes[p->pinned_rows[i]];
    (void)vkr_ui_push_id_u64(ui, 0x9100u + i);
    const VkrUiId node_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("node"));
    (void)hierarchy_entity_row(editor, p, frame, n, p->pinned_depths[i], cols,
                               3.0f + (float32_t)i * HIERARCHY_ROW_PT,
                               n->entity.u64 == frame->selected_entity.u64,
                               node_id, list, now);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  return height;
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
  hierarchy_toolbar(editor, frame, w);
  VkrUiWidgetConfig c;
  bool8_t any_additive = false_v;
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i)
    any_additive |= frame->additive[i] != NULL;
  if (!frame->scene && !frame->world && !any_additive) {
    c = widget_at(8, 64, w - 16, 40);
    c.style.text_color = theme->text_secondary;
    c.placement.justify = VKR_UI_ALIGN_CENTER;
    c.icon = frame->world_loading ? VKR_UI_ICON_SPINNER : VKR_UI_ICON_SCENE;
    c.icon_size_pt = 18.0f;
    c.icon_color = theme->text_disabled;
    /* Scenes open from Content or the Scenes menu; a World whose document
       changed streams back in. */
    vkr_ui_label(ui, string8_lit("no.scene"),
                 frame->world_loading ? string8_lit("Loading World...")
                                      : string8_lit("No scene loaded"),
                 &c);
    return;
  }
  if (p->rebuild || p->generation != frame->scene_generation ||
      p->tree_world != frame->world ||
      (frame->scene &&
       p->structure_revision != frame->scene->structure_revision) ||
      (frame->world &&
       p->world_structure_revision != frame->world->structure_revision) ||
      tree_additive_changed(p, frame) ||
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
  /* Models whose textures cook are marked and locked (ADR-077). */
  char revisions[16][37];
  const uint32_t cooking_count = vkr_editor_content_cooking(
      editor->content, revisions, ArrayCount(revisions), NULL, 0u);
  uint64_t cooking_key = UINT64_C(14695981039346656037) ^ cooking_count;
  for (uint32_t r = 0u; r < cooking_count; ++r) {
    for (const char *byte = revisions[r]; *byte; ++byte) {
      cooking_key = (cooking_key ^ (uint8_t)*byte) * UINT64_C(1099511628211);
    }
  }
  const float64_t marked_at = vkr_platform_get_absolute_time();
  if (cooking_key != p->cooking_key ||
      (cooking_count && marked_at - p->cooking_marked_at > 1.0)) {
    tree_mark_cooking(p, frame, revisions, cooking_count);
    p->cooking_key = cooking_key;
    p->cooking_marked_at = marked_at;
  }
  const HierarchyColumns cols = hierarchy_columns(w);
  hierarchy_column_header(ui, cols, HIERARCHY_TOOLS_PT, heading);
  const float32_t pinned_top = HIERARCHY_TOOLS_PT + HIERARCHY_HEADER_PT;
  const float32_t pinned_h = hierarchy_pinned_build(
      editor, p, frame, cols, pinned_top,
      (h - pinned_top - HIERARCHY_FOOTER_PT) * 0.4f, rect, marked_at);
  const float32_t list_top = pinned_top + pinned_h;
  const float32_t row_h = HIERARCHY_ROW_PT;
  float32_t page = Max(0.0f, h - list_top - HIERARCHY_FOOTER_PT - 2.0f);
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
  VkrUiPanelConfig list = vkr_ui_panel_config_default();
  c = widget_at(0, list_top, w, page);
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
    (void)vkr_ui_scroll_area_offset(ui, &p->hierarchy_scroll);
    const uint32_t first = (uint32_t)(p->hierarchy_scroll / row_h);
    const uint32_t end =
        Min(p->row_count,
            first + Min(TREE_VISIBLE_SLOTS, (uint32_t)(page / row_h) + 2u));
    for (uint32_t row = first; row < end; ++row) {
      EditorTreeNode *n = &p->nodes[p->rows[row]];
      const uint32_t slot = row - first;
      const float32_t y = row * row_h;
      const float32_t visible_y = y - p->hierarchy_scroll;
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
      if (p->pending_focus.u64 == n->entity.u64 && !n->header) {
        ui->focused_id = node_id;
        p->pending_focus = VKR_ENTITY_ID_INVALID;
      }
      if (n->header) {
        hierarchy_container_row(editor, p, frame, n, cols, y, heading);
        (void)vkr_ui_pop_id(ui);
        continue;
      }
      (void)hierarchy_entity_row(editor, p, frame, n, n->depth, cols, y,
                                 selected_row == row, node_id, rect, marked_at);
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
        /* Container headers are not entities; navigation stops on them. */
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
  float32_t numbers[PHYSICS_NUMBER_COUNT] = {0};
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
    snprintf(p->error, sizeof(p->error), "Invalid name or physics values.");
    return false_v;
  }
  return true_v;
}

static void inspector_clear_focus(VkrUiSystem *ui) {
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.scroll"));
  (void)vkr_ui_push_id_label(ui, string8_lit("inspector.fields"));
  const char *labels[] = {"name",  "visibility", "inherit", "apply", "revert",
                          "frame", "undo",       "redo",    "save"};
  for (uint32_t i = 0; i < ArrayCount(labels); ++i) {
    VkrUiId id = vkr_ui_id_stack_widget_label(
        &ui->id_stack, string8_create((uint8_t *)labels[i], strlen(labels[i])));
    if (ui->focused_id == id)
      ui->focused_id = 0;
    if (ui->active_id == id)
      ui->active_id = 0;
  }
  (void)vkr_ui_pop_id(ui);
  (void)vkr_ui_pop_id(ui);
}

static bool8_t physics_number_widget(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                     float32_t w, float32_t *y,
                                     const char *label, uint32_t index,
                                     bool8_t disabled) {
  (void)vkr_ui_push_id_u64(ui, index);
  VkrUiWidgetConfig c = widget_at(INSPECTOR_PAD_PT, *y, w * 0.53f - 11, 24);
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
    if (inspector_button(
            ui, string8_lit("preset.next"),
            string8_create_formatted(ui->frame_allocator, "Preset: %s (next)",
                                     settings.presets[p->preset_index].name),
            &c)) {
      p->preset_index = (p->preset_index + 1u) % settings.preset_count;
    }
    *y += 26;
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
    c.disabled = disabled;
    if (inspector_button(ui, string8_lit("preset.apply"),
                         string8_lit("Copy preset to draft"), &c) &&
        physics_numbers_parse(p, body)) {
      const VkrSceneCollisionPreset *preset =
          &settings.presets[p->preset_index];
      body->collision_layer = preset->membership;
      body->collision_mask = preset->mask;
      body->body.sensor = preset->sensor;
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
      c = widget_at(INSPECTOR_PAD_PT + mask * (w - INSPECTOR_PAD_PT * 2) / 2,
                    *y, (w - INSPECTOR_PAD_PT * 2) / 2 - 2, 24);
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  vkr_ui_label(ui, string8_lit("bone.name"),
               string8_create_formatted(ui->frame_allocator, "Bone %u: %.*s",
                                        attachment->source_node,
                                        (int)name.length, name.str),
               &c);
  *y += 26;
  for (uint32_t next = 0; next < 2; ++next) {
    c = widget_at(INSPECTOR_PAD_PT + next * (w - INSPECTOR_PAD_PT * 2) / 2, *y,
                  (w - INSPECTOR_PAD_PT * 2) / 2 - 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
        target_body.present && target_body.body.enabled;
    bool8_t target_has_shape = false_v;
    if (target_available) {
      for (uint32_t shape = 0; shape < target_body.collider_count; ++shape) {
        target_has_shape |= target_body.colliders[shape].enabled;
      }
    }
    if (!target_available || !target_has_shape ||
        vkr_scene_physics_body_is_disabled(f->scene, resolved_target)) {
      c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
      vkr_ui_label(ui, string8_lit("joint.suspended"),
                   string8_lit("Suspended: target body unavailable"), &c);
      *y += 26;
    }
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  vkr_ui_label(ui, string8_lit("ragdoll.title"),
               string8_lit("Ragdoll from skin joints"), &c);
  *y += 26;
  const char *actions[] = {"Create ragdoll", "Enable ragdoll",
                           "Disable ragdoll", "Delete ragdoll"};
  for (uint32_t i = 0; i < ArrayCount(actions); ++i) {
    if (i == 2) {
      *y += 26;
    }
    c = widget_at(INSPECTOR_PAD_PT + (i % 2u) * (w - INSPECTOR_PAD_PT * 2) / 2,
                  *y, (w - INSPECTOR_PAD_PT * 2) / 2 - 2, 24);
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

/* Descriptor rows over part of the physics draft. A change marks the draft;
   a drag holds the commit until it ends, so one gesture is one edit. */
static bool8_t physics_details(VkrEditorScenePanels *p,
                               const VkrSampleUiFrame *f, float32_t w,
                               float32_t *y, const VkrTypeDesc *type,
                               void *value, bool8_t disabled) {
  const VkrEditorDetailsResult result = vkr_editor_details_type(
      &p->details, f->ui, f->input, w, y, type, value, NULL, disabled);
  p->changed |= result.changed;
  p->physics_dragging |= result.gesture && p->details.gesture_active;
  return result.focused;
}

static bool8_t physics_body_widgets(VkrEditorScenePanels *p,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y, bool8_t disabled) {
  VkrUiSystem *ui = f->ui;
  VkrScenePhysicsSnapshot *body = &p->values.physics;
  bool8_t focused = false_v;
  const bool8_t muted =
      vkr_scene_physics_body_is_disabled(f->scene, f->selected_entity);
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  focused |= physics_details(p, f, w, y, &vkr_scene_physics_body_type,
                             &body->body, disabled);
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
    const bool8_t open = p->open_collider == shape->authored_id;
    VkrUiWidgetConfig c =
        widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
    vkr_editor_ghost_style(&c);
    c.icon = open ? VKR_UI_ICON_CHEVRON_DOWN : VKR_UI_ICON_CHEVRON_RIGHT;
    c.icon_size_pt = 11.0f;
    if (inspector_button(
            ui, string8_lit("expand"),
            string8_create_formatted(ui->frame_allocator, "%s collider %llu%s",
                                     shapes[shape->shape] + 2,
                                     (unsigned long long)shape->authored_id,
                                     shape->enabled ? "" : " (disabled)"),
            &c)) {
      p->open_collider = open ? 0 : shape->authored_id;
    }
    *y += 26;
    if (!open) {
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    focused |= physics_details(p, f, w, y, &vkr_scene_physics_collider_type,
                               shape, disabled);
    if (shape->shape < VKR_PHYSICS_CONVEX_HULL) {
      c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
      c.disabled = disabled;
      if (inspector_button(ui, string8_lit("fit"),
                           string8_lit("Fit loaded bounds (approx.)"), &c) &&
          physics_fit_collider(p, f, shape)) {
        p->changed = true_v;
      }
      *y += 26;
    }
    c = widget_at(INSPECTOR_PAD_PT, *y, (w - INSPECTOR_PAD_PT * 2) / 2 - 3, 24);
    c.disabled =
        disabled || body->collider_count == VKR_SCENE_PHYSICS_MAX_COLLIDERS;
    if (inspector_button(ui, string8_lit("duplicate"), string8_lit("Duplicate"),
                         &c)) {
      const uint64_t id = physics_next_collider_id(body);
      body->colliders[body->collider_count] = *shape;
      body->colliders[body->collider_count++].authored_id = id;
      p->changed = true_v;
    }
    c = widget_at(w / 2, *y, w / 2 - 5, 24);
    c.disabled = disabled;
    bool8_t removed = false_v;
    if (inspector_button(ui, string8_lit("remove"), string8_lit("Remove"),
                         &c)) {
      MemCopy(shape, shape + 1,
              (body->collider_count - i - 1u) * sizeof(*shape));
      body->collider_count--;
      MemZero(&body->colliders[body->collider_count], sizeof(*shape));
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  vkr_ui_label(ui, string8_lit("impulse.title"),
               string8_lit("Test impulse (N s; unsaved)"), &c);
  *y += 26;
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  (void)vkr_ui_checkbox(ui, string8_lit("impulse.at.point"),
                        string8_lit("Apply at world point"),
                        &p->impulse_at_point, &c);
  *y += 26;
  const char *labels[] = {"Impulse X",     "Impulse Y",     "Impulse Z",
                          "World point X", "World point Y", "World point Z"};
  for (uint32_t i = 0; i < (p->impulse_at_point ? 6u : 3u); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    c = widget_at(INSPECTOR_PAD_PT, *y, w * 0.53f - 11, 24);
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
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  c.disabled = p->changed || !body->body.enabled;
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
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
  c.text.font = heading;
  vkr_ui_label(ui, string8_lit("title"), string8_lit("Physics body (m, kg, s)"),
               &c);
  *y += 26;
  const SceneTransform *transform = vkr_entity_get_component(
      f->scene->world, f->selected_entity, f->scene->comp_transform);
  const bool8_t eligible = transform && transform->trs_editable;
  c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 44);
  const char *error = NULL;
  (void)vkr_scene_physics_validate(f->scene, f->selected_entity, body, &error);
  uint32_t enabled_colliders = 0;
  for (uint32_t i = 0; i < body->collider_count; ++i) {
    enabled_colliders += body->colliders[i].enabled;
  }
  const char *status =
      !eligible             ? "Physics requires an editable TRS transform."
      : !paused             ? "Pause simulation to edit physics."
      : error               ? error
      : !body->present      ? "No body. Add a shape to create one."
      : !body->body.enabled ? "Disabled: body excluded from simulation."
      : !enabled_colliders  ? "No enabled colliders: body suspended."
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
    c = widget_at(INSPECTOR_PAD_PT + (i % 3u) * (w - INSPECTOR_PAD_PT * 2) / 3,
                  *y, (w - INSPECTOR_PAD_PT * 2) / 3 - 3, 24);
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
        body->body.motion = VKR_PHYSICS_STATIC;
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
    c = widget_at(INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, 24);
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
  if (body->present && body->body.motion == VKR_PHYSICS_DYNAMIC &&
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
  INSPECTOR_SECTION_MESH,
} InspectorSection;

/* Collapsible section header; returns true while the section is expanded. */
static bool8_t inspector_section(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                 float32_t w, float32_t *y,
                                 InspectorSection section, VkrUiIcon icon,
                                 Vec4 icon_color, const char *title,
                                 VkrFontHandle heading) {
  (void)vkr_ui_push_id_u64(ui, 0x5ec70000u + section);
  const bool8_t expanded = vkr_editor_details_section(
      ui, string8_lit("section"), w, y, icon, icon_color,
      string8_create((uint8_t *)title, strlen(title)), heading,
      &p->section_collapsed[section]);
  (void)vkr_ui_pop_id(ui);
  return expanded;
}

static void inspector_checkbox_row(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                   float32_t w, float32_t *y, String8 id,
                                   const char *label, bool8_t *value,
                                   bool8_t disabled, String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t label_w = vkr_editor_details_label_width(w);
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y, label_w - 4.0f, INSPECTOR_ROW_PT - 2);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){5, 2, 5, 2};
  c.style.text_color = theme->text_secondary;
  c.style.font_size_pt = theme->font_body;
  c.tooltip = tooltip;
  (void)vkr_ui_push_id_label(ui, id);
  vkr_ui_label(ui, string8_lit("label"),
               string8_create((uint8_t *)label, strlen(label)), &c);
  c = widget_at(INSPECTOR_PAD_PT + label_w, *y + 2, INSPECTOR_ROW_PT - 4,
                INSPECTOR_ROW_PT - 4);
  c.placement.align = VKR_UI_ALIGN_START;
  c.style.padding_pt = (VkrUiEdges){3, 0, 3, 0};
  c.disabled = disabled;
  c.tooltip = tooltip;
  p->changed |=
      vkr_ui_checkbox(ui, string8_lit("check"), (String8){0}, value, &c);
  (void)vkr_ui_pop_id(ui);
  *y += INSPECTOR_ROW_PT + 2;
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
  const VkrUiIcon icon = vkr_editor_entity_icon(
      f->scene, f->selected_entity, node && node->child != NO_ROW, &icon_color);
  /* One 26-point row: type badge, name field and visibility toggle. */
  const float32_t card = INSPECTOR_ROW_PT;
  VkrUiWidgetConfig badge = widget_at(INSPECTOR_PAD_PT, *y + 4, card, card);
  badge.style.background_color = vkr_ui_color_alpha(icon_color, 0.16f);
  badge.style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  badge.style.padding_pt = (VkrUiEdges){5, 5, 5, 5};
  badge.icon = icon;
  badge.icon_size_pt = 15.0f;
  badge.icon_color = icon_color;
  vkr_ui_label(ui, string8_lit("badge"), (String8){0}, &badge);

  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT + card + 6, *y + 4,
                w - INSPECTOR_PAD_PT * 2 - card * 2 - 12, card);
  c.read_only = !(p->values.fields & VKR_SCENE_EDIT_NAME);
  c.style.font_size_pt = theme->font_body;
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
  if (p->rename_request) {
    p->rename_request = false_v;
    if (!c.read_only) {
      ui->focused_id =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("name"));
      ui->focused_is_text = true_v;
      (void)vkr_ui_keyboard_layer_set(ui, 0u);
    }
  }
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
      widget_at(w - INSPECTOR_PAD_PT - card, *y + 4, card, card).placement;
  eye.style.min_size_pt = eye.style.max_size_pt = (Vec2){card, card};
  eye.style.padding_pt = (VkrUiEdges){5, 5, 5, 5};
  eye.icon_size_pt = 15.0f;
  eye.icon_color = p->values.visibility.visible ? theme->text_secondary
                                                : theme->text_disabled;
  eye.disabled = !can_hide;
  if (can_hide &&
      vkr_ui_button(ui, string8_lit("visibility"), (String8){0}, &eye)) {
    p->values.visibility.visible = !p->values.visibility.visible;
    p->changed = true_v;
  }
  *y += card + 6;
  VkrUiWidgetConfig subtitle =
      widget_at(INSPECTOR_PAD_PT + card + 6, *y, w - INSPECTOR_PAD_PT * 2, 16);
  subtitle.placement.align = VKR_UI_ALIGN_START;
  subtitle.style.font_size_pt = theme->font_caption;
  subtitle.style.text_color = theme->text_secondary;
  subtitle.style.padding_pt = (VkrUiEdges){0, 2, 0, 2};
  const InspectorLights lights = inspector_lights(&p->values);
  const char *world_kind = NULL;
  const VkrTypeDesc *world_type = NULL;
  for (uint32_t i = 0; !world_kind && (world_type = vkr_scene_world_type(i));
       ++i) {
    /* The Script row names scripts; the kind is what the object is. */
    if (!vkr_scene_world_type_registered(world_type) &&
        vkr_scene_get_typed(f->scene, f->selected_entity, world_type))
      world_kind = world_type->label;
  }
  const char *kind =
      lights.directional          ? "Directional light"
      : lights.spot               ? "Spot light"
      : lights.point              ? "Point light"
      : lights.rectangle          ? "Rectangle light"
      : p->values.physics.present ? "Physics body"
      : vkr_entity_get_component(f->scene->world, f->selected_entity,
                                 f->scene->comp_mesh_renderer)
          ? "Mesh"
      : world_kind ? world_kind
                   : "Node";
  vkr_ui_label(ui, string8_lit("kind"),
               string8_create_formatted(ui->frame_allocator, "%s", kind),
               &subtitle);
  *y += 20;
  /* Unplaced world entities have no visibility to inherit. */
  if (can_hide)
    inspector_checkbox_row(p, ui, w, y, string8_lit("inherit"),
                           "Inherit visibility",
                           &p->values.visibility.inherit_parent, false_v,
                           string8_lit("Hidden when any ancestor is hidden"));
  return field_focus;
}

/* Component a section changed this build, and its gesture. */
typedef struct InspectorComponentEdit {
  const VkrTypeDesc *type;
  uint64_t gesture;
  /* World components apply from these bytes; others from the draft. */
  bool8_t world;
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
} InspectorComponentEdit;

/* Details rows for one component carried by the edit values. Components the
 * entity cannot edit, such as a transform without an editable TRS, show
 * read-only from `fallback`. */
static bool8_t inspector_component_rows(VkrEditorScenePanels *p,
                                        const VkrSampleUiFrame *f, float32_t w,
                                        float32_t *y, const VkrTypeDesc *type,
                                        const void *fallback,
                                        InspectorComponentEdit *edit) {
  uint8_t component[VKR_TYPE_VALUE_MAX];
  const bool8_t editable =
      vkr_scene_edit_component_get(&p->values, type, component);
  if (!editable) {
    if (!fallback) {
      return false_v;
    }
    MemCopy(component, fallback, type->size);
  }
  const VkrEditorDetailsResult result = vkr_editor_details_type(
      &p->details, f->ui, f->input, w, y, type, component, NULL, !editable);
  if (result.changed && editable) {
    (void)vkr_scene_edit_component_set(&p->values, type, component);
    edit->type = type;
    edit->gesture = result.gesture;
  }
  return result.focused;
}

static bool8_t inspector_transform_section(VkrEditorScenePanels *p,
                                           const VkrSampleUiFrame *f,
                                           float32_t w, float32_t *y,
                                           VkrFontHandle heading,
                                           const SceneTransform *tr,
                                           InspectorComponentEdit *edit) {
  const VkrUiTheme *theme = vkr_ui_theme();
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
  const SceneTransform shown = {.position = p->values.position,
                                .rotation = p->values.rotation,
                                .scale = p->values.scale};
  return inspector_component_rows(p, f, w, y, &vkr_scene_transform_type, &shown,
                                  edit);
}

/* Presets button on a component section header (ADR-076), left of `right`
   points; the menu opens once the Details build finishes. */
static void inspector_preset_button(VkrEditorScenePanels *p, VkrUiSystem *ui,
                                    float32_t right, float32_t header_y,
                                    const VkrTypeDesc *type) {
  if (vkr_editor_details_section_action(
          ui, string8_lit("presets"), right, header_y, VKR_UI_ICON_SPARKLE,
          string8_lit("Presets: save these values or apply saved ones "
                      "(undoable)"))) {
    p->preset_menu = type;
  }
}

/* One directional light is the sun (ADR-058) and at most one the moon
   (ADR-081); another enabled one says which light is, and a sun candidate
   offers to become the sun where setting its Atmosphere sun flag is enough:
   a scene's own flagged light outranks the World's. */
static void inspector_sun_note(VkrEditorScenePanels *p,
                               const VkrSampleUiFrame *f,
                               const VkrScene *resolver, float32_t w,
                               float32_t *y, InspectorComponentEdit *edit) {
  const SceneDirectionalLight *light = &p->values.directional_light;
  const bool8_t moon_light = light->atmosphere_moon;
  const VkrSceneSun *sun = !resolver    ? NULL
                           : moon_light ? &resolver->moon
                                        : &resolver->sun;
  if (!light->enabled || !sun ||
      (sun->found && sun->owner == f->scene &&
       sun->entity.u64 == f->selected_entity.u64)) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  const String8 name =
      sun->found ? vkr_scene_get_name(sun->owner, sun->entity) : (String8){0};
  VkrUiWidgetConfig note = widget_at(
      INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, INSPECTOR_ROW_PT);
  note.style.font_size_pt = theme->font_caption;
  note.style.text_color = theme->text_secondary;
  note.icon = VKR_UI_ICON_INFO_FILL;
  note.icon_size_pt = 12.0f;
  note.icon_color = theme->warning;
  const char *role = moon_light ? "moon" : "sun";
  String8 status = string8_lit("Inactive: another light is the sun");
  if (name.length) {
    status = string8_create_formatted(ui->frame_allocator,
                                      "Inactive: %.*s is the %s",
                                      (int)name.length, name.str, role);
  } else if (moon_light && !resolver->atmosphere.authored_settings.enabled) {
    status = string8_lit("Inactive: only an atmosphere has a moon");
  } else if (moon_light) {
    status = string8_lit("Inactive: another light is the moon");
  }
  vkr_ui_label(ui, string8_lit("sun.inactive"), status, &note);
  *y += INSPECTOR_ROW_PT;
  const bool8_t flag_wins = !moon_light &&
                            resolver->atmosphere.authored_settings.enabled &&
                            !light->atmosphere_sun && f->scene == resolver &&
                            (!sun->found || sun->owner != resolver);
  if (!flag_wins || edit->type) {
    return;
  }
  VkrUiWidgetConfig use =
      widget_at(INSPECTOR_PAD_PT, *y + 2.0f, w - INSPECTOR_PAD_PT * 2, 24.0f);
  vkr_editor_action_style(&use, VKR_FONT_HANDLE_INVALID);
  use.icon = VKR_UI_ICON_DIRECTIONAL_LIGHT;
  use.icon_size_pt = 13.0f;
  use.tooltip = string8_lit("Set Atmosphere sun on this light; a scene's own "
                            "sun outranks the World's");
  if (vkr_ui_button(ui, string8_lit("sun.use"), string8_lit("Use as sun"),
                    &use)) {
    SceneDirectionalLight next = *light;
    next.atmosphere_sun = true_v;
    (void)vkr_scene_edit_component_set(
        &p->values, &vkr_scene_directional_light_type, &next);
    edit->type = &vkr_scene_directional_light_type;
  }
  *y += 30.0f;
}

static bool8_t inspector_light_section(VkrEditorScenePanels *p,
                                       const VkrSampleUiFrame *f,
                                       const VkrScene *resolver, float32_t w,
                                       float32_t *y, VkrFontHandle heading,
                                       const InspectorLights *lights,
                                       InspectorComponentEdit *edit) {
  const char *title = lights->rectangle ? "Rectangle light"
                      : lights->spot    ? "Spot light"
                      : lights->point   ? "Point light"
                                        : "Directional light";
  const VkrUiIcon icon = lights->rectangle ? VKR_UI_ICON_RECT_LIGHT
                         : lights->spot    ? VKR_UI_ICON_SPOT_LIGHT
                         : lights->point   ? VKR_UI_ICON_POINT_LIGHT
                                           : VKR_UI_ICON_DIRECTIONAL_LIGHT;
  const VkrTypeDesc *type = lights->point ? &vkr_scene_point_light_type
                            : lights->rectangle
                                ? &vkr_scene_rectangle_light_type
                                : &vkr_scene_directional_light_type;
  const float32_t header_y = *y;
  const bool8_t expanded =
      inspector_section(p, f->ui, w, y, INSPECTOR_SECTION_LIGHT, icon,
                        (Vec4){0.98f, 0.78f, 0.36f, 1.0f}, title, heading);
  inspector_preset_button(p, f->ui, w - 8.0f, header_y, type);
  if (!expanded)
    return false_v;
  if (lights->directional && !lights->point && !lights->rectangle) {
    inspector_sun_note(p, f, resolver, w, y, edit);
  }
  return inspector_component_rows(p, f, w, y, type, NULL, edit);
}

/* Read-only rows generated from the mesh info descriptor (ADR-076). */
static void inspector_mesh_section(VkrEditorScenePanels *p,
                                   const VkrSampleUiFrame *f, float32_t w,
                                   float32_t *y, VkrFontHandle heading) {
  SceneMeshInfo info;
  /* A World or added scene object reads its own container. */
  const VkrScene *scene = vkr_editor_entity_scene(f, f->selected_entity);
  if (!scene || !vkr_scene_mesh_info(scene, f->selected_entity, &info) ||
      !inspector_section(p, f->ui, w, y, INSPECTOR_SECTION_MESH,
                         VKR_UI_ICON_MESH, (Vec4){0.62f, 0.78f, 0.98f, 1.0f},
                         "Mesh", heading)) {
    return;
  }
  (void)vkr_editor_details_type(&p->details, f->ui, f->input, w, y,
                                &vkr_scene_mesh_info_type, &info, NULL, true_v);
}

/* One section per world component on the entity (ADR-076). A singleton that
   lost resolution to another instance says so; its values still edit. A
   World object resolves through `resolver`, the scene that renders. */
static bool8_t inspector_world_sections(VkrEditorScenePanels *p,
                                        const VkrSampleUiFrame *f,
                                        const VkrScene *resolver, float32_t w,
                                        float32_t *y, VkrFontHandle heading,
                                        InspectorComponentEdit *edit) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  bool8_t focused = false_v;
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    const void *current =
        vkr_scene_get_typed(f->scene, f->selected_entity, type);
    if (!current || i >= ArrayCount(p->world_collapsed)) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, 0x3b7d0000u + i);
    const float32_t header_y = *y;
    const bool8_t expanded = vkr_editor_details_section(
        ui, string8_lit("world"), w, y, vkr_editor_world_type_icon(type),
        (Vec4){0.62f, 0.78f, 0.98f, 1.0f},
        string8_create((uint8_t *)type->label, strlen(type->label)), heading,
        &p->world_collapsed[i]);
    /* Live components can be removed and hold presets; load-baked ones keep
     * their document block. */
    if (vkr_scene_world_type_live(type)) {
      inspector_preset_button(p, ui, w - 34.0f, header_y, type);
      if (vkr_editor_details_section_action(
              ui, string8_lit("remove"), w - 8.0f, header_y, VKR_UI_ICON_TRASH,
              string8_lit("Remove this component (undoable)"))) {
        VkrSceneEditRequest request = {.action =
                                           VKR_SCENE_EDIT_REMOVE_COMPONENT,
                                       .entity = f->selected_entity};
        request.values.component_type = type;
        *f->scene_edit = request;
      }
    }
    if (expanded) {
      if ((type->flags & VKR_TYPE_FLAG_SINGLETON) &&
          !vkr_scene_singleton_active(
              f->scene == f->world && resolver ? resolver : f->scene,
              f->selected_entity, type)) {
        VkrUiWidgetConfig note = widget_at(
            INSPECTOR_PAD_PT, *y, w - INSPECTOR_PAD_PT * 2, INSPECTOR_ROW_PT);
        note.style.font_size_pt = theme->font_caption;
        note.style.text_color = theme->text_secondary;
        note.icon = VKR_UI_ICON_INFO_FILL;
        note.icon_size_pt = 12.0f;
        note.icon_color = theme->warning;
        vkr_ui_label(ui, string8_lit("inactive"),
                     string8_lit("Inactive: another instance takes effect"),
                     &note);
        *y += INSPECTOR_ROW_PT;
      }
      _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
      MemCopy(value, current, type->size);
      const VkrEditorDetailsResult result = vkr_editor_details_type(
          &p->details, ui, f->input, w, y, type, value, NULL, false_v);
      focused |= result.focused;
      if (result.changed && !edit->type) {
        edit->type = type;
        edit->world = true_v;
        edit->gesture = result.gesture;
        MemCopy(edit->value, value, type->size);
      }
      /* The collision matrix is part of the World's physics settings; its
         layer names and masks keep their dedicated editor. */
      if (type == &vkr_scene_physics_settings_type) {
        VkrUiWidgetConfig open = widget_at(INSPECTOR_PAD_PT, *y + 2.0f,
                                           w - INSPECTOR_PAD_PT * 2, 24.0f);
        vkr_editor_action_style(&open, VKR_FONT_HANDLE_INVALID);
        open.icon = VKR_UI_ICON_LAYERS;
        open.icon_size_pt = 13.0f;
        open.tooltip = string8_lit("Edit collision layers, the collision "
                                   "matrix and layer presets");
        if (vkr_ui_button(ui, string8_lit("collision.layers"),
                          string8_lit("Collision layers\xe2\x80\xa6"), &open)) {
          p->open_collision_layers = true_v;
        }
        *y += 30.0f;
      }
    }
    (void)vkr_ui_pop_id(ui);
  }
  return focused;
}

/* Script slot (ADR-079): a picker naming the entity's script, which opens
   the project's script types, and a button opening its source. */
static void inspector_script_row(VkrEditorUi *editor, VkrEditorScenePanels *p,
                                 const VkrSampleUiFrame *f, float32_t w,
                                 float32_t *y) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = f->ui;
  const VkrTypeDesc *script =
      vkr_editor_entity_script(f->scene, f->selected_entity);
  const float32_t label_w = vkr_editor_details_label_width(w);
  VkrUiWidgetConfig label =
      widget_at(INSPECTOR_PAD_PT, *y + 4.0f, label_w - 4.0f, 26.0f);
  label.placement.align = VKR_UI_ALIGN_START;
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = theme->text_secondary;
  label.style.padding_pt = (VkrUiEdges){5, 2, 5, 2};
  vkr_ui_label(ui, string8_lit("script.label"), string8_lit("Script"), &label);
  char source[VKR_EDITOR_SCRIPT_PATH];
  const bool8_t editable =
      script &&
      vkr_editor_script_source(editor, f, script, source, sizeof(source));
  const float32_t picker_x = INSPECTOR_PAD_PT + label_w;
  const float32_t picker_w =
      Max(40.0f, w - picker_x - INSPECTOR_PAD_PT - 32.0f);
  VkrUiWidgetConfig picker = widget_at(picker_x, *y + 4.0f, picker_w, 26.0f);
  vkr_editor_field_style(&picker);
  picker.placement.align = VKR_UI_ALIGN_START;
  picker.style.padding_pt = (VkrUiEdges){4, 8, 4, 8};
  picker.style.text_color = script ? theme->text : theme->text_secondary;
  picker.icon = VKR_UI_ICON_CODE;
  picker.icon_size_pt = 13.0f;
  picker.icon_color =
      script ? (Vec4){0.80f, 0.66f, 0.98f, 1.0f} : theme->text_secondary;
  picker.tooltip = string8_lit("Choose the script this object runs, or make a "
                               "new one");
  const char *name = script ? script->label : "None";
  picker.trailing_icon = VKR_UI_ICON_CHEVRON_DOWN;
  if (vkr_ui_button(ui, string8_lit("script.pick"),
                    string8_create((uint8_t *)name, strlen(name)), &picker)) {
    p->script_menu = true_v;
    p->script_menu_pt = (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                               (float32_t)ui->mouse_y / ui->content_scale};
  }
  VkrUiWidgetConfig edit = vkr_editor_icon_button_config(
      0, 0, VKR_UI_ICON_PENCIL_LINE,
      editable ? string8_lit("Edit this script's source")
               : string8_lit("No project source for this script"));
  edit.placement =
      widget_at(w - INSPECTOR_PAD_PT - 26.0f, *y + 4.0f, 26.0f, 26.0f)
          .placement;
  edit.disabled = !editable;
  if (vkr_ui_button(ui, string8_lit("script.edit"), (String8){0}, &edit)) {
    (void)vkr_editor_code_open(editor->code, editor, source);
  }
  *y += 32.0f;
}

/* Opens the list of live component types the selection can take. */
static void inspector_add_component(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *f, float32_t w,
                                    float32_t *y) {
  VkrUiSystem *ui = f->ui;
  VkrUiWidgetConfig c =
      widget_at(INSPECTOR_PAD_PT, *y + 6.0f, w - INSPECTOR_PAD_PT * 2, 28);
  vkr_editor_action_style(&c, VKR_FONT_HANDLE_INVALID);
  c.icon = VKR_UI_ICON_ADD;
  c.icon_size_pt = 13.0f;
  c.tooltip = string8_lit("Add fog, sky, clouds or post process to this "
                          "object");
  if (vkr_ui_button(ui, string8_lit("component.add"),
                    string8_lit("Add component"), &c)) {
    vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_ADD_COMPONENT,
                            (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                                   (float32_t)ui->mouse_y / ui->content_scale});
    editor->context_entity = f->selected_entity;
  }
  *y += 40.0f;
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

void vkr_editor_inspector_build(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading) {
  VkrEditorScenePanels *p = editor->scene_panels;
  /* The selected entity's container supplies scene and journal. */
  const VkrSampleUiFrame container =
      vkr_editor_entity_frame(frame, frame->selected_entity);
  const VkrSampleUiFrame *f = &container;
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
                 string8_lit("Select an object in the Scene or Outliner"), &c);
    inspector_clear_focus(ui);
    vkr_editor_details_cancel(&p->details, ui);
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
      vkr_editor_details_cancel(&p->details, ui);
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
  (void)vkr_ui_scroll_area_offset(ui, &p->inspector_scroll);
  float32_t y = 8;
  bool8_t field_focus = false_v;
  InspectorComponentEdit component_edit = {0};
  vkr_editor_details_begin(&p->details);
  /* Entity references name objects of the selection's own container. */
  p->details.entity_scene = vkr_editor_entity_scene(f, f->selected_entity);
  p->physics_dragging = false_v;
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
  vkr_editor_details_error(&p->details, ui, w, &y);
  /* World entities such as fog or the sky have no placement. */
  if (tr)
    field_focus |=
        inspector_transform_section(p, f, w, &y, heading, tr, &component_edit);
  if (lights.light)
    field_focus |= inspector_light_section(
        p, f, frame->scene ? frame->scene : frame->world, w, &y, heading,
        &lights, &component_edit);
  inspector_mesh_section(p, f, w, &y, heading);
  /* Placed objects carry a script slot above the script's own section;
     world settings do not. */
  if (tr)
    inspector_script_row(editor, p, f, w, &y);
  field_focus |=
      inspector_world_sections(p, f, frame->scene ? frame->scene : frame->world,
                               w, &y, heading, &component_edit);
  inspector_add_component(editor, f, w, &y);
  const float32_t physics_y = y;
  const bool8_t physics_open = inspector_section(
      p, ui, w, &y, INSPECTOR_SECTION_PHYSICS, VKR_UI_ICON_PHYSICS,
      (Vec4){0.45f, 0.84f, 0.56f, 1.0f}, "Physics", heading);
  if (p->values.physics.present)
    inspector_preset_button(p, ui, w - 8.0f, physics_y,
                            &vkr_scene_physics_body_type);
  if (physics_open)
    field_focus |= physics_inspector_build(p, f, w, &y, heading);
  /* A section's Presets button opens its menu once the sections are built. */
  if (p->open_collision_layers) {
    p->open_collision_layers = false_v;
    vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_PHYSICS, true_v);
  }
  if (p->script_menu) {
    p->script_menu = false_v;
    vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_SCRIPT,
                            p->script_menu_pt);
    editor->context_entity = f->selected_entity;
  }
  if (p->preset_menu) {
    vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_PRESET,
                            (Vec2){(float32_t)ui->mouse_x / ui->content_scale,
                                   (float32_t)ui->mouse_y / ui->content_scale});
    editor->context_entity = f->selected_entity;
    editor->context_type = p->preset_menu;
    p->preset_menu = NULL;
  }
  inspector_debug_section(p, f, w, &y, heading, tr);
  field_focus &=
      !ui->mouse_captured && ui->keyboard_input_layer == ui->input_layer;
  (void)vkr_ui_pop_id(ui);
  (void)vkr_ui_scroll_area_end(ui);
  p->inspector_height = y + 16;

  vkr_editor_details_end(&p->details);
  vkr_editor_context_open_choice(editor, &p->details);
  vkr_editor_color_picker_open(editor, &p->details);

  /* Component rows apply each finished entry, toggle or drag step at once; a
   * drag folds into one undo entry through its gesture. */
  if (component_edit.type && component_edit.world) {
    VkrSceneEditValues values;
    MemZero(&values, sizeof(values));
    values.fields = VKR_SCENE_EDIT_COMPONENT;
    values.component_type = component_edit.type;
    MemCopy(values.component, component_edit.value, component_edit.type->size);
    *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                           .entity = f->selected_entity,
                                           .values = values,
                                           .gesture = component_edit.gesture};
    return;
  }
  if (component_edit.type) {
    VkrSceneEditValues values = p->original_values;
    uint8_t component[VKR_TYPE_VALUE_MAX];
    (void)vkr_scene_edit_component_get(&p->values, component_edit.type,
                                       component);
    (void)vkr_scene_edit_component_set(&values, component_edit.type, component);
    values.fields = vkr_scene_edit_component_field(component_edit.type);
    *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                           .entity = f->selected_entity,
                                           .values = values,
                                           .gesture = component_edit.gesture};
    return;
  }

  /* Name, visibility and physics drafts: Escape reverts; Enter or leaving
   * the field commits; toggles apply at once. */
  if (field_focus && pressed(f->input, KEY_ESCAPE)) {
    inspector_read(p, f);
    return;
  }
  const bool8_t commit = p->changed && !p->physics_dragging &&
                         (!field_focus || pressed(f->input, KEY_ENTER));
  if (commit) {
    VkrSceneEditValues values;
    if (inspector_parse(p, &values)) {
      if (values.fields)
        *f->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                               .entity = f->selected_entity,
                                               .values = values};
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

/* Per-row flags the scene state saves by object identity. */
typedef enum PanelFlag {
  PANEL_FLAG_EXPANDED,
  PANEL_FLAG_PINNED,
  PANEL_FLAG_ICONS_HIDDEN,
} PanelFlag;

static bool8_t *panel_flag(EditorTreeNode *node, PanelFlag flag) {
  return flag == PANEL_FLAG_PINNED         ? &node->pinned
         : flag == PANEL_FLAG_ICONS_HIDDEN ? &node->icons_hidden
                                           : &node->expanded;
}

/* Identities of the primary scene's rows with `flag` set. */
static bool8_t panels_write_flags(const VkrEditorScenePanels *panels,
                                  const VkrSampleUiFrame *frame, PanelFlag flag,
                                  VkrJsonWriter *writer) {
  for (uint32_t i = 0; i < panels->capacity; ++i) {
    EditorTreeNode *node = &panels->nodes[i];
    VkrSampleEntityIdentity identity;
    if (*panel_flag(node, flag) &&
        vkr_sample_entity_identity(frame->scene, node->entity, &identity) &&
        !vkr_sample_entity_identity_write_json(&identity, writer)) {
      return false_v;
    }
  }
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
  if (!panels_write_flags(panels, frame, PANEL_FLAG_EXPANDED, writer) ||
      !vkr_json_writer_end_array(writer)) {
    return false_v;
  }
  /* Pins and hidden viewport icons are optional members. */
  return vkr_json_writer_name(writer, string8_lit("pinned")) &&
         vkr_json_writer_begin_array(writer) &&
         panels_write_flags(panels, frame, PANEL_FLAG_PINNED, writer) &&
         vkr_json_writer_end_array(writer) &&
         vkr_json_writer_name(writer, string8_lit("icons_hidden")) &&
         vkr_json_writer_begin_array(writer) &&
         panels_write_flags(panels, frame, PANEL_FLAG_ICONS_HIDDEN, writer) &&
         vkr_json_writer_end_array(writer) &&
         vkr_json_writer_end_object(writer);
}

static bool8_t panels_restore_flags(VkrEditorScenePanels *panels,
                                    const VkrSampleUiFrame *frame,
                                    String8 array, PanelFlag flag,
                                    bool8_t apply) {
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
      TreeContainer containers[TREE_CONTAINER_COUNT];
      uint32_t total = 0u;
      const uint32_t node = tree_node_index(
          containers, tree_containers(frame, containers, &total), entity);
      if (node != NO_ROW && node < panels->capacity) {
        *panel_flag(&panels->nodes[node], flag) = true_v;
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
  String8 arrays[3] = {{0}};
  static const char *const members[3] = {"expanded", "pinned", "icons_hidden"};
  if (!vkr_json_get_int(&reader, "version", &version) || version != 1 ||
      !vkr_json_get_float(&reader, "hierarchy_scroll", &scroll) ||
      !isfinite(scroll) || scroll < 0 ||
      !vkr_editor_project_json_member(json, "expanded", &arrays[0], NULL) ||
      !rebuild_tree(panels, frame)) {
    return false_v;
  }
  for (uint32_t f = 1; f < ArrayCount(members); ++f) {
    if (!vkr_editor_project_json_member(json, members[f], &arrays[f], NULL))
      arrays[f] = string8_lit("[]");
  }
  for (uint32_t f = 0; f < ArrayCount(members); ++f) {
    if (!panels_restore_flags(panels, frame, arrays[f], (PanelFlag)f,
                              false_v)) {
      return false_v;
    }
  }
  for (uint32_t i = 0; i < panels->capacity; ++i) {
    panels->nodes[i].expanded = false_v;
    panels->nodes[i].pinned = false_v;
    panels->nodes[i].icons_hidden = false_v;
  }
  for (uint32_t f = 0; f < ArrayCount(members); ++f) {
    if (!panels_restore_flags(panels, frame, arrays[f], (PanelFlag)f, true_v)) {
      return false_v;
    }
  }
  panels->hierarchy_scroll = scroll;
  panels->rebuild = true_v;
  return true_v;
}

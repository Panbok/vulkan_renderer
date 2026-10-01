#include "core/logger.h"
#include "editor_internal.h"
#include "renderer/systems/vkr_scene_types.h"

#include <math.h>

#define EDITOR_LABEL_SIZE_PT 26.0f
#define EDITOR_LABEL_GAP_PT 4.0f

/* Readable tint from a linear light color: normalized, display-encoded and
 * lifted toward white so dim or saturated lights stay legible. */
static Vec4 editor_label_tint(Vec3 linear) {
  const float32_t peak = Max(linear.x, Max(linear.y, linear.z));
  if (!(peak > 0.0f))
    return (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
  Vec4 tint = {powf(linear.x / peak, 1.0f / 2.2f),
               powf(linear.y / peak, 1.0f / 2.2f),
               powf(linear.z / peak, 1.0f / 2.2f), 1.0f};
  return vkr_ui_color_mix(tint, (Vec4){1.0f, 1.0f, 1.0f, 1.0f}, 0.25f);
}

/* One icon's presentation. Abstract objects (the sun, sky and fog layers,
 * post process) have no meaningful place, so they stack at the world origin;
 * local lights, fog boxes and probes follow their transforms. */
typedef struct EditorLabelKind {
  VkrUiIcon icon;
  Vec4 tint;
  bool8_t enabled;
  bool8_t abstract;
} EditorLabelKind;

typedef struct EditorLabelBuild {
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  const VkrScene *scene;
  /* Component whose query is being visited. */
  VkrComponentTypeId component;
  uint32_t capacity;
  uint32_t stacked;
  /* The entity being built is an empty object. */
  bool8_t empty;
} EditorLabelBuild;

/* Iconic components in icon priority order: lights, then world types.
   Scripts are tags on an object and never give it an icon of their own. */
static uint32_t editor_label_components(const VkrScene *scene,
                                        VkrComponentTypeId *ids) {
  uint32_t count = 0u;
  ids[count++] = scene->comp_directional_light;
  ids[count++] = scene->comp_point_light;
  ids[count++] = scene->comp_rectangle_light;
  for (uint32_t i = 0; i < scene->type_count; ++i) {
    if (!vkr_scene_world_type_registered(scene->types[i].type)) {
      ids[count++] = scene->types[i].id;
    }
  }
  return count;
}

/* What the entity shows as, or false when it has no icon or its kind is
 * filtered out. */
static bool8_t editor_label_kind(const VkrEditorUi *editor,
                                 const VkrScene *scene, VkrEntityId entity,
                                 EditorLabelKind *out) {
  const VkrWorld *world = scene->world;
  const SceneDirectionalLight *directional =
      vkr_entity_get_component(world, entity, scene->comp_directional_light);
  if (directional) {
    *out = (EditorLabelKind){VKR_UI_ICON_DIRECTIONAL_LIGHT,
                             editor_label_tint(directional->color),
                             directional->enabled, true_v};
    return editor->labels_directional;
  }
  const ScenePointLight *point =
      vkr_entity_get_component(world, entity, scene->comp_point_light);
  if (point) {
    const bool8_t spot = point->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
    *out = (EditorLabelKind){
        spot ? VKR_UI_ICON_SPOT_LIGHT : VKR_UI_ICON_POINT_LIGHT,
        editor_label_tint(point->color), point->enabled, false_v};
    return spot ? editor->labels_spot : editor->labels_point;
  }
  const SceneRectangleLight *rectangle =
      vkr_entity_get_component(world, entity, scene->comp_rectangle_light);
  if (rectangle) {
    *out = (EditorLabelKind){VKR_UI_ICON_RECT_LIGHT,
                             editor_label_tint(rectangle->color),
                             rectangle->enabled, false_v};
    return editor->labels_point;
  }
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    /* Shapes, text and animated meshes are visible geometry and need no
       icon. */
    if (type == &vkr_scene_shape_type || type == &vkr_scene_text_type ||
        type == &vkr_scene_animation_type ||
        vkr_scene_world_type_registered(type) ||
        !vkr_scene_get_typed(scene, entity, type)) {
      continue;
    }
    /* Only one-per-frame settings are abstract and stack at the origin;
       fog boxes, probes, Player Starts and scripts mark their position. */
    const bool8_t placed = !(type->flags & VKR_TYPE_FLAG_SINGLETON);
    *out =
        (EditorLabelKind){vkr_editor_world_type_icon(type),
                          (Vec4){0.62f, 0.78f, 0.98f, 1.0f}, true_v, !placed};
    return placed ? editor->labels_markers : editor->labels_environment;
  }
  return false_v;
}

static void editor_label_build(EditorLabelBuild *build, VkrEntityId entity) {
  VkrEditorUi *editor = build->editor;
  const VkrSampleUiFrame *frame = build->frame;
  const VkrScene *scene = build->scene;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  EditorLabelKind kind = {VKR_UI_ICON_EMPTY, theme->text_secondary, true_v,
                          false_v};
  if (editor->label_anchor_count >= build->capacity ||
      (build->empty && !editor->labels_empty) ||
      (!build->empty && !editor_label_kind(editor, scene, entity, &kind))) {
    return;
  }
  const bool8_t placed =
      !kind.abstract &&
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  const float32_t size = EDITOR_LABEL_SIZE_PT;
  const bool8_t selected = entity.u64 == frame->selected_entity.u64;
  /* A selected placed object keeps a faint icon that clicks through to the
     transform gizmo's center handles beneath it. */
  const bool8_t under_gizmo = selected && placed;
  const bool8_t hidden = !vkr_scene_entity_visible(scene, entity);
  VkrUiWidgetConfig label = vkr_ui_widget_config_default();
  label.placement = VKR_UI_PLACEMENT_DEFAULT;
  label.placement.column = label.placement.row = 0;
  label.placement.justify = label.placement.align = VKR_UI_ALIGN_START;
  label.placement.margin_pt = (VkrUiEdges){100000.0f, 0, 0, 100000.0f};
  label.style.min_size_pt = label.style.max_size_pt = (Vec2){size, size};
  label.style.padding_pt = (VkrUiEdges){4, 4, 4, 4};
  label.style.corner_radius_pt =
      (Vec4){size * 0.5f, size * 0.5f, size * 0.5f, size * 0.5f};
  label.style.background_color = selected
                                     ? vkr_ui_color_alpha(theme->accent, 0.9f)
                                     : (Vec4){0.03f, 0.035f, 0.045f, 0.62f};
  label.style.hover_background_color =
      selected ? theme->accent_hover : (Vec4){0.10f, 0.11f, 0.13f, 0.85f};
  label.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  label.style.border_color =
      selected ? theme->text_on_accent : (Vec4){1.0f, 1.0f, 1.0f, 0.14f};
  label.icon = kind.icon;
  label.icon_size_pt = 17.0f;
  label.icon_color = !kind.enabled || hidden ? theme->text_disabled
                     : selected              ? theme->text_on_accent
                                             : kind.tint;
  label.tooltip = vkr_scene_get_name(scene, entity);
  (void)vkr_ui_push_id_u64(ui, entity.u64);
  if (under_gizmo) {
    label.style.background_color = vkr_ui_color_alpha(theme->accent, 0.35f);
    label.style.border_color = vkr_ui_color_alpha(theme->text_on_accent, 0.3f);
    label.icon_color = vkr_ui_color_alpha(label.icon_color, 0.55f);
    label.tooltip = (String8){0};
    vkr_ui_label(ui, string8_lit("object"), (String8){0}, &label);
  } else if (vkr_ui_button(ui, string8_lit("object"), (String8){0}, &label)) {
    *frame->scene_edit = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT,
                                               .entity = entity};
  }
  editor->label_anchors[editor->label_anchor_count++] = (VkrEditorLabelAnchor){
      .widget =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("object")),
      .entity = entity,
      .scene = scene,
      .stack = placed ? UINT32_MAX : build->stacked++,
  };
  (void)vkr_ui_pop_id(ui);
}

/* An entity with several iconic components shows once, from the query of
   its first one. */
static bool8_t editor_label_owned(const EditorLabelBuild *build,
                                  VkrEntityId entity) {
  VkrComponentTypeId ids[3 + VKR_SCENE_TYPE_MAX];
  const uint32_t count = editor_label_components(build->scene, ids);
  for (uint32_t i = 0; i < count; ++i) {
    if (vkr_entity_has_component(build->scene->world, entity, ids[i])) {
      return ids[i] == build->component;
    }
  }
  return false_v;
}

static void editor_label_chunk(const VkrArchetype *arch, VkrChunk *chunk,
                               void *user) {
  (void)arch;
  EditorLabelBuild *build = user;
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const uint32_t count = vkr_entity_chunk_count(chunk);
  for (uint32_t i = 0; i < count; ++i) {
    if (editor_label_owned(build, entities[i])) {
      editor_label_build(build, entities[i]);
    }
  }
}

static void editor_count_chunk(const VkrArchetype *arch, VkrChunk *chunk,
                               void *user) {
  (void)arch;
  *(uint32_t *)user += vkr_entity_chunk_count(chunk);
}

/* Placed objects that nothing draws or marks, such as an Empty: no
   geometry, light, physics body, iconic component or child. They get an empty
   icon so they can be found and picked. Writes up to VKR_EDITOR_LABEL_EMPTY_MAX
   into `out` and returns how many. */
static uint32_t editor_label_empties(const VkrScene *scene,
                                     VkrAllocator *scratch, VkrEntityId *out) {
  VkrWorld *world = scene->world;
  const uint32_t indices = world->dir.living;
  uint8_t *parents = indices
                         ? vkr_allocator_alloc(scratch, indices,
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
                         : NULL;
  if (!parents) {
    return 0u;
  }
  MemZero(parents, indices);
  for (uint32_t i = 0; i < indices; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    const SceneTransform *transform =
        vkr_entity_is_alive(world, entity)
            ? vkr_entity_get_component(world, entity, scene->comp_transform)
            : NULL;
    if (transform && transform->parent.u64 &&
        transform->parent.parts.index < indices) {
      parents[transform->parent.parts.index] = 1u;
    }
  }
  VkrComponentTypeId ids[3 + VKR_SCENE_TYPE_MAX];
  const uint32_t iconic = editor_label_components(scene, ids);
  const VkrComponentTypeId drawn[] = {
      scene->comp_mesh_renderer, scene->comp_text3d, scene->comp_shape,
      scene->comp_physics_body, scene->comp_physics_collider};
  uint32_t count = 0u;
  for (uint32_t i = 0; i < indices && count < VKR_EDITOR_LABEL_EMPTY_MAX; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(world, i);
    if (parents[i] || !vkr_entity_is_alive(world, entity) ||
        !vkr_entity_has_component(world, entity, scene->comp_transform)) {
      continue;
    }
    bool8_t marked = false_v;
    for (uint32_t c = 0; c < ArrayCount(drawn) && !marked; ++c) {
      marked = vkr_entity_has_component(world, entity, drawn[c]);
    }
    for (uint32_t c = 0; c < iconic && !marked; ++c) {
      marked = vkr_entity_has_component(world, entity, ids[c]);
    }
    if (!marked) {
      out[count++] = entity;
    }
  }
  return count;
}

/* Visit each iconic component's query; `build` notes the component. */
static void editor_label_visit(const VkrScene *scene, VkrChunkFn visit,
                               void *user, EditorLabelBuild *build) {
  VkrWorld *world = scene->world;
  VkrComponentTypeId ids[3 + VKR_SCENE_TYPE_MAX];
  const uint32_t count = editor_label_components(scene, ids);
  for (uint32_t i = 0; i < count; ++i) {
    if (build) {
      build->component = ids[i];
    }
    VkrQuery query;
    vkr_entity_query_build(world, &ids[i], 1, NULL, 0, &query);
    vkr_entity_query_each_chunk(world, &query, visit, user);
  }
}

void vkr_editor_labels_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  editor->label_anchor_count = 0;
  editor->label_anchors = NULL;
  editor->label_scene_generation = frame->scene_generation;
  const VkrScene *scenes[2u + VKR_SCENE_ADDITIVE_MAX] = {frame->scene,
                                                         frame->world};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = frame->additive[i];
  }
  if (!editor->labels_enabled || !frame->mapping_valid ||
      frame->scene_rendering_stopped || (!frame->scene && !frame->world))
    return;
  VkrUiSystem *ui = frame->ui;
  uint32_t capacity = 0;
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    if (scenes[i]) {
      editor_label_visit(scenes[i], editor_count_chunk, &capacity, NULL);
      /* The scan walks every entity; it reruns only when the container's
         structure changes. */
      VkrEditorLabelEmpties *cache = &editor->label_empties[i];
      if (cache->scene != scenes[i] ||
          cache->generation != frame->scene_generation ||
          cache->revision != scenes[i]->structure_revision) {
        cache->scene = scenes[i];
        cache->generation = frame->scene_generation;
        cache->revision = scenes[i]->structure_revision;
        cache->count = editor_label_empties(scenes[i], ui->frame_allocator,
                                            cache->entities);
      }
      capacity += cache->count;
    }
  }
  if (!capacity)
    return;
  /* Later navbar, toolbar, windows, commands and tooltip need fewer than 80
     nodes. Keep room for them even when a scene exceeds the UI's label budget.
   */
  const uint32_t reserved = 96u + 1u;
  const uint32_t available =
      ui->frame_node_count + reserved < ui->frame_node_capacity
          ? ui->frame_node_capacity - ui->frame_node_count - reserved
          : 0u;
  if (capacity > available && !editor->label_capacity_warned) {
    log_warn("Object icons exceed UI capacity; filter icon types to show the "
             "rest");
    editor->label_capacity_warned = true_v;
  }
  capacity = Min(capacity, available);
  if (!capacity)
    return;
  editor->label_anchors = vkr_allocator_alloc(
      ui->frame_allocator, sizeof(*editor->label_anchors) * (uint64_t)capacity,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!editor->label_anchors)
    return;
  const Vec4 image = frame->mapping.image_rect_px;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){image.y / ui->content_scale, 0, 0,
                                           image.x / ui->content_scale};
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){image.z / ui->content_scale, image.w / ui->content_scale};
  panel.clip_children = true_v;
  editor->label_panel = vkr_ui_id_stack_widget_label(
      &ui->id_stack, string8_lit("editor.object.icons"));
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.object.icons"), &panel))
    return;
  EditorLabelBuild build = {
      .editor = editor, .frame = frame, .capacity = capacity};
  for (uint32_t i = 0; i < ArrayCount(scenes); ++i) {
    if (scenes[i]) {
      build.scene = scenes[i];
      (void)vkr_ui_push_id_u64(ui, 0x1ab0u + i);
      editor_label_visit(scenes[i], editor_label_chunk, &build, &build);
      build.empty = true_v;
      const VkrEditorLabelEmpties *cache = &editor->label_empties[i];
      for (uint32_t e = 0; e < cache->count; ++e) {
        if (vkr_scene_entity_alive(scenes[i], cache->entities[e])) {
          editor_label_build(&build, cache->entities[e]);
        }
      }
      build.empty = false_v;
      (void)vkr_ui_pop_id(ui);
    }
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_labels_project(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if ((!frame->scene && !frame->world) || frame->scene_rendering_stopped ||
      editor->label_scene_generation != frame->scene_generation ||
      !editor->label_anchor_count)
    return;
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  const float32_t size = EDITOR_LABEL_SIZE_PT;
  (void)vkr_ui_widget_set_rect(frame->ui, editor->label_panel,
                               (VkrUiRect){image.x / scale, image.y / scale,
                                           image.z / scale, image.w / scale});
  for (uint32_t i = 0; i < editor->label_anchor_count; ++i) {
    const VkrEditorLabelAnchor anchor = editor->label_anchors[i];
    /* Edits applied after the build can delete an object or unload its
       container; either hides the icon. A removed container is never
       dereferenced. */
    bool8_t loaded =
        anchor.scene == frame->scene || anchor.scene == frame->world;
    for (uint32_t c = 0; !loaded && c < VKR_SCENE_ADDITIVE_MAX; ++c) {
      loaded = frame->additive[c] && anchor.scene == frame->additive[c];
    }
    const SceneTransform *transform =
        anchor.stack == UINT32_MAX && anchor.scene && loaded
            ? vkr_entity_get_component_if_alive_const(
                  anchor.scene->world, anchor.entity,
                  anchor.scene->comp_transform)
            : NULL;
    const bool8_t anchored =
        loaded && (anchor.stack != UINT32_MAX || transform);
    const Vec3 position =
        transform ? mat4_position(transform->world) : vec3_zero();
    const Vec4 clip = anchored ? mat4_mul_vec4(frame->view_projection,
                                               vec3_to_vec4(position, 1.0f))
                               : (Vec4){0};
    Vec2 offset = {100000.0f, 100000.0f};
    if (clip.w > 0.0f && clip.z >= 0.0f && clip.z <= clip.w) {
      offset.x =
          image.z * (clip.x / clip.w * 0.5f + 0.5f) / scale - size * 0.5f;
      offset.y =
          image.w * (clip.y / clip.w * 0.5f + 0.5f) / scale - size - 6.0f;
      /* Abstract objects rise in a column above the origin. */
      if (anchor.stack != UINT32_MAX) {
        offset.y -= (float32_t)anchor.stack * (size + EDITOR_LABEL_GAP_PT);
      }
    }
    if (offset.x < 0 || offset.y < 0 || offset.x + size > image.z / scale ||
        offset.y + size > image.w / scale)
      offset = (Vec2){100000.0f, 100000.0f};
    (void)vkr_ui_widget_set_rect(frame->ui, anchor.widget,
                                 (VkrUiRect){offset.x, offset.y, size, size});
  }
}

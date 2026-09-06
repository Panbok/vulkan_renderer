#include "core/logger.h"
#include "editor_internal.h"

typedef struct EditorLabelBuild {
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  bool8_t directional;
  uint32_t capacity;
} EditorLabelBuild;

static void editor_light_labels(const VkrArchetype *arch, VkrChunk *chunk,
                                void *user) {
  (void)arch;
  EditorLabelBuild *build = user;
  const VkrSampleUiFrame *frame = build->frame;
  VkrEditorUi *editor = build->editor;
  VkrUiSystem *ui = frame->ui;
  const VkrScene *scene = frame->scene;
  const VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  const ScenePointLight *points =
      build->directional
          ? NULL
          : vkr_entity_chunk_column_const(chunk, scene->comp_point_light);
  const SceneDirectionalLight *directions =
      build->directional
          ? vkr_entity_chunk_column_const(chunk, scene->comp_directional_light)
          : NULL;
  const float32_t size = 32.0f;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  (void)vkr_ui_push_id_u64(ui, build->directional ? 1u : 2u);
  for (uint32_t i = 0;
       i < count && editor->label_anchor_count < build->capacity; ++i) {
    const bool8_t spot =
        !build->directional && points[i].kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
    if (!build->directional &&
        !(spot ? editor->labels_spot : editor->labels_point))
      continue;
    const bool8_t enabled =
        build->directional ? directions[i].enabled : points[i].enabled;
    VkrUiWidgetConfig label = vkr_ui_widget_config_default();
    label.placement = VKR_UI_PLACEMENT_DEFAULT;
    label.placement.column = label.placement.row = 0;
    label.placement.justify = label.placement.align = VKR_UI_ALIGN_START;
    label.placement.margin_pt = (VkrUiEdges){100000.0f, 0, 0, 100000.0f};
    label.style.min_size_pt = label.style.max_size_pt = (Vec2){size, size};
    label.style.padding_pt = (VkrUiEdges){2, 2, 2, 2};
    label.style.font_size_pt = 28;
    label.text.font = editor->label_font;
    label.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
    label.style.background_color = (Vec4){0.025f, 0.035f, 0.05f, 0.86f};
    label.style.text_color = !enabled ? (Vec4){0.5f, 0.53f, 0.58f, 1}
                             : build->directional ? (Vec4){1, 0.79f, 0.30f, 1}
                             : spot               ? (Vec4){0.40f, 0.83f, 1, 1}
                                                  : (Vec4){0.60f, 1, 0.65f, 1};
    if (entities[i].u64 == frame->selected_entity.u64) {
      label.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      label.style.border_color = (Vec4){1, 0.79f, 0.30f, 1};
    }
    label.tooltip = vkr_scene_get_name(scene, entities[i]);
    (void)vkr_ui_push_id_u64(ui, entities[i].u64);
    if (vkr_ui_button(ui, string8_lit("light"),
                      build->directional ? string8_lit("D")
                      : spot             ? string8_lit("S")
                                         : string8_lit("P"),
                      &label))
      *frame->scene_edit = (VkrSceneEditRequest){
          .action = VKR_SCENE_EDIT_SELECT, .entity = entities[i]};
    editor->label_anchors[editor->label_anchor_count++] =
        (VkrEditorLabelAnchor){.widget = vkr_ui_id_stack_widget_label(
                                   &ui->id_stack, string8_lit("light")),
                               .entity = entities[i]};
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_pop_id(ui);
}

static void editor_count_lights(const VkrArchetype *arch, VkrChunk *chunk,
                                void *user) {
  (void)arch;
  *(uint32_t *)user += vkr_entity_chunk_count(chunk);
}

void vkr_editor_labels_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  editor->label_anchor_count = 0;
  editor->label_anchors = NULL;
  editor->label_scene_generation = frame->scene_generation;
  if (!editor->labels_enabled || !frame->scene || !frame->mapping_valid ||
      frame->scene_rendering_stopped ||
      !(editor->labels_directional || editor->labels_spot ||
        editor->labels_point))
    return;
  VkrUiSystem *ui = frame->ui;
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
  uint32_t capacity = 0;
  VkrQuery count_query;
  VkrComponentTypeId count_types[] = {frame->scene->comp_transform,
                                      frame->scene->comp_directional_light};
  if (editor->labels_directional) {
    vkr_entity_query_build(frame->scene->world, count_types, 2, NULL, 0,
                           &count_query);
    vkr_entity_query_each_chunk(frame->scene->world, &count_query,
                                editor_count_lights, &capacity);
  }
  if (editor->labels_spot || editor->labels_point) {
    count_types[1] = frame->scene->comp_point_light;
    vkr_entity_query_build(frame->scene->world, count_types, 2, NULL, 0,
                           &count_query);
    vkr_entity_query_each_chunk(frame->scene->world, &count_query,
                                editor_count_lights, &capacity);
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
    log_warn("Light labels exceed UI capacity; filter label types to show "
             "remaining lights");
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
  editor->label_panel = vkr_ui_id_stack_widget_label(
      &ui->id_stack, string8_lit("editor.light.labels"));
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.light.labels"), &panel))
    return;
  EditorLabelBuild build = {
      .editor = editor, .frame = frame, .capacity = capacity};
  VkrQuery query;
  VkrComponentTypeId types[] = {frame->scene->comp_transform,
                                frame->scene->comp_directional_light};
  if (editor->labels_directional) {
    build.directional = true_v;
    vkr_entity_query_build(frame->scene->world, types, 2, NULL, 0, &query);
    vkr_entity_query_each_chunk(frame->scene->world, &query,
                                editor_light_labels, &build);
  }
  if (editor->labels_spot || editor->labels_point) {
    build.directional = false_v;
    types[1] = frame->scene->comp_point_light;
    vkr_entity_query_build(frame->scene->world, types, 2, NULL, 0, &query);
    vkr_entity_query_each_chunk(frame->scene->world, &query,
                                editor_light_labels, &build);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_labels_project(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame) {
  if (!frame->scene || frame->scene_rendering_stopped ||
      editor->label_scene_generation != frame->scene_generation)
    return;
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  if (!editor->label_anchor_count)
    return;
  (void)vkr_ui_widget_set_rect(frame->ui, editor->label_panel,
                               (VkrUiRect){image.x / scale, image.y / scale,
                                           image.z / scale, image.w / scale});
  for (uint32_t i = 0; i < editor->label_anchor_count; ++i) {
    const VkrEditorLabelAnchor anchor = editor->label_anchors[i];
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        frame->scene->world, anchor.entity, frame->scene->comp_transform);
    const Vec4 clip =
        mat4_mul_vec4(frame->view_projection,
                      vec3_to_vec4(mat4_position(transform->world), 1.0f));
    Vec2 offset = {100000.0f, 100000.0f};
    if (clip.w > 0.0f && clip.z >= 0.0f && clip.z <= clip.w) {
      offset.x = image.z * (clip.x / clip.w * 0.5f + 0.5f) / scale - 16.0f;
      offset.y = image.w * (clip.y / clip.w * 0.5f + 0.5f) / scale - 40.0f;
    }
    if (offset.x < 0 || offset.y < 0 || offset.x + 32 > image.z / scale ||
        offset.y + 32 > image.w / scale)
      offset = (Vec2){100000.0f, 100000.0f};
    (void)vkr_ui_widget_set_rect(frame->ui, anchor.widget,
                                 (VkrUiRect){offset.x, offset.y, 32, 32});
  }
}

#include "editor_views.h"

#include "editor_internal.h"
#include "editor_level.h"
#include "editor_material.h"

#include <math.h>
#include <stdio.h>

typedef struct ViewInfo {
  const char *id;
  const char *name;
  VkrUiIcon icon;
  const char *tooltip;
} ViewInfo;

static const ViewInfo s_views[VKR_EDITOR_VIEW_COUNT] = {
    [VKR_EDITOR_VIEW_SCENE] = {"scene", "Scene", VKR_UI_ICON_SCENE,
                               "The scene viewport"},
    [VKR_EDITOR_VIEW_MODEL] = {"model", "Model", VKR_UI_ICON_MESH,
                               "The selection alone under the scene's "
                               "light: drag the orbit ring or middle-drag "
                               "to turn it, scroll to zoom"},
    [VKR_EDITOR_VIEW_GRAPH] = {"graph", "Graph", VKR_UI_ICON_GRAPH,
                               "The open material's node graph and "
                               "inspector"},
    [VKR_EDITOR_VIEW_SCRIPT] = {"script", "Script", VKR_UI_ICON_CODE,
                                "The Script editor"},
};

/* The pitch the Model view first looks down at its subject, in degrees. */
#define MODEL_PITCH_START -20.0f
/* Degrees the orbit turns per pixel of drag. */
#define MODEL_ORBIT_DEGREES_PER_PX 0.35f
#define MODEL_RING_PT 52.0f

uint32_t vkr_editor_views_builtin(VkrEditorWorkbenchKind kind) {
  uint32_t views = 1u << VKR_EDITOR_VIEW_SCENE;
  if (kind == VKR_EDITOR_WORKBENCH_ART) {
    views |= (1u << VKR_EDITOR_VIEW_MODEL) | (1u << VKR_EDITOR_VIEW_GRAPH);
  } else if (kind == VKR_EDITOR_WORKBENCH_SCRIPTING) {
    views |= 1u << VKR_EDITOR_VIEW_SCRIPT;
  }
  return views;
}

VkrEditorView vkr_editor_view_builtin(VkrEditorWorkbenchKind kind) {
  /* Scripting's task is the code; Art works on the level it shows. */
  return kind == VKR_EDITOR_WORKBENCH_SCRIPTING ? VKR_EDITOR_VIEW_SCRIPT
                                                : VKR_EDITOR_VIEW_SCENE;
}

const char *vkr_editor_view_id(VkrEditorView view) {
  return view < VKR_EDITOR_VIEW_COUNT ? s_views[view].id : "";
}

const char *vkr_editor_view_name(VkrEditorView view) {
  return view < VKR_EDITOR_VIEW_COUNT ? s_views[view].name : "";
}

VkrUiIcon vkr_editor_view_icon(VkrEditorView view) {
  return view < VKR_EDITOR_VIEW_COUNT ? s_views[view].icon : VKR_UI_ICON_NONE;
}

String8 vkr_editor_view_tooltip(VkrEditorView view) {
  if (view >= VKR_EDITOR_VIEW_COUNT) {
    return (String8){0};
  }
  return string8_create_from_cstr((const uint8_t *)s_views[view].tooltip,
                                  strlen(s_views[view].tooltip));
}

static bool8_t view_word_is(String8 word, const char *text) {
  const uint64_t length = strlen(text);
  if (word.length != length) {
    return false_v;
  }
  for (uint64_t i = 0u; i < length; ++i) {
    const uint8_t a = word.str[i];
    const uint8_t b = (uint8_t)text[i];
    if ((a >= 'A' && a <= 'Z' ? a + 32u : a) !=
        (b >= 'A' && b <= 'Z' ? b + 32u : b)) {
      return false_v;
    }
  }
  return true_v;
}

VkrEditorView vkr_editor_view_find(String8 word) {
  for (uint32_t i = 0u; i < VKR_EDITOR_VIEW_COUNT; ++i) {
    if (view_word_is(word, s_views[i].id) ||
        view_word_is(word, s_views[i].name)) {
      return (VkrEditorView)i;
    }
  }
  return VKR_EDITOR_VIEW_COUNT;
}

static VkrEditorWorkbench *view_workbench(VkrEditorUi *editor) {
  return &editor->workbenches.items[editor->workbenches.active];
}

uint32_t vkr_editor_views_offered(const VkrEditorUi *editor) {
  return editor->workbenches.items[editor->workbenches.active].views |
         (1u << VKR_EDITOR_VIEW_SCENE);
}

VkrEditorView vkr_editor_view_active(const VkrEditorUi *editor) {
  const VkrEditorWorkbench *item =
      &editor->workbenches.items[editor->workbenches.active];
  return ((item->views >> item->view) & 1u) ? item->view
                                            : VKR_EDITOR_VIEW_SCENE;
}

/* Whether the Views tab is the shown tab of its stack. */
static bool8_t views_tab_shown(const VkrUiDockTree *dock) {
  for (uint32_t i = 0u; i < dock->node_high_water; ++i) {
    const VkrUiDockNode *node = &dock->nodes[i];
    if (node->used && node->kind == VKR_UI_DOCK_NODE_TABS &&
        node->as.leaf.tabs[node->as.leaf.active_tab].panel_kind ==
            VKR_UI_DOCK_PANEL_SCENE_VIEWPORT) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_view_shown(const VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              VkrEditorView view) {
  return frame->dock && !frame->scene_only &&
         vkr_editor_view_active(editor) == view && views_tab_shown(frame->dock);
}

void vkr_editor_view_show(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                          uint32_t workbench, VkrEditorView view) {
  if (workbench >= editor->workbenches.count || view >= VKR_EDITOR_VIEW_COUNT) {
    return;
  }
  VkrEditorWorkbench *item = &editor->workbenches.items[workbench];
  item->views |= 1u << view;
  item->view = view;
  if (workbench == editor->workbenches.active && frame && frame->dock) {
    vkr_editor_dock_show(frame->dock, VKR_UI_DOCK_PANEL_SCENE_VIEWPORT);
  }
}

void vkr_editor_view_remove(VkrEditorUi *editor, VkrEditorView view) {
  if (view == VKR_EDITOR_VIEW_SCENE || view >= VKR_EDITOR_VIEW_COUNT) {
    return;
  }
  VkrEditorWorkbench *item = view_workbench(editor);
  item->views &= ~(1u << view);
  if (item->view == view) {
    item->view = VKR_EDITOR_VIEW_SCENE;
  }
}

VkrEditorView vkr_editor_window_view(VkrEditorWindowKind kind) {
  return kind == VKR_EDITOR_WINDOW_SCRIPT ? VKR_EDITOR_VIEW_SCRIPT
                                          : VKR_EDITOR_VIEW_COUNT;
}

// =============================================================================
// Model view
// =============================================================================

/* Moves the Scene camera onto the orbit, as free flight moves it. */
static void model_apply(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorModelView *model = &editor->model;
  if (!frame->editor_state_request) {
    return;
  }
  const float32_t yaw = vkr_to_radians(model->yaw);
  const float32_t pitch = vkr_to_radians(model->pitch);
  const Vec3 forward =
      vec3_new(cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw));
  VkrSampleEditorStateRequest *request = frame->editor_state_request;
  request->move_camera = true_v;
  request->camera_position =
      vec3_sub(model->pivot, vec3_scale(forward, model->distance));
  request->camera_yaw = model->yaw;
  request->camera_pitch = model->pitch;
}

/* Grows [lo, hi] by `entity`'s world box, else by a metre around its
   origin; false for a dead entity. */
static bool8_t model_grow_box(const VkrSampleUiFrame *frame, VkrEntityId entity,
                              VkrBrushGeometry *scratch, Vec3 *lo, Vec3 *hi) {
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  Vec3 box_lo = {0};
  Vec3 box_hi = {0};
  if (!scratch ||
      !vkr_editor_entity_world_box(scene, entity, scratch, &box_lo, &box_hi)) {
    const SceneTransform *transform =
        vkr_entity_get_component(scene->world, entity, scene->comp_transform);
    if (!transform) {
      return false_v;
    }
    const Vec3 origin = mat4_position(transform->world);
    box_lo = vec3_sub(origin, vec3_new(0.5f, 0.5f, 0.5f));
    box_hi = vec3_add(origin, vec3_new(0.5f, 0.5f, 0.5f));
  }
  *lo = vec3_new(Min(lo->x, box_lo.x), Min(lo->y, box_lo.y),
                 Min(lo->z, box_lo.z));
  *hi = vec3_new(Max(hi->x, box_hi.x), Max(hi->y, box_hi.y),
                 Max(hi->z, box_hi.z));
  return true_v;
}

/* Centres the orbit on the selection's box and backs off until the box
   fills the Scene's narrower side, as Frame selected does. */
static void model_frame(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorModelView *model = &editor->model;
  VkrBrushGeometry *scratch =
      vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*scratch),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  Vec3 lo = vec3_new(INFINITY, INFINITY, INFINITY);
  Vec3 hi = vec3_new(-INFINITY, -INFINITY, -INFINITY);
  bool8_t found = model_grow_box(frame, model->subject, scratch, &lo, &hi);
  for (uint32_t i = 0u; i < editor->selection_extra_count; ++i) {
    found |=
        model_grow_box(frame, editor->selection_extra[i], scratch, &lo, &hi);
  }
  if (!found) {
    return;
  }
  model->pivot = vec3_scale(vec3_add(lo, hi), 0.5f);
  const float32_t radius = Max(0.25f, vec3_length(vec3_sub(hi, lo)) * 0.5f);
  const Vec4 panel = frame->mapping.panel_rect_px;
  const float32_t aspect = panel.w > 0.0f ? panel.z / panel.w : 1.0f;
  /* The recall reports the Scene's fixed vertical lens. */
  const float32_t fov = frame->scene_recall.field_of_view > 0.0f
                            ? frame->scene_recall.field_of_view
                            : 60.0f;
  const float32_t half_angle =
      atanf(tanf(vkr_to_radians(fov) * 0.5f) * Min(1.0f, aspect));
  model->distance = radius / Max(0.01f, sinf(half_angle)) * 1.15f;
  if (!model->framed) {
    /* The first subject keeps the Scene's heading, seen from a little
       above; later ones keep the orbit's angles. */
    model->yaw =
        frame->scene_recall.camera_valid ? frame->scene_recall.yaw : -90.0f;
    model->pitch = MODEL_PITCH_START;
    model->framed = true_v;
  }
  model_apply(editor, frame);
}

static void model_enter(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorModelView *model = &editor->model;
  model->camera_saved = frame->scene_recall.camera_valid;
  model->scene_position = frame->scene_recall.position;
  model->scene_yaw = frame->scene_recall.yaw;
  model->scene_pitch = frame->scene_recall.pitch;
  model->scene_generation = frame->scene_generation;
  MemCopy(model->hidden, editor->hidden,
          editor->hidden_count * sizeof(editor->hidden[0]));
  model->hidden_count = editor->hidden_count;
  model->hidden_isolate = editor->hidden_isolate;
  model->subject = VKR_ENTITY_ID_INVALID;
  model->subject_extra = 0u;
  model->orbiting = false_v;
}

/* Puts the Scene's Hide set and camera back. A scene opened meanwhile has
   its own camera, and the objects it hid are gone. */
static void model_leave(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorModelView *model = &editor->model;
  model->orbiting = false_v;
  if (frame->scene_generation != model->scene_generation) {
    return;
  }
  MemCopy(editor->hidden, model->hidden,
          model->hidden_count * sizeof(editor->hidden[0]));
  editor->hidden_count = model->hidden_count;
  editor->hidden_isolate = model->hidden_isolate;
  editor->hidden_dirty = true_v;
  if (model->camera_saved && frame->editor_state_request) {
    VkrSampleEditorStateRequest *request = frame->editor_state_request;
    request->move_camera = true_v;
    request->camera_position = model->scene_position;
    request->camera_yaw = model->scene_yaw;
    request->camera_pitch = model->scene_pitch;
  }
}

/* A new selection becomes the subject: it alone draws and the orbit
   frames it. An empty selection keeps the last subject. */
static void model_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  VkrEditorModelView *model = &editor->model;
  if (frame->scene_generation != model->scene_generation) {
    model_enter(editor, frame);
    model->camera_saved = false_v;
  }
  const VkrEntityId selected = frame->selected_entity;
  if (selected.u64 && (selected.u64 != model->subject.u64 ||
                       editor->selection_extra_count != model->subject_extra)) {
    model->subject = selected;
    model->subject_extra = editor->selection_extra_count;
    vkr_editor_hide_selection(editor, frame, true_v);
    model_frame(editor, frame);
  }
  if (model->orbiting &&
      (frame->mouse_captured ||
       (!input_is_button_down(frame->input, BUTTON_LEFT) &&
        !input_is_button_down(frame->input, BUTTON_MIDDLE)))) {
    model->orbiting = false_v;
  }
}

// =============================================================================
// Update and bodies
// =============================================================================

void vkr_editor_views_update(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  if (!frame->dock) {
    return;
  }
  VkrEditorWorkbench *item = view_workbench(editor);
  item->views |= 1u << VKR_EDITOR_VIEW_SCENE;
  /* The Views panel is the only host of a window's body while its
     workbench offers the view, as a docked panel is (ADR-089). */
  for (uint32_t kind = 0u; kind < VKR_EDITOR_WINDOW_COUNT; ++kind) {
    const VkrEditorView view = vkr_editor_window_view(kind);
    if (view != VKR_EDITOR_VIEW_COUNT && editor->windows[kind].visible &&
        ((item->views >> view) & 1u)) {
      editor->windows[kind].visible = false_v;
      vkr_editor_view_show(editor, frame, editor->workbenches.active, view);
    }
  }
  /* The app's Scene-only mode has no Views panel; a maximized Scene keeps
     the Model view. */
  const VkrEditorView view = frame->scene_only && !frame->scene_maximized
                                 ? VKR_EDITOR_VIEW_SCENE
                                 : vkr_editor_view_active(editor);
  frame->dock->scene_view_hidden =
      view == VKR_EDITOR_VIEW_GRAPH || view == VKR_EDITOR_VIEW_SCRIPT;
  if (editor->view_shown != view) {
    if (editor->view_shown == VKR_EDITOR_VIEW_MODEL) {
      model_leave(editor, frame);
    }
    editor->view_shown = view;
    if (view == VKR_EDITOR_VIEW_MODEL) {
      model_enter(editor, frame);
    }
  }
  if (view == VKR_EDITOR_VIEW_MODEL) {
    model_update(editor, frame);
  }
}

/* The caption, orbit ring and Frame button over the Scene image, and the
   orbit's mouse: a drag on the ring or a middle drag over the image turns
   it, the wheel moves in and out. */
static void model_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                        VkrUiRect rect) {
  VkrUiSystem *ui = frame->ui;
  VkrEditorModelView *model = &editor->model;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = Max(ui->content_scale, 0.001f);
  const float32_t width = rect.width / scale;
  const float32_t height = rect.height / scale;
  if (width < 120.0f || height < 120.0f) {
    return;
  }

  /* Caption: what the view shows, or how to give it a subject. */
  const VkrScene *scene = vkr_editor_entity_scene(frame, model->subject);
  const bool8_t subject =
      scene && vkr_scene_entity_alive(scene, model->subject);
  char caption[160];
  if (subject) {
    const String8 name = vkr_scene_get_name(scene, model->subject);
    const uint32_t extra = editor->selection_extra_count;
    snprintf(caption, sizeof(caption), "%.*s%s", (int)Min(name.length, 96u),
             name.length ? (const char *)name.str : "Object",
             extra ? " and the rest of the selection" : "");
  } else {
    snprintf(caption, sizeof(caption),
             "Select an object in the Outliner or the Scene to see it alone");
  }
  /* The caption sits at the bottom centre, clear of the orientation gizmo
     on the left; the ring and Frame stand above the frame-time chip on the
     right. */
  const float32_t bottom = height - 12.0f;
  const float32_t caption_w = Min(width - 220.0f, 460.0f);
  VkrUiWidgetConfig label =
      vkr_editor_details_widget((width - Max(caption_w, 120.0f)) * 0.5f,
                                bottom - 26.0f, Max(caption_w, 120.0f), 26.0f);
  label.style.font_size_pt = theme->font_body;
  label.style.text_color = subject ? theme->text : theme->text_secondary;
  label.style.background_color = vkr_ui_color_alpha(theme->window, 0.82f);
  label.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
  label.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, 10.0f};
  label.icon = VKR_UI_ICON_MESH;
  label.icon_size_pt = 13.0f;
  label.icon_color = theme->accent;
  label.tooltip = vkr_editor_view_tooltip(VKR_EDITOR_VIEW_MODEL);
  vkr_ui_label(
      ui, string8_lit("model.caption"),
      string8_create_from_cstr((const uint8_t *)caption, strlen(caption)),
      &label);
  if (!subject) {
    return;
  }

  /* The orbit ring: press and drag on it to turn the subject. */
  const float32_t ring_x = width - MODEL_RING_PT - 12.0f;
  const float32_t ring_y = bottom - 36.0f - MODEL_RING_PT;
  VkrUiWidgetConfig ring =
      vkr_editor_details_widget(ring_x, ring_y, MODEL_RING_PT, MODEL_RING_PT);
  ring.style.background_color = vkr_ui_color_alpha(theme->window, 0.82f);
  ring.style.hover_background_color =
      vkr_ui_color_mix(theme->window, theme->accent, 0.3f);
  ring.style.corner_radius_pt =
      (Vec4){MODEL_RING_PT * 0.5f, MODEL_RING_PT * 0.5f, MODEL_RING_PT * 0.5f,
             MODEL_RING_PT * 0.5f};
  ring.style.border_pt = (VkrUiEdges){1.5f, 1.5f, 1.5f, 1.5f};
  ring.style.border_color =
      model->orbiting ? theme->accent : vkr_ui_color_alpha(theme->text, 0.35f);
  ring.icon = VKR_UI_ICON_ROTATE;
  ring.icon_size_pt = 22.0f;
  ring.icon_color = model->orbiting ? theme->accent : theme->text;
  ring.tooltip = string8_lit("Drag to orbit the model; middle-drag anywhere "
                             "over it does the same, the wheel zooms");
  const VkrUiId ring_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("model.orbit"));
  (void)vkr_ui_button(ui, string8_lit("model.orbit"), (String8){0}, &ring);
  if (ui->hot_id == ring_id && ui->mouse_pressed && !frame->mouse_captured) {
    model->orbiting = true_v;
  }

  VkrUiWidgetConfig reframe = vkr_editor_details_widget(
      ring_x - 40.0f, ring_y + (MODEL_RING_PT - 32.0f) * 0.5f, 32.0f, 32.0f);
  reframe.style.background_color = vkr_ui_color_alpha(theme->window, 0.82f);
  reframe.style.hover_background_color = theme->row_hover;
  reframe.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
  reframe.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  reframe.icon = VKR_UI_ICON_FRAME;
  reframe.icon_size_pt = 16.0f;
  reframe.tooltip = string8_lit("Frame the model again");
  if (vkr_ui_button(ui, string8_lit("model.frame"), (String8){0}, &reframe)) {
    model_frame(editor, frame);
  }

  /* Middle drag and the wheel over the Scene image, not over other UI. */
  const bool8_t over_image =
      !frame->mouse_captured && ui->mouse_input_layer == 0u &&
      ui->hot_id == VKR_UI_ID_NONE && (float32_t)ui->mouse_x >= rect.x &&
      (float32_t)ui->mouse_x < rect.x + rect.width &&
      (float32_t)ui->mouse_y >= rect.y &&
      (float32_t)ui->mouse_y < rect.y + rect.height;
  if (over_image && input_button_just_pressed(frame->input, BUTTON_MIDDLE)) {
    model->orbiting = true_v;
  }
  bool8_t moved = false_v;
  if (model->orbiting) {
    int32_t dx = 0;
    int32_t dy = 0;
    input_get_mouse_delta(frame->input, &dx, &dy);
    if (dx || dy) {
      model->yaw += (float32_t)dx * MODEL_ORBIT_DEGREES_PER_PX;
      model->pitch = vkr_clamp_f32(
          model->pitch - (float32_t)dy * MODEL_ORBIT_DEGREES_PER_PX, -85.0f,
          85.0f);
      moved = true_v;
    }
    ui->capture.mouse = true_v;
  }
  if (over_image && ui->mouse_wheel != 0) {
    model->distance = vkr_clamp_f32(
        model->distance * expf(-(float32_t)ui->mouse_wheel * 0.12f), 0.05f,
        5000.0f);
    moved = true_v;
    ui->capture.mouse = true_v;
  }
  if (moved) {
    model_apply(editor, frame);
  }
}

void vkr_editor_views_build_body(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrUiRect rect) {
  switch (editor->view_shown) {
  case VKR_EDITOR_VIEW_MODEL:
    model_build(editor, frame, rect);
    break;
  case VKR_EDITOR_VIEW_GRAPH:
    vkr_editor_material_panel_build(editor, frame, rect);
    break;
  case VKR_EDITOR_VIEW_SCRIPT:
    vkr_editor_code_build(editor->code, editor, frame, rect);
    break;
  default:
    break;
  }
}

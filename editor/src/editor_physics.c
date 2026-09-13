#include "editor_physics.h"
#include "editor_ui.h"
#include "renderer/systems/vkr_scene_physics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PHYSICS_LINE_MAX 512u
#define PHYSICS_RESERVED_NODES 256u

struct VkrEditorPhysicsLine {
  VkrUiId widget;
  VkrEntityId entity;
  Vec3 from;
  Vec3 to;
};

static VkrUiWidgetConfig physics_widget(float32_t x, float32_t y,
                                        float32_t width, float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement.column = 0;
  c.placement.row = 0;
  c.placement.justify = VKR_UI_ALIGN_START;
  c.placement.align = VKR_UI_ALIGN_START;
  c.placement.margin_pt = (VkrUiEdges){y, 0, 0, x};
  c.style.min_size_pt = c.style.max_size_pt = (Vec2){width, height};
  c.style.font_size_pt = 11;
  c.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
  return c;
}

static void physics_line(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                         VkrEntityId entity, Vec3 from, Vec3 to, Vec4 color,
                         uint32_t capacity) {
  if (editor->physics_line_count >= capacity) {
    editor->physics_lines_truncated = true_v;
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const uint32_t index = editor->physics_line_count++;
  (void)vkr_ui_push_id_u64(ui, index);
  const Vec2 hidden[4] = {{0}, {0}, {0}, {0}};
  VkrUiWidgetConfig c = physics_widget(0, 0, 1, 1);
  c.style.background_color = (Vec4){0};
  c.style.padding_pt = (VkrUiEdges){0};
  c.style.text_color = color;
  vkr_ui_bezier(ui, string8_lit("edge"), hidden, 1.5f, &c);
  editor->physics_lines[index] = (VkrEditorPhysicsLine){
      .widget =
          vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("edge")),
      .entity = entity,
      .from = from,
      .to = to,
  };
  (void)vkr_ui_pop_id(ui);
}

static void physics_shape(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                          VkrEntityId entity, const VkrSceneColliderConfig *c,
                          Vec4 color, uint32_t capacity) {
  if (c->shape == VKR_PHYSICS_CONVEX_HULL ||
      c->shape == VKR_PHYSICS_TRIANGLE_MESH) {
    const VkrEntityId owner = vkr_scene_physics_owner(frame->scene, entity);
    const VkrCollisionGeometry *geometry = vkr_scene_physics_collider_geometry(
        frame->scene, owner, c->authored_id);
    if (!geometry) {
      return;
    }
    for (uint32_t i = 0; i < geometry->index_count; i += 3) {
      for (uint32_t edge = 0; edge < 3; ++edge) {
        if (editor->physics_line_count >= capacity) {
          editor->physics_lines_truncated = true_v;
          return;
        }
        const float32_t *a =
            geometry->positions + geometry->indices[i + edge] * 3;
        const float32_t *b =
            geometry->positions + geometry->indices[i + (edge + 1) % 3] * 3;
        physics_line(editor, frame, entity, (Vec3){a[0], a[1], a[2]},
                     (Vec3){b[0], b[1], b[2]}, color, capacity);
      }
    }
    return;
  }
  if (c->shape == VKR_PHYSICS_BOX) {
    for (uint32_t corner = 0; corner < 8; ++corner) {
      const Vec3 from = {(corner & 1) ? c->half_extent.x : -c->half_extent.x,
                         (corner & 2) ? c->half_extent.y : -c->half_extent.y,
                         (corner & 4) ? c->half_extent.z : -c->half_extent.z};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        if (corner & (1u << axis)) {
          continue;
        }
        Vec3 to = from;
        if (axis == 0) {
          to.x = -to.x;
        } else if (axis == 1) {
          to.y = -to.y;
        } else {
          to.z = -to.z;
        }
        physics_line(editor, frame, entity, from, to, color, capacity);
      }
    }
    return;
  }
  if (c->shape == VKR_PHYSICS_CAPSULE) {
    const Vec3 offsets[4] = {{c->radius, 0, 0},
                             {-c->radius, 0, 0},
                             {0, 0, c->radius},
                             {0, 0, -c->radius}};
    for (uint32_t i = 0; i < ArrayCount(offsets); ++i) {
      Vec3 from = offsets[i];
      Vec3 to = offsets[i];
      from.y = -c->half_height;
      to.y = c->half_height;
      physics_line(editor, frame, entity, from, to, color, capacity);
    }
  }
  const uint32_t circles = c->shape == VKR_PHYSICS_CAPSULE ? 4u : 3u;
  for (uint32_t circle = 0; circle < circles; ++circle) {
    for (uint32_t segment = 0; segment < 24; ++segment) {
      Vec3 points[2];
      for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        const uint32_t sample = (segment + endpoint) % 24u;
        const float32_t angle = (float32_t)sample * (6.28318530718f / 24.0f);
        const float32_t x = c->radius * cosf(angle);
        const float32_t y = c->radius * sinf(angle);
        Vec3 p = circle == 0   ? (Vec3){x, y, 0}
                 : circle == 1 ? (Vec3){0, y, x}
                               : (Vec3){x, 0, y};
        if (c->shape == VKR_PHYSICS_CAPSULE) {
          if (circle < 2) {
            p.y += segment < 12 ? c->half_height : -c->half_height;
          } else {
            p.y = circle == 2 ? c->half_height : -c->half_height;
          }
        }
        points[endpoint] = p;
      }
      physics_line(editor, frame, entity, points[0], points[1], color,
                   capacity);
    }
  }
}

void vkr_editor_physics_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  editor->physics_line_count = 0;
  editor->physics_lines = NULL;
  editor->physics_lines_truncated = false_v;
  editor->physics_scene_generation = frame->scene_generation;
  if (!frame->scene || !frame->mapping_valid ||
      frame->scene_rendering_stopped) {
    return;
  }
  const uint32_t bodies = vkr_scene_physics_body_count(frame->scene);
  if (!bodies) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t width = image.z / ui->content_scale;
  const float32_t height = image.w / ui->content_scale;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.placement.justify = VKR_UI_ALIGN_START;
  panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){image.y / ui->content_scale, 0, 0,
                                           image.x / ui->content_scale};
  panel.style.min_size_pt = panel.style.max_size_pt = (Vec2){width, height};
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.background_color = (Vec4){0};
  panel.clip_children = true_v;
  editor->physics_panel = vkr_ui_id_stack_widget_label(
      &ui->id_stack, string8_lit("physics.overlay"));
  if (!vkr_ui_panel_begin(ui, string8_lit("physics.overlay"), &panel)) {
    return;
  }
  const uint32_t available =
      ui->frame_node_count + PHYSICS_RESERVED_NODES < ui->frame_node_capacity
          ? ui->frame_node_capacity - ui->frame_node_count -
                PHYSICS_RESERVED_NODES
          : 0u;
  const uint32_t capacity = Min(PHYSICS_LINE_MAX, available);
  if (frame->collision_display && !capacity) {
    editor->physics_lines_truncated = true_v;
  }
  if (frame->collision_display && capacity) {
    editor->physics_lines = vkr_allocator_alloc(
        ui->frame_allocator, capacity * sizeof(*editor->physics_lines),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (editor->physics_lines) {
      const VkrEntityId selected =
          vkr_scene_physics_owner(frame->scene, frame->selected_entity);
      for (uint32_t i = 0; i < bodies; ++i) {
        const VkrEntityId owner = vkr_scene_physics_body_at(frame->scene, i);
        if (frame->collision_display == 1 && owner.u64 != selected.u64) {
          continue;
        }
        VkrScenePhysicsSnapshot body;
        if (!vkr_scene_physics_read(frame->scene, owner, &body)) {
          continue;
        }
        VkrPhysicsPose pose = {0};
        (void)vkr_scene_physics_get_pose(frame->scene, owner, &pose);
        for (uint32_t j = 0; j < body.collider_count; ++j) {
          const VkrSceneColliderConfig *c = &body.colliders[j];
          const bool8_t enabled =
              body.enabled && c->enabled &&
              !vkr_scene_physics_is_disabled(frame->scene) &&
              !vkr_scene_physics_body_is_disabled(frame->scene, owner);
          const Vec4 color =
              !enabled                            ? (Vec4){0.5f, 0.5f, 0.5f, 1}
              : body.sensor                       ? (Vec4){1, 0.55f, 0.95f, 1}
              : body.motion == VKR_PHYSICS_STATIC ? (Vec4){0.3f, 0.9f, 0.45f, 1}
              : body.motion == VKR_PHYSICS_KINEMATIC ? (Vec4){0.4f, 0.7f, 1, 1}
              : pose.active                          ? (Vec4){1, 0.7f, 0.15f, 1}
                            : (Vec4){0.5f, 0.7f, 0.7f, 1};
          physics_shape(editor, frame,
                        vkr_scene_physics_collider_entity(frame->scene, owner,
                                                          c->authored_id),
                        c, color, capacity);
        }
      }
    } else {
      editor->physics_lines_truncated = true_v;
    }
  }
  char status[192];
  char debt[48] = {0};
  const float64_t pending_time = vkr_scene_physics_debt(frame->scene);
  if (pending_time >= VKR_SCENE_PHYSICS_FIXED_DT) {
    snprintf(debt, sizeof(debt), " | Catch-up: %.3fs", pending_time);
  }
  const char *error = vkr_scene_physics_error(frame->scene);
  snprintf(status, sizeof(status), "%u bodies | %s%s%s", bodies,
           error && error[0]           ? error
           : frame->simulation_running ? "Simulating"
                                       : "Paused",
           editor->physics_lines_truncated ? " | Overlay limit reached" : "",
           debt);
  VkrUiWidgetConfig label = physics_widget(
      8, Min(56.0f, Max(0.0f, height - 62)), Max(1.0f, width - 16), 22);
  label.style.background_color = (Vec4){0.025f, 0.035f, 0.05f, 0.85f};
  vkr_ui_label(
      ui, string8_lit("status"),
      string8_create_from_cstr((const uint8_t *)status, strlen(status)),
      &label);
  const char *display[] = {"Collision: Off", "Collision: Selected",
                           "Collision: All"};
  const char *titles[] = {
      "Step", "Reset", display[Min(frame->collision_display, 2u)],
      vkr_scene_physics_is_disabled(frame->scene) ? "Restore physics"
                                                  : "Disable physics"};
  const VkrSampleTransportAction actions[] = {
      VKR_SAMPLE_TRANSPORT_STEP_SIMULATION,
      VKR_SAMPLE_TRANSPORT_RESET_SIMULATION,
      VKR_SAMPLE_TRANSPORT_CYCLE_COLLISION_DISPLAY,
      VKR_SAMPLE_TRANSPORT_TOGGLE_PHYSICS};
  const float32_t button_width = Min(118.0f, Max(24.0f, (width - 28) / 4));
  for (uint32_t i = 0; i < ArrayCount(actions); ++i) {
    VkrUiWidgetConfig button =
        physics_widget(8 + i * (button_width + 4),
                       Min(84.0f, Max(0.0f, height - 34)), button_width, 26);
    button.style.background_color = (Vec4){0.055f, 0.075f, 0.1f, 0.94f};
    button.disabled = i == 0 && frame->simulation_running;
    button.tooltip =
        i == 3
            ? string8_lit("Temporary session override; Reset restores physics")
            : (String8){0};
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("control"),
                      string8_create_from_cstr((const uint8_t *)titles[i],
                                               strlen(titles[i])),
                      &button)) {
      *frame->transport_action = actions[i];
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static float32_t physics_clip_distance(Vec4 p, uint32_t plane) {
  switch (plane) {
  case 0:
    return p.w + p.x;
  case 1:
    return p.w - p.x;
  case 2:
    return p.w + p.y;
  case 3:
    return p.w - p.y;
  case 4:
    return p.z;
  default:
    return p.w - p.z;
  }
}

void vkr_editor_physics_project(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  if (!frame->scene || frame->scene_rendering_stopped ||
      editor->physics_scene_generation != frame->scene_generation ||
      !editor->physics_line_count) {
    return;
  }
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = frame->ui->content_scale;
  (void)vkr_ui_widget_set_rect(frame->ui, editor->physics_panel,
                               (VkrUiRect){image.x / scale, image.y / scale,
                                           image.z / scale, image.w / scale});
  for (uint32_t i = 0; i < editor->physics_line_count; ++i) {
    const VkrEditorPhysicsLine *line = &editor->physics_lines[i];
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        frame->scene->world, line->entity, frame->scene->comp_transform);
    Vec2 points[4] = {{0}, {0}, {0}, {0}};
    if (transform) {
      const Mat4 mvp = mat4_mul(frame->view_projection, transform->world);
      Vec4 a = mat4_mul_vec4(mvp, vec3_to_vec4(line->from, 1));
      Vec4 b = mat4_mul_vec4(mvp, vec3_to_vec4(line->to, 1));
      bool8_t visible = true_v;
      for (uint32_t plane = 0; plane < 6; ++plane) {
        const float32_t da = physics_clip_distance(a, plane);
        const float32_t db = physics_clip_distance(b, plane);
        if (!isfinite(da) || !isfinite(db) || (da < 0 && db < 0)) {
          visible = false_v;
          break;
        }
        if ((da < 0) != (db < 0)) {
          const Vec4 intersection =
              vec4_add(a, vec4_scale(vec4_sub(b, a), da / (da - db)));
          if (da < 0) {
            a = intersection;
          } else {
            b = intersection;
          }
        }
      }
      if (visible && a.w > 0.000001f && b.w > 0.000001f) {
        points[0] = points[1] =
            (Vec2){(a.x / a.w * 0.5f + 0.5f) * image.z / scale,
                   (a.y / a.w * 0.5f + 0.5f) * image.w / scale};
        points[2] = points[3] =
            (Vec2){(b.x / b.w * 0.5f + 0.5f) * image.z / scale,
                   (b.y / b.w * 0.5f + 0.5f) * image.w / scale};
      }
    }
    (void)vkr_ui_bezier_set_points(frame->ui, line->widget, points);
  }
}

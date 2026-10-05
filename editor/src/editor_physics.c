#include "editor_physics.h"
#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_ops.h"
#include "editor_terrain.h"
#include "editor_ui.h"
#include "renderer/systems/vkr_scene_brush.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_population.h"
#include "renderer/systems/vkr_scene_types.h"
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

/* A capsule standing on `foot`, as the player's character will, with a
 * line toward the start's -Z facing. */
static void physics_player_start(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, uint32_t capacity) {
  const VkrPhysicsCharacterDesc character = vkr_physics_character_default();
  const float32_t radius = character.radius;
  const float32_t half = character.half_height;
  const Vec3 center = {0.0f, half + radius, 0.0f};
  const Vec4 color = {0.35f, 0.68f, 1.0f, 1.0f};
  for (uint32_t side = 0; side < 4; ++side) {
    const float32_t angle = (float32_t)side * 1.57079632679f;
    const Vec3 edge = {radius * cosf(angle), 0.0f, radius * sinf(angle)};
    physics_line(editor, frame, entity,
                 vec3_add(center, vec3_new(edge.x, -half, edge.z)),
                 vec3_add(center, vec3_new(edge.x, half, edge.z)), color,
                 capacity);
  }
  for (uint32_t circle = 0; circle < 4; ++circle) {
    for (uint32_t segment = 0; segment < 24; ++segment) {
      Vec3 points[2];
      for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        const uint32_t sample = (segment + endpoint) % 24u;
        const float32_t angle = (float32_t)sample * (6.28318530718f / 24.0f);
        const float32_t x = radius * cosf(angle);
        const float32_t y = radius * sinf(angle);
        Vec3 p = circle == 0   ? (Vec3){x, y, 0}
                 : circle == 1 ? (Vec3){0, y, x}
                               : (Vec3){x, 0, y};
        if (circle < 2) {
          p.y += segment < 12 ? half : -half;
        } else {
          p.y = circle == 2 ? half : -half;
        }
        points[endpoint] = vec3_add(center, p);
      }
      physics_line(editor, frame, entity, points[0], points[1], color,
                   capacity);
    }
  }
  /* Facing: the player looks along the start's -Z. */
  const Vec3 eye = {0.0f, half * 2.0f + radius * 0.6f, 0.0f};
  const Vec3 tip = vec3_add(eye, vec3_new(0.0f, 0.0f, -0.8f));
  physics_line(editor, frame, entity, eye, tip, color, capacity);
  physics_line(editor, frame, entity, tip,
               vec3_add(tip, vec3_new(0.18f, 0.0f, 0.18f)), color, capacity);
  physics_line(editor, frame, entity, tip,
               vec3_add(tip, vec3_new(-0.18f, 0.0f, 0.18f)), color, capacity);
}

/* Player Starts of every loaded container, while not playing. */
static void physics_player_starts(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  uint32_t capacity) {
  const VkrScene *scenes[2] = {frame->scene, frame->world};
  for (uint32_t c = 0; c < ArrayCount(scenes); ++c) {
    VkrEntityId starts[16];
    const uint32_t count =
        scenes[c]
            ? vkr_scene_find_typed(scenes[c], &vkr_scene_player_start_type,
                                   starts, ArrayCount(starts))
            : 0u;
    for (uint32_t i = 0; i < Min(count, (uint32_t)ArrayCount(starts)); ++i) {
      if (vkr_scene_entity_visible(scenes[c], starts[i])) {
        physics_player_start(editor, frame, starts[i], capacity);
      }
    }
  }
}

static uint32_t physics_player_start_count(const VkrSampleUiFrame *frame) {
  return (frame->scene
              ? vkr_scene_find_typed(frame->scene, &vkr_scene_player_start_type,
                                     NULL, 0u)
              : 0u) +
         (frame->world
              ? vkr_scene_find_typed(frame->world, &vkr_scene_player_start_type,
                                     NULL, 0u)
              : 0u);
}

/* Box outlines of the entities in pending agent changes, in each entity's
   local frame, so they follow it as it moves. */
static void physics_pending_changes(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t capacity) {
  const VkrEditorOps *ops = vkr_editor_agent_ops(editor->agent);
  const Vec4 color = {1.0f, 0.72f, 0.18f, 1.0f};
  for (uint32_t c = 0; c < vkr_editor_ops_change_count(ops); ++c) {
    const VkrEditorChange *change = vkr_editor_ops_change(ops, c);
    for (uint32_t e = 0; e < change->entity_count; ++e) {
      const VkrEntityId entity = change->entities[e];
      const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
      Vec3 lo = {0};
      Vec3 hi = {0};
      if (!scene || !vkr_scene_entity_alive(scene, entity)) {
        continue;
      }
      if (!vkr_scene_entity_local_bounds(scene, entity, &lo, &hi)) {
        /* An empty object shows as a half-meter marker. */
        lo = vec3_new(-0.25f, -0.25f, -0.25f);
        hi = vec3_new(0.25f, 0.25f, 0.25f);
      }
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const Vec3 from = {(corner & 1) ? hi.x : lo.x,
                           (corner & 2) ? hi.y : lo.y,
                           (corner & 4) ? hi.z : lo.z};
        for (uint32_t axis = 0; axis < 3; ++axis) {
          if (corner & (1u << axis)) {
            continue;
          }
          Vec3 to = from;
          if (axis == 0) {
            to.x = hi.x;
          } else if (axis == 1) {
            to.y = hi.y;
          } else {
            to.z = hi.z;
          }
          physics_line(editor, frame, entity, from, to, color, capacity);
        }
      }
    }
  }
}

/* The box a brush drag outlines, in world space (no entity). */
static void physics_brush_draft(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                uint32_t capacity) {
  const Vec3 a = editor->brush_draw_start;
  const Vec3 b = editor->brush_draw_end;
  const Vec3 lo = vec3_new(Min(a.x, b.x), a.y, Min(a.z, b.z));
  const Vec3 hi =
      vec3_new(Max(a.x, b.x), a.y + editor->brush_draw_height, Max(a.z, b.z));
  const Vec4 color = {0.35f, 0.75f, 1.0f, 1.0f};
  for (uint32_t corner = 0; corner < 8; ++corner) {
    const Vec3 from = {(corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                       (corner & 4) ? hi.z : lo.z};
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (corner & (1u << axis)) {
        continue;
      }
      Vec3 to = from;
      if (axis == 0) {
        to.x = hi.x;
      } else if (axis == 1) {
        to.y = hi.y;
      } else {
        to.z = hi.z;
      }
      physics_line(editor, frame, VKR_ENTITY_ID_INVALID, from, to, color,
                   capacity);
    }
  }
}

/* World position of an entity's origin. */
static bool8_t physics_entity_at(const VkrScene *scene, VkrEntityId entity,
                                 Vec3 *out) {
  const SceneTransform *transform =
      vkr_scene_entity_alive(scene, entity)
          ? vkr_entity_get_component(scene->world, entity,
                                     scene->comp_transform)
          : NULL;
  if (!transform) {
    return false_v;
  }
  *out = mat4_position(transform->world);
  return true_v;
}

/* Entity IO lines (ADR-084): from the selection to its connections'
   targets, and from the sources of connections that reach it; a selected
   connection draws its one line. Returns how many it would draw. */
static uint32_t physics_io_lines(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 uint32_t capacity, bool8_t draw) {
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene = vkr_editor_entity_scene(frame, selected);
  if (!scene || !vkr_scene_entity_alive(scene, selected)) {
    return 0u;
  }
  VkrEntityRef self = {0};
  const bool8_t referable = vkr_scene_entity_ref(scene, selected, &self);
  const bool8_t connection_selected =
      vkr_scene_get_typed(scene, selected, &vkr_scene_io_connection_type) !=
      NULL;
  uint32_t count = 0u;
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    const SceneIoConnection *value =
        vkr_scene_entity_alive(scene, entity)
            ? vkr_scene_get_typed(scene, entity, &vkr_scene_io_connection_type)
            : NULL;
    const SceneTransform *transform =
        value ? vkr_entity_get_component(scene->world, entity,
                                         scene->comp_transform)
              : NULL;
    if (!transform) {
      continue;
    }
    const bool8_t out = transform->parent.u64 == selected.u64;
    const bool8_t in =
        referable && MemCompare(&value->target, &self, sizeof(self)) == 0;
    if (!(connection_selected ? entity.u64 == selected.u64 : out || in)) {
      continue;
    }
    Vec3 from = {0};
    Vec3 to = {0};
    if (!physics_entity_at(scene, transform->parent, &from) ||
        !physics_entity_at(
            scene, vkr_scene_find_entity_ref(scene, &value->target), &to)) {
      continue;
    }
    count++;
    if (draw) {
      /* Outgoing amber, incoming blue. */
      const Vec4 color = (out || connection_selected)
                             ? (Vec4){0.98f, 0.82f, 0.35f, 1.0f}
                             : (Vec4){0.45f, 0.78f, 0.98f, 1.0f};
      physics_line(editor, frame, VKR_ENTITY_ID_INVALID, from, to, color,
                   capacity);
    }
  }
  return count;
}

/* Whether the selection is a brush face. */
/* Level tool outlines in world space: the selected brush face, the cut the
   clip tool previews, and the issues and region of the last level check
   while the Level checks window is open. */
/* Splines as curves, the selected one finer and brighter, and the selected
   scatter's box (ADR-084). Counts
   the lines when `draw` is false. */
static uint32_t physics_population_lines(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         uint32_t capacity, bool8_t draw) {
  const VkrScene *scene = frame->scene;
  if (!scene) {
    return 0u;
  }
  VkrEntityId selected_spline = frame->selected_entity;
  const SceneTransform *selected_transform =
      vkr_scene_entity_alive(scene, selected_spline)
          ? vkr_entity_get_component(scene->world, selected_spline,
                                     scene->comp_transform)
          : NULL;
  if (selected_transform && vkr_scene_get_typed(scene, selected_spline,
                                                &vkr_scene_spline_point_type)) {
    selected_spline = selected_transform->parent;
  }
  uint32_t count = 0u;
  Vec3 points[VKR_SPLINE_POINT_MAX];
  for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
    const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
    if (!vkr_scene_entity_alive(scene, entity) ||
        !vkr_scene_get_typed(scene, entity, &vkr_scene_spline_type)) {
      continue;
    }
    bool8_t closed = false_v;
    const uint32_t point_count = vkr_scene_spline_world_points(
        scene, entity, points, VKR_SPLINE_POINT_MAX, &closed);
    const uint32_t segments = vkr_spline_segment_count(point_count, closed);
    const bool8_t selected = entity.u64 == selected_spline.u64;
    const uint32_t chords = selected ? 12u : 4u;
    const Vec4 color = selected ? (Vec4){1.0f, 0.72f, 0.2f, 1.0f}
                                : (Vec4){0.7f, 0.75f, 0.85f, 1.0f};
    for (uint32_t s = 0; s < segments; ++s) {
      Vec3 from =
          vkr_spline_evaluate(points, point_count, closed, s, 0.0f, NULL);
      for (uint32_t c = 1; c <= chords; ++c) {
        const Vec3 to =
            vkr_spline_evaluate(points, point_count, closed, s,
                                (float32_t)c / (float32_t)chords, NULL);
        if (draw) {
          physics_line(editor, frame, VKR_ENTITY_ID_INVALID, from, to, color,
                       capacity);
        }
        count++;
        from = to;
      }
    }
  }
  const SceneScatter *scatter =
      selected_transform ? vkr_scene_get_typed(scene, frame->selected_entity,
                                               &vkr_scene_scatter_type)
                         : NULL;
  if (scatter) {
    const Vec3 e = scatter->extents;
    Vec3 corners[8];
    for (uint32_t c = 0; c < 8u; ++c) {
      corners[c] =
          mat4_mul_vec3(selected_transform->world,
                        vec3_new((c & 1u) ? e.x : -e.x, (c & 2u) ? e.y : -e.y,
                                 (c & 4u) ? e.z : -e.z));
    }
    static const uint8_t edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                         {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                         {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (uint32_t edge = 0; edge < 12u; ++edge) {
      if (draw) {
        physics_line(editor, frame, VKR_ENTITY_ID_INVALID,
                     corners[edges[edge][0]], corners[edges[edge][1]],
                     (Vec4){0.45f, 0.9f, 0.5f, 1.0f}, capacity);
      }
      count++;
    }
  }
  return count;
}

static void physics_level_tools(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                uint32_t capacity) {
  if (editor->face_handle_count) {
    /* The brush's edges, each face's arrow, the hot face's outline and
       where a drag would put it. */
    const VkrScene *scene =
        vkr_editor_entity_scene(frame, editor->face_handle_faces[0]);
    const SceneTransform *face_transform =
        scene ? vkr_entity_get_component(scene->world,
                                         editor->face_handle_faces[0],
                                         scene->comp_transform)
              : NULL;
    VkrBrushGeometry *geometry =
        vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*geometry),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    VkrEntityId faces[VKR_BRUSH_FACE_MAX];
    const uint32_t face_count =
        geometry && face_transform
            ? vkr_editor_brush_build(scene, face_transform->parent, geometry,
                                     faces)
            : 0u;
    const int32_t hot = editor->face_handle_hot;
    const VkrEntityId hot_face =
        hot >= 0 ? editor->face_handle_faces[hot] : VKR_ENTITY_ID_INVALID;
    const Vec4 edge = {1.0f, 0.85f, 0.2f, 0.55f};
    const Vec4 highlight = {1.0f, 0.85f, 0.2f, 1.0f};
    const Vec4 arrow = {0.35f, 0.75f, 1.0f, 1.0f};
    for (uint32_t f = 0; f < face_count; ++f) {
      const VkrBrushPolygon polygon = geometry->polygons[f];
      const bool8_t is_hot = faces[f].u64 == hot_face.u64;
      const bool8_t handled = editor->face_handle_count > 1u ||
                              faces[f].u64 == editor->face_handle_faces[0].u64;
      if (!handled) {
        continue;
      }
      const Vec3 offset =
          is_hot && editor->face_dragging
              ? vec3_scale(geometry->normals[f], editor->face_drag_distance)
              : vec3_zero();
      for (uint32_t c = 0; c < polygon.count; ++c) {
        const Vec3 a = geometry->vertices[polygon.first + c];
        const Vec3 b =
            geometry->vertices[polygon.first + (c + 1u) % polygon.count];
        physics_line(editor, frame, VKR_ENTITY_ID_INVALID, a, b,
                     is_hot ? highlight : edge, capacity);
        if (is_hot && editor->face_dragging &&
            editor->face_drag_distance != 0.0f) {
          physics_line(editor, frame, VKR_ENTITY_ID_INVALID,
                       vec3_add(a, offset), vec3_add(b, offset), arrow,
                       capacity);
        }
      }
    }
    for (uint32_t i = 0; i < editor->face_handle_count; ++i) {
      const float32_t length =
          editor->face_handle_length * ((int32_t)i == hot ? 1.25f : 1.0f);
      const Vec3 tip =
          vec3_add(editor->face_handle_centers[i],
                   vec3_scale(editor->face_handle_normals[i], length));
      const Vec4 color = (int32_t)i == hot ? highlight : arrow;
      physics_line(editor, frame, VKR_ENTITY_ID_INVALID,
                   editor->face_handle_centers[i], tip, color, capacity);
      const float32_t knob = length * 0.12f;
      static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      for (uint32_t axis = 0; axis < 3u; ++axis) {
        const Vec3 half = vec3_scale(axes[axis], knob);
        physics_line(editor, frame, VKR_ENTITY_ID_INVALID, vec3_sub(tip, half),
                     vec3_add(tip, half), color, capacity);
      }
    }
  }
  Vec3 circle[48];
  const uint32_t circle_count = vkr_editor_terrain_brush_outline(
      editor, frame, circle, ArrayCount(circle));
  for (uint32_t i = 0; i < circle_count; ++i) {
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, circle[i],
                 circle[(i + 1u) % circle_count],
                 (Vec4){0.45f, 0.95f, 0.55f, 1.0f}, capacity);
  }
  if (editor->clip_tool && editor->clip_has_first) {
    const Vec4 color = {1.0f, 0.35f, 0.35f, 1.0f};
    const Vec3 a = editor->clip_first;
    const Vec3 b = editor->clip_current;
    const Vec3 up = {0.0f, 4.0f, 0.0f};
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, a, b, color, capacity);
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, vec3_add(a, up),
                 vec3_add(b, up), color, capacity);
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, a, vec3_add(a, up),
                 color, capacity);
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, b, vec3_add(b, up),
                 color, capacity);
  }
  /* The extra selected brushes outline their faces, as the Scene outlines
     the primary selection. */
  for (uint32_t s = 0u; s < editor->selection_extra_count; ++s) {
    const VkrEntityId brush = editor->selection_extra[s];
    const VkrScene *scene = vkr_editor_entity_scene(frame, brush);
    if (!scene || !vkr_scene_get_typed(scene, brush, &vkr_scene_brush_type)) {
      continue;
    }
    static VkrBrushGeometry outline_scratch;
    VkrEntityId faces[VKR_BRUSH_FACE_MAX];
    const uint32_t face_count =
        Min(vkr_scene_brush_faces(scene, brush, faces, VKR_BRUSH_FACE_MAX),
            (uint32_t)VKR_BRUSH_FACE_MAX);
    for (uint32_t f = 0u; f < face_count; ++f) {
      Vec3 corners[32];
      const uint32_t corner_count = vkr_editor_brush_face_outline(
          scene, faces[f], &outline_scratch, corners, ArrayCount(corners));
      for (uint32_t c = 0u; c < corner_count; ++c) {
        physics_line(editor, frame, VKR_ENTITY_ID_INVALID, corners[c],
                     corners[(c + 1u) % corner_count],
                     (Vec4){1.0f, 0.62f, 0.2f, 1.0f}, capacity);
      }
    }
  }
  /* Stairs and corridors between the start and the pointer. */
  if (editor->path_tool != VKR_EDITOR_SCENE_TOOL_NONE &&
      editor->path_has_first) {
    const Vec4 color = {0.45f, 0.75f, 1.0f, 1.0f};
    const Vec3 a = editor->path_first;
    const Vec3 b = {editor->path_current.x, a.y, editor->path_current.z};
    const Vec3 up = {0.0f, 3.0f, 0.0f};
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, a, b, color, capacity);
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, b, vec3_add(b, up),
                 color, capacity);
    physics_line(
        editor, frame, VKR_ENTITY_ID_INVALID,
        editor->path_tool == VKR_EDITOR_SCENE_TOOL_STAIRS ? a : vec3_add(a, up),
        vec3_add(b, up), color, capacity);
  }
  const VkrEditorLevelReport *report = editor->level_report;
  if (!report || !report->checked ||
      !vkr_editor_window_shown(editor, frame, VKR_EDITOR_WINDOW_LEVEL)) {
    return;
  }
  const Vec4 marker = {1.0f, 0.3f, 0.25f, 1.0f};
  const Vec3 offsets[3] = {
      {0.25f, 0.0f, 0.0f}, {0.0f, 0.25f, 0.0f}, {0.0f, 0.0f, 0.25f}};
  for (uint32_t i = 0; i < report->count; ++i) {
    /* A half-meter cross at each issue. */
    const Vec3 at = report->issues[i].position;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      physics_line(editor, frame, VKR_ENTITY_ID_INVALID,
                   vec3_sub(at, offsets[axis]), vec3_add(at, offsets[axis]),
                   marker, capacity);
    }
  }
  /* The checked region's floor rectangle. */
  const Vec4 region = {0.55f, 0.55f, 0.6f, 1.0f};
  const Vec3 lo = report->min;
  const Vec3 hi = report->max;
  const Vec3 corners[4] = {
      vec3_new(lo.x, lo.y, lo.z), vec3_new(hi.x, lo.y, lo.z),
      vec3_new(hi.x, lo.y, hi.z), vec3_new(lo.x, lo.y, hi.z)};
  for (uint32_t i = 0; i < 4u; ++i) {
    physics_line(editor, frame, VKR_ENTITY_ID_INVALID, corners[i],
                 corners[(i + 1u) % 4u], region, capacity);
  }
}

void vkr_editor_physics_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame) {
  editor->physics_line_count = 0;
  editor->physics_lines = NULL;
  editor->physics_lines_truncated = false_v;
  editor->physics_scene_generation = frame->scene_generation;
  if ((!frame->scene && !frame->world) || !frame->mapping_valid ||
      frame->scene_rendering_stopped) {
    return;
  }
  const uint32_t bodies =
      frame->scene ? vkr_scene_physics_body_count(frame->scene) : 0u;
  const uint32_t starts =
      frame->scripts_running ? 0u : physics_player_start_count(frame);
  /* Pending agent changes and a brush being drawn draw in this overlay too
     (ADR-084). */
  const VkrEditorLevelReport *report = editor->level_report;
  const uint32_t changes =
      vkr_editor_ops_change_count(vkr_editor_agent_ops(editor->agent)) +
      (editor->brush_dragging || editor->brush_raising ? 1u : 0u) +
      (editor->clip_tool && editor->clip_has_first ? 1u : 0u) +
      (editor->selection_extra_count ? 1u : 0u) +
      (editor->path_tool != VKR_EDITOR_SCENE_TOOL_NONE && editor->path_has_first
           ? 1u
           : 0u) +
      (editor->terrain_tool && editor->terrain_hit_valid ? 1u : 0u) +
      (editor->face_handle_count ? 1u : 0u) +
      (report && report->checked &&
               vkr_editor_window_shown(editor, frame, VKR_EDITOR_WINDOW_LEVEL)
           ? 1u
           : 0u) +
      (frame->scripts_running ? 0u
                              : physics_io_lines(editor, frame, 0u, false_v)) +
      physics_population_lines(editor, frame, 0u, false_v);
  if (!bodies && !starts && !changes) {
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
  if ((frame->view_state.collision_display || starts || changes) && !capacity) {
    editor->physics_lines_truncated = true_v;
  }
  if ((frame->view_state.collision_display || starts || changes) && capacity) {
    editor->physics_lines = vkr_allocator_alloc(
        ui->frame_allocator, capacity * sizeof(*editor->physics_lines),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (editor->physics_lines && starts) {
    physics_player_starts(editor, frame, capacity);
  }
  if (editor->physics_lines && changes) {
    physics_pending_changes(editor, frame, capacity);
    if (editor->brush_dragging || editor->brush_raising) {
      physics_brush_draft(editor, frame, capacity);
    }
    physics_level_tools(editor, frame, capacity);
    if (!frame->scripts_running) {
      (void)physics_io_lines(editor, frame, capacity, true_v);
    }
    (void)physics_population_lines(editor, frame, capacity, true_v);
  }
  if (frame->view_state.collision_display && capacity && bodies) {
    if (editor->physics_lines) {
      const VkrEntityId selected =
          vkr_scene_physics_owner(frame->scene, frame->selected_entity);
      for (uint32_t i = 0; i < bodies; ++i) {
        const VkrEntityId owner = vkr_scene_physics_body_at(frame->scene, i);
        if (frame->view_state.collision_display == 1 &&
            owner.u64 != selected.u64) {
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
              body.body.enabled && c->enabled &&
              !vkr_scene_physics_is_disabled(frame->scene) &&
              !vkr_scene_physics_body_is_disabled(frame->scene, owner);
          const Vec4 color = !enabled           ? (Vec4){0.5f, 0.5f, 0.5f, 1}
                             : body.body.sensor ? (Vec4){1, 0.55f, 0.95f, 1}
                             : body.body.motion == VKR_PHYSICS_STATIC
                                 ? (Vec4){0.3f, 0.9f, 0.45f, 1}
                             : body.body.motion == VKR_PHYSICS_KINEMATIC
                                 ? (Vec4){0.4f, 0.7f, 1, 1}
                             : pose.active ? (Vec4){1, 0.7f, 0.15f, 1}
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
  /* Simulation controls live on the toolbar and the Show menu; the Scene
     only reports a failing simulation, as UE5 prints viewport warnings. */
  const char *error = bodies ? vkr_scene_physics_error(frame->scene) : NULL;
  if (error && error[0]) {
    const VkrUiTheme *theme = vkr_ui_theme();
    VkrUiWidgetConfig label = physics_widget(
        10, Min(48.0f, Max(0.0f, height - 30)), Max(1.0f, width - 20), 20);
    label.style.padding_pt = (VkrUiEdges){2, 0, 2, 0};
    label.style.text_color = theme->error;
    label.style.font_size_pt = theme->font_caption;
    label.icon = VKR_UI_ICON_WARNING_FILL;
    label.icon_size_pt = 12.0f;
    label.icon_color = theme->error;
    vkr_ui_label(
        ui, string8_lit("status"),
        string8_create_from_cstr((const uint8_t *)error, strlen(error)),
        &label);
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
  if ((!frame->scene && !frame->world) || frame->scene_rendering_stopped ||
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
    const VkrScene *scene =
        line->entity.u64 ? vkr_editor_entity_scene(frame, line->entity) : NULL;
    const SceneTransform *transform =
        scene ? vkr_entity_get_component_if_alive_const(
                    scene->world, line->entity, scene->comp_transform)
              : NULL;
    Vec2 points[4] = {{0}, {0}, {0}, {0}};
    /* A line without an entity lies in world space. */
    if (transform || !line->entity.u64) {
      const Mat4 mvp = transform
                           ? mat4_mul(frame->view_projection, transform->world)
                           : frame->view_projection;
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

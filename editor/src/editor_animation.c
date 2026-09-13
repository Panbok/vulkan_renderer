#include "editor_animation.h"
#include "editor_internal.h"
#include "renderer/systems/vkr_scene_animation.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static VkrEditorAnimationDocument animation_default_document(void) {
  VkrEditorAnimationDocument document = {
      .graph = {.node_count = 1, .parameter_count = 2, .cycle_seconds = 1},
      .output_position = {640, 70},
      .crossfade_seconds = 0.2f};
  for (uint32_t i = 0; i < VKR_EDITOR_ANIMATION_NODES; ++i) {
    document.node_positions[i] =
        (Vec2){20 + (i % 2u) * 220.0f, 8 + (i / 2u) * 42.0f};
  }
  return document;
}

static VkrUiWidgetConfig animation_widget(uint32_t column, uint32_t row) {
  VkrUiWidgetConfig widget = vkr_ui_widget_config_default();
  widget.placement.column = column;
  widget.placement.row = row;
  widget.style.font_size_pt = 12;
  widget.style.padding_pt = (VkrUiEdges){5, 7, 5, 7};
  widget.style.background_color = (Vec4){0.12f, 0.17f, 0.22f, 1};
  return widget;
}

static bool8_t animation_button(VkrUiSystem *ui, const char *id,
                                const char *text, uint32_t column, uint32_t row,
                                bool8_t disabled) {
  VkrUiWidgetConfig widget = animation_widget(column, row);
  widget.disabled = disabled;
  return vkr_ui_button(ui, string8_create((uint8_t *)id, strlen(id)),
                       string8_create((uint8_t *)text, strlen(text)), &widget);
}

static void animation_label(VkrUiSystem *ui, const char *id, const char *text,
                            uint32_t column, uint32_t row) {
  VkrUiWidgetConfig widget = animation_widget(column, row);
  widget.style.background_color = (Vec4){0};
  vkr_ui_label(ui, string8_create((uint8_t *)id, strlen(id)),
               string8_create((uint8_t *)text, strlen(text)), &widget);
}

static void animation_remember(VkrEditorAnimation *animation) {
  if (animation->undo_cursor == VKR_EDITOR_ANIMATION_UNDO) {
    MemCopy(animation->undo, animation->undo + 1,
            sizeof(animation->undo[0]) * (VKR_EDITOR_ANIMATION_UNDO - 1u));
    animation->undo_cursor--;
  }
  animation->undo[animation->undo_cursor++] = animation->document;
  animation->undo_count = animation->undo_cursor;
  animation->graph_applied = false_v;
  animation->graph_dirty = true_v;
  animation->pose_seek = true_v;
  if (!animation->sequence) {
    animation->clip_preview = false_v;
  }
}

static float64_t animation_overlap(const VkrEditorAnimationDocument *document,
                                   const VkrAnimationAsset *asset,
                                   uint32_t left) {
  if (left + 1u >= document->block_count) {
    return 0;
  }
  return Min(document->crossfade_seconds,
             0.5 * Min(asset->clips[document->blocks[left]].duration,
                       asset->clips[document->blocks[left + 1u]].duration));
}

static bool8_t animation_sample_sequence(VkrEditorAnimation *animation,
                                         const VkrAnimationAsset *asset) {
  VkrAnimationSample samples[2] = {0};
  uint32_t count = 0;
  float64_t start = 0;
  for (uint32_t i = 0; i < animation->document.block_count; ++i) {
    const uint32_t clip = animation->document.blocks[i];
    const float64_t duration = asset->clips[clip].duration;
    const float64_t local = animation->time - start;
    const float64_t incoming =
        i ? animation_overlap(&animation->document, asset, i - 1u) : 0;
    const float64_t outgoing =
        animation_overlap(&animation->document, asset, i);
    if (local >= 0 &&
        (local < duration ||
         (i + 1u == animation->document.block_count && local <= duration))) {
      float64_t weight = 1;
      if (incoming > 0 && local < incoming) {
        weight = local / incoming;
      }
      if (outgoing > 0 && local > duration - outgoing) {
        weight = (duration - local) / outgoing;
      }
      if (weight > 0 && count < ArrayCount(samples)) {
        samples[count++] = (VkrAnimationSample){
            .clip = clip, .time = local, .weight = (float32_t)weight};
      }
    }
    start += duration - outgoing;
  }
  if (!count && animation->document.block_count) {
    const uint32_t clip =
        animation->document.blocks[animation->document.block_count - 1u];
    samples[count++] = (VkrAnimationSample){
        .clip = clip, .time = asset->clips[clip].duration, .weight = 1};
  }
  return !count || vkr_animation_player_sample_blend(
                       animation->player, samples, count, animation->pose_seek);
}

static float64_t animation_duration(const VkrEditorAnimation *animation,
                                    const VkrAnimationAsset *asset) {
  if (animation->clip_preview) {
    return asset->clips[animation->selected_clip].duration;
  }
  if (!animation->sequence) {
    if (!animation->graph_dirty && animation->graph_instance.asset == asset &&
        animation->graph_instance.state <
            animation->document.graph.state_count) {
      return animation->document.graph.states[animation->graph_instance.state]
          .cycle_seconds;
    }
    return animation->document.graph.cycle_seconds;
  }
  float64_t duration = 0;
  for (uint32_t i = 0; i < animation->document.block_count; ++i) {
    duration += asset->clips[animation->document.blocks[i]].duration;
    /* Half-duration limits keep overlaps pairwise for deterministic scrubbing.
     */
    duration -= animation_overlap(&animation->document, asset, i);
  }
  return duration;
}

static bool8_t
animation_document_valid(const VkrEditorAnimationDocument *document,
                         uint32_t clip_count) {
  if (!document->graph.node_count ||
      document->graph.node_count > VKR_EDITOR_ANIMATION_NODES ||
      document->block_count > VKR_EDITOR_ANIMATION_BLOCKS ||
      document->graph.root >= document->graph.node_count) {
    return false_v;
  }
  if (!isfinite(document->crossfade_seconds) ||
      document->crossfade_seconds < 0 || document->crossfade_seconds > 5 ||
      !vkr_animation_graph_validate(&document->graph, NULL, NULL)) {
    return false_v;
  }
  for (uint32_t i = 0; i < document->graph.node_count; ++i) {
    const VkrAnimationGraphNode *node = &document->graph.nodes[i];
    if (node->kind == VKR_ANIMATION_GRAPH_CLIP && node->clip >= clip_count) {
      return false_v;
    }
    for (uint32_t j = 0; j < node->sample_count; ++j) {
      if (node->samples[j].clip >= clip_count) {
        return false_v;
      }
    }
  }
  for (uint32_t i = 0; i < document->block_count; ++i) {
    if (document->blocks[i] >= clip_count) {
      return false_v;
    }
  }
  return true_v;
}

static void animation_sample(VkrEditorAnimation *animation) {
  const VkrAnimationAsset *asset =
      vkr_animation_player_asset(animation->player);
  if (!asset) {
    return;
  }
  bool8_t ok = true_v;
  if (animation->clip_preview) {
    if (animation->pose_seek) {
      ok = vkr_animation_player_seek(animation->player, animation->time);
    }
  } else if (animation->sequence) {
    ok = animation_sample_sequence(animation, asset);
  } else {
    if (!animation->graph_dirty && !animation->pose_seek &&
        !animation->graph_parameter_dirty) {
      return;
    }
    if (animation->graph_dirty) {
      ok = vkr_animation_graph_initialize(&animation->graph_instance,
                                          &animation->document.graph, asset,
                                          &animation->error);
      if (ok) {
        animation->graph_dirty = false_v;
      }
    }
    if (ok) {
      ok = animation->pose_seek
               ? vkr_animation_graph_seek(&animation->graph_instance,
                                          animation->player, animation->time)
               : vkr_animation_graph_evaluate(&animation->graph_instance,
                                              animation->player);
    }
  }
  animation->pose_seek = false_v;
  animation->graph_parameter_dirty = false_v;
  if (!ok) {
    animation->playing = false_v;
    if (!animation->error) {
      animation->error = "Invalid graph edit; last valid pose retained.";
    }
  } else {
    animation->error = NULL;
  }
}

void vkr_editor_animation_shutdown(VkrEditorAnimation *animation) {
  vkr_animation_player_destroy(animation->player);
  animation->player = NULL;
  animation->source_player = NULL;
  animation->graph_applied = false_v;
}

void vkr_editor_animation_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  VkrEditorAnimation *animation = &editor->animation;
  const bool8_t visible = editor->windows[VKR_EDITOR_WINDOW_ANIMATION].visible;
  VkrEntityId wrapper = frame->selected_entity;
  VkrAnimationPlayer *source = NULL;
  if (visible && frame->scene) {
    uint32_t ancestors = 0;
    while (vkr_scene_entity_alive(frame->scene, wrapper) &&
           ancestors++ < frame->scene->world->dir.capacity) {
      source = vkr_scene_animation_get_player(frame->scene, wrapper);
      if (source) {
        break;
      }
      const SceneTransform *transform = vkr_entity_get_component(
          frame->scene->world, wrapper, frame->scene->comp_transform);
      if (!transform || transform->parent.u64 == wrapper.u64) {
        break;
      }
      wrapper = transform->parent;
    }
    if (!source &&
        !vkr_scene_entity_alive(frame->scene, frame->selected_entity)) {
      if (animation->scene_generation == frame->scene_generation) {
        wrapper = animation->wrapper;
        source = vkr_scene_animation_get_player(frame->scene, wrapper);
      }
      for (uint32_t i = 0; !source && i < frame->scene->world->dir.capacity;
           ++i) {
        if (!frame->scene->world->dir.records[i].chunk) {
          continue;
        }
        wrapper = vkr_entity_id_from_index(frame->scene->world, i);
        source = vkr_scene_animation_get_player(frame->scene, wrapper);
      }
    }
  }
  if (!source || source != animation->source_player ||
      animation->scene_generation != frame->scene_generation ||
      animation->wrapper.u64 != wrapper.u64) {
    vkr_editor_animation_shutdown(animation);
    animation->error = NULL;
    if (!source) {
      return;
    }
    const VkrAnimationAsset *asset = vkr_animation_player_asset(source);
    if (asset->source_fingerprint != animation->fingerprint ||
        !animation_document_valid(&animation->document, asset->clip_count)) {
      animation->document = animation_default_document();
      const VkrAnimationGraphInstance *controller =
          vkr_scene_animation_get_graph(frame->scene, wrapper);
      if (controller &&
          controller->graph.node_count <= VKR_EDITOR_ANIMATION_NODES) {
        animation->document.graph = controller->graph;
      }
      animation->undo_count = animation->undo_cursor = 0;
      animation->selected_clip = animation->selected_block =
          animation->clip_page = 0;
      animation->selected_node = animation->selected_sample =
          animation->selected_triangle = 0;
      animation->selected_state = animation->selected_transition = 0;
    }
    animation->fingerprint = asset->source_fingerprint;
    animation->source_player = source;
    animation->scene_generation = frame->scene_generation;
    animation->wrapper = wrapper;
    animation->time = 0;
    animation->graph_dirty = animation->pose_seek = true_v;
    animation->rate = 1;
    animation->preview_yaw = 0.4f;
    animation->preview_pitch = 0.15f;
    animation->preview_distance = 3.0f;
    animation->orbit_dragging = false_v;
    animation->loop = true_v;
    animation->playing = false_v;
    animation->clip_preview = true_v;
    animation->sequence = false_v;
    animation->player = vkr_animation_player_create(
        asset, frame->ui->frame_allocator, animation->selected_clip,
        animation->loop, 1, false_v, &animation->error);
    if (!animation->player) {
      return;
    }
  }
  if (animation->player && animation->playing) {
    if (animation->clip_preview) {
      vkr_animation_player_set_playing(animation->player, true_v);
      (void)vkr_animation_player_set_rate(animation->player, animation->rate);
      if (!vkr_animation_player_advance(animation->player,
                                        frame->ui->delta_time)) {
        animation->playing = false_v;
      }
      animation->time = vkr_animation_player_time(animation->player);
    } else if (!animation->sequence && animation->rate >= 0) {
      animation_sample(animation);
      if (!vkr_animation_graph_advance(
              &animation->graph_instance, animation->player,
              frame->ui->delta_time * animation->rate)) {
        animation->playing = false_v;
      }
      animation->time = animation->graph_instance.time;
    } else {
      const float64_t duration = animation_duration(
          animation, vkr_animation_player_asset(animation->player));
      animation->time += frame->ui->delta_time * animation->rate;
      if (animation->loop && duration > 0) {
        animation->time = fmod(animation->time, duration);
        if (animation->time < 0) {
          animation->time += duration;
        }
      } else {
        animation->time = Max(0.0, Min(animation->time, duration));
        if (animation->time == duration || animation->time == 0) {
          animation->playing = false_v;
        }
      }
      animation->pose_seek = true_v;
      animation_sample(animation);
    }
  }
}

static void animation_graph(VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame) {
  VkrEditorAnimation *animation = &editor->animation;
  VkrUiSystem *ui = frame->ui;
  const VkrEditorWindowState *window =
      &editor->windows[VKR_EDITOR_WINDOW_ANIMATION];
  const Vec2 extent = {Max(170.0f, window->size_pt.x - 16), 180};
  /* Body margin8 + window title28; row0=190, rows1..5=27, six5pt gaps. */
  const Vec2 origin = {window->position_pt.x + 8, window->position_pt.y + 391};
  const Vec2 mouse = {ui->mouse_x / ui->content_scale,
                      ui->mouse_y / ui->content_scale};
  const bool8_t down = input_is_button_down(frame->input, BUTTON_LEFT);
  if (ui->mouse_pressed && ui->mouse_input_layer == window->z_order + 1u &&
      !frame->mouse_captured) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(frame->input, BUTTON_LEFT, &press_x,
                                    &press_y);
    const Vec2 press = {press_x / ui->content_scale - origin.x,
                        press_y / ui->content_scale - origin.y};
    for (uint32_t cursor = animation->document.graph.node_count + 1u;
         cursor > 0; --cursor) {
      const uint32_t i = cursor - 1u;
      const bool8_t output = i == animation->document.graph.node_count;
      const Vec2 position = output ? animation->document.output_position
                                   : animation->document.node_positions[i];
      if (press.x >= position.x && press.x < position.x + 170 &&
          press.y >= position.y && press.y < position.y + 20) {
        animation->graph_drag_node =
            output ? VKR_EDITOR_ANIMATION_NODES + 1u : i + 1u;
        animation->graph_drag_origin = position;
        animation->graph_drag_grab =
            (Vec2){press.x - position.x, press.y - position.y};
        animation->graph_drag_modified = false_v;
        break;
      }
    }
  }
  if (animation->graph_drag_node && (down || ui->mouse_released) &&
      !frame->mouse_captured) {
    ui->capture.mouse = true_v;
    Vec2 *position =
        animation->graph_drag_node == VKR_EDITOR_ANIMATION_NODES + 1u
            ? &animation->document.output_position
            : &animation->document
                   .node_positions[animation->graph_drag_node - 1u];
    Vec2 next = {
        vkr_clamp_f32(mouse.x - origin.x - animation->graph_drag_grab.x, 0,
                      extent.x - 170),
        vkr_clamp_f32(mouse.y - origin.y - animation->graph_drag_grab.y, 0,
                      extent.y - 36)};
    if (animation->graph_drag_modified ||
        fabsf(next.x - animation->graph_drag_origin.x) +
                fabsf(next.y - animation->graph_drag_origin.y) >
            3) {
      if (!animation->graph_drag_modified) {
        const bool8_t dirty = animation->graph_dirty;
        const bool8_t seek = animation->pose_seek;
        const bool8_t applied = animation->graph_applied;
        const bool8_t clip_preview = animation->clip_preview;
        animation_remember(animation);
        animation->graph_dirty = dirty;
        animation->pose_seek = seek;
        animation->graph_applied = applied;
        animation->clip_preview = clip_preview;
        animation->graph_drag_modified = true_v;
      }
      *position = next;
    }
  }
  for (uint32_t i = 0; i <= animation->document.graph.node_count; ++i) {
    Vec2 *position = i == animation->document.graph.node_count
                         ? &animation->document.output_position
                         : &animation->document.node_positions[i];
    position->x = vkr_clamp_f32(position->x, 0, extent.x - 170);
    position->y = vkr_clamp_f32(position->y, 0, extent.y - 36);
  }
  VkrUiPanelConfig canvas = vkr_ui_panel_config_default();
  canvas.placement.row = 6;
  canvas.placement.column_span = 4;
  canvas.style.background_color = (Vec4){0.035f, 0.045f, 0.06f, 1};
  canvas.style.padding_pt = (VkrUiEdges){0};
  canvas.clip_children = true_v;
  const VkrUiTrack track = {.value = 1, .unit = VKR_UI_TRACK_FR};
  canvas.columns = &track;
  canvas.rows = &track;
  canvas.column_count = canvas.row_count = 1;
  if (!vkr_ui_panel_begin(ui, string8_lit("graph.canvas"), &canvas)) {
    return;
  }
  for (uint32_t edge = 0; edge <= animation->document.graph.node_count * 2u;
       ++edge) {
    const bool8_t output = edge == animation->document.graph.node_count * 2u;
    const uint32_t target_index = edge / 2u;
    const VkrAnimationGraphNode *node =
        output ? NULL : &animation->document.graph.nodes[target_index];
    if (!output && node->kind != VKR_ANIMATION_GRAPH_BLEND2) {
      continue;
    }
    const uint32_t source_index =
        output ? animation->document.graph.root : node->inputs[edge % 2u];
    if (source_index >= animation->document.graph.node_count) {
      continue;
    }
    const Vec2 source = animation->document.node_positions[source_index];
    const Vec2 target = output
                            ? animation->document.output_position
                            : animation->document.node_positions[target_index];
    const float32_t target_y = target.y + (output ? 28 : 24 + 8 * (edge % 2u));
    const Vec2 points[4] = {{source.x + 160, source.y + 28},
                            {source.x + 220, source.y + 28},
                            {target.x - 60, target_y},
                            {target.x + 10, target_y}};
    VkrUiWidgetConfig wire = animation_widget(0, 0);
    wire.style.background_color = (Vec4){0};
    wire.style.padding_pt = (VkrUiEdges){0};
    wire.style.text_color = (Vec4){0.42f, 0.90f, 0.72f, 1};
    (void)vkr_ui_push_id_u64(ui, edge + 100u);
    vkr_ui_bezier(ui, string8_lit("pose.connection"), points, 2.2f, &wire);
    (void)vkr_ui_pop_id(ui);
  }
  const VkrAnimationAsset *asset =
      vkr_animation_player_asset(animation->player);
  for (uint32_t i = 0; i <= animation->document.graph.node_count; ++i) {
    const bool8_t output = i == animation->document.graph.node_count;
    const uint32_t drag_id = output ? VKR_EDITOR_ANIMATION_NODES + 1u : i + 1u;
    const Vec2 position = output ? animation->document.output_position
                                 : animation->document.node_positions[i];
    (void)vkr_ui_push_id_u64(ui, drag_id);
    VkrUiPanelConfig card = vkr_ui_panel_config_default();
    card.placement.column = 0;
    card.placement.row = 0;
    card.placement.justify = VKR_UI_ALIGN_START;
    card.placement.align = VKR_UI_ALIGN_START;
    card.placement.margin_pt.left = position.x;
    card.placement.margin_pt.top = position.y;
    card.style.min_size_pt = card.style.max_size_pt = (Vec2){170, 36};
    card.style.background_color = output ? (Vec4){0.10f, 0.24f, 0.20f, 1}
                                         : (Vec4){0.10f, 0.15f, 0.22f, 1};
    if (!output && i == animation->selected_node) {
      card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
      card.style.border_color = (Vec4){0.95f, 0.72f, 0.30f, 1};
    }
    card.style.corner_radius_pt = (Vec4){4, 4, 4, 4};
    card.style.padding_pt = (VkrUiEdges){0};
    card.clip_children = true_v;
    const VkrUiTrack columns[] = {
        {.value = output ? 20 : 150, .unit = VKR_UI_TRACK_PX},
        {.value = output ? 150 : 20, .unit = VKR_UI_TRACK_PX}};
    const VkrUiTrack rows[] = {{.value = 20, .unit = VKR_UI_TRACK_PX},
                               {.value = 16, .unit = VKR_UI_TRACK_PX}};
    card.columns = columns;
    card.column_count = 2;
    card.rows = rows;
    card.row_count = 2;
    if (vkr_ui_panel_begin(ui, string8_lit("card"), &card)) {
      char title[128];
      if (output) {
        snprintf(title, sizeof(title), "Output Pose");
      } else {
        const VkrAnimationGraphNode *node = &animation->document.graph.nodes[i];
        const char *names[] = {"Clip", "Blend 2", "Space 1D", "Space 2D"};
        if (node->kind == VKR_ANIMATION_GRAPH_CLIP) {
          snprintf(title, sizeof(title), "%u Clip: %.*s", i,
                   (int)Min(asset->clips[node->clip].name.length, 70u),
                   asset->clips[node->clip].name.str);
        } else {
          snprintf(title, sizeof(title), "%u %s [P%u]", i, names[node->kind],
                   node->parameter_x);
        }
      }
      VkrUiWidgetConfig header = animation_widget(0, 0);
      header.placement.column_span = 2;
      header.style.padding_pt = (VkrUiEdges){2, 5, 2, 5};
      header.style.font_size_pt = 11;
      header.style.background_color = output ? (Vec4){0.13f, 0.34f, 0.25f, 1}
                                             : (Vec4){0.15f, 0.27f, 0.40f, 1};
      header.tooltip =
          string8_lit("Drag to move; select a node, then click Output to "
                      "connect; edit inputs in Properties");
      bool8_t connect = vkr_ui_button(
          ui, string8_lit("header"),
          string8_create((uint8_t *)title, strlen(title)), &header);
      connect &= !animation->graph_drag_modified;
      VkrUiWidgetConfig port = animation_widget(output ? 0 : 1, 1);
      port.placement.justify = port.placement.align = VKR_UI_ALIGN_CENTER;
      port.style.padding_pt = (VkrUiEdges){0};
      port.style.min_size_pt = port.style.max_size_pt = (Vec2){10, 10};
      port.style.corner_radius_pt = (Vec4){5, 5, 5, 5};
      port.style.background_color =
          output || i == animation->document.graph.root
              ? (Vec4){0.42f, 0.90f, 0.72f, 1}
              : (Vec4){0.55f, 0.63f, 0.71f, 1};
      port.tooltip = string8_lit("Pose connection");
      connect |= vkr_ui_button(ui, string8_lit("port"), (String8){0}, &port);
      if (!output && animation->document.graph.nodes[i].kind ==
                         VKR_ANIMATION_GRAPH_BLEND2) {
        for (uint32_t input = 0; input < 2; ++input) {
          VkrUiWidgetConfig input_port = animation_widget(0, 1);
          input_port.placement.align = input_port.placement.justify =
              VKR_UI_ALIGN_START;
          input_port.placement.margin_pt.left = 7;
          input_port.placement.margin_pt.top = input ? 9 : 1;
          input_port.style.min_size_pt = input_port.style.max_size_pt =
              (Vec2){6, 6};
          input_port.style.padding_pt = (VkrUiEdges){0};
          input_port.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
          input_port.style.background_color = (Vec4){0.42f, 0.90f, 0.72f, 1};
          input_port.tooltip =
              input ? string8_lit("Connect selected node to input B")
                    : string8_lit("Connect selected node to input A");
          input_port.disabled = animation->selected_node == i;
          if (vkr_ui_button(
                  ui, input ? string8_lit("input.b") : string8_lit("input.a"),
                  (String8){0}, &input_port)) {
            animation_remember(animation);
            animation->document.graph.nodes[i].inputs[input] =
                animation->selected_node;
          }
        }
      }
      VkrUiWidgetConfig label = animation_widget(output ? 1 : 0, 1);
      label.style.background_color = (Vec4){0};
      label.style.padding_pt = (VkrUiEdges){0, 8, 0, 8};
      label.style.font_size_pt = 10;
      vkr_ui_label(ui, string8_lit("type"),
                   output ? string8_lit("Result") : string8_lit("Pose"),
                   &label);
      if (connect && output) {
        animation_remember(animation);
        animation->document.graph.root = animation->selected_node;
        animation->clip_preview = false_v;
      } else if (connect) {
        animation->selected_node = i;
        animation->selected_sample = animation->selected_triangle = 0;
        if (animation->clip_preview) {
          animation->pose_seek = true_v;
        }
        animation->clip_preview = false_v;
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (!down || frame->mouse_captured) {
    animation->graph_drag_node = 0;
    animation->graph_drag_modified = false_v;
  }
  (void)vkr_ui_panel_end(ui);
}

static bool8_t animation_property_float(VkrEditorAnimation *animation,
                                        VkrUiSystem *ui, const char *id,
                                        float32_t *value, float32_t min,
                                        float32_t max, uint32_t column,
                                        uint32_t row) {
  float32_t next = *value;
  VkrUiWidgetConfig config = animation_widget(column, row);
  if (!vkr_ui_slider_f32(ui, string8_create((uint8_t *)id, strlen(id)), &next,
                         min, max, &config)) {
    return false_v;
  }
  if (!animation->property_edit_active) {
    animation_remember(animation);
  }
  animation->property_edit_active =
      input_is_button_down(ui->input, BUTTON_LEFT);
  animation->graph_dirty = true_v;
  *value = next;
  return true_v;
}

static void animation_property_double(VkrEditorAnimation *animation,
                                      VkrUiSystem *ui, const char *id,
                                      float64_t *value, float32_t min,
                                      float32_t max, uint32_t column,
                                      uint32_t row) {
  float32_t next = (float32_t)*value;
  if (animation_property_float(animation, ui, id, &next, min, max, column,
                               row)) {
    *value = next;
  }
}

static bool8_t animation_index(VkrEditorAnimation *animation, VkrUiSystem *ui,
                               const char *id, const char *label,
                               uint32_t *value, uint32_t count, uint32_t column,
                               uint32_t row) {
  char text[96];
  snprintf(text, sizeof(text), "%s: %u >", label, *value);
  if (!animation_button(ui, id, text, column, row, count < 2)) {
    return false_v;
  }
  animation_remember(animation);
  *value = (*value + 1u) % count;
  return true_v;
}

static void animation_add_node(VkrEditorAnimation *animation,
                               VkrAnimationGraphNodeKind kind) {
  if (animation->document.graph.node_count == VKR_EDITOR_ANIMATION_NODES) {
    return;
  }
  animation_remember(animation);
  const uint32_t index = animation->document.graph.node_count++;
  VkrAnimationGraphNode *node = &animation->document.graph.nodes[index];
  *node = (VkrAnimationGraphNode){
      .kind = kind, .clip = animation->selected_clip, .parameter_y = 1};
  node->inputs[0] = animation->selected_node;
  node->inputs[1] = animation->selected_node;
  if (kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ||
      kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
    node->sample_count = kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ? 2u : 3u;
    node->samples[0] =
        (VkrAnimationGraphSample){.clip = animation->selected_clip};
    node->samples[1] =
        (VkrAnimationGraphSample){.clip = animation->selected_clip, .x = 1};
    node->samples[2] =
        (VkrAnimationGraphSample){.clip = animation->selected_clip, .y = 1};
    if (kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
      node->triangle_count = 1;
      node->triangles[0] = (VkrAnimationGraphTriangle){.samples = {0, 1, 2}};
    }
  }
  animation->selected_node = index;
  animation->selected_sample = animation->selected_triangle = 0;
  animation->clip_preview = false_v;
}

static void animation_properties(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  VkrEditorAnimation *animation = &editor->animation;
  VkrUiSystem *ui = frame->ui;
  VkrAnimationGraph *graph = &animation->document.graph;
  animation->selected_node =
      Min(animation->selected_node, graph->node_count - 1u);
  VkrAnimationGraphNode *node = &graph->nodes[animation->selected_node];
  const VkrUiTrack columns[] = {{.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack rows[] = {{.value = 27, .unit = VKR_UI_TRACK_PX},
                             {.value = 27, .unit = VKR_UI_TRACK_PX},
                             {.value = 27, .unit = VKR_UI_TRACK_PX},
                             {.value = 27, .unit = VKR_UI_TRACK_PX},
                             {.value = 27, .unit = VKR_UI_TRACK_PX},
                             {.value = 27, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 6;
  panel.placement.column_span = 4;
  panel.columns = columns;
  panel.column_count = ArrayCount(columns);
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
  panel.style.gap_pt = 2;
  panel.style.background_color = (Vec4){0.035f, 0.045f, 0.06f, 1};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("properties"), &panel)) {
    return;
  }
  char text[160];
  if (animation->authoring_page <= 3) {
    if (animation_button(ui, "node.previous", "Previous node", 0, 0,
                         !animation->selected_node)) {
      --animation->selected_node;
      animation->selected_sample = animation->selected_triangle = 0;
    }
    snprintf(text, sizeof(text), "Node %u / %u", animation->selected_node,
             graph->node_count);
    animation_label(ui, "node.index", text, 1, 0);
    if (animation_button(ui, "node.next", "Next node", 2, 0,
                         animation->selected_node + 1u >= graph->node_count)) {
      ++animation->selected_node;
      animation->selected_sample = animation->selected_triangle = 0;
    }
    node = &graph->nodes[animation->selected_node];
    if (animation_button(ui, "node.output", "Connect to Output", 3, 0,
                         false_v)) {
      animation_remember(animation);
      graph->root = animation->selected_node;
    }
  }
  if (animation->authoring_page == 1) {
    const char *kinds[] = {"Clip", "Weighted Blend 2", "Blend Space 1D",
                           "Blend Space 2D"};
    animation_label(ui, "node.kind", kinds[node->kind], 0, 1);
    if (animation_button(ui, "node.clip", "Use browser clip", 1, 1,
                         node->kind != VKR_ANIMATION_GRAPH_CLIP)) {
      animation_remember(animation);
      node->clip = animation->selected_clip;
    }
    animation_index(animation, ui, "parameter.x", "X parameter",
                    &node->parameter_x, graph->parameter_count, 2, 1);
    animation_index(animation, ui, "parameter.y", "Y parameter",
                    &node->parameter_y, graph->parameter_count, 3, 1);
    if (node->kind == VKR_ANIMATION_GRAPH_BLEND2) {
      animation_index(animation, ui, "input.a", "Input A node",
                      &node->inputs[0], graph->node_count, 0, 2);
      animation_index(animation, ui, "input.b", "Input B node",
                      &node->inputs[1], graph->node_count, 1, 2);
      animation_label(ui, "blend.rule", "X: 0 = A, 1 = B", 2, 2);
    } else {
      animation_label(ui, "space.help", "Samples: page 3; triangles: page 4", 0,
                      2);
    }
    snprintf(text, sizeof(text), "Cycle %.2fs", graph->cycle_seconds);
    animation_label(ui, "cycle.label", text, 0, 3);
    animation_property_double(animation, ui, "cycle", &graph->cycle_seconds,
                              0.05f, 10, 1, 3);
    snprintf(text, sizeof(text), "Clip / series fade %.2fs",
             animation->document.crossfade_seconds);
    animation_label(ui, "fade.label", text, 2, 3);
    animation_property_float(animation, ui, "fade",
                             &animation->document.crossfade_seconds, 0, 5, 3,
                             3);
    if (animation_button(ui, "node.remove", "Remove last node", 0, 4,
                         graph->node_count <= 1)) {
      animation_remember(animation);
      --graph->node_count;
      if (graph->root >= graph->node_count) {
        graph->root = 0;
      }
      for (uint32_t i = 0; i < graph->node_count; ++i) {
        for (uint32_t input = 0; input < 2; ++input) {
          if (graph->nodes[i].inputs[input] >= graph->node_count) {
            graph->nodes[i].inputs[input] = 0;
          }
        }
      }
      for (uint32_t i = 0; i < graph->state_count; ++i) {
        if (graph->states[i].root >= graph->node_count) {
          graph->states[i].root = 0;
        }
      }
      animation->selected_node =
          Min(animation->selected_node, graph->node_count - 1u);
    }
    animation_label(ui, "cycle.help", "Cycles share normalized phase", 1, 4);
    animation_label(ui, "valid.help", "Cycles in pose wires are rejected", 0,
                    5);
  } else if (animation->authoring_page == 2) {
    const bool8_t space = node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ||
                          node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D;
    if (!space) {
      animation_label(ui, "sample.help", "Select a 1D or 2D node", 0, 1);
    } else {
      animation->selected_sample =
          Min(animation->selected_sample, node->sample_count - 1u);
      if (animation_button(ui, "sample.previous", "Previous sample", 0, 1,
                           !animation->selected_sample)) {
        --animation->selected_sample;
      }
      if (animation_button(ui, "sample.next", "Next sample", 1, 1,
                           animation->selected_sample + 1u >=
                               node->sample_count)) {
        ++animation->selected_sample;
      }
      if (animation_button(ui, "sample.add", "Add browser clip", 2, 1,
                           node->sample_count == VKR_ANIMATION_GRAPH_SAMPLES)) {
        animation_remember(animation);
        animation->selected_sample = node->sample_count;
        node->samples[node->sample_count] =
            (VkrAnimationGraphSample){.clip = animation->selected_clip,
                                      .x = (float32_t)node->sample_count};
        ++node->sample_count;
      }
      if (animation_button(
              ui, "sample.remove", "Remove last sample", 3, 1,
              node->sample_count <=
                  (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE1D ? 2u : 3u))) {
        animation_remember(animation);
        --node->sample_count;
        for (uint32_t i = 0; i < node->triangle_count;) {
          const VkrAnimationGraphTriangle triangle = node->triangles[i];
          if (triangle.samples[0] >= node->sample_count ||
              triangle.samples[1] >= node->sample_count ||
              triangle.samples[2] >= node->sample_count) {
            node->triangles[i] = node->triangles[--node->triangle_count];
          } else {
            ++i;
          }
        }
        animation->selected_sample =
            Min(animation->selected_sample, node->sample_count - 1u);
      }
      VkrAnimationGraphSample *sample =
          &node->samples[animation->selected_sample];
      snprintf(text, sizeof(text), "Sample %u clip %u",
               animation->selected_sample, sample->clip);
      animation_label(ui, "sample.label", text, 0, 2);
      if (animation_button(ui, "sample.clip", "Use browser clip", 1, 2,
                           false_v)) {
        animation_remember(animation);
        sample->clip = animation->selected_clip;
      }
      snprintf(text, sizeof(text), "X %.2f", sample->x);
      animation_label(ui, "sample.x.label", text, 0, 3);
      animation_property_float(animation, ui, "sample.x", &sample->x, -5, 5, 1,
                               3);
      snprintf(text, sizeof(text), "Y %.2f", sample->y);
      animation_label(ui, "sample.y.label", text, 2, 3);
      if (node->kind == VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
        animation_property_float(animation, ui, "sample.y", &sample->y, -5, 5,
                                 3, 3);
      }
      animation_label(ui, "sample.rule", "1D: unique X; 2D: author triangles",
                      0, 4);
    }
  } else if (animation->authoring_page == 3) {
    if (node->kind != VKR_ANIMATION_GRAPH_BLENDSPACE2D) {
      animation_label(ui, "triangle.help", "Select a 2D node", 0, 1);
    } else {
      if (animation_button(ui, "triangle.add", "Add triangle", 0, 1,
                           node->triangle_count ==
                               VKR_ANIMATION_GRAPH_TRIANGLES)) {
        animation_remember(animation);
        animation->selected_triangle = node->triangle_count;
        node->triangles[node->triangle_count++] =
            (VkrAnimationGraphTriangle){.samples = {0, 1, 2}};
      }
      if (animation_button(ui, "triangle.remove", "Remove last triangle", 1, 1,
                           !node->triangle_count)) {
        animation_remember(animation);
        --node->triangle_count;
      }
      if (node->triangle_count) {
        animation->selected_triangle =
            Min(animation->selected_triangle, node->triangle_count - 1u);
        if (animation_button(ui, "triangle.next", "Next triangle", 2, 1,
                             node->triangle_count < 2)) {
          animation->selected_triangle =
              (animation->selected_triangle + 1u) % node->triangle_count;
        }
        snprintf(text, sizeof(text), "Triangle %u",
                 animation->selected_triangle);
        animation_label(ui, "triangle.index", text, 3, 1);
        VkrAnimationGraphTriangle *triangle =
            &node->triangles[animation->selected_triangle];
        animation_index(animation, ui, "triangle.a", "Sample A",
                        &triangle->samples[0], node->sample_count, 0, 2);
        animation_index(animation, ui, "triangle.b", "Sample B",
                        &triangle->samples[1], node->sample_count, 1, 2);
        animation_index(animation, ui, "triangle.c", "Sample C",
                        &triangle->samples[2], node->sample_count, 2, 2);
      }
      animation_label(ui, "triangle.rule",
                      "Nondegenerate triangles; edges clamp", 0, 4);
    }
  } else if (animation->authoring_page == 4) {
    if (animation_button(ui, "state.add", "Add selected node state", 0, 0,
                         graph->state_count == VKR_ANIMATION_GRAPH_STATES)) {
      animation_remember(animation);
      animation->selected_state = graph->state_count;
      graph->states[graph->state_count++] =
          (VkrAnimationGraphState){.root = animation->selected_node,
                                   .cycle_seconds = graph->cycle_seconds};
    }
    if (animation_button(ui, "state.remove", "Remove last state", 1, 0,
                         !graph->state_count)) {
      animation_remember(animation);
      --graph->state_count;
      graph->initial_state = 0;
      for (uint32_t i = 0; i < graph->transition_count;) {
        if (graph->transitions[i].from >= graph->state_count ||
            graph->transitions[i].to >= graph->state_count) {
          graph->transitions[i] = graph->transitions[--graph->transition_count];
        } else {
          ++i;
        }
      }
    }
    if (graph->state_count) {
      animation->selected_state =
          Min(animation->selected_state, graph->state_count - 1u);
      if (animation_button(ui, "state.next", "Next state", 2, 0,
                           graph->state_count < 2)) {
        animation->selected_state =
            (animation->selected_state + 1u) % graph->state_count;
      }
      snprintf(text, sizeof(text), "State %u", animation->selected_state);
      animation_label(ui, "state.index", text, 3, 0);
      VkrAnimationGraphState *state = &graph->states[animation->selected_state];
      animation_index(animation, ui, "state.root", "Root node", &state->root,
                      graph->node_count, 0, 1);
      if (animation_button(ui, "state.initial", "Set initial state", 1, 1,
                           false_v)) {
        animation_remember(animation);
        graph->initial_state = animation->selected_state;
      }
      snprintf(text, sizeof(text), "Cycle %.2fs", state->cycle_seconds);
      animation_label(ui, "state.cycle.label", text, 2, 1);
      animation_property_double(animation, ui, "state.cycle",
                                &state->cycle_seconds, 0.05f, 10, 3, 1);
    }
    snprintf(text, sizeof(text), "Active state %u; initial %u",
             animation->graph_instance.state, graph->initial_state);
    animation_label(ui, "state.active", text, 0, 3);
    if (animation->graph_instance.transitioning) {
      snprintf(text, sizeof(text), "Transition %u: %.2fs",
               animation->graph_instance.transition,
               animation->graph_instance.transition_time);
      animation_label(ui, "state.transition", text, 1, 3);
    }
    animation_label(ui, "state.help", "Transitions page: ordered conditions", 0,
                    4);
  } else if (animation->authoring_page == 5) {
    if (animation_button(ui, "transition.add", "Add transition", 0, 0,
                         graph->state_count < 2 ||
                             graph->transition_count ==
                                 VKR_ANIMATION_GRAPH_TRANSITIONS)) {
      animation_remember(animation);
      animation->selected_transition = graph->transition_count;
      graph->transitions[graph->transition_count++] =
          (VkrAnimationGraphTransition){
              .from = 0,
              .to = 1,
              .threshold = 0.5f,
              .duration = animation->document.crossfade_seconds,
              .exit_time = -1};
    }
    if (animation_button(ui, "transition.remove", "Remove last transition", 1,
                         0, !graph->transition_count)) {
      animation_remember(animation);
      --graph->transition_count;
    }
    if (graph->transition_count) {
      animation->selected_transition =
          Min(animation->selected_transition, graph->transition_count - 1u);
      if (animation_button(ui, "transition.next", "Next transition", 2, 0,
                           graph->transition_count < 2)) {
        animation->selected_transition =
            (animation->selected_transition + 1u) % graph->transition_count;
      }
      snprintf(text, sizeof(text), "Transition %u",
               animation->selected_transition);
      animation_label(ui, "transition.index", text, 3, 0);
      VkrAnimationGraphTransition *transition =
          &graph->transitions[animation->selected_transition];
      animation_index(animation, ui, "transition.from", "From state",
                      &transition->from, graph->state_count, 0, 1);
      animation_index(animation, ui, "transition.to", "To state",
                      &transition->to, graph->state_count, 1, 1);
      animation_index(animation, ui, "transition.parameter", "Parameter",
                      &transition->parameter, graph->parameter_count, 2, 1);
      if (animation_button(ui, "transition.comparison",
                           transition->comparison ==
                                   VKR_ANIMATION_GRAPH_GREATER_EQUAL
                               ? ">= threshold"
                               : "<= threshold",
                           3, 1, false_v)) {
        animation_remember(animation);
        transition->comparison =
            transition->comparison == VKR_ANIMATION_GRAPH_GREATER_EQUAL
                ? VKR_ANIMATION_GRAPH_LESS_EQUAL
                : VKR_ANIMATION_GRAPH_GREATER_EQUAL;
      }
      snprintf(text, sizeof(text), "Threshold %.2f", transition->threshold);
      animation_label(ui, "threshold.label", text, 0, 2);
      animation_property_float(animation, ui, "threshold",
                               &transition->threshold, -5, 5, 1, 2);
      snprintf(text, sizeof(text), "Fade %.2fs", transition->duration);
      animation_label(ui, "transition.fade.label", text, 2, 2);
      animation_property_double(animation, ui, "transition.fade",
                                &transition->duration, 0, 5, 3, 2);
      if (animation_button(ui, "transition.exit.enabled",
                           transition->exit_time < 0 ? "Enable exit gate"
                                                     : "Disable exit gate",
                           0, 3, false_v)) {
        animation_remember(animation);
        transition->exit_time = transition->exit_time < 0 ? 0.8 : -1;
      }
      if (transition->exit_time >= 0) {
        snprintf(text, sizeof(text), "Exit phase %.2f", transition->exit_time);
        animation_label(ui, "exit.label", text, 1, 3);
        animation_property_double(animation, ui, "exit", &transition->exit_time,
                                  0, 1, 2, 3);
      }
      animation_label(ui, "transition.rule", "First matching transition wins",
                      0, 4);
    }
  } else if (animation->authoring_page == 6) {
    for (uint32_t i = 0; i < graph->parameter_count; ++i) {
      char id[32];
      snprintf(id, sizeof(id), "parameter.%u.label", i);
      snprintf(text, sizeof(text), "P%u: %.3f", i, graph->parameters[i]);
      animation_label(ui, id, text, (i % 2u) * 2u, i / 2u);
      snprintf(id, sizeof(id), "parameter.%u", i);
      const bool8_t applied = animation->graph_applied;
      const bool8_t dirty = animation->graph_dirty;
      const bool8_t seek = animation->pose_seek;
      if (animation_property_float(animation, ui, id, &graph->parameters[i], -5,
                                   5, (i % 2u) * 2u + 1u, i / 2u)) {
        animation->graph_applied = applied;
        animation->graph_dirty = dirty;
        animation->pose_seek = seek;
        animation->graph_parameter_dirty = true_v;
        (void)vkr_animation_graph_set_parameter(&animation->graph_instance, i,
                                                graph->parameters[i]);
        if (applied) {
          (void)vkr_scene_animation_set_parameter(
              frame->scene, animation->wrapper, i, graph->parameters[i]);
        }
      }
    }
    if (animation_button(ui, "parameter.add", "Add parameter", 0, 4,
                         graph->parameter_count ==
                             VKR_ANIMATION_GRAPH_PARAMETERS)) {
      animation_remember(animation);
      graph->parameters[graph->parameter_count++] = 0;
    }
    animation_label(ui, "parameter.help",
                    "Live values drive graph and conditions", 1, 4);
    animation_label(ui, "seek.help", "Scrub replays with current parameters", 0,
                    5);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_animation_build(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  VkrEditorAnimation *animation = &editor->animation;
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement.row = 1;
  body.style.padding_pt = (VkrUiEdges){8, 8, 8, 8};
  body.style.gap_pt = 5;
  body.clip_children = true_v;
  const VkrUiTrack columns[] = {{.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR},
                                {.value = 1, .unit = VKR_UI_TRACK_FR}};
  VkrUiTrack rows[9];
  for (uint32_t i = 0; i < ArrayCount(rows); ++i) {
    rows[i] = (VkrUiTrack){.value = 27, .unit = VKR_UI_TRACK_PX};
  }
  rows[0].value = 190;
  rows[6].value = 180;
  body.columns = columns;
  body.column_count = ArrayCount(columns);
  body.rows = rows;
  body.row_count = ArrayCount(rows);
  if (!vkr_ui_panel_begin(ui, string8_lit("animation.body"), &body)) {
    return;
  }
  const bool8_t keyboard_scope =
      !ui->focused_is_text &&
      ui->keyboard_input_layer ==
          editor->windows[VKR_EDITOR_WINDOW_ANIMATION].z_order + 1u;
  if (keyboard_scope && frame->scene_shortcuts_blocked) {
    *frame->scene_shortcuts_blocked = true_v;
  }
  if (!animation->player) {
    VkrUiWidgetConfig empty = animation_widget(0, 0);
    empty.placement.column_span = 4;
    const char *message =
        animation->error
            ? animation->error
            : "Select a character with an animation bank in the Hierarchy.";
    vkr_ui_label(ui, string8_lit("empty"),
                 string8_create((uint8_t *)message, strlen(message)), &empty);
    (void)vkr_ui_panel_end(ui);
    return;
  }
  const bool8_t undo_key = keyboard_scope &&
                           input_key_shortcut_modifier(frame->input, KEY_Z) &&
                           input_key_just_pressed(frame->input, KEY_Z);
  const bool8_t redo_key =
      undo_key &&
      (input_key_press_modifiers(frame->input, KEY_Z) & VKR_INPUT_MOD_SHIFT);
  const VkrAnimationAsset *asset =
      vkr_animation_player_asset(animation->player);
  const float64_t duration = animation_duration(animation, asset);
  VkrUiPanelConfig preview = vkr_ui_panel_config_default();
  preview.placement.column_span = 2;
  preview.style.background_color = (Vec4){0.025f, 0.035f, 0.045f, 1};
  preview.style.gap_pt = 3;
  preview.clip_children = true_v;
  const VkrUiTrack preview_rows[] = {{.value = 1, .unit = VKR_UI_TRACK_FR},
                                     {.value = 27, .unit = VKR_UI_TRACK_PX}};
  preview.rows = preview_rows;
  preview.row_count = ArrayCount(preview_rows);
  preview.columns = columns;
  preview.column_count = ArrayCount(columns);
  if (vkr_ui_panel_begin(ui, string8_lit("preview"), &preview)) {
    VkrUiWidgetConfig image = animation_widget(0, 0);
    image.placement.column_span = 4;
    image.style.padding_pt = (VkrUiEdges){0};
    image.tooltip = string8_lit(
        "Drag to orbit; scroll to zoom. Preview has its own camera and clock.");
    if (frame->animation_preview) {
      VkrUiTextureRef preview_texture = VKR_UI_TEXTURE_REF_ANIMATION_PREVIEW;
      /* The native target changes without a resource-handle replacement. */
      preview_texture.generation = (uint32_t)ui->frame_index;
      vkr_ui_image(ui, string8_lit("model"), preview_texture, (Vec2){512, 512},
                   &image);
    }
    if (animation_button(ui, "orbit.left", "Orbit left", 0, 1, false_v)) {
      animation->preview_yaw -= 0.2f;
    }
    if (animation_button(ui, "orbit.right", "Orbit right", 1, 1, false_v)) {
      animation->preview_yaw += 0.2f;
    }
    VkrUiWidgetConfig zoom = animation_widget(2, 1);
    zoom.tooltip = string8_lit("Preview camera distance");
    (void)vkr_ui_slider_f32(ui, string8_lit("zoom"),
                            &animation->preview_distance, 1.2f, 8, &zoom);
    if (animation_button(ui, "camera.reset", "Reset camera", 3, 1, false_v)) {
      animation->preview_yaw = 0.4f;
      animation->preview_pitch = 0.15f;
      animation->preview_distance = 3;
    }
    (void)vkr_ui_panel_end(ui);
  }
  const VkrEditorWindowState *window =
      &editor->windows[VKR_EDITOR_WINDOW_ANIMATION];
  const float32_t mouse_x = ui->mouse_x / ui->content_scale;
  const float32_t mouse_y = ui->mouse_y / ui->content_scale;
  const bool8_t over_preview =
      ui->mouse_input_layer == window->z_order + 1u &&
      mouse_x >= window->position_pt.x + 8 &&
      mouse_x < window->position_pt.x + (window->size_pt.x - 5) * 0.5f &&
      mouse_y >= window->position_pt.y + 36 &&
      mouse_y < window->position_pt.y + 198;
  if (ui->mouse_pressed && over_preview && !frame->mouse_captured) {
    animation->orbit_dragging = true_v;
  }
  if (!input_is_button_down(frame->input, BUTTON_LEFT) ||
      frame->mouse_captured) {
    animation->orbit_dragging = false_v;
  }
  if (animation->orbit_dragging) {
    int32_t dx = 0;
    int32_t dy = 0;
    input_get_mouse_delta(frame->input, &dx, &dy);
    animation->preview_yaw += dx * 0.01f;
    animation->preview_pitch =
        vkr_clamp_f32(animation->preview_pitch + dy * 0.01f, -1.3f, 1.3f);
    ui->capture.mouse = true_v;
  }
  if (over_preview && ui->mouse_wheel) {
    animation->preview_distance = vkr_clamp_f32(
        animation->preview_distance - ui->mouse_wheel * 0.2f, 1.2f, 8);
    ui->capture.mouse = true_v;
  }
  if (animation_button(ui, "play", animation->playing ? "Pause" : "Play", 0, 1,
                       animation->sequence &&
                           !animation->document.block_count)) {
    animation->playing = !animation->playing;
  }
  if (animation_button(ui, "rewind", "Start", 1, 1, false_v)) {
    animation->time = 0;
    animation->pose_seek = true_v;
  }
  if (animation_button(ui, "back", "- 1/60 s", 2, 1, false_v)) {
    animation->playing = false_v;
    animation->time = Max(0.0, animation->time - 1.0 / 60.0);
    animation->pose_seek = true_v;
  }
  if (animation_button(ui, "forward", "+ 1/60 s", 3, 1, false_v)) {
    animation->playing = false_v;
    animation->time = animation->clip_preview || animation->sequence
                          ? Min(duration, animation->time + 1.0 / 60.0)
                          : animation->time + 1.0 / 60.0;
    animation->pose_seek = true_v;
  }
  VkrUiWidgetConfig scrub = animation_widget(0, 2);
  scrub.placement.column_span = 4;
  const bool8_t graph_mode = !animation->clip_preview && !animation->sequence;
  float64_t graph_clock = animation->time;
  if (graph_mode && !animation->pose_seek && !animation->graph_dirty &&
      animation->graph_instance.state < animation->document.graph.state_count) {
    graph_clock = animation->graph_instance.state_time;
  }
  const float64_t graph_phase = duration > 0 ? fmod(graph_clock, duration) : 0;
  float32_t time = (float32_t)(graph_mode ? graph_phase : animation->time);
  if (vkr_ui_slider_f32(ui, string8_lit("scrub"), &time, 0, (float32_t)duration,
                        &scrub)) {
    animation->playing = false_v;
    animation->time = time;
    animation->pose_seek = true_v;
  }
  char status[160];
  snprintf(status, sizeof(status), "%s %.3f / %.3fs",
           animation->clip_preview ? "Clip"
           : animation->sequence   ? "Series"
                                   : "Graph",
           animation->time, duration);
  if (graph_mode) {
    snprintf(status, sizeof(status), "%.1fs | phase %.2f / %.2fs%s",
             animation->time, graph_phase, duration,
             animation->graph_instance.transitioning ? " fade" : "");
    if (animation->document.graph.state_count) {
      snprintf(status, sizeof(status), "S%u %.1fs | phase %.2f/%.2f%s",
               animation->graph_instance.state, animation->time, graph_phase,
               duration,
               animation->graph_instance.transitioning ? " fade" : "");
    }
  } else if (animation->clip_preview &&
             vkr_animation_player_crossfade_active(animation->player)) {
    snprintf(status, sizeof(status), "Clip fade %.0f%% %.2fs",
             100 * vkr_animation_player_crossfade_progress(animation->player),
             animation->time);
  }
  animation_label(ui, "time", status, 0, 3);
  VkrUiWidgetConfig loop = animation_widget(1, 3);
  loop.disabled = !animation->clip_preview && !animation->sequence;
  if (vkr_ui_checkbox(ui, string8_lit("loop"),
                      loop.disabled ? string8_lit("Graph cycles")
                                    : string8_lit("Loop"),
                      &animation->loop, &loop) &&
      animation->clip_preview) {
    if (vkr_animation_player_select_clip(
            animation->player, animation->selected_clip, animation->loop)) {
      animation->pose_seek = true_v;
    }
  }
  VkrUiWidgetConfig rate = animation_widget(2, 3);
  (void)vkr_ui_slider_f32(ui, string8_lit("speed"), &animation->rate, -2, 2,
                          &rate);
  snprintf(status, sizeof(status), "Speed %.2fx", animation->rate);
  animation_label(ui, "rate", status, 3, 3);
  if (animation_button(ui, "graph",
                       animation->sequence || animation->clip_preview
                           ? "Graph"
                           : "Graph [active]",
                       0, 4, false_v)) {
    animation->sequence = false_v;
    animation->clip_preview = false_v;
    animation->time = 0;
    animation->pose_seek = true_v;
  }
  if (animation_button(ui, "sequence",
                       animation->sequence ? "Sequence [active]" : "Sequence",
                       1, 4, false_v)) {
    animation->sequence = true_v;
    animation->clip_preview = false_v;
    animation->time = 0;
    animation->pose_seek = true_v;
  }
  if (animation_button(ui, "undo", "Undo edit", 2, 4,
                       !animation->undo_cursor) ||
      (undo_key && !redo_key && animation->undo_cursor)) {
    animation->graph_drag_node = 0;
    animation->graph_applied = false_v;
    VkrEditorAnimationDocument swap = animation->document;
    animation->document = animation->undo[--animation->undo_cursor];
    animation->undo[animation->undo_cursor] = swap;
    animation->time = 0;
    animation->graph_dirty = animation->pose_seek = true_v;
  }
  if (animation_button(ui, "redo", "Redo edit", 3, 4,
                       animation->undo_cursor == animation->undo_count) ||
      (redo_key && animation->undo_cursor < animation->undo_count)) {
    animation->graph_drag_node = 0;
    animation->graph_applied = false_v;
    VkrEditorAnimationDocument swap = animation->document;
    animation->document = animation->undo[animation->undo_cursor];
    animation->undo[animation->undo_cursor++] = swap;
    animation->time = 0;
    animation->graph_dirty = animation->pose_seek = true_v;
  }
  VkrUiPanelConfig browser = vkr_ui_panel_config_default();
  browser.placement.column = 2;
  browser.placement.column_span = 2;
  browser.placement.row = 0;
  browser.columns = columns;
  browser.column_count = 2;
  const VkrUiTrack browser_rows[] = {{.value = 27, .unit = VKR_UI_TRACK_PX},
                                     {.value = 1, .unit = VKR_UI_TRACK_FR},
                                     {.value = 1, .unit = VKR_UI_TRACK_FR},
                                     {.value = 1, .unit = VKR_UI_TRACK_FR},
                                     {.value = 1, .unit = VKR_UI_TRACK_FR}};
  browser.rows = browser_rows;
  browser.row_count = ArrayCount(browser_rows);
  browser.style.gap_pt = 4;
  browser.clip_children = true_v;
  if (vkr_ui_panel_begin(ui, string8_lit("clip.browser"), &browser)) {
    const uint32_t page_count = (asset->clip_count + 7u) / 8u;
    if (animation_button(ui, "clips.previous", "Previous clips", 0, 0,
                         !animation->clip_page)) {
      --animation->clip_page;
    }
    if (animation_button(ui, "clips.next", "Next clips", 1, 0,
                         animation->clip_page + 1u >= page_count)) {
      ++animation->clip_page;
    }
    for (uint32_t slot = 0; slot < 8; ++slot) {
      const uint32_t clip = animation->clip_page * 8u + slot;
      if (clip >= asset->clip_count) {
        break;
      }
      (void)vkr_ui_push_id_u64(ui, clip);
      VkrUiWidgetConfig item = animation_widget(slot % 2u, 1u + slot / 2u);
      item.style.background_color = clip == animation->selected_clip
                                        ? (Vec4){0.16f, 0.36f, 0.46f, 1}
                                        : (Vec4){0.12f, 0.17f, 0.22f, 1};
      snprintf(status, sizeof(status), "%u  %.*s", clip,
               (int)Min(asset->clips[clip].name.length, 100u),
               asset->clips[clip].name.str);
      if (vkr_ui_button(ui, string8_lit("clip"),
                        string8_create((uint8_t *)status, strlen(status)),
                        &item)) {
        animation->selected_clip = clip;
        if (!animation->sequence) {
          animation->clip_preview = true_v;
          animation->time = 0;
          animation->pose_seek = false_v;
          if (!vkr_animation_player_crossfade(
                  animation->player, clip, animation->loop,
                  animation->document.crossfade_seconds)) {
            animation->error = "Clip crossfade failed.";
          }
          vkr_animation_player_set_playing(animation->player,
                                           animation->playing);
        }
      }
      (void)vkr_ui_pop_id(ui);
    }
    (void)vkr_ui_panel_end(ui);
  }
  animation->selected_node =
      Min(animation->selected_node, animation->document.graph.node_count - 1u);
  if (!animation->sequence) {
    static const char *pages[] = {
        "1 Canvas", "2 Node properties", "3 Space samples", "4 Space triangles",
        "5 States", "6 Transitions",     "7 Parameters"};
    if (animation_button(ui, "page.previous", "Previous page", 0, 5,
                         !animation->authoring_page)) {
      --animation->authoring_page;
    }
    animation_label(ui, "page.name", pages[animation->authoring_page], 1, 5);
    if (animation_button(ui, "page.next", "Next page", 2, 5,
                         animation->authoring_page + 1u == ArrayCount(pages))) {
      ++animation->authoring_page;
    }
    if (animation_button(ui, "apply.scene", "Apply to scene", 3, 5,
                         !frame->scene)) {
      if (!vkr_scene_animation_apply_graph(frame->scene, animation->wrapper,
                                           &animation->document.graph,
                                           &animation->error)) {
        animation->playing = false_v;
      } else {
        animation->graph_applied = true_v;
      }
    }
    if (!animation->authoring_page) {
      animation_graph(editor, frame);
    } else {
      animation_properties(editor, frame);
    }
    const char *add_ids[] = {"add.clip", "add.blend", "add.space1",
                             "add.space2"};
    const char *add_labels[] = {"Add Clip", "Add Blend 2", "Add 1D Space",
                                "Add 2D Space"};
    for (uint32_t i = 0; i < 4; ++i) {
      if (animation_button(ui, add_ids[i], add_labels[i], i, 7,
                           animation->document.graph.node_count ==
                               VKR_EDITOR_ANIMATION_NODES)) {
        animation_add_node(animation, (VkrAnimationGraphNodeKind)i);
      }
    }
  } else {
    snprintf(status, sizeof(status), "Overlap fade %.2fs",
             animation->document.crossfade_seconds);
    animation_label(ui, "sequence.fade.label", status, 2, 5);
    animation_property_float(animation, ui, "sequence.fade",
                             &animation->document.crossfade_seconds, 0, 5, 3,
                             5);
    if (animation_button(ui, "append", "Append selected clip", 0, 5,
                         animation->document.block_count ==
                             VKR_EDITOR_ANIMATION_BLOCKS)) {
      animation_remember(animation);
      animation->document.blocks[animation->document.block_count++] =
          animation->selected_clip;
    }
    if (animation_button(ui, "remove", "Remove block", 1, 5,
                         !animation->document.block_count)) {
      animation_remember(animation);
      for (uint32_t i = animation->selected_block + 1u;
           i < animation->document.block_count; ++i) {
        animation->document.blocks[i - 1u] = animation->document.blocks[i];
      }
      --animation->document.block_count;
      animation->selected_block = 0;
      animation->time = 0;
    }
    for (uint32_t direction = 0; direction < 2; ++direction) {
      const bool8_t disabled = !animation->document.block_count ||
                               (direction ? animation->selected_block + 1u >=
                                                animation->document.block_count
                                          : animation->selected_block == 0);
      if (animation_button(ui, direction ? "later" : "earlier",
                           direction ? "Move later" : "Move earlier",
                           2u + direction, 7, disabled)) {
        animation_remember(animation);
        const uint32_t other = direction ? animation->selected_block + 1u
                                         : animation->selected_block - 1u;
        uint32_t clip = animation->document.blocks[other];
        animation->document.blocks[other] =
            animation->document.blocks[animation->selected_block];
        animation->document.blocks[animation->selected_block] = clip;
        animation->selected_block = other;
        animation->time = 0;
      }
    }
    VkrUiPanelConfig timeline = vkr_ui_panel_config_default();
    timeline.placement.row = 6;
    timeline.placement.column_span = 4;
    timeline.placement.row_span = 1;
    timeline.style.gap_pt = 2;
    timeline.style.padding_pt = (VkrUiEdges){8, 2, 8, 2};
    timeline.style.background_color = (Vec4){0.035f, 0.055f, 0.075f, 1};
    timeline.clip_children = true_v;
    const VkrUiTrack timeline_track = {.value = 1, .unit = VKR_UI_TRACK_FR};
    timeline.columns = &timeline_track;
    timeline.column_count = 1;
    const float64_t timeline_duration = animation_duration(animation, asset);
    const float32_t timeline_width =
        Max(1.0f, editor->windows[VKR_EDITOR_WINDOW_ANIMATION].size_pt.x - 20);
    if (vkr_ui_panel_begin(ui, string8_lit("timeline"), &timeline)) {
      float64_t start = 0;
      for (uint32_t i = 0; i < animation->document.block_count; ++i) {
        const uint32_t clip = animation->document.blocks[i];
        const float64_t end = start + asset->clips[clip].duration;
        (void)vkr_ui_push_id_u64(ui, i);
        const float64_t fade =
            animation_overlap(&animation->document, asset, i);
        snprintf(status, sizeof(status), "%u\n%.2f - %.2fs\nFade %.2fs", clip,
                 start, end, fade);
        VkrUiWidgetConfig block = animation_widget(0, 0);
        block.placement.justify = VKR_UI_ALIGN_START;
        block.placement.align = VKR_UI_ALIGN_START;
        block.placement.margin_pt.left =
            timeline_duration > 0
                ? (float32_t)(start / timeline_duration) * timeline_width
                : 0;
        block.placement.margin_pt.top = (i % 2u) * 18.0f;
        block.style.min_size_pt = block.style.max_size_pt = (Vec2){
            Max(2.0f, timeline_duration > 0
                          ? (float32_t)((end - start) / timeline_duration) *
                                timeline_width
                          : timeline_width),
            130};
        block.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
        block.style.border_color = (Vec4){0.42f, 0.90f, 0.72f, 1};
        block.style.background_color = i == animation->selected_block
                                           ? (Vec4){0.20f, 0.42f, 0.50f, 1}
                                           : (Vec4){0.14f, 0.24f, 0.34f, 1};
        block.tooltip = asset->clips[clip].name;
        if (vkr_ui_button(ui, string8_lit("block"),
                          string8_create((uint8_t *)status, strlen(status)),
                          &block)) {
          animation->selected_block = i;
          animation->time = start;
          animation->pose_seek = true_v;
          animation->playing = false_v;
        }
        (void)vkr_ui_pop_id(ui);
        start = end - animation_overlap(&animation->document, asset, i);
      }
      (void)vkr_ui_panel_end(ui);
    }
    if (animation->document.block_count) {
      uint32_t selected =
          Min(animation->selected_block, animation->document.block_count - 1u);
      const VkrAnimationClip *clip =
          &asset->clips[animation->document.blocks[selected]];
      snprintf(status, sizeof(status), "Block %u: %.*s (%.3fs)", selected + 1u,
               (int)Min(clip->name.length, 90u), clip->name.str,
               clip->duration);
      VkrUiWidgetConfig detail = animation_widget(0, 7);
      detail.placement.column_span = 2;
      vkr_ui_label(ui, string8_lit("selected.block"),
                   string8_create((uint8_t *)status, strlen(status)), &detail);
    }
  }
  if (ui->keyboard_input_layer ==
          editor->windows[VKR_EDITOR_WINDOW_ANIMATION].z_order + 1u &&
      !ui->focused_is_text && input_key_just_pressed(frame->input, KEY_SPACE)) {
    animation->playing = !animation->playing;
    ui->capture.keyboard = true_v;
  }
  animation->selected_block =
      animation->document.block_count
          ? Min(animation->selected_block, animation->document.block_count - 1u)
          : 0;
  if (animation->clip_preview || animation->sequence) {
    animation->time =
        Min(animation->time, animation_duration(animation, asset));
  }
  if (!input_is_button_down(frame->input, BUTTON_LEFT)) {
    animation->property_edit_active = false_v;
  }
  /* UI edits can change the active clip without changing time. */
  animation_sample(animation);
  if (frame->animation_preview) {
    *frame->animation_preview = (VkrAnimationPreviewRequest){
        .wrapper = animation->wrapper,
        .player = animation->player,
        .scene_generation = frame->scene_generation,
        .yaw = animation->preview_yaw,
        .pitch = animation->preview_pitch,
        .distance = animation->preview_distance};
  }
  if (animation->error) {
    VkrUiWidgetConfig error = animation_widget(0, 8);
    error.placement.column_span = 4;
    error.style.text_color = (Vec4){1, 0.6f, 0.4f, 1};
    vkr_ui_label(
        ui, string8_lit("error"),
        string8_create((uint8_t *)animation->error, strlen(animation->error)),
        &error);
  }
  (void)vkr_ui_panel_end(ui);
}

bool8_t vkr_editor_animation_write_settings(const VkrEditorAnimation *animation,
                                            VkrJsonWriter *writer) {
  char fingerprint[17];
  snprintf(fingerprint, sizeof(fingerprint), "%016llx",
           (unsigned long long)animation->fingerprint);
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_json_writer_name(writer, string8_lit("fingerprint")) &&
      vkr_json_writer_string(writer, string8_create((uint8_t *)fingerprint,
                                                    strlen(fingerprint))) &&
      vkr_json_writer_name(writer, string8_lit("graph")) &&
      vkr_animation_graph_write_json(writer, &animation->document.graph) &&
      vkr_json_writer_name(writer, string8_lit("crossfade")) &&
      vkr_json_writer_f64(writer, animation->document.crossfade_seconds) &&
      vkr_json_writer_name(writer, string8_lit("blocks")) &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < animation->document.block_count; ++i) {
    ok = vkr_json_writer_u64(writer, animation->document.blocks[i]);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_json_writer_name(writer, string8_lit("positions")) &&
       vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i <= animation->document.graph.node_count; ++i) {
    Vec2 position = i == animation->document.graph.node_count
                        ? animation->document.output_position
                        : animation->document.node_positions[i];
    ok = vkr_json_writer_f64(writer, position.x) &&
         vkr_json_writer_f64(writer, position.y);
  }
  return ok && vkr_json_writer_end_array(writer) &&
         vkr_json_writer_end_object(writer);
}

void vkr_editor_animation_read_settings(VkrEditorAnimation *animation,
                                        String8 json) {
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  String8 fingerprint = {0};
  int32_t output = 0;
  if (!vkr_json_get_string(&reader, "fingerprint", &fingerprint) ||
      fingerprint.length != 16) {
    return;
  }
  uint64_t key = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t c = fingerprint.str[i];
    const uint32_t digit = c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10u
                                                  : 16u;
    if (digit == 16) {
      return;
    }
    key = (key << 4u) | digit;
  }
  VkrEditorAnimationDocument document = animation_default_document();
  reader.pos = 0;
  if (vkr_json_find_field(&reader, "graph")) {
    if (!vkr_animation_graph_read_json(&reader, NULL, &document.graph, NULL) ||
        document.graph.node_count > VKR_EDITOR_ANIMATION_NODES) {
      return;
    }
    reader.pos = 0;
    if (!vkr_json_get_float(&reader, "crossfade",
                            &document.crossfade_seconds)) {
      return;
    }
  } else {
    /* Migrate the earlier clip-only workspace representation once at
     * load. */
    reader.pos = 0;
    if (!vkr_json_get_int(&reader, "output", &output) || output < 0) {
      return;
    }
    document.graph.root = (uint32_t)output;
    document.graph.node_count = 0;
    reader.pos = 0;
    if (!vkr_json_find_array(&reader, "nodes")) {
      return;
    }
    while (vkr_json_next_array_element(&reader)) {
      int32_t clip = 0;
      if (document.graph.node_count == VKR_EDITOR_ANIMATION_NODES ||
          !vkr_json_parse_int(&reader, &clip) || clip < 0) {
        return;
      }
      document.graph.nodes[document.graph.node_count++] =
          (VkrAnimationGraphNode){.kind = VKR_ANIMATION_GRAPH_CLIP,
                                  .clip = (uint32_t)clip};
    }
  }
  reader.pos = 0;
  if (!vkr_json_find_array(&reader, "blocks")) {
    return;
  }
  while (vkr_json_next_array_element(&reader)) {
    int32_t clip = 0;
    if (document.block_count == VKR_EDITOR_ANIMATION_BLOCKS ||
        !vkr_json_parse_int(&reader, &clip) || clip < 0) {
      return;
    }
    document.blocks[document.block_count++] = (uint32_t)clip;
  }
  reader = vkr_json_reader_from_string(json);
  if (vkr_json_find_array(&reader, "positions")) {
    uint32_t count = 0;
    while (vkr_json_next_array_element(&reader)) {
      float64_t value = 0;
      if (count >= (document.graph.node_count + 1u) * 2u ||
          !vkr_json_parse_double(&reader, &value) || !isfinite(value) ||
          value < 0 || value > 10000) {
        return;
      }
      Vec2 *position = count / 2u == document.graph.node_count
                           ? &document.output_position
                           : &document.node_positions[count / 2u];
      if (count % 2u) {
        position->y = (float32_t)value;
      } else {
        position->x = (float32_t)value;
      }
      ++count;
    }
    if (count != (document.graph.node_count + 1u) * 2u) {
      return;
    }
  }
  if (animation_document_valid(&document, UINT32_MAX)) {
    vkr_editor_animation_shutdown(animation);
    animation->document = document;
    animation->fingerprint = key;
    animation->graph_dirty = animation->pose_seek = true_v;
    animation->undo_count = animation->undo_cursor = 0;
  }
}

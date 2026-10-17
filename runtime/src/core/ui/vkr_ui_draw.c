#include "core/ui/vkr_ui_draw.h"

static bool8_t vkr_ui_draw_vec4_finite(Vec4 value) {
  return vkr_is_finite_f32(value.x) && vkr_is_finite_f32(value.y) &&
         vkr_is_finite_f32(value.z) && vkr_is_finite_f32(value.w);
}

static bool8_t vkr_ui_draw_texture_equal(VkrUiTextureRef a, VkrUiTextureRef b) {
  return a.id == b.id && a.generation == b.generation;
}

bool8_t vkr_ui_draw_buffer_begin(VkrUiDrawBuffer *buffer,
                                 VkrUiDrawCommand *commands,
                                 uint32_t command_capacity,
                                 VkrUiRect target_rect_px) {
  if (!buffer || !commands || command_capacity == 0u ||
      !vkr_ui_rect_has_area(target_rect_px))
    return false_v;
  *buffer = (VkrUiDrawBuffer){
      .commands = commands,
      .command_capacity = command_capacity,
      .clip_stack = {target_rect_px},
      .clip_count = 1u,
  };
  return true_v;
}

bool8_t vkr_ui_draw_buffer_push_clip(VkrUiDrawBuffer *buffer,
                                     VkrUiRect clip_rect_px) {
  if (!buffer || buffer->clip_count == 0u ||
      buffer->clip_count >= ArrayCount(buffer->clip_stack) ||
      !vkr_ui_rect_is_finite(clip_rect_px))
    return false_v;
  buffer->clip_stack[buffer->clip_count] = vkr_ui_rect_intersect(
      buffer->clip_stack[buffer->clip_count - 1u], clip_rect_px);
  buffer->clip_count++;
  return true_v;
}

bool8_t vkr_ui_draw_buffer_pop_clip(VkrUiDrawBuffer *buffer) {
  if (!buffer || buffer->clip_count <= 1u ||
      buffer->clip_count > ArrayCount(buffer->clip_stack))
    return false_v;
  buffer->clip_count--;
  return true_v;
}

bool8_t vkr_ui_draw_buffer_push(VkrUiDrawBuffer *buffer,
                                VkrUiDrawCommand command) {
  if (!buffer || buffer->clip_count == 0u ||
      buffer->clip_count > ArrayCount(buffer->clip_stack))
    return false_v;
  if (buffer->command_count == buffer->command_capacity) {
    buffer->dropped_command_count++;
    return false_v;
  }
  command.clip_rect_px = buffer->clip_stack[buffer->clip_count - 1u];
  buffer->commands[buffer->command_count++] = command;
  return true_v;
}

bool8_t vkr_ui_draw_buffer_solid(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 color) {
  return vkr_ui_draw_buffer_push(buffer,
                                 (VkrUiDrawCommand){
                                     .rect_px = rect_px,
                                     .uv_rect = {0.0f, 0.0f, 1.0f, 1.0f},
                                     .color = color,
                                     .mode = VKR_UI_DRAW_MODE_QUAD,
                                 });
}

bool8_t vkr_ui_draw_buffer_gradient(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                    Vec4 start, Vec4 end, bool8_t vertical) {
  return vkr_ui_draw_buffer_push(
      buffer, (VkrUiDrawCommand){
                  .rect_px = rect_px,
                  .uv_rect = {0.0f, 0.0f, 1.0f, 1.0f},
                  .color = start,
                  .border_color = end,
                  .mode = VKR_UI_DRAW_MODE_QUAD,
                  .gradient = vertical ? VKR_UI_DRAW_GRADIENT_VERTICAL
                                       : VKR_UI_DRAW_GRADIENT_HORIZONTAL,
              });
}

bool8_t vkr_ui_draw_buffer_polygon(VkrUiDrawBuffer *buffer,
                                   const Vec2 corners_px[4], Vec4 color) {
  VkrUiDrawCommand command = {.color = color, .mode = VKR_UI_DRAW_MODE_QUAD};
  const uint32_t count =
      corners_px[2].x == corners_px[3].x && corners_px[2].y == corners_px[3].y
          ? 3u
          : 4u;
  Vec2 normals[4];
  float32_t area = 0.0f;
  for (uint32_t i = 0u; i < count; ++i) {
    const Vec2 a = corners_px[i], b = corners_px[(i + 1u) % count];
    const float32_t dx = b.x - a.x, dy = b.y - a.y;
    const float32_t length = vkr_sqrt_f32(dx * dx + dy * dy);
    if (!vkr_is_finite_f32(length) || length == 0.0f)
      return false_v;
    normals[i] = (Vec2){dy / length, -dx / length};
    area += (a.x - corners_px[0].x) * (b.y - corners_px[0].y) -
            (a.y - corners_px[0].y) * (b.x - corners_px[0].x);
  }
  if (!vkr_is_finite_f32(area) || area == 0.0f)
    return false_v;
  const float32_t half_fringe = area > 0.0f ? 0.5f : -0.5f;
  for (uint32_t i = 0u; i < count; ++i) {
    const Vec2 previous = normals[(i + count - 1u) % count], next = normals[i];
    const float32_t denominator =
        1.0f + previous.x * next.x + previous.y * next.y;
    if (denominator <= 0.0f)
      return false_v;
    const Vec2 offset = {(previous.x + next.x) * half_fringe / denominator,
                         (previous.y + next.y) * half_fringe / denominator};
    command.corners_px[i] =
        (Vec2){corners_px[i].x - offset.x, corners_px[i].y - offset.y};
    command.corners_px[count + i] =
        (Vec2){corners_px[i].x + offset.x, corners_px[i].y + offset.y};
  }
  command.corner_count = count;
  float32_t left = command.corners_px[count].x, right = left;
  float32_t top = command.corners_px[count].y, bottom = top;
  for (uint32_t i = count + 1u; i < count * 2u; ++i) {
    left = Min(left, command.corners_px[i].x);
    right = Max(right, command.corners_px[i].x);
    top = Min(top, command.corners_px[i].y);
    bottom = Max(bottom, command.corners_px[i].y);
  }
  command.rect_px = (VkrUiRect){left, top, right - left, bottom - top};
  return vkr_ui_draw_buffer_push(buffer, command);
}

bool8_t vkr_ui_draw_buffer_image(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 uv_rect, Vec4 color,
                                 VkrUiTextureRef texture) {
  return vkr_ui_draw_buffer_push(buffer, (VkrUiDrawCommand){
                                             .rect_px = rect_px,
                                             .uv_rect = uv_rect,
                                             .color = color,
                                             .texture = texture,
                                             .mode = VKR_UI_DRAW_MODE_IMAGE,
                                         });
}

bool8_t vkr_ui_draw_buffer_text_quad(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                     Vec4 uv_rect, Vec4 color,
                                     VkrUiTextureRef texture,
                                     VkrUiDrawMode text_mode,
                                     float32_t screen_px_range,
                                     Vec2 sdf_unit_range) {
  if (text_mode != VKR_UI_DRAW_MODE_MTSDF_TEXT &&
      text_mode != VKR_UI_DRAW_MODE_BITMAP_TEXT)
    return false_v;
  return vkr_ui_draw_buffer_push(buffer, (VkrUiDrawCommand){
                                             .rect_px = rect_px,
                                             .uv_rect = uv_rect,
                                             .color = color,
                                             .texture = texture,
                                             .mode = text_mode,
                                             .screen_px_range = screen_px_range,
                                             .sdf_unit_range = sdf_unit_range,
                                         });
}

bool8_t vkr_ui_draw_buffer_box(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                               Vec4 color, Vec4 corner_radius_px,
                               Vec4 border_color, float32_t border_px,
                               float32_t softness_px) {
  return vkr_ui_draw_buffer_push(buffer,
                                 (VkrUiDrawCommand){
                                     .rect_px = rect_px,
                                     .uv_rect = {0.0f, 0.0f, 1.0f, 1.0f},
                                     .color = color,
                                     .corner_radius_px = corner_radius_px,
                                     .border_color = border_color,
                                     .border_px = border_px,
                                     .softness_px = softness_px,
                                     .mode = VKR_UI_DRAW_MODE_BOX,
                                 });
}

bool8_t vkr_ui_draw_buffer_rects(VkrUiDrawBuffer *buffer, VkrUiRect bounds_px,
                                 const VkrUiDrawRect *rects, uint32_t count,
                                 uint64_t content_hash) {
  if (!rects || count == 0u) {
    return false_v;
  }

  return vkr_ui_draw_buffer_push(buffer, (VkrUiDrawCommand){
                                             .rect_px = bounds_px,
                                             .color = {1.0f, 1.0f, 1.0f, 1.0f},
                                             .mode = VKR_UI_DRAW_MODE_QUAD,
                                             .run = VKR_UI_DRAW_RUN_RECTS,
                                             .run_count = count,
                                             .run_rects = rects,
                                             .run_scale = 1.0f,
                                             .run_hash = content_hash,
                                         });
}

bool8_t vkr_ui_draw_buffer_polyline(VkrUiDrawBuffer *buffer,
                                    VkrUiRect bounds_px, const Vec2 *points,
                                    uint32_t count, float32_t width_px,
                                    Vec4 color, uint64_t content_hash) {
  if (!points || count < 2u || !vkr_is_finite_f32(width_px) ||
      width_px <= 0.0f) {
    return false_v;
  }

  return vkr_ui_draw_buffer_push(buffer, (VkrUiDrawCommand){
                                             .rect_px = bounds_px,
                                             .color = color,
                                             .mode = VKR_UI_DRAW_MODE_QUAD,
                                             .run = VKR_UI_DRAW_RUN_POLYLINE,
                                             .run_count = count,
                                             .run_points = points,
                                             .run_scale = 1.0f,
                                             .run_width_px = width_px,
                                             .run_hash = content_hash,
                                         });
}

/* A run is untextured flat geometry with its own placement; its borrowed
 * elements are checked here, where every command is validated. */
static bool8_t vkr_ui_draw_run_valid(const VkrUiDrawCommand *command) {
  if (command->run > VKR_UI_DRAW_RUN_POLYLINE ||
      command->mode != VKR_UI_DRAW_MODE_QUAD || command->texture.id != 0u ||
      command->corner_count != 0u ||
      command->gradient != VKR_UI_DRAW_GRADIENT_NONE ||
      !vkr_is_finite_f32(command->run_scale) || command->run_scale <= 0.0f) {
    return false_v;
  }

  if (command->run == VKR_UI_DRAW_RUN_RECTS) {
    if (!command->run_rects || command->run_count == 0u) {
      return false_v;
    }
    for (uint32_t i = 0u; i < command->run_count; ++i) {
      const VkrUiDrawRect *item = &command->run_rects[i];
      if (!vkr_ui_rect_is_finite(item->rect) || item->rect.width < 0.0f ||
          item->rect.height < 0.0f || !vkr_ui_draw_vec4_finite(item->color)) {
        return false_v;
      }
    }
    return true_v;
  }

  if (!command->run_points || command->run_count < 2u ||
      !vkr_is_finite_f32(command->run_width_px) ||
      command->run_width_px <= 0.0f) {
    return false_v;
  }
  for (uint32_t i = 0u; i < command->run_count; ++i) {
    if (!vkr_is_finite_f32(command->run_points[i].x) ||
        !vkr_is_finite_f32(command->run_points[i].y)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_ui_draw_command_valid(VkrUiDrawCommand command) {
  if (!vkr_ui_rect_has_area(command.rect_px) ||
      !vkr_ui_rect_is_finite(command.clip_rect_px) ||
      !vkr_ui_draw_vec4_finite(command.uv_rect) ||
      !vkr_ui_draw_vec4_finite(command.color) ||
      !vkr_ui_draw_vec4_finite(command.corner_radius_px) ||
      !vkr_ui_draw_vec4_finite(command.border_color) ||
      !vkr_is_finite_f32(command.border_px) || command.border_px < 0.0f ||
      !vkr_is_finite_f32(command.softness_px) || command.softness_px < 0.0f ||
      command.mode >= VKR_UI_DRAW_MODE_COUNT ||
      command.gradient > VKR_UI_DRAW_GRADIENT_VERTICAL ||
      (command.gradient &&
       (command.mode != VKR_UI_DRAW_MODE_QUAD || command.corner_count)) ||
      !vkr_is_finite_f32(command.screen_px_range) ||
      command.screen_px_range < 0.0f)
    return false_v;
  if (command.run != VKR_UI_DRAW_RUN_NONE) {
    return vkr_ui_draw_run_valid(&command);
  }
  if (command.corner_count) {
    if (command.mode != VKR_UI_DRAW_MODE_QUAD || command.texture.id != 0u ||
        (command.corner_count != 3u && command.corner_count != 4u))
      return false_v;
    for (uint32_t i = 0u; i < command.corner_count * 2u; ++i)
      if (!vkr_is_finite_f32(command.corners_px[i].x) ||
          !vkr_is_finite_f32(command.corners_px[i].y))
        return false_v;
  }
  if ((command.mode == VKR_UI_DRAW_MODE_MTSDF_TEXT ||
       command.mode == VKR_UI_DRAW_MODE_BITMAP_TEXT ||
       command.mode == VKR_UI_DRAW_MODE_IMAGE) &&
      command.texture.id == 0u)
    return false_v;
  if ((command.mode == VKR_UI_DRAW_MODE_QUAD ||
       command.mode == VKR_UI_DRAW_MODE_BOX) &&
      command.texture.id != 0u)
    return false_v;
  if (command.mode == VKR_UI_DRAW_MODE_MTSDF_TEXT &&
      (command.screen_px_range <= 0.0f ||
       !vkr_is_finite_f32(command.sdf_unit_range.x) ||
       !vkr_is_finite_f32(command.sdf_unit_range.y) ||
       command.sdf_unit_range.x <= 0.0f || command.sdf_unit_range.y <= 0.0f))
    return false_v;
  if (command.mode == VKR_UI_DRAW_MODE_BOX &&
      (command.corner_radius_px.x < 0.0f || command.corner_radius_px.y < 0.0f ||
       command.corner_radius_px.z < 0.0f || command.corner_radius_px.w < 0.0f))
    return false_v;
  return true_v;
}

static VkrUiRect vkr_ui_draw_integral_scissor(VkrUiRect clip,
                                              uint32_t target_width,
                                              uint32_t target_height) {
  const VkrUiRect target = {
      .width = (float32_t)target_width,
      .height = (float32_t)target_height,
  };
  clip = vkr_ui_rect_intersect(clip, target);
  const float32_t right = vkr_ceil_f32(clip.x + clip.width);
  const float32_t bottom = vkr_ceil_f32(clip.y + clip.height);
  clip.x = vkr_floor_f32(clip.x);
  clip.y = vkr_floor_f32(clip.y);
  clip.width = right - clip.x;
  clip.height = bottom - clip.y;
  return vkr_ui_rect_intersect(clip, target);
}

static Vec4 vkr_ui_draw_clamp_radii(Vec4 radii, VkrUiRect rect) {
  const float32_t limit = Min(rect.width, rect.height) * 0.5f;
  return (Vec4){Min(radii.x, limit), Min(radii.y, limit), Min(radii.z, limit),
                Min(radii.w, limit)};
}

/* Untextured primitives ignore the batch texture, so they extend any batch
 * with the same scissor. A textured command may also adopt an untextured
 * batch's texture before anything in it samples. */
static bool8_t vkr_ui_draw_batch_accepts(VkrUiDrawBatch *batch,
                                         const VkrUiDrawCommand *command,
                                         VkrUiRect scissor) {
  if (!batch ||
      MemCompare(&batch->scissor_rect_px, &scissor, sizeof(scissor)) != 0)
    return false_v;
  if (command->texture.id == 0u)
    return true_v;
  if (batch->texture.id == 0u) {
    batch->texture = command->texture;
    batch->sdf_unit_range = command->sdf_unit_range;
    return true_v;
  }
  return vkr_ui_draw_texture_equal(batch->texture, command->texture) &&
         batch->sdf_unit_range.x == command->sdf_unit_range.x &&
         batch->sdf_unit_range.y == command->sdf_unit_range.y;
}

/* Outer edge of the emitted quad. A feathered box extends past its rectangle;
 * a crisp box keeps its antialiased edge inside the rectangle, so widget
 * geometry matches the laid-out bounds. */
static float32_t vkr_ui_draw_box_margin(const VkrUiDrawCommand *command) {
  return command->mode == VKR_UI_DRAW_MODE_BOX && command->softness_px > 0.0f
             ? command->softness_px + 1.0f
             : 0.0f;
}

static void vkr_ui_draw_write_quad(const VkrUiDrawCommand *command,
                                   uint32_t target_height,
                                   VkrUiDrawOutput *out_draws) {
  const uint32_t base = out_draws->vertex_count;
  const float32_t margin = vkr_ui_draw_box_margin(command);
  const VkrUiRect rect = {
      command->rect_px.x - margin,
      command->rect_px.y - margin,
      command->rect_px.width + margin * 2.0f,
      command->rect_px.height + margin * 2.0f,
  };
  const float32_t left = rect.x;
  const float32_t right = rect.x + rect.width;
  const float32_t bottom = (float32_t)target_height - rect.y - rect.height;
  const float32_t top = (float32_t)target_height - rect.y;
  const Vec4 uv = command->uv_rect;
  const Vec2 half = {command->rect_px.width * 0.5f,
                     command->rect_px.height * 0.5f};
  const Vec2 outer = {half.x + margin, half.y + margin};
  const VkrUiVertex shared = {
      .color = command->color,
      .border_color = command->border_color,
      .corner_radius_px =
          vkr_ui_draw_clamp_radii(command->corner_radius_px, command->rect_px),
      .half_extent_px = half,
      .border_px = command->border_px,
      .softness_px = command->softness_px,
      .mode = (uint32_t)command->mode,
  };
  /* Local offsets are Y-down like the authored rectangle. */
  const Vec2 positions[4] = {
      {left, bottom}, {right, bottom}, {right, top}, {left, top}};
  const Vec2 texcoords[4] = {
      {uv.x, uv.w}, {uv.z, uv.w}, {uv.z, uv.y}, {uv.x, uv.y}};
  const Vec2 locals[4] = {{-outer.x, outer.y},
                          {outer.x, outer.y},
                          {outer.x, -outer.y},
                          {-outer.x, -outer.y}};
  /* Corners 1 and 2 are the right edge; 0 and 1 the bottom. */
  const bool8_t ends[3][4] = {{false_v, false_v, false_v, false_v},
                              {false_v, true_v, true_v, false_v},
                              {true_v, true_v, false_v, false_v}};
  for (uint32_t i = 0u; i < 4u; ++i) {
    VkrUiVertex vertex = shared;
    if (ends[command->gradient][i]) {
      vertex.color = command->border_color;
    }
    vertex.position = positions[i];
    vertex.texcoord = texcoords[i];
    vertex.local_px = locals[i];
    out_draws->vertices[base + i] = vertex;
  }

  const uint32_t indices[] = {base + 0u, base + 1u, base + 2u,
                              base + 2u, base + 3u, base + 0u};
  MemCopy(out_draws->indices + out_draws->index_count, indices,
          sizeof(indices));
  out_draws->vertex_count += 4u;
  out_draws->index_count += ArrayCount(indices);
}

static void vkr_ui_draw_write_polygon(const VkrUiDrawCommand *command,
                                      uint32_t target_height,
                                      VkrUiDrawOutput *out) {
  const uint32_t base = out->vertex_count, count = command->corner_count;
  for (uint32_t i = 0u; i < count * 2u; ++i) {
    Vec4 color = command->color;
    if (i >= count)
      color.w = 0.0f;
    out->vertices[base + i] = (VkrUiVertex){
        .position = {command->corners_px[i].x,
                     (float32_t)target_height - command->corners_px[i].y},
        .color = color};
  }
  for (uint32_t i = 1u; i + 1u < count; ++i) {
    out->indices[out->index_count++] = base;
    out->indices[out->index_count++] = base + i;
    out->indices[out->index_count++] = base + i + 1u;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t next = (i + 1u) % count;
    const uint32_t indices[] = {
        base + i,    base + count + i, base + count + next, base + count + next,
        base + next, base + i};
    MemCopy(out->indices + out->index_count, indices, sizeof(indices));
    out->index_count += ArrayCount(indices);
  }
  out->vertex_count += count * 2u;
}

/* A run point in Y-down target pixels. */
static Vec2 vkr_ui_draw_run_point(const VkrUiDrawCommand *command,
                                  uint32_t index) {
  return (Vec2){
      command->rect_px.x + command->run_points[index].x * command->run_scale,
      command->rect_px.y + command->run_points[index].y * command->run_scale,
  };
}

static Vec2 vkr_ui_draw_direction(Vec2 from, Vec2 to) {
  const float32_t dx = to.x - from.x;
  const float32_t dy = to.y - from.y;
  const float32_t length = vkr_sqrt_f32(dx * dx + dy * dy);
  if (!(length > 0.0001f)) {
    return (Vec2){0};
  }
  return (Vec2){dx / length, dy / length};
}

/* Flat quads; empty and transparent rectangles emit nothing. */
static void vkr_ui_draw_write_rects(const VkrUiDrawCommand *command,
                                    uint32_t target_height,
                                    VkrUiDrawOutput *out) {
  const float32_t scale = command->run_scale;
  const float32_t height = (float32_t)target_height;
  for (uint32_t i = 0u; i < command->run_count; ++i) {
    const VkrUiDrawRect *item = &command->run_rects[i];
    const Vec4 color = vec4_mul(item->color, command->color);
    if (item->rect.width <= 0.0f || item->rect.height <= 0.0f ||
        color.w <= 0.0f) {
      continue;
    }

    const float32_t left = command->rect_px.x + item->rect.x * scale;
    const float32_t right = left + item->rect.width * scale;
    const float32_t top = height - (command->rect_px.y + item->rect.y * scale);
    const float32_t bottom = top - item->rect.height * scale;
    const Vec2 positions[4] = {
        {left, bottom}, {right, bottom}, {right, top}, {left, top}};
    const uint32_t base = out->vertex_count;
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
      out->vertices[base + corner] = (VkrUiVertex){
          .position = positions[corner],
          .color = color,
      };
    }

    const uint32_t indices[] = {base + 0u, base + 1u, base + 2u,
                                base + 2u, base + 3u, base + 0u};
    MemCopy(out->indices + out->index_count, indices, sizeof(indices));
    out->index_count += ArrayCount(indices);
    out->vertex_count += 4u;
  }
}

/* Four vertices per point across the stroke: outer fringe, opaque core edge,
 * opaque core edge, outer fringe. Each segment joins its two points' rows
 * with three quads. The core ends half a pixel inside the stroke edge and
 * the fringe half a pixel outside it, as polygon edges do. */
static void vkr_ui_draw_write_polyline(const VkrUiDrawCommand *command,
                                       uint32_t target_height,
                                       VkrUiDrawOutput *out) {
  const uint32_t count = command->run_count;
  const float32_t half_width =
      command->run_width_px * command->run_scale * 0.5f;
  const float32_t core = Max(half_width - 0.5f, 0.0f);
  const float32_t fringe = half_width + 0.5f;
  Vec4 color = command->color;
  /* A stroke thinner than a pixel fades instead of thinning further. */
  if (half_width < 0.5f) {
    color.w *= half_width * 2.0f;
  }
  Vec4 clear = color;
  clear.w = 0.0f;

  const uint32_t base = out->vertex_count;
  for (uint32_t i = 0u; i < count; ++i) {
    const Vec2 point = vkr_ui_draw_run_point(command, i);
    const Vec2 previous =
        i > 0u ? vkr_ui_draw_run_point(command, i - 1u) : point;
    const Vec2 next =
        i + 1u < count ? vkr_ui_draw_run_point(command, i + 1u) : point;
    Vec2 incoming = vkr_ui_draw_direction(previous, point);
    Vec2 outgoing = vkr_ui_draw_direction(point, next);
    if (incoming.x == 0.0f && incoming.y == 0.0f) {
      incoming = outgoing;
    }
    if (outgoing.x == 0.0f && outgoing.y == 0.0f) {
      outgoing = incoming;
    }
    if (incoming.x == 0.0f && incoming.y == 0.0f) {
      incoming = (Vec2){1.0f, 0.0f};
      outgoing = incoming;
    }

    /* The joint's normal bisects its segments; the mitre keeps the stroke's
     * width across each, up to four times the width at sharp turns. */
    Vec2 tangent = vkr_ui_draw_direction(
        (Vec2){0}, (Vec2){incoming.x + outgoing.x, incoming.y + outgoing.y});
    if (tangent.x == 0.0f && tangent.y == 0.0f) {
      tangent = incoming;
    }
    const Vec2 normal = {-tangent.y, tangent.x};
    const float32_t cosine = normal.x * -incoming.y + normal.y * incoming.x;
    const float32_t mitre = 1.0f / Max(cosine, 0.25f);
    const float32_t offsets[4] = {fringe, core, -core, -fringe};
    for (uint32_t row = 0u; row < 4u; ++row) {
      const float32_t offset = offsets[row] * mitre;
      out->vertices[base + i * 4u + row] = (VkrUiVertex){
          .position = {point.x + normal.x * offset,
                       (float32_t)target_height -
                           (point.y + normal.y * offset)},
          .color = row == 0u || row == 3u ? clear : color,
      };
    }
  }

  for (uint32_t i = 0u; i + 1u < count; ++i) {
    const uint32_t a = base + i * 4u;
    const uint32_t b = a + 4u;
    for (uint32_t row = 0u; row < 3u; ++row) {
      const uint32_t indices[] = {a + row,      a + row + 1u, b + row + 1u,
                                  b + row + 1u, b + row,      a + row};
      MemCopy(out->indices + out->index_count, indices, sizeof(indices));
      out->index_count += ArrayCount(indices);
    }
  }
  out->vertex_count += count * 4u;
}

/* Vertices and indices a command may write; empty rects of a run emit less. */
static void vkr_ui_draw_command_size(const VkrUiDrawCommand *command,
                                     uint64_t *out_vertices,
                                     uint64_t *out_indices) {
  if (command->run == VKR_UI_DRAW_RUN_RECTS) {
    *out_vertices = (uint64_t)command->run_count * 4u;
    *out_indices = (uint64_t)command->run_count * 6u;
  } else if (command->run == VKR_UI_DRAW_RUN_POLYLINE) {
    *out_vertices = (uint64_t)command->run_count * 4u;
    *out_indices = (uint64_t)(command->run_count - 1u) * 18u;
  } else if (command->corner_count) {
    *out_vertices = command->corner_count * 2u;
    *out_indices = command->corner_count * 9u - 6u;
  } else {
    *out_vertices = 4u;
    *out_indices = 6u;
  }
}

VkrUiDrawBuildResult vkr_ui_draw_build(const VkrUiDrawBuffer *buffer,
                                       uint32_t target_width,
                                       uint32_t target_height,
                                       VkrUiDrawOutput *out_draws) {
  if (!buffer || !buffer->commands || !out_draws || !out_draws->vertices ||
      !out_draws->indices || !out_draws->batches || target_width == 0u ||
      target_height == 0u || buffer->command_count > buffer->command_capacity ||
      buffer->clip_count == 0u ||
      buffer->clip_count > ArrayCount(buffer->clip_stack)) {
    return (VkrUiDrawBuildResult){.status = VKR_UI_DRAW_BUILD_INVALID};
  }
  for (uint32_t i = 0u; i < buffer->command_count; ++i) {
    if (!vkr_ui_draw_command_valid(buffer->commands[i]))
      return (VkrUiDrawBuildResult){.status = VKR_UI_DRAW_BUILD_INVALID};
  }

  out_draws->vertex_count = 0u;
  out_draws->index_count = 0u;
  out_draws->batch_count = 0u;
  uint32_t dropped = buffer->dropped_command_count;
  for (uint32_t i = 0u; i < buffer->command_count; ++i) {
    const VkrUiDrawCommand *command = &buffer->commands[i];
    /* A run draws only inside its bounds. */
    const VkrUiRect clip =
        command->run != VKR_UI_DRAW_RUN_NONE
            ? vkr_ui_rect_intersect(command->clip_rect_px, command->rect_px)
            : command->clip_rect_px;
    const VkrUiRect scissor =
        vkr_ui_draw_integral_scissor(clip, target_width, target_height);
    if (!vkr_ui_rect_has_area(scissor))
      continue;
    VkrUiDrawBatch *current =
        out_draws->batch_count
            ? &out_draws->batches[out_draws->batch_count - 1u]
            : NULL;
    const bool8_t starts_batch =
        !current || !vkr_ui_draw_batch_accepts(current, command, scissor);
    uint64_t vertices = 0u;
    uint64_t indices = 0u;
    vkr_ui_draw_command_size(command, &vertices, &indices);
    if (out_draws->vertex_capacity - out_draws->vertex_count < vertices ||
        out_draws->index_capacity - out_draws->index_count < indices ||
        (starts_batch && out_draws->batch_count == out_draws->batch_capacity)) {
      dropped += buffer->command_count - i;
      break;
    }

    if (starts_batch) {
      out_draws->batches[out_draws->batch_count++] = (VkrUiDrawBatch){
          .first_index = out_draws->index_count,
          .texture = command->texture,
          .scissor_rect_px = scissor,
          .sdf_unit_range = command->sdf_unit_range,
      };
    }
    const uint32_t first_index = out_draws->index_count;
    if (command->run == VKR_UI_DRAW_RUN_RECTS) {
      vkr_ui_draw_write_rects(command, target_height, out_draws);
    } else if (command->run == VKR_UI_DRAW_RUN_POLYLINE) {
      vkr_ui_draw_write_polyline(command, target_height, out_draws);
    } else if (command->corner_count) {
      vkr_ui_draw_write_polygon(command, target_height, out_draws);
    } else {
      vkr_ui_draw_write_quad(command, target_height, out_draws);
    }
    const uint32_t written = out_draws->index_count - first_index;
    if (starts_batch && written == 0u) {
      /* A run of empty rectangles opens no batch. */
      out_draws->batch_count--;
    } else {
      out_draws->batches[out_draws->batch_count - 1u].index_count += written;
    }
  }
  return (VkrUiDrawBuildResult){
      .status = dropped ? VKR_UI_DRAW_BUILD_TRUNCATED : VKR_UI_DRAW_BUILD_OK,
      .dropped_command_count = dropped,
  };
}

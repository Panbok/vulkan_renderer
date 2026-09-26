#include "core/ui/vkr_ui_draw.h"

#include <math.h>

static bool8_t vkr_ui_draw_vec4_finite(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w);
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
    const float32_t length = sqrtf(dx * dx + dy * dy);
    if (!isfinite(length) || length == 0.0f)
      return false_v;
    normals[i] = (Vec2){dy / length, -dx / length};
    area += (a.x - corners_px[0].x) * (b.y - corners_px[0].y) -
            (a.y - corners_px[0].y) * (b.x - corners_px[0].x);
  }
  if (!isfinite(area) || area == 0.0f)
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

static bool8_t vkr_ui_draw_command_valid(VkrUiDrawCommand command) {
  if (!vkr_ui_rect_has_area(command.rect_px) ||
      !vkr_ui_rect_is_finite(command.clip_rect_px) ||
      !vkr_ui_draw_vec4_finite(command.uv_rect) ||
      !vkr_ui_draw_vec4_finite(command.color) ||
      !vkr_ui_draw_vec4_finite(command.corner_radius_px) ||
      !vkr_ui_draw_vec4_finite(command.border_color) ||
      !isfinite(command.border_px) || command.border_px < 0.0f ||
      !isfinite(command.softness_px) || command.softness_px < 0.0f ||
      command.mode >= VKR_UI_DRAW_MODE_COUNT ||
      !isfinite(command.screen_px_range) || command.screen_px_range < 0.0f)
    return false_v;
  if (command.corner_count) {
    if (command.mode != VKR_UI_DRAW_MODE_QUAD || command.texture.id != 0u ||
        (command.corner_count != 3u && command.corner_count != 4u))
      return false_v;
    for (uint32_t i = 0u; i < command.corner_count * 2u; ++i)
      if (!isfinite(command.corners_px[i].x) ||
          !isfinite(command.corners_px[i].y))
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
      (command.screen_px_range <= 0.0f || !isfinite(command.sdf_unit_range.x) ||
       !isfinite(command.sdf_unit_range.y) ||
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
  const float32_t right = ceilf(clip.x + clip.width);
  const float32_t bottom = ceilf(clip.y + clip.height);
  clip.x = floorf(clip.x);
  clip.y = floorf(clip.y);
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
  for (uint32_t i = 0u; i < 4u; ++i) {
    VkrUiVertex vertex = shared;
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
    const VkrUiRect scissor = vkr_ui_draw_integral_scissor(
        command->clip_rect_px, target_width, target_height);
    if (!vkr_ui_rect_has_area(scissor))
      continue;
    VkrUiDrawBatch *current =
        out_draws->batch_count
            ? &out_draws->batches[out_draws->batch_count - 1u]
            : NULL;
    const bool8_t starts_batch =
        !current || !vkr_ui_draw_batch_accepts(current, command, scissor);
    const uint32_t vertices =
        command->corner_count ? command->corner_count * 2u : 4u;
    const uint32_t indices =
        command->corner_count ? command->corner_count * 9u - 6u : 6u;
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
    if (command->corner_count)
      vkr_ui_draw_write_polygon(command, target_height, out_draws);
    else
      vkr_ui_draw_write_quad(command, target_height, out_draws);
    out_draws->batches[out_draws->batch_count - 1u].index_count += indices;
  }
  return (VkrUiDrawBuildResult){
      .status = dropped ? VKR_UI_DRAW_BUILD_TRUNCATED : VKR_UI_DRAW_BUILD_OK,
      .dropped_command_count = dropped,
  };
}

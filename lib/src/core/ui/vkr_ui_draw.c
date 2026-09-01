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

bool8_t vkr_ui_draw_buffer_image(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 uv_rect, Vec4 color,
                                 VkrUiTextureRef texture) {
  return vkr_ui_draw_buffer_push(buffer, (VkrUiDrawCommand){
                                             .rect_px = rect_px,
                                             .uv_rect = uv_rect,
                                             .color = color,
                                             .texture = texture,
                                             .mode = VKR_UI_DRAW_MODE_QUAD,
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

bool8_t vkr_ui_draw_buffer_rounded_rect(VkrUiDrawBuffer *buffer,
                                        VkrUiRect rect_px, Vec4 color,
                                        Vec4 corner_radius_px) {
  return vkr_ui_draw_buffer_push(buffer,
                                 (VkrUiDrawCommand){
                                     .rect_px = rect_px,
                                     .uv_rect = {0.0f, 0.0f, 1.0f, 1.0f},
                                     .color = color,
                                     .corner_radius_px = corner_radius_px,
                                     .mode = VKR_UI_DRAW_MODE_ROUNDED_RECT,
                                 });
}

static bool8_t vkr_ui_draw_command_valid(VkrUiDrawCommand command) {
  if (!vkr_ui_rect_has_area(command.rect_px) ||
      !vkr_ui_rect_is_finite(command.clip_rect_px) ||
      !vkr_ui_draw_vec4_finite(command.uv_rect) ||
      !vkr_ui_draw_vec4_finite(command.color) ||
      !vkr_ui_draw_vec4_finite(command.corner_radius_px) ||
      command.mode >= VKR_UI_DRAW_MODE_COUNT ||
      !isfinite(command.screen_px_range) || command.screen_px_range < 0.0f)
    return false_v;
  if ((command.mode == VKR_UI_DRAW_MODE_MTSDF_TEXT ||
       command.mode == VKR_UI_DRAW_MODE_BITMAP_TEXT) &&
      command.texture.id == 0u)
    return false_v;
  if (command.mode == VKR_UI_DRAW_MODE_MTSDF_TEXT &&
      (command.screen_px_range <= 0.0f || !isfinite(command.sdf_unit_range.x) ||
       !isfinite(command.sdf_unit_range.y) ||
       command.sdf_unit_range.x <= 0.0f || command.sdf_unit_range.y <= 0.0f))
    return false_v;
  if (command.mode == VKR_UI_DRAW_MODE_ROUNDED_RECT &&
      (command.texture.id != 0u || command.corner_radius_px.x < 0.0f ||
       command.corner_radius_px.y < 0.0f || command.corner_radius_px.z < 0.0f ||
       command.corner_radius_px.w < 0.0f))
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

static bool8_t vkr_ui_draw_batch_matches(const VkrUiDrawBatch *batch,
                                         const VkrUiDrawCommand *command,
                                         VkrUiRect scissor, Vec4 radii) {
  if (!batch || batch->mode != command->mode ||
      !vkr_ui_draw_texture_equal(batch->texture, command->texture) ||
      batch->screen_px_range != command->screen_px_range ||
      batch->sdf_unit_range.x != command->sdf_unit_range.x ||
      batch->sdf_unit_range.y != command->sdf_unit_range.y ||
      MemCompare(&batch->scissor_rect_px, &scissor, sizeof(scissor)) != 0)
    return false_v;
  if (command->mode != VKR_UI_DRAW_MODE_ROUNDED_RECT)
    return true_v;
  return batch->rect_extent_px.x == command->rect_px.width &&
         batch->rect_extent_px.y == command->rect_px.height &&
         MemCompare(&batch->corner_radius_px, &radii, sizeof(radii)) == 0;
}

static void vkr_ui_draw_write_quad(const VkrUiDrawCommand *command,
                                   uint32_t target_height,
                                   VkrUiDrawOutput *out_draws) {
  const uint32_t base = out_draws->vertex_count;
  const float32_t left = command->rect_px.x;
  const float32_t right = command->rect_px.x + command->rect_px.width;
  const float32_t bottom =
      (float32_t)target_height - command->rect_px.y - command->rect_px.height;
  const float32_t top = (float32_t)target_height - command->rect_px.y;
  const Vec4 uv = command->uv_rect;
  out_draws->vertices[base + 0u] = (VkrUiVertex){.position = {left, bottom},
                                                 .texcoord = {uv.x, uv.w},
                                                 .color = command->color};
  out_draws->vertices[base + 1u] = (VkrUiVertex){.position = {right, bottom},
                                                 .texcoord = {uv.z, uv.w},
                                                 .color = command->color};
  out_draws->vertices[base + 2u] = (VkrUiVertex){.position = {right, top},
                                                 .texcoord = {uv.z, uv.y},
                                                 .color = command->color};
  out_draws->vertices[base + 3u] = (VkrUiVertex){.position = {left, top},
                                                 .texcoord = {uv.x, uv.y},
                                                 .color = command->color};

  const uint32_t indices[] = {base + 0u, base + 1u, base + 2u,
                              base + 2u, base + 3u, base + 0u};
  MemCopy(out_draws->indices + out_draws->index_count, indices,
          sizeof(indices));
  out_draws->vertex_count += 4u;
  out_draws->index_count += ArrayCount(indices);
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
    const Vec4 radii =
        vkr_ui_draw_clamp_radii(command->corner_radius_px, command->rect_px);
    const bool8_t starts_batch =
        out_draws->batch_count == 0u ||
        !vkr_ui_draw_batch_matches(
            &out_draws->batches[out_draws->batch_count - 1u], command, scissor,
            radii);
    if (out_draws->vertex_capacity - out_draws->vertex_count < 4u ||
        out_draws->index_capacity - out_draws->index_count < 6u ||
        (starts_batch && out_draws->batch_count == out_draws->batch_capacity)) {
      dropped += buffer->command_count - i;
      break;
    }

    if (starts_batch) {
      out_draws->batches[out_draws->batch_count++] = (VkrUiDrawBatch){
          .first_index = out_draws->index_count,
          .texture = command->texture,
          .scissor_rect_px = scissor,
          .mode = command->mode,
          .screen_px_range = command->screen_px_range,
          .sdf_unit_range = command->sdf_unit_range,
          .rect_extent_px = {command->rect_px.width, command->rect_px.height},
          .corner_radius_px = radii,
      };
    }
    vkr_ui_draw_write_quad(command, target_height, out_draws);
    out_draws->batches[out_draws->batch_count - 1u].index_count += 6u;
  }
  return (VkrUiDrawBuildResult){
      .status = dropped ? VKR_UI_DRAW_BUILD_TRUNCATED : VKR_UI_DRAW_BUILD_OK,
      .dropped_command_count = dropped,
  };
}

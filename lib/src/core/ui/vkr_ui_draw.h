/**
 * @file vkr_ui_draw.h
 * @brief Allocation-free UI draw commands and single-stream geometry builder.
 */
#pragma once

#include "core/ui/vkr_ui_types.h"
#include "core/ui/vkr_ui_vertex.h"

#define VKR_UI_CLIP_STACK_CAPACITY 32u

typedef enum VkrUiDrawMode {
  VKR_UI_DRAW_MODE_QUAD = 0,
  VKR_UI_DRAW_MODE_MTSDF_TEXT = 1,
  VKR_UI_DRAW_MODE_BITMAP_TEXT = 2,
  VKR_UI_DRAW_MODE_ROUNDED_RECT = 3,
  VKR_UI_DRAW_MODE_COUNT,
} VkrUiDrawMode;

typedef struct VkrUiDrawCommand {
  VkrUiRect rect_px;
  VkrUiRect clip_rect_px;
  /** Top-left texture coordinates: u0, v0, u1, v1. */
  Vec4 uv_rect;
  Vec4 color;
  Vec4 corner_radius_px;
  VkrUiTextureRef texture;
  VkrUiDrawMode mode;
  float32_t screen_px_range;
  /** Normalized atlas distance range used by the MTSDF shader. */
  Vec2 sdf_unit_range;
} VkrUiDrawCommand;

typedef struct VkrUiDrawBuffer {
  VkrUiDrawCommand *commands;
  uint32_t command_count;
  uint32_t command_capacity;
  uint32_t dropped_command_count;
  VkrUiRect clip_stack[VKR_UI_CLIP_STACK_CAPACITY];
  uint32_t clip_count;
} VkrUiDrawBuffer;

typedef struct VkrUiDrawBatch {
  uint32_t first_index;
  uint32_t index_count;
  VkrUiTextureRef texture;
  /** Integral Y-down attachment pixels. */
  VkrUiRect scissor_rect_px;
  VkrUiDrawMode mode;
  float32_t screen_px_range;
  Vec2 sdf_unit_range;
  /** Rounded-rect root data; zero for other modes. */
  Vec2 rect_extent_px;
  Vec4 corner_radius_px;
} VkrUiDrawBatch;

typedef struct VkrUiDrawOutput {
  VkrUiVertex *vertices;
  uint32_t vertex_count;
  uint32_t vertex_capacity;
  uint32_t *indices;
  uint32_t index_count;
  uint32_t index_capacity;
  VkrUiDrawBatch *batches;
  uint32_t batch_count;
  uint32_t batch_capacity;
} VkrUiDrawOutput;

typedef enum VkrUiDrawBuildStatus {
  VKR_UI_DRAW_BUILD_OK = 0,
  VKR_UI_DRAW_BUILD_TRUNCATED,
  VKR_UI_DRAW_BUILD_INVALID,
} VkrUiDrawBuildStatus;

typedef struct VkrUiDrawBuildResult {
  VkrUiDrawBuildStatus status;
  uint32_t dropped_command_count;
} VkrUiDrawBuildResult;

bool8_t vkr_ui_draw_buffer_begin(VkrUiDrawBuffer *buffer,
                                 VkrUiDrawCommand *commands,
                                 uint32_t command_capacity,
                                 VkrUiRect target_rect_px);
bool8_t vkr_ui_draw_buffer_push_clip(VkrUiDrawBuffer *buffer,
                                     VkrUiRect clip_rect_px);
bool8_t vkr_ui_draw_buffer_pop_clip(VkrUiDrawBuffer *buffer);
bool8_t vkr_ui_draw_buffer_push(VkrUiDrawBuffer *buffer,
                                VkrUiDrawCommand command);
bool8_t vkr_ui_draw_buffer_solid(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 color);
bool8_t vkr_ui_draw_buffer_image(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                 Vec4 uv_rect, Vec4 color,
                                 VkrUiTextureRef texture);
bool8_t vkr_ui_draw_buffer_text_quad(VkrUiDrawBuffer *buffer, VkrUiRect rect_px,
                                     Vec4 uv_rect, Vec4 color,
                                     VkrUiTextureRef texture,
                                     VkrUiDrawMode text_mode,
                                     float32_t screen_px_range,
                                     Vec2 sdf_unit_range);
bool8_t vkr_ui_draw_buffer_rounded_rect(VkrUiDrawBuffer *buffer,
                                        VkrUiRect rect_px, Vec4 color,
                                        Vec4 corner_radius_px);

VkrUiDrawBuildResult vkr_ui_draw_build(const VkrUiDrawBuffer *buffer,
                                       uint32_t target_width,
                                       uint32_t target_height,
                                       VkrUiDrawOutput *out_draws);

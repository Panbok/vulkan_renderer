#pragma once
#include "defines.h"
#include "math/vec.h"

typedef struct VkrUiRect {
  float32_t x;
  float32_t y;
  float32_t width;
  float32_t height;
} VkrUiRect;

typedef struct VkrUiTextureRef {
  uint32_t id;
  uint32_t generation;
} VkrUiTextureRef;

typedef struct VkrUiVertex {
  /** Y-up attachment-pixel position. */
  Vec2 position;
  Vec2 texcoord;
  /** Linear RGB and linear alpha for attachment blending. */
  Vec4 color;
} VkrUiVertex;

typedef enum VkrUiDrawMode {
  VKR_UI_DRAW_MODE_QUAD = 0,
  VKR_UI_DRAW_MODE_MTSDF_TEXT = 1,
  VKR_UI_DRAW_MODE_BITMAP_TEXT = 2,
  VKR_UI_DRAW_MODE_ROUNDED_RECT = 3,
  VKR_UI_DRAW_MODE_COUNT,
} VkrUiDrawMode;

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

#define VKR_UI_TEXTURE_REF_NONE ((VkrUiTextureRef){0})

_Static_assert(sizeof(VkrUiVertex) == 32u,
               "VkrUiVertex must remain a 32-byte GPU record");

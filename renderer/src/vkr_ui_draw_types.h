#pragma once
#include "defines.h"
#include "math/vec.h"

#include <stddef.h>

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

/** One UI stream vertex. Every primitive shares this record, so a batch
 * changes only with its texture or scissor. */
typedef struct VkrUiVertex {
  /** Y-up attachment-pixel position. */
  Vec2 position;
  Vec2 texcoord;
  /** Linear RGB and linear alpha for attachment blending. */
  Vec4 color;
  /** SDF box border color, linear like `color`. */
  Vec4 border_color;
  /** SDF box radii: top-left, top-right, bottom-right, bottom-left. */
  Vec4 corner_radius_px;
  /** Y-down pixel offset of this vertex from the SDF box center. */
  Vec2 local_px;
  Vec2 half_extent_px;
  float32_t border_px;
  /** Zero keeps a one-pixel antialiased edge; larger values feather the box
   * edge over that many pixels on each side (shadows and glows). */
  float32_t softness_px;
  /** VkrUiDrawMode of the primitive that owns this vertex. */
  uint32_t mode;
  uint32_t reserved;
} VkrUiVertex;

typedef enum VkrUiDrawMode {
  /** Vertex color; convex polygons carry their own alpha fringe. */
  VKR_UI_DRAW_MODE_QUAD = 0,
  VKR_UI_DRAW_MODE_MTSDF_TEXT = 1,
  VKR_UI_DRAW_MODE_BITMAP_TEXT = 2,
  /** Rounded box with optional border and feathered edge. */
  VKR_UI_DRAW_MODE_BOX = 3,
  /** Vertex color multiplied by the batch texture. */
  VKR_UI_DRAW_MODE_IMAGE = 4,
  VKR_UI_DRAW_MODE_COUNT,
} VkrUiDrawMode;

typedef struct VkrUiDrawBatch {
  uint32_t first_index;
  uint32_t index_count;
  /** Texture sampled by glyph and image vertices; untextured primitives join
   * any batch and ignore it. */
  VkrUiTextureRef texture;
  /** Integral Y-down attachment pixels. */
  VkrUiRect scissor_rect_px;
  /** Normalized MTSDF distance range of the batch atlas. */
  Vec2 sdf_unit_range;
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

/** Graph-owned animation preview, resolved by the current frame's backend. */
#define VKR_UI_TEXTURE_REF_ANIMATION_PREVIEW ((VkrUiTextureRef){UINT32_MAX, 0u})

#define VKR_UI_TEXTURE_REF_NONE ((VkrUiTextureRef){0})

_Static_assert(sizeof(VkrUiVertex) == 96u,
               "VkrUiVertex must remain a 96-byte GPU record");
_Static_assert(offsetof(VkrUiVertex, border_color) == 32u &&
                   offsetof(VkrUiVertex, corner_radius_px) == 48u &&
                   offsetof(VkrUiVertex, local_px) == 64u &&
                   offsetof(VkrUiVertex, border_px) == 80u &&
                   offsetof(VkrUiVertex, mode) == 88u,
               "VkrUiVertex GPU offsets drifted");

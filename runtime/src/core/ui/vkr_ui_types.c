#include "core/ui/vkr_ui_types.h"

#include <math.h>

bool8_t vkr_ui_rect_is_finite(VkrUiRect rect) {
  return isfinite(rect.x) && isfinite(rect.y) && isfinite(rect.width) &&
         isfinite(rect.height);
}

bool8_t vkr_ui_rect_has_area(VkrUiRect rect) {
  return vkr_ui_rect_is_finite(rect) && rect.width > 0.0f && rect.height > 0.0f;
}

VkrUiRect vkr_ui_rect_intersect(VkrUiRect a, VkrUiRect b) {
  const float32_t left = Max(a.x, b.x);
  const float32_t top = Max(a.y, b.y);
  const float32_t right = Min(a.x + a.width, b.x + b.width);
  const float32_t bottom = Min(a.y + a.height, b.y + b.height);
  return (VkrUiRect){
      .x = left,
      .y = top,
      .width = Max(0.0f, right - left),
      .height = Max(0.0f, bottom - top),
  };
}

VkrUiRect vkr_ui_rect_inset(VkrUiRect rect, VkrUiEdges edges) {
  const float32_t horizontal = edges.left + edges.right;
  const float32_t vertical = edges.top + edges.bottom;
  return (VkrUiRect){
      .x = rect.x + edges.left,
      .y = rect.y + edges.top,
      .width = Max(0.0f, rect.width - horizontal),
      .height = Max(0.0f, rect.height - vertical),
  };
}

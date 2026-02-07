#include "renderer/vkr_viewport.h"

#include "math/vkr_math.h"

bool8_t
vkr_viewport_mapping_window_to_target_pixel(const VkrViewportMapping *mapping,
                                            int32_t window_x, int32_t window_y,
                                            uint32_t *out_x, uint32_t *out_y) {
  if (!mapping || !out_x || !out_y) {
    return false_v;
  }
  if (mapping->target_width == 0 || mapping->target_height == 0) {
    return false_v;
  }

  const int32_t img_x = (int32_t)vkr_round_f32(mapping->image_rect_px.x);
  const int32_t img_y = (int32_t)vkr_round_f32(mapping->image_rect_px.y);
  const uint32_t img_w =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(mapping->image_rect_px.z));
  const uint32_t img_h =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(mapping->image_rect_px.w));

  if (window_x < img_x || window_y < img_y) {
    return false_v;
  }

  const int32_t local_x_i = window_x - img_x;
  const int32_t local_y_i = window_y - img_y;
  if (local_x_i < 0 || local_y_i < 0) {
    return false_v;
  }
  if ((uint32_t)local_x_i >= img_w || (uint32_t)local_y_i >= img_h) {
    return false_v;
  }

  const uint32_t local_x = (uint32_t)local_x_i;
  const uint32_t local_y = (uint32_t)local_y_i;

  // Map edges-to-edges for stable picking (top-left -> 0,0; bottom-right ->
  // w-1,h-1).
  uint32_t target_x = 0;
  if (img_w > 1u && mapping->target_width > 1u) {
    target_x = (uint32_t)(((uint64_t)local_x) *
                          (uint64_t)(mapping->target_width - 1u) /
                          (uint64_t)(img_w - 1u));
  }

  uint32_t target_y = 0;
  if (img_h > 1u && mapping->target_height > 1u) {
    target_y = (uint32_t)(((uint64_t)local_y) *
                          (uint64_t)(mapping->target_height - 1u) /
                          (uint64_t)(img_h - 1u));
  }

  *out_x = vkr_min_u32(target_x, mapping->target_width - 1u);
  *out_y = vkr_min_u32(target_y, mapping->target_height - 1u);
  return true_v;
}

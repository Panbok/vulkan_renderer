#include "renderer/systems/vkr_editor_viewport.h"

#include "math/vkr_math.h"

#include <math.h>

static Vec4 vkr_editor_viewport_compute_panel_rect(uint32_t width,
                                                   uint32_t height) {
  const uint32_t top_bar =
      (uint32_t)vkr_max_f32(32.0f, vkr_round_f32((float32_t)height * 0.06f));
  const uint32_t bottom_panel =
      (uint32_t)vkr_max_f32(180.0f, vkr_round_f32((float32_t)height * 0.24f));
  const uint32_t left_panel =
      (uint32_t)vkr_max_f32(220.0f, vkr_round_f32((float32_t)width * 0.18f));
  const uint32_t right_panel =
      (uint32_t)vkr_max_f32(280.0f, vkr_round_f32((float32_t)width * 0.22f));
  const uint32_t gutter = 8u;

  const uint32_t x = left_panel + gutter;
  const uint32_t y = top_bar + gutter;
  const uint32_t used_w = left_panel + right_panel + gutter * 2u;
  const uint32_t used_h = top_bar + bottom_panel + gutter * 2u;
  const uint32_t w = width > used_w ? width - used_w : 1u;
  const uint32_t h = height > used_h ? height - used_h : 1u;

  return (Vec4){(float32_t)x, (float32_t)y, (float32_t)w, (float32_t)h};
}

bool8_t vkr_editor_viewport_compute_mapping(uint32_t window_width,
                                            uint32_t window_height,
                                            VkrViewportFitMode fit_mode,
                                            float32_t render_scale,
                                            VkrViewportMapping *out_mapping) {
  if (!out_mapping || window_width == 0 || window_height == 0) {
    return false_v;
  }

  const Vec4 panel =
      vkr_editor_viewport_compute_panel_rect(window_width, window_height);
  return vkr_editor_viewport_mapping_from_panel_rect(panel, fit_mode,
                                                     render_scale, out_mapping);
}

bool8_t vkr_editor_viewport_mapping_from_panel_rect(
    Vec4 panel, VkrViewportFitMode fit_mode, float32_t render_scale,
    VkrViewportMapping *out_mapping) {
  if (!out_mapping || !isfinite(panel.x) || !isfinite(panel.y) ||
      !isfinite(panel.z) || !isfinite(panel.w) || panel.z <= 0.0f ||
      panel.w <= 0.0f || fit_mode > VKR_VIEWPORT_FIT_CONTAIN ||
      !isfinite(render_scale))
    return false_v;
  const float32_t clamped_scale = vkr_clamp_f32(render_scale, 0.25f, 2.0f);
  const float32_t panel_w = vkr_max_f32(1.0f, panel.z);
  const float32_t panel_h = vkr_max_f32(1.0f, panel.w);
  const uint32_t target_w =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_w * clamped_scale));
  const uint32_t target_h =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_h * clamped_scale));
  return vkr_editor_viewport_mapping_from_panel_rect_and_target(
      panel, fit_mode, target_w, target_h, out_mapping);
}

bool8_t vkr_editor_viewport_mapping_from_panel_rect_and_target(
    Vec4 panel, VkrViewportFitMode fit_mode, uint32_t target_w,
    uint32_t target_h, VkrViewportMapping *out_mapping) {
  if (!out_mapping || !isfinite(panel.x) || !isfinite(panel.y) ||
      !isfinite(panel.z) || !isfinite(panel.w) || panel.z <= 0.0f ||
      panel.w <= 0.0f || fit_mode > VKR_VIEWPORT_FIT_CONTAIN ||
      target_w == 0u || target_h == 0u)
    return false_v;
  const float32_t panel_w = vkr_max_f32(1.0f, panel.z);
  const float32_t panel_h = vkr_max_f32(1.0f, panel.w);
  Vec4 image = panel;

  if (fit_mode == VKR_VIEWPORT_FIT_CONTAIN) {
    const float32_t target_aspect = (float32_t)target_w / (float32_t)target_h;
    const float32_t panel_aspect = panel_w / panel_h;

    if (target_aspect > panel_aspect) {
      const float32_t scale = panel_w / (float32_t)target_w;
      const float32_t img_h = vkr_max_f32(1.0f, (float32_t)target_h * scale);
      const float32_t y = panel.y + (panel_h - img_h) * 0.5f;
      image = (Vec4){panel.x, y, panel_w, img_h};
    } else if (target_aspect < panel_aspect) {
      const float32_t scale = panel_h / (float32_t)target_h;
      const float32_t img_w = vkr_max_f32(1.0f, (float32_t)target_w * scale);
      const float32_t x = panel.x + (panel_w - img_w) * 0.5f;
      image = (Vec4){x, panel.y, img_w, panel_h};
    }

    image.x = vkr_round_f32(image.x);
    image.y = vkr_round_f32(image.y);
    image.z = vkr_max_f32(1.0f, vkr_round_f32(image.z));
    image.w = vkr_max_f32(1.0f, vkr_round_f32(image.w));
  }

  *out_mapping = (VkrViewportMapping){
      .panel_rect_px = panel,
      .image_rect_px = image,
      .target_width = target_w,
      .target_height = target_h,
      .fit_mode = fit_mode,
  };
  return true_v;
}

bool8_t vkr_editor_viewport_build_payload(const VkrViewportMapping *mapping,
                                          VkrEditorPassPayload *out_payload) {
  if (!mapping || !out_payload || mapping->target_width == 0u ||
      mapping->target_height == 0u || mapping->image_rect_px.z <= 0.0f ||
      mapping->image_rect_px.w <= 0.0f) {
    return false_v;
  }
  *out_payload =
      (VkrEditorPassPayload){.image_rect_px = mapping->image_rect_px};
  return true_v;
}

#include "core/ui/vkr_ui_style.h"

#include <math.h>

static bool8_t vkr_ui_edges_valid(VkrUiEdges edges) {
  return isfinite(edges.top) && isfinite(edges.right) &&
         isfinite(edges.bottom) && isfinite(edges.left) && edges.top >= 0.0f &&
         edges.right >= 0.0f && edges.bottom >= 0.0f && edges.left >= 0.0f;
}

static bool8_t vkr_ui_vec2_nonnegative(Vec2 value) {
  return isfinite(value.x) && isfinite(value.y) && value.x >= 0.0f &&
         value.y >= 0.0f;
}

static bool8_t vkr_ui_vec4_nonnegative(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w) && value.x >= 0.0f && value.y >= 0.0f &&
         value.z >= 0.0f && value.w >= 0.0f;
}

static bool8_t vkr_ui_vec4_finite(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w);
}

static VkrUiEdges vkr_ui_edges_scale(VkrUiEdges edges, float32_t scale) {
  return (VkrUiEdges){
      .top = edges.top * scale,
      .right = edges.right * scale,
      .bottom = edges.bottom * scale,
      .left = edges.left * scale,
  };
}

VkrUiStyle vkr_ui_style_default(void) {
  return (VkrUiStyle){
      .font_size_pt = 14.0f,
      .background_color = {0.0f, 0.0f, 0.0f, 0.0f},
      .border_color = {0.0f, 0.0f, 0.0f, 0.0f},
      .text_color = {1.0f, 1.0f, 1.0f, 1.0f},
  };
}

bool8_t vkr_ui_style_resolve(const VkrUiStyle *style, float32_t content_scale,
                             VkrUiResolvedStyle *out_style) {
  if (!style || !out_style || !isfinite(content_scale) ||
      content_scale <= 0.0f || !vkr_ui_edges_valid(style->margin_pt) ||
      !vkr_ui_edges_valid(style->border_pt) ||
      !vkr_ui_edges_valid(style->padding_pt) ||
      !vkr_ui_vec4_nonnegative(style->corner_radius_pt) ||
      !vkr_ui_vec2_nonnegative(style->min_size_pt) ||
      !vkr_ui_vec2_nonnegative(style->max_size_pt) ||
      (style->max_size_pt.x > 0.0f &&
       style->max_size_pt.x < style->min_size_pt.x) ||
      (style->max_size_pt.y > 0.0f &&
       style->max_size_pt.y < style->min_size_pt.y) ||
      !isfinite(style->gap_pt) || style->gap_pt < 0.0f ||
      !isfinite(style->font_size_pt) || style->font_size_pt < 0.0f ||
      !vkr_ui_vec4_finite(style->background_color) ||
      !vkr_ui_vec4_finite(style->border_color) ||
      !vkr_ui_vec4_finite(style->text_color))
    return false_v;

  *out_style = (VkrUiResolvedStyle){
      .margin_px = vkr_ui_edges_scale(style->margin_pt, content_scale),
      .border_px = vkr_ui_edges_scale(style->border_pt, content_scale),
      .padding_px = vkr_ui_edges_scale(style->padding_pt, content_scale),
      .corner_radius_px = vec4_scale(style->corner_radius_pt, content_scale),
      .min_size_px = vec2_scale(style->min_size_pt, content_scale),
      .max_size_px = vec2_scale(style->max_size_pt, content_scale),
      .gap_px = style->gap_pt * content_scale,
      .font_size_px = style->font_size_pt * content_scale,
      .background_color = style->background_color,
      .border_color = style->border_color,
      .text_color = style->text_color,
  };
  return true_v;
}

VkrUiRect vkr_ui_style_content_rect(VkrUiRect border_box,
                                    const VkrUiResolvedStyle *style) {
  if (!style)
    return (VkrUiRect){0};
  const VkrUiEdges insets = {
      .top = style->border_px.top + style->padding_px.top,
      .right = style->border_px.right + style->padding_px.right,
      .bottom = style->border_px.bottom + style->padding_px.bottom,
      .left = style->border_px.left + style->padding_px.left,
  };
  return vkr_ui_rect_inset(border_box, insets);
}

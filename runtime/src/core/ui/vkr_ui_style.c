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

vkr_global const VkrUiTheme vkr_ui_dark_theme = {
    .window = {0.071f, 0.075f, 0.082f, 1.0f},
    .panel = {0.106f, 0.110f, 0.122f, 1.0f},
    .header = {0.086f, 0.090f, 0.098f, 1.0f},
    .field = {0.063f, 0.067f, 0.075f, 1.0f},
    .raised = {0.165f, 0.173f, 0.188f, 1.0f},
    .raised_hover = {0.212f, 0.220f, 0.243f, 1.0f},
    .raised_active = {0.129f, 0.133f, 0.149f, 1.0f},
    .popup = {0.133f, 0.137f, 0.153f, 0.98f},
    .overlay = {0.078f, 0.082f, 0.094f, 0.86f},
    .border = {0.200f, 0.208f, 0.231f, 1.0f},
    .border_strong = {0.302f, 0.314f, 0.345f, 1.0f},
    .separator = {0.047f, 0.051f, 0.055f, 1.0f},
    .text = {0.894f, 0.902f, 0.918f, 1.0f},
    .text_secondary = {0.620f, 0.639f, 0.671f, 1.0f},
    .text_disabled = {0.408f, 0.420f, 0.447f, 1.0f},
    .text_on_accent = {1.0f, 1.0f, 1.0f, 1.0f},
    .accent = {0.231f, 0.510f, 0.965f, 1.0f},
    .accent_hover = {0.376f, 0.608f, 0.980f, 1.0f},
    .accent_active = {0.165f, 0.408f, 0.827f, 1.0f},
    .selection = {0.149f, 0.325f, 0.620f, 1.0f},
    .selection_inactive = {0.200f, 0.216f, 0.251f, 1.0f},
    .row_hover = {1.0f, 1.0f, 1.0f, 0.055f},
    .focus_ring = {0.376f, 0.608f, 0.980f, 1.0f},
    .warning = {0.961f, 0.678f, 0.259f, 1.0f},
    .error = {0.937f, 0.341f, 0.329f, 1.0f},
    .success = {0.361f, 0.788f, 0.490f, 1.0f},
    .info = {0.408f, 0.686f, 0.965f, 1.0f},
    .axis_x = {0.906f, 0.318f, 0.318f, 1.0f},
    .axis_y = {0.463f, 0.784f, 0.282f, 1.0f},
    .axis_z = {0.286f, 0.553f, 0.961f, 1.0f},
    .shadow = {0.0f, 0.0f, 0.0f, 0.45f},
    .radius_small = 3.0f,
    .radius = 4.0f,
    .radius_large = 8.0f,
    .space_xs = 2.0f,
    .space_sm = 4.0f,
    .space_md = 8.0f,
    .space_lg = 12.0f,
    .space_xl = 16.0f,
    .font_caption = 11.0f,
    .font_body = 12.5f,
    .font_emphasis = 13.0f,
    .font_title = 15.0f,
    .font_heading = 20.0f,
    .control_height = 24.0f,
    .row_height = 22.0f,
    .icon_size = 15.0f,
    .motion_rate = 22.0f,
    .tooltip_delay_seconds = 0.45f,
};

const VkrUiTheme *vkr_ui_theme(void) { return &vkr_ui_dark_theme; }

Vec4 vkr_ui_color_mix(Vec4 a, Vec4 b, float32_t t) {
  return (Vec4){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

Vec4 vkr_ui_color_alpha(Vec4 color, float32_t alpha) {
  color.w = alpha;
  return color;
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
      !vkr_ui_vec4_finite(style->text_color) ||
      !vkr_ui_vec4_finite(style->hover_background_color) ||
      !vkr_ui_vec4_finite(style->active_background_color) ||
      !vkr_ui_vec4_finite(style->shadow_color) ||
      !isfinite(style->shadow_offset_pt.x) ||
      !isfinite(style->shadow_offset_pt.y) ||
      !isfinite(style->shadow_blur_pt) || style->shadow_blur_pt < 0.0f)
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
      .hover_background_color = style->hover_background_color,
      .active_background_color = style->active_background_color,
      .shadow_color = style->shadow_color,
      .shadow_offset_px = vec2_scale(style->shadow_offset_pt, content_scale),
      .shadow_blur_px = style->shadow_blur_pt * content_scale,
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

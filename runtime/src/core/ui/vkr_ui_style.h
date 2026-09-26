/**
 * @file vkr_ui_style.h
 * @brief UI box model authored in logical points and resolved to pixels.
 */
#pragma once

#include "core/ui/vkr_ui_types.h"

typedef struct VkrUiStyle {
  VkrUiEdges margin_pt;
  VkrUiEdges border_pt;
  VkrUiEdges padding_pt;
  Vec4 corner_radius_pt;
  Vec2 min_size_pt;
  /** A zero component is unbounded. */
  Vec2 max_size_pt;
  float32_t gap_pt;
  float32_t font_size_pt;
  Vec4 background_color;
  Vec4 border_color;
  Vec4 text_color;
  /** Interaction states; zero alpha derives them from background_color and
   * VKR_UI_COLOR_NONE (negative alpha) keeps the background unchanged. */
  Vec4 hover_background_color;
  Vec4 active_background_color;
  /** Drop shadow beneath the box; zero alpha or blur draws none. */
  Vec4 shadow_color;
  Vec2 shadow_offset_pt;
  float32_t shadow_blur_pt;
} VkrUiStyle;

typedef struct VkrUiResolvedStyle {
  VkrUiEdges margin_px;
  VkrUiEdges border_px;
  VkrUiEdges padding_px;
  Vec4 corner_radius_px;
  Vec2 min_size_px;
  Vec2 max_size_px;
  float32_t gap_px;
  float32_t font_size_px;
  Vec4 background_color;
  Vec4 border_color;
  Vec4 text_color;
  Vec4 hover_background_color;
  Vec4 active_background_color;
  Vec4 shadow_color;
  Vec2 shadow_offset_px;
  float32_t shadow_blur_px;
} VkrUiResolvedStyle;

/** Semantic color, size and spacing tokens shared by every UI surface.
 * Colors are authored sRGB with linear alpha, like VkrUiStyle colors. */
typedef struct VkrUiTheme {
  /* Surfaces from back to front. */
  Vec4 window;
  Vec4 panel;
  Vec4 header;
  Vec4 field;
  Vec4 raised;
  Vec4 raised_hover;
  Vec4 raised_active;
  Vec4 popup;
  Vec4 overlay;
  /* Lines. */
  Vec4 border;
  Vec4 border_strong;
  Vec4 separator;
  /* Text. */
  Vec4 text;
  Vec4 text_secondary;
  Vec4 text_disabled;
  Vec4 text_on_accent;
  /* One accent for selection, active state and primary actions. */
  Vec4 accent;
  Vec4 accent_hover;
  Vec4 accent_active;
  Vec4 selection;
  Vec4 selection_inactive;
  Vec4 row_hover;
  Vec4 focus_ring;
  /* Status; amber is reserved for warnings and unsaved state. */
  Vec4 warning;
  Vec4 error;
  Vec4 success;
  Vec4 info;
  Vec4 axis_x;
  Vec4 axis_y;
  Vec4 axis_z;
  Vec4 shadow;
  /* Metrics in logical points. */
  float32_t radius_small;
  float32_t radius;
  float32_t radius_large;
  float32_t space_xs;
  float32_t space_sm;
  float32_t space_md;
  float32_t space_lg;
  float32_t space_xl;
  float32_t font_caption;
  float32_t font_body;
  float32_t font_emphasis;
  float32_t font_title;
  float32_t font_heading;
  float32_t control_height;
  float32_t row_height;
  float32_t icon_size;
  /* Motion: exponential approach rate per second and tooltip delay. */
  float32_t motion_rate;
  float32_t tooltip_delay_seconds;
} VkrUiTheme;

/** The process-wide dark theme. */
const VkrUiTheme *vkr_ui_theme(void);

/** Mix two authored colors, including alpha. */
Vec4 vkr_ui_color_mix(Vec4 a, Vec4 b, float32_t t);

/** Replace the alpha of an authored color. */
Vec4 vkr_ui_color_alpha(Vec4 color, float32_t alpha);

/** Style color that disables a derived interaction state. */
#define VKR_UI_COLOR_NONE ((Vec4){0.0f, 0.0f, 0.0f, -1.0f})

VkrUiStyle vkr_ui_style_default(void);

/** Resolve every dimension once at the cold per-frame style boundary. */
bool8_t vkr_ui_style_resolve(const VkrUiStyle *style, float32_t content_scale,
                             VkrUiResolvedStyle *out_style);

VkrUiRect vkr_ui_style_content_rect(VkrUiRect border_box,
                                    const VkrUiResolvedStyle *style);

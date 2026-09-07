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
} VkrUiResolvedStyle;

VkrUiStyle vkr_ui_style_default(void);

/** Resolve every dimension once at the cold per-frame style boundary. */
bool8_t vkr_ui_style_resolve(const VkrUiStyle *style, float32_t content_scale,
                             VkrUiResolvedStyle *out_style);

VkrUiRect vkr_ui_style_content_rect(VkrUiRect border_box,
                                    const VkrUiResolvedStyle *style);

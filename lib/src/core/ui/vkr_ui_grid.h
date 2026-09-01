/**
 * @file vkr_ui_grid.h
 * @brief Allocation-free CSS-grid-like track resolution and arrangement.
 */
#pragma once

#include "core/ui/vkr_ui_types.h"

typedef enum VkrUiTrackUnit {
  VKR_UI_TRACK_PX = 0,
  /** Fraction of the resolved container extent: 0.25 means 25 percent. */
  VKR_UI_TRACK_PCT,
  VKR_UI_TRACK_FR,
  VKR_UI_TRACK_AUTO,
  VKR_UI_TRACK_UNIT_COUNT,
} VkrUiTrackUnit;

typedef struct VkrUiTrack {
  float32_t value;
  VkrUiTrackUnit unit;
  float32_t min_px;
  /** Zero means unbounded. */
  float32_t max_px;
} VkrUiTrack;

typedef struct VkrUiGridAxisOutput {
  float32_t *offsets_px;
  float32_t *sizes_px;
  uint32_t capacity;
  uint32_t count;
  uint32_t fr_iterations;
  float32_t resolved_extent_px;
} VkrUiGridAxisOutput;

/** Resolve one axis. AUTO intrinsic sizes are indexed by track. */
bool8_t vkr_ui_grid_resolve_tracks(const VkrUiTrack *tracks,
                                   uint32_t track_count, float32_t available_px,
                                   float32_t gap_px,
                                   const float32_t *auto_intrinsic_px,
                                   VkrUiGridAxisOutput *out_axis);

typedef enum VkrUiAlign {
  VKR_UI_ALIGN_STRETCH = 0,
  VKR_UI_ALIGN_START,
  VKR_UI_ALIGN_CENTER,
  VKR_UI_ALIGN_END,
  VKR_UI_ALIGN_COUNT,
} VkrUiAlign;

#define VKR_UI_GRID_AUTO UINT32_MAX

typedef struct VkrUiGridItem {
  uint32_t column;
  uint32_t row;
  uint32_t column_span;
  uint32_t row_span;
  VkrUiAlign justify;
  VkrUiAlign align;
  Vec2 intrinsic_size_px;
  /** Zero components are unbounded. */
  Vec2 max_size_px;
  VkrUiEdges margin_px;
} VkrUiGridItem;

typedef struct VkrUiGridAxisView {
  const float32_t *offsets_px;
  const float32_t *sizes_px;
  uint32_t count;
} VkrUiGridAxisView;

typedef struct VkrUiGridCell {
  uint32_t column;
  uint32_t row;
} VkrUiGridCell;

typedef struct VkrUiGridIntrinsicOutput {
  float32_t width_px;
  float32_t height_px;
} VkrUiGridIntrinsicOutput;

/** Resolve explicit and row-major auto placements without arranging pixels. */
bool8_t vkr_ui_grid_resolve_placements(
    uint32_t column_count, uint32_t row_count, const VkrUiGridItem *items,
    uint32_t item_count, uint8_t *occupancy, uint32_t occupancy_capacity,
    VkrUiGridCell *out_cells, uint32_t out_cell_capacity);

/**
 * Measure a grid with already-resolved placements. Percentage and fractional
 * tracks have no containing extent during this pass, so their intrinsic base
 * is their minimum plus any item contribution. Fixed tracks do not grow.
 */
bool8_t vkr_ui_grid_measure_intrinsic(
    const VkrUiTrack *columns, uint32_t column_count, const VkrUiTrack *rows,
    uint32_t row_count, float32_t gap_px, const VkrUiGridItem *items,
    const VkrUiGridCell *cells, uint32_t item_count, float32_t *column_sizes_px,
    uint32_t column_size_capacity, float32_t *row_sizes_px,
    uint32_t row_size_capacity, VkrUiGridIntrinsicOutput *out_intrinsic);

/**
 * Arrange items in Y-down pixel space. `occupancy` is caller-owned scratch with
 * at least columns.count * rows.count bytes and is cleared by the function.
 */
bool8_t
vkr_ui_grid_arrange_items(VkrUiRect content_rect, VkrUiGridAxisView columns,
                          VkrUiGridAxisView rows, const VkrUiGridItem *items,
                          uint32_t item_count, uint8_t *occupancy,
                          uint32_t occupancy_capacity, VkrUiRect *out_rects,
                          uint32_t out_rect_capacity);

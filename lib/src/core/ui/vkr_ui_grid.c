#include "core/ui/vkr_ui_grid.h"

#include <math.h>

static bool8_t vkr_ui_grid_track_valid(VkrUiTrack track) {
  if (track.unit >= VKR_UI_TRACK_UNIT_COUNT || !isfinite(track.value) ||
      !isfinite(track.min_px) || !isfinite(track.max_px) ||
      track.min_px < 0.0f || track.max_px < 0.0f ||
      (track.max_px > 0.0f && track.max_px < track.min_px))
    return false_v;
  if (track.unit == VKR_UI_TRACK_FR)
    return track.value > 0.0f;
  return track.value >= 0.0f;
}

static float32_t vkr_ui_grid_clamp_track(float32_t value, VkrUiTrack track) {
  value = Max(value, track.min_px);
  return track.max_px > 0.0f ? Min(value, track.max_px) : value;
}

bool8_t vkr_ui_grid_resolve_tracks(const VkrUiTrack *tracks,
                                   uint32_t track_count, float32_t available_px,
                                   float32_t gap_px,
                                   const float32_t *auto_intrinsic_px,
                                   VkrUiGridAxisOutput *out_axis) {
  if (!tracks || track_count == 0u || !out_axis || !out_axis->offsets_px ||
      !out_axis->sizes_px || out_axis->capacity < track_count ||
      !isfinite(available_px) || available_px < 0.0f || !isfinite(gap_px) ||
      gap_px < 0.0f)
    return false_v;

  bool8_t has_auto = false_v;
  for (uint32_t i = 0u; i < track_count; ++i) {
    if (!vkr_ui_grid_track_valid(tracks[i]))
      return false_v;
    has_auto |= tracks[i].unit == VKR_UI_TRACK_AUTO;
  }
  if (has_auto && !auto_intrinsic_px)
    return false_v;

  const float32_t gaps = gap_px * (float32_t)(track_count - 1u);
  const float32_t track_space = Max(0.0f, available_px - gaps);
  float32_t fixed_sum = 0.0f;
  uint32_t fr_count = 0u;
  for (uint32_t i = 0u; i < track_count; ++i) {
    const VkrUiTrack track = tracks[i];
    float32_t resolved = 0.0f;
    switch (track.unit) {
    case VKR_UI_TRACK_PX:
      resolved = track.value;
      break;
    case VKR_UI_TRACK_PCT:
      resolved = track.value * available_px;
      break;
    case VKR_UI_TRACK_AUTO:
      if (!isfinite(auto_intrinsic_px[i]) || auto_intrinsic_px[i] < 0.0f)
        return false_v;
      resolved = auto_intrinsic_px[i];
      break;
    case VKR_UI_TRACK_FR:
      out_axis->sizes_px[i] = -1.0f;
      fr_count++;
      continue;
    default:
      return false_v;
    }
    resolved = vkr_ui_grid_clamp_track(resolved, track);
    out_axis->sizes_px[i] = resolved;
    fixed_sum += resolved;
  }

  uint32_t iterations = 0u;
  while (fr_count > 0u && iterations < 3u) {
    iterations++;
    float32_t frozen_sum = 0.0f;
    float32_t free_weight = 0.0f;
    for (uint32_t i = 0u; i < track_count; ++i) {
      if (tracks[i].unit != VKR_UI_TRACK_FR)
        continue;
      if (out_axis->sizes_px[i] >= 0.0f)
        frozen_sum += out_axis->sizes_px[i];
      else
        free_weight += tracks[i].value;
    }

    const float32_t remainder = Max(0.0f, track_space - fixed_sum - frozen_sum);
    bool8_t clamped_any = false_v;
    for (uint32_t i = 0u; i < track_count; ++i) {
      if (tracks[i].unit != VKR_UI_TRACK_FR || out_axis->sizes_px[i] >= 0.0f)
        continue;
      const float32_t proposed = remainder * tracks[i].value / free_weight;
      const float32_t resolved = vkr_ui_grid_clamp_track(proposed, tracks[i]);
      out_axis->offsets_px[i] = resolved;
      if (resolved != proposed) {
        out_axis->sizes_px[i] = resolved;
        fr_count--;
        clamped_any = true_v;
      }
    }

    if (!clamped_any || iterations == 3u) {
      for (uint32_t i = 0u; i < track_count; ++i) {
        if (tracks[i].unit == VKR_UI_TRACK_FR && out_axis->sizes_px[i] < 0.0f) {
          out_axis->sizes_px[i] = out_axis->offsets_px[i];
          fr_count--;
        }
      }
    }
  }

  float32_t cursor = 0.0f;
  for (uint32_t i = 0u; i < track_count; ++i) {
    out_axis->offsets_px[i] = cursor;
    cursor += out_axis->sizes_px[i];
    if (i + 1u < track_count)
      cursor += gap_px;
  }
  out_axis->count = track_count;
  out_axis->fr_iterations = iterations;
  out_axis->resolved_extent_px = cursor;
  return true_v;
}

static bool8_t vkr_ui_grid_edges_valid(VkrUiEdges edges) {
  return isfinite(edges.top) && isfinite(edges.right) &&
         isfinite(edges.bottom) && isfinite(edges.left) && edges.top >= 0.0f &&
         edges.right >= 0.0f && edges.bottom >= 0.0f && edges.left >= 0.0f;
}

static bool8_t vkr_ui_grid_item_valid(VkrUiGridItem item, uint32_t columns,
                                      uint32_t rows) {
  return item.column_span > 0u && item.row_span > 0u &&
         item.column_span <= columns && item.row_span <= rows &&
         (item.column == VKR_UI_GRID_AUTO ||
          (item.column < columns &&
           item.column_span <= columns - item.column)) &&
         (item.row == VKR_UI_GRID_AUTO ||
          (item.row < rows && item.row_span <= rows - item.row)) &&
         item.justify < VKR_UI_ALIGN_COUNT && item.align < VKR_UI_ALIGN_COUNT &&
         isfinite(item.intrinsic_size_px.x) &&
         isfinite(item.intrinsic_size_px.y) &&
         item.intrinsic_size_px.x >= 0.0f && item.intrinsic_size_px.y >= 0.0f &&
         isfinite(item.max_size_px.x) && isfinite(item.max_size_px.y) &&
         item.max_size_px.x >= 0.0f && item.max_size_px.y >= 0.0f &&
         vkr_ui_grid_edges_valid(item.margin_px);
}

static bool8_t vkr_ui_grid_cells_free(const uint8_t *occupancy,
                                      uint32_t column_count, uint32_t column,
                                      uint32_t row, uint32_t column_span,
                                      uint32_t row_span) {
  for (uint32_t y = row; y < row + row_span; ++y) {
    for (uint32_t x = column; x < column + column_span; ++x) {
      if (occupancy[y * column_count + x])
        return false_v;
    }
  }
  return true_v;
}

static void vkr_ui_grid_mark_cells(uint8_t *occupancy, uint32_t column_count,
                                   uint32_t column, uint32_t row,
                                   uint32_t column_span, uint32_t row_span) {
  for (uint32_t y = row; y < row + row_span; ++y)
    for (uint32_t x = column; x < column + column_span; ++x)
      occupancy[y * column_count + x] = 1u;
}

static bool8_t vkr_ui_grid_place_item(const uint8_t *occupancy,
                                      uint32_t column_count, uint32_t row_count,
                                      VkrUiGridItem item, uint32_t *out_column,
                                      uint32_t *out_row) {
  const uint32_t column_begin =
      item.column == VKR_UI_GRID_AUTO ? 0u : item.column;
  const uint32_t column_end = item.column == VKR_UI_GRID_AUTO
                                  ? column_count - item.column_span + 1u
                                  : item.column + 1u;
  const uint32_t row_begin = item.row == VKR_UI_GRID_AUTO ? 0u : item.row;
  const uint32_t row_end = item.row == VKR_UI_GRID_AUTO
                               ? row_count - item.row_span + 1u
                               : item.row + 1u;
  for (uint32_t row = row_begin; row < row_end; ++row) {
    for (uint32_t column = column_begin; column < column_end; ++column) {
      if (vkr_ui_grid_cells_free(occupancy, column_count, column, row,
                                 item.column_span, item.row_span)) {
        *out_column = column;
        *out_row = row;
        return true_v;
      }
    }
  }
  return false_v;
}

bool8_t vkr_ui_grid_resolve_placements(
    uint32_t column_count, uint32_t row_count, const VkrUiGridItem *items,
    uint32_t item_count, uint8_t *occupancy, uint32_t occupancy_capacity,
    VkrUiGridCell *out_cells, uint32_t out_cell_capacity) {
  const uint64_t cell_count = (uint64_t)column_count * row_count;
  if (column_count == 0u || row_count == 0u || !occupancy ||
      cell_count > occupancy_capacity || item_count > out_cell_capacity ||
      (item_count > 0u && (!items || !out_cells)))
    return false_v;
  for (uint32_t i = 0u; i < item_count; ++i)
    if (!vkr_ui_grid_item_valid(items[i], column_count, row_count))
      return false_v;

  MemZero(occupancy, cell_count);
  for (uint32_t i = 0u; i < item_count; ++i) {
    const VkrUiGridItem item = items[i];
    uint32_t column = item.column;
    uint32_t row = item.row;
    if ((column == VKR_UI_GRID_AUTO || row == VKR_UI_GRID_AUTO) &&
        !vkr_ui_grid_place_item(occupancy, column_count, row_count, item,
                                &column, &row))
      return false_v;
    vkr_ui_grid_mark_cells(occupancy, column_count, column, row,
                           item.column_span, item.row_span);
    out_cells[i] = (VkrUiGridCell){.column = column, .row = row};
  }
  return true_v;
}

static bool8_t vkr_ui_grid_cell_valid(VkrUiGridCell cell, VkrUiGridItem item,
                                      uint32_t column_count,
                                      uint32_t row_count) {
  return cell.column < column_count && cell.row < row_count &&
         item.column_span <= column_count - cell.column &&
         item.row_span <= row_count - cell.row;
}

static bool8_t vkr_ui_grid_measure_axis(
    const VkrUiTrack *tracks, uint32_t track_count, float32_t gap_px,
    const VkrUiGridItem *items, const VkrUiGridCell *cells, uint32_t item_count,
    bool8_t columns, float32_t *sizes_px, float32_t *out_extent_px) {
  for (uint32_t track_index = 0u; track_index < track_count; ++track_index) {
    const VkrUiTrack track = tracks[track_index];
    sizes_px[track_index] = vkr_ui_grid_clamp_track(
        track.unit == VKR_UI_TRACK_PX ? track.value : 0.0f, track);
  }

  for (uint32_t item_index = 0u; item_index < item_count; ++item_index) {
    const VkrUiGridItem item = items[item_index];
    const VkrUiGridCell cell = cells[item_index];
    const uint32_t start = columns ? cell.column : cell.row;
    const uint32_t span = columns ? item.column_span : item.row_span;
    const float32_t margin = columns
                                 ? item.margin_px.left + item.margin_px.right
                                 : item.margin_px.top + item.margin_px.bottom;
    const float32_t intrinsic =
        columns ? item.intrinsic_size_px.x : item.intrinsic_size_px.y;
    float32_t occupied = gap_px * (float32_t)(span - 1u);
    for (uint32_t offset = 0u; offset < span; ++offset)
      occupied += sizes_px[start + offset];

    float32_t deficit = Max(0.0f, intrinsic + margin - occupied);
    for (uint32_t offset = 0u; offset < span && deficit > 0.0f; ++offset) {
      const uint32_t track_index = start + offset;
      const VkrUiTrack track = tracks[track_index];
      if (track.unit == VKR_UI_TRACK_PX)
        continue;
      const float32_t capacity =
          track.max_px > 0.0f ? Max(0.0f, track.max_px - sizes_px[track_index])
                              : deficit;
      const float32_t growth = Min(deficit, capacity);
      sizes_px[track_index] += growth;
      deficit -= growth;
    }
  }

  float32_t extent = gap_px * (float32_t)(track_count - 1u);
  for (uint32_t track_index = 0u; track_index < track_count; ++track_index)
    extent += sizes_px[track_index];
  *out_extent_px = extent;
  return true_v;
}

bool8_t vkr_ui_grid_measure_intrinsic(
    const VkrUiTrack *columns, uint32_t column_count, const VkrUiTrack *rows,
    uint32_t row_count, float32_t gap_px, const VkrUiGridItem *items,
    const VkrUiGridCell *cells, uint32_t item_count, float32_t *column_sizes_px,
    uint32_t column_size_capacity, float32_t *row_sizes_px,
    uint32_t row_size_capacity, VkrUiGridIntrinsicOutput *out_intrinsic) {
  if (!columns || !rows || column_count == 0u || row_count == 0u ||
      !isfinite(gap_px) || gap_px < 0.0f ||
      (item_count > 0u && (!items || !cells)) || !column_sizes_px ||
      column_size_capacity < column_count || !row_sizes_px ||
      row_size_capacity < row_count || !out_intrinsic)
    return false_v;
  for (uint32_t i = 0u; i < column_count; ++i)
    if (!vkr_ui_grid_track_valid(columns[i]))
      return false_v;
  for (uint32_t i = 0u; i < row_count; ++i)
    if (!vkr_ui_grid_track_valid(rows[i]))
      return false_v;
  for (uint32_t i = 0u; i < item_count; ++i) {
    if (!vkr_ui_grid_item_valid(items[i], column_count, row_count) ||
        !vkr_ui_grid_cell_valid(cells[i], items[i], column_count, row_count))
      return false_v;
  }

  return vkr_ui_grid_measure_axis(columns, column_count, gap_px, items, cells,
                                  item_count, true_v, column_sizes_px,
                                  &out_intrinsic->width_px) &&
         vkr_ui_grid_measure_axis(rows, row_count, gap_px, items, cells,
                                  item_count, false_v, row_sizes_px,
                                  &out_intrinsic->height_px);
}

static float32_t vkr_ui_grid_aligned_origin(float32_t start,
                                            float32_t available, float32_t size,
                                            VkrUiAlign alignment) {
  if (alignment == VKR_UI_ALIGN_CENTER)
    return start + (available - size) * 0.5f;
  if (alignment == VKR_UI_ALIGN_END)
    return start + available - size;
  return start;
}

bool8_t
vkr_ui_grid_arrange_items(VkrUiRect content_rect, VkrUiGridAxisView columns,
                          VkrUiGridAxisView rows, const VkrUiGridItem *items,
                          uint32_t item_count, uint8_t *occupancy,
                          uint32_t occupancy_capacity, VkrUiRect *out_rects,
                          uint32_t out_rect_capacity) {
  const uint64_t cell_count = (uint64_t)columns.count * rows.count;
  if (!vkr_ui_rect_is_finite(content_rect) || !columns.offsets_px ||
      !columns.sizes_px || !rows.offsets_px || !rows.sizes_px ||
      columns.count == 0u || rows.count == 0u || !occupancy ||
      cell_count > occupancy_capacity || item_count > out_rect_capacity ||
      (item_count > 0u && (!items || !out_rects)))
    return false_v;

  for (uint32_t i = 0u; i < item_count; ++i) {
    if (!vkr_ui_grid_item_valid(items[i], columns.count, rows.count))
      return false_v;
  }
  MemZero(occupancy, cell_count);

  for (uint32_t i = 0u; i < item_count; ++i) {
    const VkrUiGridItem item = items[i];
    uint32_t column = item.column;
    uint32_t row = item.row;
    if ((column == VKR_UI_GRID_AUTO || row == VKR_UI_GRID_AUTO) &&
        !vkr_ui_grid_place_item(occupancy, columns.count, rows.count, item,
                                &column, &row))
      return false_v;
    vkr_ui_grid_mark_cells(occupancy, columns.count, column, row,
                           item.column_span, item.row_span);

    const uint32_t last_column = column + item.column_span - 1u;
    const uint32_t last_row = row + item.row_span - 1u;
    const float32_t cell_x = content_rect.x + columns.offsets_px[column];
    const float32_t cell_y = content_rect.y + rows.offsets_px[row];
    const float32_t cell_right = content_rect.x +
                                 columns.offsets_px[last_column] +
                                 columns.sizes_px[last_column];
    const float32_t cell_bottom =
        content_rect.y + rows.offsets_px[last_row] + rows.sizes_px[last_row];
    const float32_t inner_width = Max(
        0.0f, cell_right - cell_x - item.margin_px.left - item.margin_px.right);
    const float32_t inner_height =
        Max(0.0f,
            cell_bottom - cell_y - item.margin_px.top - item.margin_px.bottom);
    float32_t width = item.justify == VKR_UI_ALIGN_STRETCH
                          ? inner_width
                          : Min(item.intrinsic_size_px.x, inner_width);
    float32_t height = item.align == VKR_UI_ALIGN_STRETCH
                           ? inner_height
                           : Min(item.intrinsic_size_px.y, inner_height);
    if (item.max_size_px.x > 0.0f)
      width = Min(width, item.max_size_px.x);
    if (item.max_size_px.y > 0.0f)
      height = Min(height, item.max_size_px.y);
    const float32_t inner_x = cell_x + item.margin_px.left;
    const float32_t inner_y = cell_y + item.margin_px.top;
    out_rects[i] = (VkrUiRect){
        .x = vkr_ui_grid_aligned_origin(inner_x, inner_width, width,
                                        item.justify),
        .y = vkr_ui_grid_aligned_origin(inner_y, inner_height, height,
                                        item.align),
        .width = width,
        .height = height,
    };
  }
  return true_v;
}

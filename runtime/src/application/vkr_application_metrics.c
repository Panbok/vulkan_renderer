#include "application/vkr_application_metrics.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_ui_system.h"

VkrApplicationMetricsSnapshot
vkr_application_metrics_snapshot(const VkrRenderAssets *assets,
                                 const VkrUiSystem *ui,
                                 const VkrLightingSystem *lighting) {
  const VkrMaterialTextureStreamStats texture_streams =
      vkr_material_system_get_texture_stream_stats(&assets->material_system);
  return (VkrApplicationMetricsSnapshot){
      .ui_dirty_tile_ratio = ui->dirty_tile_ratio,
      .ui_dirty_tiles = ui->dirty_tile_count,
      .ui_tile_count = ui->tile_count,
      .lighting_point_selected = lighting->point_light_count,
      .lighting_point_dropped = lighting->point_light_dropped_count,
      .lighting_point_grid_cells = lighting->point_light_grid.cell_count,
      .lighting_point_grid_references =
          lighting->point_light_grid.reference_count,
      .lighting_point_grid_max_lights_per_cell =
          lighting->point_light_grid.max_lights_per_cell,
      .lighting_point_grid_global_lights =
          lighting->point_light_grid.global_light_count,
      .texture_transcode_cache_hits =
          vkr_atomic_uint64_load(&assets->texture_system.transcode_cache_hits,
                                 VKR_MEMORY_ORDER_RELAXED),
      .texture_transcode_cache_misses =
          vkr_atomic_uint64_load(&assets->texture_system.transcode_cache_misses,
                                 VKR_MEMORY_ORDER_RELAXED),
      .texture_transcode_cache_writes =
          vkr_atomic_uint64_load(&assets->texture_system.transcode_cache_writes,
                                 VKR_MEMORY_ORDER_RELAXED),
      .material_texture_stream_pending = texture_streams.pending_count,
      .material_texture_stream_demanded_missing =
          texture_streams.demanded_missing_count,
      .material_texture_stream_demanded_evicted =
          texture_streams.demanded_evicted_count,
      .material_texture_stream_in_flight =
          assets->material_system.texture_stream_active_count,
      .material_texture_stream_resident =
          assets->material_system.texture_stream_resident_count,
      .material_texture_stream_evicted =
          assets->material_system.texture_stream_evicted_count,
      .material_texture_stream_resident_bytes =
          assets->material_system.texture_stream_resident_bytes,
      .material_texture_stream_budget_bytes =
          assets->material_system.texture_stream_budget_bytes,
      .material_texture_stream_applied =
          assets->material_system.texture_stream_applied_total,
      .material_texture_stream_failed =
          assets->material_system.texture_stream_failed_total,
      .material_texture_stream_evicted_total =
          assets->material_system.texture_stream_evicted_total,
      .material_texture_stream_pressure_stalls =
          assets->material_system.texture_stream_pressure_stalls_total,
      .material_texture_stream_automatic_pressure_active =
          assets->texture_pressure_active ? 1u : 0u,
  };
}

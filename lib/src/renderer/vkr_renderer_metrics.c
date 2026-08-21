#include "renderer/vkr_renderer_metrics.h"

#include "core/vkr_json_writer.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vulkan/vkr_vulkan_renderer.h"

vkr_internal bool8_t vkr_renderer_metric_register_full(
    VkrMetrics *metrics, const char *name, VkrMetricDomain domain,
    VkrMetricKind kind, VkrMetricUnit unit, VkrMetricScalar scalar,
    VkrMetricWriter writer, bool8_t required, VkrMetricId *out_id) {
  const VkrMetricDescription description = {
      .name =
          string8_create_from_cstr((const uint8_t *)name, string_length(name)),
      .domain = domain,
      .kind = kind,
      .unit = unit,
      .scalar = scalar,
      .writer = writer,
      .required_when_enabled = required,
  };
  if (vkr_metrics_register(metrics, &description, out_id)) {
    return true_v;
  }
  log_error("Metric registration failed for '%s' (%u/%u slots used)", name,
            metrics ? metrics->slot_count : 0u, VKR_METRICS_MAX_SLOTS);
  return false_v;
}

vkr_internal bool8_t vkr_renderer_metric_register(
    VkrMetrics *metrics, const char *name, VkrMetricDomain domain,
    VkrMetricKind kind, VkrMetricUnit unit, VkrMetricScalar scalar,
    VkrMetricId *out_id) {
  return vkr_renderer_metric_register_full(
      metrics, name, domain, kind, unit, scalar,
      VKR_METRIC_WRITER_RENDER_THREAD, false_v, out_id);
}

vkr_internal bool8_t vkr_renderer_event_register(VkrMetrics *metrics,
                                                 const char *name,
                                                 VkrMetricDomain domain,
                                                 VkrMetricId *out_id) {
  // Event sources are written from loader and job threads, never the render
  // thread, so their slots must be concurrent.
  return vkr_renderer_metric_register_full(
      metrics, name, domain, VKR_METRIC_KIND_DURATION,
      VKR_METRIC_UNIT_NANOSECONDS, VKR_METRIC_SCALAR_U64,
      VKR_METRIC_WRITER_CONCURRENT, false_v, out_id);
}

/** Instantaneous u64 gauge. */
#define VKR_REGISTER_U64(FIELD, NAME, DOMAIN, UNIT)                            \
  if (!vkr_renderer_metric_register(metrics, NAME, DOMAIN,                     \
                                    VKR_METRIC_KIND_GAUGE, UNIT,               \
                                    VKR_METRIC_SCALAR_U64, &ids->FIELD))       \
  return false_v

/** Work-volume gauge whose absence makes a report incomplete. */
#define VKR_REGISTER_U64_REQUIRED(FIELD, NAME, DOMAIN, UNIT)                   \
  if (!vkr_renderer_metric_register_full(                                      \
          metrics, NAME, DOMAIN, VKR_METRIC_KIND_GAUGE, UNIT,                  \
          VKR_METRIC_SCALAR_U64, VKR_METRIC_WRITER_RENDER_THREAD, true_v,      \
          &ids->FIELD))                                                        \
  return false_v

#define VKR_REGISTER_F64(FIELD, NAME, DOMAIN, UNIT)                            \
  if (!vkr_renderer_metric_register(metrics, NAME, DOMAIN,                     \
                                    VKR_METRIC_KIND_GAUGE, UNIT,               \
                                    VKR_METRIC_SCALAR_F64, &ids->FIELD))       \
  return false_v

/**
 * Cumulative pull source published as a per-frame delta. The underlying
 * registry counts up for the process lifetime; a report that averaged the
 * running total would describe nothing, so the frame delta is what ships.
 */
#define VKR_REGISTER_COUNTER(FIELD, NAME, DOMAIN)                              \
  if (!vkr_renderer_metric_register(                                           \
          metrics, NAME, DOMAIN, VKR_METRIC_KIND_COUNTER,                      \
          VKR_METRIC_UNIT_COUNT, VKR_METRIC_SCALAR_U64, &ids->FIELD))          \
  return false_v

/**
 * Stable report names for the logical GPU allocation owners. Designated so a
 * reordered enumerator cannot silently rename a published row; the assertion
 * catches a new enumerator, which would otherwise leave a NULL here and be
 * found only when snprintf read it.
 */
vkr_internal const char
    *const vkr_gpu_allocation_owner_names[VKR_GPU_ALLOCATION_OWNER_COUNT] = {
        [VKR_GPU_ALLOCATION_OWNER_UNKNOWN] = "unknown",
        [VKR_GPU_ALLOCATION_OWNER_MESH] = "mesh",
        [VKR_GPU_ALLOCATION_OWNER_TEXTURE] = "texture",
        [VKR_GPU_ALLOCATION_OWNER_FONT] = "font",
        [VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH] = "render_graph",
        [VKR_GPU_ALLOCATION_OWNER_SHADER] = "shader",
        [VKR_GPU_ALLOCATION_OWNER_INSTANCE] = "instance",
        [VKR_GPU_ALLOCATION_OWNER_INDIRECT] = "indirect",
        [VKR_GPU_ALLOCATION_OWNER_STAGING] = "staging",
        [VKR_GPU_ALLOCATION_OWNER_READBACK] = "readback",
        [VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN] = "swapchain",
};
_Static_assert(VKR_GPU_ALLOCATION_OWNER_COUNT == 11,
               "A new VkrGpuAllocationOwner needs a report name here");

typedef struct VkrGpuOwnerMetricRowDescription {
  const char *suffix;
  VkrMetricKind kind;
  VkrMetricUnit unit;
  /**
   * True for rows derived from the backend's handle table. When that table
   * saturates the live and peak figures drift, while the cumulative totals
   * stay exact and keep publishing without an inexactness mark.
   */
  bool8_t follows_handle_table_exactness;
} VkrGpuOwnerMetricRowDescription;

vkr_internal const VkrGpuOwnerMetricRowDescription
    vkr_gpu_owner_metric_rows[VKR_GPU_OWNER_METRIC_ROW_COUNT] = {
        [VKR_GPU_OWNER_METRIC_ROW_LIVE_BYTES] = {"bytes.live",
                                                 VKR_METRIC_KIND_GAUGE,
                                                 VKR_METRIC_UNIT_BYTES, true_v},
        [VKR_GPU_OWNER_METRIC_ROW_PEAK_BYTES] = {"bytes.peak",
                                                 VKR_METRIC_KIND_GAUGE,
                                                 VKR_METRIC_UNIT_BYTES, true_v},
        [VKR_GPU_OWNER_METRIC_ROW_ALLOCATED_BYTES] = {"bytes.allocated",
                                                      VKR_METRIC_KIND_COUNTER,
                                                      VKR_METRIC_UNIT_BYTES,
                                                      false_v},
        [VKR_GPU_OWNER_METRIC_ROW_LIVE_ALLOCATIONS] = {"allocations.live",
                                                       VKR_METRIC_KIND_GAUGE,
                                                       VKR_METRIC_UNIT_COUNT,
                                                       true_v},
        [VKR_GPU_OWNER_METRIC_ROW_PEAK_ALLOCATIONS] = {"allocations.peak",
                                                       VKR_METRIC_KIND_GAUGE,
                                                       VKR_METRIC_UNIT_COUNT,
                                                       true_v},
        [VKR_GPU_OWNER_METRIC_ROW_CREATED_ALLOCATIONS] =
            {"allocations.created", VKR_METRIC_KIND_COUNTER,
             VKR_METRIC_UNIT_COUNT, false_v},
};

/**
 * @brief Projects one owner's totals onto the published row order.
 *
 * Keeping this beside the row table is what makes a new row a two-line change:
 * the enum, the description, and this projection are the only places the row
 * set appears.
 */
vkr_internal void vkr_gpu_owner_metric_row_values(
    const VkrGpuAllocationOwnerTotals *totals,
    VkrRendererCumulativeBaselines *baselines, uint32_t owner,
    uint64_t out_values[VKR_GPU_OWNER_METRIC_ROW_COUNT]) {
  out_values[VKR_GPU_OWNER_METRIC_ROW_LIVE_BYTES] = totals->live_bytes;
  out_values[VKR_GPU_OWNER_METRIC_ROW_PEAK_BYTES] = totals->peak_bytes;
  out_values[VKR_GPU_OWNER_METRIC_ROW_ALLOCATED_BYTES] =
      vkr_renderer_metrics_cumulative_delta(
          totals->total_bytes, &baselines->gpu_owner[owner].allocated_bytes);
  out_values[VKR_GPU_OWNER_METRIC_ROW_LIVE_ALLOCATIONS] =
      totals->live_allocation_count;
  out_values[VKR_GPU_OWNER_METRIC_ROW_PEAK_ALLOCATIONS] =
      totals->peak_allocation_count;
  out_values[VKR_GPU_OWNER_METRIC_ROW_CREATED_ALLOCATIONS] =
      vkr_renderer_metrics_cumulative_delta(
          totals->total_allocation_count,
          &baselines->gpu_owner[owner].allocations_created);
}

typedef struct VkrRendererImplMemoryMetricDescription {
  const char *name;
  VkrMetricKind kind;
  VkrMetricUnit unit;
} VkrRendererImplMemoryMetricDescription;

#define VKR_IMPL_GAUGE(NAME, UNIT) {NAME, VKR_METRIC_KIND_GAUGE, UNIT}
#define VKR_IMPL_COUNTER(NAME, UNIT) {NAME, VKR_METRIC_KIND_COUNTER, UNIT}

vkr_internal const VkrRendererImplMemoryMetricDescription
    vkr_renderer_impl_memory_metric_descriptions[] = {
        VKR_IMPL_GAUGE("memory.gpu.heaps.live", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.heaps.peak", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.heaps.created", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.heaps.failures.capacity",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.live", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.peak", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.created",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.requested.live",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.requested.retired",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.requested.peak",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.reserved.live",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.reserved.retired",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.reserved.peak",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.bytes.alignment_waste",
                         VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.free",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.suballocations.bytes.largest_free_range",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.retirements.live", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.retirements.collected",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.residency.allocations",
                       VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.resources.live", VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.resources.released",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.bytes",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.fragmentation",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.handles",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.range_metadata",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER(
            "memory.gpu.suballocations.failures.retirement_capacity",
            VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.stale_handle",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.suballocations.failures.native",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.driver.current_allocated_bytes",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.driver.recommended_budget_bytes",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_COUNTER("memory.gpu.rings.upload.acquires",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.rings.upload.reuses",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.rings.upload.busy_failures",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.rings.readback.acquires",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.rings.readback.reuses",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_COUNTER("memory.gpu.rings.readback.busy_failures",
                         VKR_METRIC_UNIT_COUNT),
        VKR_IMPL_GAUGE("memory.gpu.heaps.bytes.used.current",
                       VKR_METRIC_UNIT_BYTES),
        VKR_IMPL_GAUGE("memory.gpu.heaps.bytes.allocated.current",
                       VKR_METRIC_UNIT_BYTES),
#define VKR_IMPL_CLASS_ROWS(CLASS)                                             \
  VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS ".allocations.live", \
                 VKR_METRIC_UNIT_COUNT),                                       \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".allocations.retired",                                   \
                     VKR_METRIC_UNIT_COUNT),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".allocations.peak",                                      \
                     VKR_METRIC_UNIT_COUNT),                                   \
      VKR_IMPL_COUNTER("memory.gpu.suballocations.owner." CLASS                \
                       ".allocations.created",                                 \
                       VKR_METRIC_UNIT_COUNT),                                 \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.requested.live",                                  \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.requested.retired",                               \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.requested.peak",                                  \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.reserved.live",                                   \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.reserved.retired",                                \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_GAUGE("memory.gpu.suballocations.owner." CLASS                  \
                     ".bytes.reserved.peak",                                   \
                     VKR_METRIC_UNIT_BYTES),                                   \
      VKR_IMPL_COUNTER("memory.gpu.suballocations.owner." CLASS                \
                       ".bytes.alignment_waste",                               \
                       VKR_METRIC_UNIT_BYTES)
        VKR_IMPL_CLASS_ROWS("buffer"),
        VKR_IMPL_CLASS_ROWS("texture"),
#undef VKR_IMPL_CLASS_ROWS
#define VKR_IMPL_SLOT_ROWS(PREFIX)                                             \
  VKR_IMPL_GAUGE(PREFIX ".live", VKR_METRIC_UNIT_COUNT),                       \
      VKR_IMPL_GAUGE(PREFIX ".peak", VKR_METRIC_UNIT_COUNT),                   \
      VKR_IMPL_GAUGE(PREFIX ".capacity", VKR_METRIC_UNIT_COUNT),               \
      VKR_IMPL_COUNTER(PREFIX ".published", VKR_METRIC_UNIT_COUNT),            \
      VKR_IMPL_COUNTER(PREFIX ".retired", VKR_METRIC_UNIT_COUNT),              \
      VKR_IMPL_COUNTER(PREFIX ".collected", VKR_METRIC_UNIT_COUNT),            \
      VKR_IMPL_COUNTER(PREFIX ".failures.capacity", VKR_METRIC_UNIT_COUNT)
        VKR_IMPL_SLOT_ROWS("memory.gpu.descriptors.sampled_image"),
        VKR_IMPL_SLOT_ROWS("memory.gpu.descriptors.sampler"),
        VKR_IMPL_SLOT_ROWS("memory.gpu.descriptors.storage_image"),
        VKR_IMPL_SLOT_ROWS("memory.gpu.materials"),
#undef VKR_IMPL_SLOT_ROWS
};

_Static_assert(ArrayCount(vkr_renderer_impl_memory_metric_descriptions) <=
                   VKR_RENDERER_IMPL_MEMORY_METRIC_MAX,
               "Renderer implementation memory metric ID storage is too small");

#undef VKR_IMPL_COUNTER
#undef VKR_IMPL_GAUGE

bool8_t vkr_renderer_metrics_register(VkrRendererMetrics *renderer_metrics,
                                      VkrMetrics *metrics) {
  if (!renderer_metrics || !metrics || metrics->sealed) {
    return false_v;
  }
  MemZero(renderer_metrics, sizeof(*renderer_metrics));
  renderer_metrics->metrics = metrics;
  renderer_metrics->previous.gpu_memory_interval_contiguous = true_v;
  renderer_metrics->previous.impl_memory_interval_contiguous = true_v;
  VkrRendererMetricIds *ids = &renderer_metrics->ids;

  VKR_REGISTER_U64_REQUIRED(world_draws_collected, "draw.world.draws_collected",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_opaque_draws, "draw.world.opaque_draws",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_transmission_draws,
                            "draw.world.transmission_draws",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_transparent_draws,
                            "draw.world.transparent_draws",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_opaque_batches, "draw.world.opaque_batches",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_draws_issued, "draw.world.commands_issued",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_draw_calls_issued, "draw.world.calls_issued",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_batches_created, "draw.world.batches_created",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(world_draws_merged, "draw.world.draws_merged",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(world_indirect_draws_issued,
                   "draw.world.indirect_commands_issued",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(world_indirect_calls_issued,
                   "draw.world.indirect_calls_issued", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_F64(world_avg_batch_size, "draw.world.avg_batch_size",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(world_max_batch_size, "draw.world.max_batch_size",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(lighting_point_selected, "lighting.point.selected",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(lighting_point_dropped, "lighting.point.dropped",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(lighting_point_grid_cells, "lighting.point.grid.cells",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(lighting_point_grid_references,
                   "lighting.point.grid.references", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(lighting_point_grid_max_lights_per_cell,
                   "lighting.point.grid.max_lights_per_cell",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(lighting_point_grid_global_lights,
                   "lighting.point.grid.global_lights", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);

  VKR_REGISTER_U64_REQUIRED(visibility_objects_tested,
                            "visibility.objects_tested", VKR_METRIC_DOMAIN_DRAW,
                            VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64_REQUIRED(visibility_culled_camera,
                            "visibility.objects_culled_camera",
                            VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_without_bounds,
                   "visibility.objects_without_bounds", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_candidate_count,
                   "visibility.gpu_candidates.count", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_candidate_capacity,
                   "visibility.gpu_candidates.capacity", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_visible_count, "visibility.gpu_visible.count",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_bucket_opaque_single,
                   "visibility.gpu_visible.bucket.opaque_single",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_bucket_opaque_double,
                   "visibility.gpu_visible.bucket.opaque_double",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_bucket_cutout_single,
                   "visibility.gpu_visible.bucket.cutout_single",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_bucket_cutout_double,
                   "visibility.gpu_visible.bucket.cutout_double",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_compaction_overflow,
                   "visibility.gpu_visible.overflow", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_gpu_resolve_invalid,
                   "visibility.gbuffer.resolve_invalid", VKR_METRIC_DOMAIN_DRAW,
                   VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_candidate_count,
                   "visibility.transmission.gpu_candidates.count",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_visible_count,
                   "visibility.transmission.gpu_visible.count",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_bucket_opaque_single,
                   "visibility.transmission.gpu_visible.bucket.opaque_single",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_bucket_opaque_double,
                   "visibility.transmission.gpu_visible.bucket.opaque_double",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_bucket_cutout_single,
                   "visibility.transmission.gpu_visible.bucket.cutout_single",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_bucket_cutout_double,
                   "visibility.transmission.gpu_visible.bucket.cutout_double",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_gpu_compaction_overflow,
                   "visibility.transmission.gpu_visible.overflow",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_pixel_compaction_overflow,
                   "visibility.transmission.compact_pixels.overflow",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  const char *transmission_coverage_names[4] = {
      "visibility.transmission.covered_pixels.layer_0",
      "visibility.transmission.covered_pixels.layer_1",
      "visibility.transmission.covered_pixels.layer_2",
      "visibility.transmission.covered_pixels.layer_3",
  };
  for (uint32_t layer = 0u; layer < ArrayCount(transmission_coverage_names);
       ++layer) {
    if (!vkr_renderer_metric_register(
            metrics, transmission_coverage_names[layer], VKR_METRIC_DOMAIN_DRAW,
            VKR_METRIC_KIND_GAUGE, VKR_METRIC_UNIT_COUNT, VKR_METRIC_SCALAR_U64,
            &ids->visibility_transmission_covered_pixels[layer]))
      return false_v;
  }
  VKR_REGISTER_U64(visibility_transmission_coverage_extent_width,
                   "visibility.transmission.coverage_extent.width",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_coverage_extent_height,
                   "visibility.transmission.coverage_extent.height",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_hzb_rejected, "visibility.hzb.rejected",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_transmission_hzb_rejected,
                   "visibility.transmission.hzb.rejected",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(visibility_hzb_history_valid, "visibility.hzb.history_valid",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(geometry_megabuffer_vertex_capacity,
                   "geometry.megabuffer.vertex_capacity_bytes",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(geometry_megabuffer_index_capacity,
                   "geometry.megabuffer.index_capacity_bytes",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(geometry_megabuffer_live_bytes,
                   "geometry.megabuffer.live_bytes",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(geometry_megabuffer_fragmentation_bytes,
                   "geometry.megabuffer.fragmentation_bytes",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(geometry_megabuffer_high_water_bytes,
                   "geometry.megabuffer.high_water_bytes",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(geometry_megabuffer_rejected_publications,
                   "geometry.megabuffer.rejected_publications_total",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(geometry_megabuffer_generation_replacements,
                   "geometry.megabuffer.generation_replacements_total",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(geometry_megabuffer_generation,
                   "geometry.megabuffer.generation",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    char name[64];
#define VKR_REGISTER_CASCADE(FIELD, SUFFIX)                                    \
  snprintf(name, sizeof(name), "draw.shadow.cascade%u." SUFFIX, i);            \
  if (!vkr_renderer_metric_register(                                           \
          metrics, name, VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_KIND_GAUGE,        \
          VKR_METRIC_UNIT_COUNT, VKR_METRIC_SCALAR_U64, &ids->FIELD[i]))       \
  return false_v
    VKR_REGISTER_CASCADE(shadow_indirect_draws_opaque, "indirect_commands");
    VKR_REGISTER_CASCADE(shadow_indirect_calls_opaque, "indirect_calls");
    VKR_REGISTER_CASCADE(shadow_indirect_overflow, "indirect_overflow");
#undef VKR_REGISTER_CASCADE
  }

  VKR_REGISTER_U64(rg_live_images, "rendergraph.images.live",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(rg_peak_images, "rendergraph.images.peak",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(rg_live_image_bytes, "rendergraph.image_bytes.live",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(rg_peak_image_bytes, "rendergraph.image_bytes.peak",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(rg_live_buffers, "rendergraph.buffers.live",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(rg_peak_buffers, "rendergraph.buffers.peak",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(rg_live_buffer_bytes, "rendergraph.buffer_bytes.live",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(rg_peak_buffer_bytes, "rendergraph.buffer_bytes.peak",
                   VKR_METRIC_DOMAIN_RENDERGRAPH, VKR_METRIC_UNIT_BYTES);

  VKR_REGISTER_U64(upload_fence_waits, "upload.fence_waits",
                   VKR_METRIC_DOMAIN_UPLOAD, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(upload_queue_idle_waits, "upload.queue_idle_waits",
                   VKR_METRIC_DOMAIN_UPLOAD, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(upload_device_idle_waits, "upload.device_idle_waits",
                   VKR_METRIC_DOMAIN_UPLOAD, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(frame_upload_exhaustions, "frame.upload_exhaustions",
                   VKR_METRIC_DOMAIN_FRAME, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(frame_command_slot_waits, "frame.command_slot_waits",
                   VKR_METRIC_DOMAIN_FRAME, VKR_METRIC_UNIT_COUNT);
  // Durations are nanoseconds; the name deliberately carries no unit suffix.
  if (!vkr_renderer_metric_register(
          metrics, "cpu.backend_present", VKR_METRIC_DOMAIN_FRAME,
          VKR_METRIC_KIND_DURATION, VKR_METRIC_UNIT_NANOSECONDS,
          VKR_METRIC_SCALAR_U64, &ids->backend_present)) {
    return false_v;
  }

#define VKR_REGISTER_BOOT(FIELD, NAME)                                         \
  if (!vkr_renderer_metric_register(                                           \
          metrics, NAME, VKR_METRIC_DOMAIN_BOOT, VKR_METRIC_KIND_DURATION,     \
          VKR_METRIC_UNIT_NANOSECONDS, VKR_METRIC_SCALAR_U64, &ids->FIELD))    \
  return false_v
  VKR_REGISTER_BOOT(boot_instance, "boot.instance");
  VKR_REGISTER_BOOT(boot_device, "boot.device");
  VKR_REGISTER_BOOT(boot_target, "boot.target");
  VKR_REGISTER_BOOT(boot_systems, "boot.systems");
  VKR_REGISTER_BOOT(boot_graph, "boot.graph");
  VKR_REGISTER_BOOT(boot_scene, "boot.scene");
#undef VKR_REGISTER_BOOT

  VKR_REGISTER_U64(job_queue_depth, "job.queue_depth", VKR_METRIC_DOMAIN_JOB,
                   VKR_METRIC_UNIT_COUNT);
  // Both are point samples taken on the render thread, not time-weighted
  // utilization over the frame; the names say "at sample time" for that reason.
  VKR_REGISTER_U64(job_workers_busy, "job.workers_busy_at_sample",
                   VKR_METRIC_DOMAIN_JOB, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_F64(job_worker_busy_ratio, "job.worker_busy_ratio_at_sample",
                   VKR_METRIC_DOMAIN_JOB, VKR_METRIC_UNIT_RATIO);
  VKR_REGISTER_COUNTER(job_completed_total, "job.completed",
                       VKR_METRIC_DOMAIN_JOB);

  VKR_REGISTER_U64(instance_occupancy, "instance_buffer.occupancy",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(instance_capacity, "instance_buffer.capacity",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(instance_overflows, "instance_buffer.overflows",
                   VKR_METRIC_DOMAIN_DRAW, VKR_METRIC_UNIT_COUNT);

  VKR_REGISTER_U64(gpu_live_allocations, "memory.gpu.allocations.live",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(gpu_peak_allocations, "memory.gpu.allocations.peak",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_COUNTER(gpu_allocations_created,
                       "memory.gpu.allocations.created",
                       VKR_METRIC_DOMAIN_MEMORY_GPU);
  VKR_REGISTER_U64(gpu_max_allocations, "memory.gpu.allocations.limit",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(gpu_live_bytes, "memory.gpu.bytes.live",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(gpu_peak_bytes, "memory.gpu.bytes.peak",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_BYTES);
  VKR_REGISTER_U64(gpu_live_totals_exact, "memory.gpu.live_totals_exact",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(gpu_heap_usage_valid, "memory.gpu.heap_usage_valid",
                   VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_UNIT_COUNT);

  for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
    for (uint32_t row = 0; row < VKR_GPU_OWNER_METRIC_ROW_COUNT; ++row) {
      char name[64];
      snprintf(name, sizeof(name), "memory.gpu.owner.%s.%s",
               vkr_gpu_allocation_owner_names[owner],
               vkr_gpu_owner_metric_rows[row].suffix);
      if (!vkr_renderer_metric_register(
              metrics, name, VKR_METRIC_DOMAIN_MEMORY_GPU,
              vkr_gpu_owner_metric_rows[row].kind,
              vkr_gpu_owner_metric_rows[row].unit, VKR_METRIC_SCALAR_U64,
              &ids->gpu_owner[owner][row])) {
        return false_v;
      }
    }
  }

  VKR_REGISTER_COUNTER(pipelines_created, "pipeline.created",
                       VKR_METRIC_DOMAIN_PIPELINE);
  VKR_REGISTER_COUNTER(pipeline_binds, "pipeline.binds",
                       VKR_METRIC_DOMAIN_PIPELINE);
  VKR_REGISTER_COUNTER(redundant_binds_avoided,
                       "pipeline.redundant_binds_avoided",
                       VKR_METRIC_DOMAIN_PIPELINE);
  VKR_REGISTER_COUNTER(meshes_batched, "pipeline.meshes_batched",
                       VKR_METRIC_DOMAIN_PIPELINE);
  VKR_REGISTER_U64(frame_pipeline_changes, "pipeline.frame_changes",
                   VKR_METRIC_DOMAIN_PIPELINE, VKR_METRIC_UNIT_COUNT);
  VKR_REGISTER_U64(frame_redundant_binds_avoided,
                   "pipeline.frame_redundant_binds_avoided",
                   VKR_METRIC_DOMAIN_PIPELINE, VKR_METRIC_UNIT_COUNT);

  VKR_REGISTER_U64(cpu_live_bytes, "memory.cpu.bytes.live",
                   VKR_METRIC_DOMAIN_MEMORY_CPU, VKR_METRIC_UNIT_BYTES);
  static const char *tag_names[VKR_ALLOCATOR_MEMORY_TAG_MAX] = {
      "unknown",   "array",    "string",   "vector", "queue",
      "struct",    "buffer",   "renderer", "file",   "texture",
      "hashtable", "freelist", "vulkan",   "gpu",
  };
  for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "memory.cpu.tag.%s.bytes", tag_names[i]);
    if (!vkr_renderer_metric_register(
            metrics, name, VKR_METRIC_DOMAIN_MEMORY_CPU, VKR_METRIC_KIND_GAUGE,
            VKR_METRIC_UNIT_BYTES, VKR_METRIC_SCALAR_U64,
            &ids->cpu_tag_bytes[i])) {
      return false_v;
    }
  }

  if (!vkr_renderer_event_register(metrics, "pipeline.create",
                                   VKR_METRIC_DOMAIN_PIPELINE,
                                   &ids->pipeline_create_event) ||
      !vkr_renderer_event_register(metrics, "shader.load",
                                   VKR_METRIC_DOMAIN_ASSET,
                                   &ids->shader_load_event) ||
      !vkr_renderer_event_register(metrics, "shader.reflection",
                                   VKR_METRIC_DOMAIN_ASSET,
                                   &ids->shader_reflection_event) ||
      !vkr_renderer_event_register(metrics, "asset.hdr_decode",
                                   VKR_METRIC_DOMAIN_ASSET,
                                   &ids->hdr_decode_event) ||
      !vkr_renderer_event_register(metrics, "ibl.equirect_to_cube",
                                   VKR_METRIC_DOMAIN_RENDERGRAPH,
                                   &ids->ibl_conversion_event) ||
      !vkr_renderer_event_register(metrics, "ibl.convolution",
                                   VKR_METRIC_DOMAIN_RENDERGRAPH,
                                   &ids->ibl_convolution_event)) {
    return false_v;
  }
  static const char *asset_names[VKR_RENDERER_ASSET_METRIC_COUNT] = {
      "asset.texture_load", "asset.mesh_load",  "asset.material_load",
      "asset.font_load",    "asset.scene_load",
  };
  for (uint32_t i = 0; i < VKR_RENDERER_ASSET_METRIC_COUNT; ++i) {
    if (!vkr_renderer_event_register(metrics, asset_names[i],
                                     VKR_METRIC_DOMAIN_ASSET,
                                     &ids->asset_load_event[i])) {
      return false_v;
    }
  }

  renderer_metrics->producers = (VkrRendererMetricsProducerConfig){
      .pipeline_create = {metrics, ids->pipeline_create_event},
      .shader_load = {metrics, ids->shader_load_event},
      .shader_reflection = {metrics, ids->shader_reflection_event},
      .hdr_decode = {metrics, ids->hdr_decode_event},
      .ibl_conversion = {metrics, ids->ibl_conversion_event},
      .ibl_convolution = {metrics, ids->ibl_convolution_event},
  };
  for (uint32_t i = 0; i < VKR_RENDERER_ASSET_METRIC_COUNT; ++i) {
    renderer_metrics->producers.asset_load[i] =
        (VkrMetricEventProducer){metrics, ids->asset_load_event[i]};
  }

  return true_v;
}

#undef VKR_REGISTER_U64
#undef VKR_REGISTER_U64_REQUIRED
#undef VKR_REGISTER_F64
#undef VKR_REGISTER_COUNTER

static bool8_t vkr_renderer_metrics_register_impl_memory(
    VkrRendererMetrics *renderer_metrics) {
  VkrMetrics *metrics = renderer_metrics->metrics;
  VkrMetricId *ids = renderer_metrics->ids.impl_memory;
  uint32_t registered = 0;
  for (; registered < ArrayCount(vkr_renderer_impl_memory_metric_descriptions);
       ++registered) {
    const VkrRendererImplMemoryMetricDescription *description =
        &vkr_renderer_impl_memory_metric_descriptions[registered];
    if (!vkr_renderer_metric_register(metrics, description->name,
                                      VKR_METRIC_DOMAIN_MEMORY_GPU,
                                      description->kind, description->unit,
                                      VKR_METRIC_SCALAR_U64, &ids[registered]))
      break;
  }
  renderer_metrics->impl_memory_metric_count = registered;
  if (registered != ArrayCount(vkr_renderer_impl_memory_metric_descriptions)) {
    log_warn("Metrics catalog holds %u/%zu implementation memory rows",
             registered,
             ArrayCount(vkr_renderer_impl_memory_metric_descriptions));
    return false_v;
  }
  return true_v;
}

bool8_t vkr_renderer_metrics_register_device_memory(
    VkrRendererMetrics *renderer_metrics, VkrRendererFrontendHandle renderer) {
  if (!renderer_metrics || !renderer_metrics->metrics || !renderer ||
      renderer_metrics->metrics->sealed) {
    return false_v;
  }

  VkrDeviceMemoryStats stats = {0};
  if (!vkr_renderer_get_device_memory_stats(renderer, &stats)) {
    return false_v;
  }

  (void)vkr_renderer_metrics_register_impl_memory(renderer_metrics);

  // Two rows per memory type and three per heap. A device at the Vulkan
  // maxima would ask for more rows than the catalog can hold, so the count is
  // clamped to what actually fits. Collection is driven by these same counts,
  // which keeps a short catalog consistent rather than merely smaller.
  VkrMetrics *metrics = renderer_metrics->metrics;
  const uint32_t requested_types =
      Min(stats.memory_type_count, (uint32_t)VKR_DEVICE_MEMORY_TYPE_MAX);
  const uint32_t requested_heaps =
      Min(stats.heap_count, (uint32_t)VKR_DEVICE_MEMORY_HEAP_MAX);
  const uint32_t available = vkr_metrics_slots_available(metrics);
  uint32_t type_count = Min(requested_types, available / 2u);
  uint32_t heap_count =
      Min(requested_heaps, (available - type_count * 2u) / 3u);
  if (type_count < requested_types || heap_count < requested_heaps) {
    log_warn("Metrics catalog holds %u/%u memory types and %u/%u heaps; "
             "per-type/per-heap rows will be partial",
             type_count, requested_types, heap_count, requested_heaps);
  }

  VkrRendererMetricIds *ids = &renderer_metrics->ids;
  char name[64];
  for (uint32_t i = 0; i < type_count; ++i) {
    snprintf(name, sizeof(name), "memory.gpu.type.%u.bytes.live", i);
    if (!vkr_renderer_metric_register(
            metrics, name, VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_KIND_GAUGE,
            VKR_METRIC_UNIT_BYTES, VKR_METRIC_SCALAR_U64,
            &ids->gpu_type_live_bytes[i])) {
      type_count = i;
      heap_count = 0;
      break;
    }
    snprintf(name, sizeof(name), "memory.gpu.type.%u.allocations.live", i);
    if (!vkr_renderer_metric_register(
            metrics, name, VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_KIND_GAUGE,
            VKR_METRIC_UNIT_COUNT, VKR_METRIC_SCALAR_U64,
            &ids->gpu_type_live_allocations[i])) {
      type_count = i;
      heap_count = 0;
      break;
    }
  }
  for (uint32_t i = 0; i < heap_count; ++i) {
#define VKR_REGISTER_HEAP_ROW(FIELD, SUFFIX)                                   \
  snprintf(name, sizeof(name), "memory.gpu.heap.%u." SUFFIX, i);               \
  if (!vkr_renderer_metric_register(                                           \
          metrics, name, VKR_METRIC_DOMAIN_MEMORY_GPU, VKR_METRIC_KIND_GAUGE,  \
          VKR_METRIC_UNIT_BYTES, VKR_METRIC_SCALAR_U64, &ids->FIELD[i])) {     \
    heap_count = i;                                                            \
    break;                                                                     \
  }
    VKR_REGISTER_HEAP_ROW(gpu_heap_size_bytes, "size_bytes");
    VKR_REGISTER_HEAP_ROW(gpu_heap_usage_bytes, "usage_bytes");
    VKR_REGISTER_HEAP_ROW(gpu_heap_budget_bytes, "budget_bytes");
#undef VKR_REGISTER_HEAP_ROW
  }

  renderer_metrics->device_memory_type_count = type_count;
  renderer_metrics->device_memory_heap_count = heap_count;
  return true_v;
}

const VkrRendererMetricsProducerConfig *
vkr_renderer_metrics_get_producers(const VkrRendererMetrics *renderer_metrics) {
  return renderer_metrics ? &renderer_metrics->producers : NULL;
}

void vkr_renderer_metrics_set_scene_boot_ns(
    VkrRendererMetrics *renderer_metrics, uint64_t duration_ns) {
  if (renderer_metrics && renderer_metrics->boot_scene_ns == 0) {
    renderer_metrics->boot_scene_ns = duration_ns;
  }
}

bool8_t
vkr_renderer_metrics_prepare_pass_table(VkrRendererMetrics *renderer_metrics,
                                        VkrRendererFrontendHandle renderer,
                                        VkrAllocator *allocator) {
  if (!renderer_metrics || !renderer || !allocator) {
    return false_v;
  }
  const uint64_t capacity = VKR_RENDERER_IMPL_MAX_PASS_TIMINGS;
  VkrRendererMetricsPassSample *samples = vkr_allocator_alloc(
      allocator, capacity * sizeof(*samples), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!samples) {
    return false_v;
  }
  MemZero(samples, capacity * sizeof(*samples));
  renderer_metrics->passes.samples = samples;
  renderer_metrics->passes.capacity = (uint32_t)capacity;
  renderer_metrics->passes.count = 0;
  renderer_metrics->passes.truncated = false_v;
  return true_v;
}

vkr_internal void
vkr_renderer_metrics_collect_passes(VkrRendererMetrics *renderer_metrics,
                                    RendererFrontend *renderer,
                                    uint64_t cpu_frame_index) {
  VkrRendererMetricsPassTable *table = &renderer_metrics->passes;
  table->count = 0;
  table->truncated = false_v;
  table->cpu_frame_index = cpu_frame_index;
  {
    const VkrRendererImplSubmitResult *result = &renderer->timing_result;
    VkrRendererMetricsPassSample samples[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS] = {
        0};
    bool8_t valid =
        result->pass_timing_count > 0 &&
        result->pass_timing_count <= VKR_RENDERER_IMPL_MAX_PASS_TIMINGS;
    for (uint32_t i = 0; valid && i < result->pass_timing_count; ++i) {
      const VkrRendererImplPassTiming *source = &result->pass_timings[i];
      valid = isfinite(source->cpu_ms) && source->cpu_ms >= 0.0 &&
              (!source->valid ||
               (isfinite(source->gpu_ms) && source->gpu_ms >= 0.0));
      if (!valid) {
        break;
      }
      VkrRendererMetricsPassSample *sample = &samples[i];
      const uint64_t length =
          Min(string_length(source->name), (uint64_t)VKR_METRIC_NAME_MAX);
      MemCopy(sample->name, source->name, length);
      sample->name[length] = '\0';
      sample->name_length = (uint8_t)length;
      sample->cpu_ms = source->cpu_ms;
      sample->gpu_ms = source->gpu_ms;
      sample->cpu_frame_index = cpu_frame_index;
      sample->gpu_source_frame_index = result->source_frame_index;
      sample->gpu_source_submit_serial = result->submit_value;
      sample->gpu_valid = source->valid;
    }
    if (valid && vkr_renderer_metrics_publish_pass_samples(
                     renderer_metrics, samples, result->pass_timing_count,
                     cpu_frame_index)) {
      const uint64_t source_frame = result->source_frame_index;
      for (uint32_t i = 0; i < table->count; ++i) {
        table->samples[i].gpu_source_frame_index = source_frame;
      }
    }
    return;
  }
}

bool8_t vkr_renderer_metrics_publish_pass_samples(
    VkrRendererMetrics *renderer_metrics,
    const VkrRendererMetricsPassSample *samples, uint32_t sample_count,
    uint64_t cpu_frame_index) {
  if (!renderer_metrics ||
      (sample_count > 0 && (!samples || !renderer_metrics->passes.samples ||
                            renderer_metrics->passes.capacity == 0)))
    return false_v;
  VkrRendererMetricsPassTable *table = &renderer_metrics->passes;
  table->count = Min(sample_count, table->capacity);
  table->truncated = sample_count > table->count;
  table->cpu_frame_index = cpu_frame_index;
  if (table->count > 0)
    MemCopy(table->samples, samples,
            (uint64_t)table->count * sizeof(*table->samples));
  for (uint32_t i = 0; i < table->count; ++i)
    table->samples[i].cpu_frame_index = cpu_frame_index;
  return true_v;
}

static uint32_t vkr_renderer_metrics_impl_class_values(
    const VkrRendererImplMemoryClassMetrics *class_metrics, uint64_t *values) {
  uint32_t i = 0;
  values[i++] = class_metrics->live_allocations;
  values[i++] = class_metrics->retired_allocations;
  values[i++] = class_metrics->peak_allocations;
  values[i++] = class_metrics->allocations_created;
  values[i++] = class_metrics->live_requested_bytes;
  values[i++] = class_metrics->retired_requested_bytes;
  values[i++] = class_metrics->peak_requested_bytes;
  values[i++] = class_metrics->live_reserved_bytes;
  values[i++] = class_metrics->retired_reserved_bytes;
  values[i++] = class_metrics->peak_reserved_bytes;
  values[i++] = class_metrics->alignment_waste_bytes;
  return i;
}

static uint32_t vkr_renderer_metrics_impl_values(
    const VkrRendererImplMemoryMetrics *memory,
    uint64_t values[VKR_RENDERER_IMPL_MEMORY_METRIC_MAX]) {
  const VkrRendererImplMemoryMetrics *suballocations = memory;
  const uint64_t heap_live = memory->native_heap_count
                                 ? memory->native_heap_count
                                 : (memory->native_heap_size > 0 ? 1u : 0u);
  const uint64_t heap_peak = memory->native_heap_peak_count
                                 ? memory->native_heap_peak_count
                                 : heap_live;
  const uint64_t heaps_created =
      memory->native_heaps_created ? memory->native_heaps_created : heap_live;
  uint32_t i = 0;
  values[i++] = heap_live;
  values[i++] = heap_peak;
  values[i++] = heaps_created;
  values[i++] = memory->native_heap_capacity_failures;
  values[i++] = suballocations->live_allocations;
  values[i++] = suballocations->peak_allocations;
  values[i++] = suballocations->allocations_created;
  values[i++] = suballocations->live_requested_bytes;
  values[i++] = suballocations->retired_requested_bytes;
  values[i++] = suballocations->peak_requested_bytes;
  values[i++] = suballocations->live_reserved_bytes;
  values[i++] = suballocations->retired_reserved_bytes;
  values[i++] = suballocations->peak_reserved_bytes;
  values[i++] = suballocations->alignment_waste_bytes;
  values[i++] = suballocations->free_bytes;
  values[i++] = suballocations->largest_free_range;
  values[i++] = suballocations->retired_allocations;
  values[i++] = suballocations->retirements_collected;
  values[i++] = memory->residency_allocation_count;
  values[i++] = memory->native_live_resources;
  values[i++] = memory->native_resources_released;
  values[i++] = suballocations->byte_exhaustion_failures;
  values[i++] = suballocations->fragmentation_failures;
  values[i++] = suballocations->handle_exhaustion_failures;
  values[i++] = suballocations->range_metadata_failures;
  values[i++] = suballocations->retirement_capacity_failures;
  values[i++] = suballocations->stale_handle_failures;
  values[i++] = suballocations->native_allocation_failures;
  values[i++] = memory->driver_current_allocated_size;
  values[i++] = memory->driver_recommended_working_set_size;
  values[i++] = memory->upload_ring_acquires;
  values[i++] = memory->upload_ring_reuses;
  values[i++] = memory->upload_ring_busy_failures;
  values[i++] = memory->readback_ring_acquires;
  values[i++] = memory->readback_ring_reuses;
  values[i++] = memory->readback_ring_busy_failures;
  values[i++] = memory->native_heap_used_size;
  values[i++] = memory->native_heap_allocated_size;
  i += vkr_renderer_metrics_impl_class_values(
      &suballocations->classes[VKR_RENDERER_IMPL_MEMORY_CLASS_BUFFER],
      &values[i]);
  i += vkr_renderer_metrics_impl_class_values(
      &suballocations->classes[VKR_RENDERER_IMPL_MEMORY_CLASS_TEXTURE],
      &values[i]);
  for (uint32_t table_index = 0;
       table_index < VKR_RENDERER_IMPL_SLOT_TABLE_COUNT; ++table_index) {
    const VkrRendererImplSlotTableMetrics *table =
        &memory->slot_tables[table_index];
    values[i++] = table->live;
    values[i++] = table->peak;
    values[i++] = table->capacity;
    values[i++] = table->published;
    values[i++] = table->retired;
    values[i++] = table->collected;
    values[i++] = table->capacity_failures;
  }
  return i;
}

static void
vkr_renderer_metrics_collect_impl_memory(VkrRendererMetrics *renderer_metrics,
                                         RendererFrontend *renderer) {
  if (renderer_metrics->impl_memory_metric_count == 0)
    return;
  uint64_t values[VKR_RENDERER_IMPL_MEMORY_METRIC_MAX] = {0};
  const uint32_t value_count =
      vkr_renderer_metrics_impl_values(&renderer->timing_result.memory, values);
  if (value_count != ArrayCount(vkr_renderer_impl_memory_metric_descriptions)) {
    log_error("Implementation memory metric projection has %u values for %zu "
              "rows",
              value_count,
              ArrayCount(vkr_renderer_impl_memory_metric_descriptions));
    renderer_metrics->previous.impl_memory_interval_contiguous = false_v;
    return;
  }
  const uint32_t count =
      Min(value_count, renderer_metrics->impl_memory_metric_count);
  const bool8_t counters_valid =
      renderer_metrics->previous.impl_memory_interval_contiguous;
  for (uint32_t i = 0; i < count; ++i) {
    const VkrMetricId id = renderer_metrics->ids.impl_memory[i];
    if (vkr_renderer_impl_memory_metric_descriptions[i].kind ==
        VKR_METRIC_KIND_COUNTER) {
      const uint64_t delta = vkr_renderer_metrics_cumulative_delta(
          values[i], &renderer_metrics->previous.impl_memory[i]);
      if (counters_valid)
        vkr_metrics_counter_add(renderer_metrics->metrics, id, delta);
      else
        vkr_metrics_mark(renderer_metrics->metrics, id,
                         VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                         VKR_METRIC_REASON_NOT_SAMPLED);
    } else {
      vkr_metrics_gauge_set_u64(renderer_metrics->metrics, id, values[i]);
    }
  }
  renderer_metrics->previous.impl_memory_interval_contiguous = true_v;
}

void vkr_renderer_metrics_collect(
    VkrRendererMetrics *renderer_metrics,
    const VkrRendererMetricsCollectContext *context) {
#if !VKR_METRICS_ENABLED
  (void)renderer_metrics;
  (void)context;
  return;
#else
  VkrMetrics *metrics = renderer_metrics->metrics;
  VkrRendererMetricIds *ids = &renderer_metrics->ids;
  RendererFrontend *renderer = (RendererFrontend *)context->renderer;
  const VkrWorldBatchMetrics *world = &context->frame_metrics->world;
  const VkrShadowMetrics *shadow = &context->frame_metrics->shadow;
  const VkrVisibilityStats *visibility = context->visibility;

#define VKR_SET_U64(FIELD, VALUE)                                              \
  vkr_metrics_gauge_set_u64(metrics, ids->FIELD, (uint64_t)(VALUE))
#define VKR_SET_F64(FIELD, VALUE)                                              \
  vkr_metrics_gauge_set_f64(metrics, ids->FIELD, (float64_t)(VALUE))
  if (context->frame_metrics->backend_present_valid) {
    vkr_metrics_duration_add_ns(metrics, ids->backend_present,
                                context->frame_metrics->backend_present_ns);
  } else {
    vkr_metrics_mark(metrics, ids->backend_present,
                     VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                     VKR_METRIC_REASON_NOT_SAMPLED);
  }

#define VKR_SET_BOOT(FIELD, VALUE)                                             \
  do {                                                                         \
    if ((VALUE) > 0) {                                                         \
      vkr_metrics_duration_add_ns(metrics, ids->FIELD, (VALUE));               \
    } else {                                                                   \
      vkr_metrics_mark(metrics, ids->FIELD,                                    \
                       VKR_METRIC_AVAILABILITY_UNAVAILABLE,                    \
                       VKR_METRIC_REASON_NOT_READY);                           \
    }                                                                          \
  } while (0)
  VKR_SET_BOOT(boot_instance, renderer->boot_metrics.instance_ns);
  VKR_SET_BOOT(boot_device, renderer->boot_metrics.device_ns);
  VKR_SET_BOOT(boot_target, renderer->boot_metrics.target_ns);
  VKR_SET_BOOT(boot_systems, renderer->boot_metrics.systems_ns);
  VKR_SET_BOOT(boot_graph, renderer->boot_metrics.graph_ns);
  VKR_SET_BOOT(boot_scene, renderer_metrics->boot_scene_ns);
#undef VKR_SET_BOOT

  // Publishes a cumulative pull source as this frame's delta. `begin_frame`
  // zeroes counter slots, so a single add produces exactly the interval value.
#define VKR_SET_DELTA(FIELD, BASELINE, VALUE)                                  \
  do {                                                                         \
    const uint64_t current_ = (uint64_t)(VALUE);                               \
    vkr_metrics_counter_add(                                                   \
        metrics, ids->FIELD,                                                   \
        vkr_renderer_metrics_cumulative_delta(                                 \
            current_, &renderer_metrics->previous.BASELINE));                  \
  } while (0)

  VkrJobSystemMetrics jobs = {0};
  vkr_job_system_get_metrics(context->job_system, &jobs);
  VKR_SET_U64(job_queue_depth, jobs.queue_depth);
  VKR_SET_U64(job_workers_busy, jobs.busy_workers);
  VKR_SET_F64(job_worker_busy_ratio,
              jobs.worker_count > 0
                  ? (float64_t)jobs.busy_workers / (float64_t)jobs.worker_count
                  : 0.0);
  VKR_SET_DELTA(job_completed_total, jobs_completed, jobs.jobs_completed_total);

  VKR_SET_U64(instance_overflows, 0u);
  VKR_SET_U64(world_draws_collected, world->draws_collected);
  VKR_SET_U64(world_opaque_draws, world->opaque_draws);
  VKR_SET_U64(world_transmission_draws, world->transmission_draws);
  VKR_SET_U64(world_transparent_draws, world->transparent_draws);
  VKR_SET_U64(world_opaque_batches, world->opaque_batches);
  VKR_SET_U64(world_draws_issued, world->draws_issued);
  VKR_SET_U64(world_draw_calls_issued, world->draw_calls_issued);
  VKR_SET_U64(world_batches_created, world->batches_created);
  VKR_SET_U64(world_draws_merged, world->draws_merged);
  VKR_SET_U64(world_indirect_draws_issued, world->indirect_draws_issued);
  VKR_SET_U64(world_indirect_calls_issued, world->indirect_calls_issued);
  VKR_SET_F64(world_avg_batch_size, world->avg_batch_size);
  VKR_SET_U64(world_max_batch_size, world->max_batch_size);
  VKR_SET_U64(lighting_point_selected,
              renderer->lighting_system.point_light_count);
  VKR_SET_U64(lighting_point_dropped,
              renderer->lighting_system.point_light_dropped_count);
  VKR_SET_U64(lighting_point_grid_cells,
              renderer->lighting_system.point_light_grid.cell_count);
  VKR_SET_U64(lighting_point_grid_references,
              renderer->lighting_system.point_light_grid.reference_count);
  VKR_SET_U64(lighting_point_grid_max_lights_per_cell,
              renderer->lighting_system.point_light_grid.max_lights_per_cell);
  VKR_SET_U64(lighting_point_grid_global_lights,
              renderer->lighting_system.point_light_grid.global_light_count);

  VKR_SET_U64(visibility_objects_tested, visibility->objects_tested);
  VKR_SET_U64(visibility_culled_camera, visibility->objects_culled_camera);
  VKR_SET_U64(visibility_without_bounds, visibility->objects_without_bounds);
  VKR_SET_U64(visibility_candidate_count, world->gpu_candidate_count);
  VKR_SET_U64(visibility_candidate_capacity, world->gpu_candidate_capacity);
  VKR_SET_U64(visibility_transmission_candidate_count,
              world->transmission_gpu_candidate_count);
  VKR_SET_U64(visibility_hzb_history_valid, world->hzb_history_valid ? 1u : 0u);
  if (world->gpu_diagnostics_valid) {
    VKR_SET_U64(visibility_gpu_visible_count, world->gpu_visible_count);
    VKR_SET_U64(visibility_gpu_bucket_opaque_single,
                world->gpu_bucket_counts[0]);
    VKR_SET_U64(visibility_gpu_bucket_opaque_double,
                world->gpu_bucket_counts[1]);
    VKR_SET_U64(visibility_gpu_bucket_cutout_single,
                world->gpu_bucket_counts[2]);
    VKR_SET_U64(visibility_gpu_bucket_cutout_double,
                world->gpu_bucket_counts[3]);
    VKR_SET_U64(visibility_gpu_compaction_overflow,
                world->gpu_compaction_overflow_count);
    VKR_SET_U64(visibility_gpu_resolve_invalid,
                world->gpu_resolve_invalid_count);
    VKR_SET_U64(visibility_transmission_gpu_visible_count,
                world->transmission_gpu_visible_count);
    VKR_SET_U64(visibility_transmission_gpu_bucket_opaque_single,
                world->transmission_gpu_bucket_counts[0]);
    VKR_SET_U64(visibility_transmission_gpu_bucket_opaque_double,
                world->transmission_gpu_bucket_counts[1]);
    VKR_SET_U64(visibility_transmission_gpu_bucket_cutout_single,
                world->transmission_gpu_bucket_counts[2]);
    VKR_SET_U64(visibility_transmission_gpu_bucket_cutout_double,
                world->transmission_gpu_bucket_counts[3]);
    VKR_SET_U64(visibility_transmission_gpu_compaction_overflow,
                world->transmission_gpu_compaction_overflow_count);
    VKR_SET_U64(visibility_transmission_pixel_compaction_overflow,
                world->transmission_pixel_compaction_overflow_count);
    VKR_SET_U64(visibility_hzb_rejected, world->gpu_occlusion_culled_count);
    VKR_SET_U64(visibility_transmission_hzb_rejected,
                world->transmission_gpu_occlusion_culled_count);
  } else {
    const VkrMetricId diagnostic_ids[] = {
        ids->visibility_gpu_visible_count,
        ids->visibility_gpu_bucket_opaque_single,
        ids->visibility_gpu_bucket_opaque_double,
        ids->visibility_gpu_bucket_cutout_single,
        ids->visibility_gpu_bucket_cutout_double,
        ids->visibility_gpu_compaction_overflow,
        ids->visibility_gpu_resolve_invalid,
        ids->visibility_transmission_gpu_visible_count,
        ids->visibility_transmission_gpu_bucket_opaque_single,
        ids->visibility_transmission_gpu_bucket_opaque_double,
        ids->visibility_transmission_gpu_bucket_cutout_single,
        ids->visibility_transmission_gpu_bucket_cutout_double,
        ids->visibility_transmission_gpu_compaction_overflow,
        ids->visibility_transmission_pixel_compaction_overflow,
        ids->visibility_hzb_rejected,
        ids->visibility_transmission_hzb_rejected,
    };
    for (uint32_t i = 0u; i < ArrayCount(diagnostic_ids); ++i) {
      vkr_metrics_mark(metrics, diagnostic_ids[i],
                       VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                       VKR_METRIC_REASON_NOT_SAMPLED);
    }
  }
  const VkrRendererImplSubmitResult *completed = &renderer->timing_result;
  if (completed->transmission_coverage_valid) {
    for (uint32_t layer = 0u;
         layer < ArrayCount(completed->transmission_covered_pixels); ++layer) {
      vkr_metrics_gauge_set_u64(
          metrics, ids->visibility_transmission_covered_pixels[layer],
          completed->transmission_covered_pixels[layer]);
    }
    VKR_SET_U64(visibility_transmission_coverage_extent_width,
                completed->transmission_coverage_extent[0]);
    VKR_SET_U64(visibility_transmission_coverage_extent_height,
                completed->transmission_coverage_extent[1]);
  } else {
    for (uint32_t layer = 0u;
         layer < ArrayCount(ids->visibility_transmission_covered_pixels);
         ++layer) {
      vkr_metrics_mark(
          metrics, ids->visibility_transmission_covered_pixels[layer],
          VKR_METRIC_AVAILABILITY_UNAVAILABLE, VKR_METRIC_REASON_NOT_SAMPLED);
    }
    vkr_metrics_mark(
        metrics, ids->visibility_transmission_coverage_extent_width,
        VKR_METRIC_AVAILABILITY_UNAVAILABLE, VKR_METRIC_REASON_NOT_SAMPLED);
    vkr_metrics_mark(
        metrics, ids->visibility_transmission_coverage_extent_height,
        VKR_METRIC_AVAILABILITY_UNAVAILABLE, VKR_METRIC_REASON_NOT_SAMPLED);
  }
  const VkrGeometryMegabufferMetrics *mega = &world->geometry_megabuffer;
  VKR_SET_U64(geometry_megabuffer_vertex_capacity, mega->vertex_capacity_bytes);
  VKR_SET_U64(geometry_megabuffer_index_capacity, mega->index_capacity_bytes);
  VKR_SET_U64(geometry_megabuffer_live_bytes, mega->live_bytes);
  VKR_SET_U64(geometry_megabuffer_fragmentation_bytes,
              mega->fragmentation_bytes);
  VKR_SET_U64(geometry_megabuffer_high_water_bytes, mega->high_water_bytes);
  VKR_SET_U64(geometry_megabuffer_rejected_publications,
              mega->rejected_publications);
  VKR_SET_U64(geometry_megabuffer_generation_replacements,
              mega->generation_replacements);
  VKR_SET_U64(geometry_megabuffer_generation, mega->generation);

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    vkr_metrics_gauge_set_u64(metrics, ids->shadow_indirect_draws_opaque[i],
                              shadow->shadow_indirect_draws_opaque[i]);
    vkr_metrics_gauge_set_u64(metrics, ids->shadow_indirect_calls_opaque[i],
                              shadow->shadow_indirect_calls_opaque[i]);
    vkr_metrics_gauge_set_u64(metrics, ids->shadow_indirect_overflow[i],
                              shadow->shadow_indirect_overflow[i]);
  }

  VkrRenderGraphResourceStats rg = {0};
  const bool8_t rg_stats_valid =
      renderer->impl.kind == VKR_RENDERER_IMPL_VULKAN &&
      vkr_vulkan_renderer_graph_resource_stats(renderer->vulkan_renderer, &rg);
  if (rg_stats_valid) {
    VKR_SET_U64(rg_live_images, rg.live_image_textures);
    VKR_SET_U64(rg_peak_images, rg.peak_image_textures);
    VKR_SET_U64(rg_live_image_bytes, rg.live_image_bytes);
    VKR_SET_U64(rg_peak_image_bytes, rg.peak_image_bytes);
    VKR_SET_U64(rg_live_buffers, rg.live_buffers);
    VKR_SET_U64(rg_peak_buffers, rg.peak_buffers);
    VKR_SET_U64(rg_live_buffer_bytes, rg.live_buffer_bytes);
    VKR_SET_U64(rg_peak_buffer_bytes, rg.peak_buffer_bytes);
  }

  // OWNERSHIP: this call resets the backend's counters, so this collector is
  // its only permitted caller. A second caller would silently steal samples,
  // and the theft would look like an absence of upload stalls. Consumers read
  // these values from the published frame, never from the backend directly.
  VkrRendererUploadWaitStats waits = {0};
  if (vkr_renderer_get_and_reset_upload_wait_stats(renderer, &waits)) {
    VKR_SET_U64(upload_fence_waits, waits.fence_wait_count);
    VKR_SET_U64(upload_queue_idle_waits, waits.queue_wait_idle_count);
    VKR_SET_U64(upload_device_idle_waits, waits.device_wait_idle_count);
    VKR_SET_U64(frame_upload_exhaustions, waits.frame_upload_exhaustion_count);
  }

  // This collector is the sole reset owner; consumers use the published frame.
  uint64_t command_slot_wait_count = 0;
  if (vkr_renderer_get_and_reset_command_slot_wait_count(
          renderer, &command_slot_wait_count)) {
    VKR_SET_U64(frame_command_slot_waits, command_slot_wait_count);
  }

  VkrDeviceMemoryStats gpu = {0};
  if (vkr_renderer_get_device_memory_stats(renderer, &gpu)) {
    const bool8_t counters_valid =
        renderer_metrics->previous.gpu_memory_interval_contiguous;
    VKR_SET_U64(gpu_live_allocations, gpu.live_allocation_count);
    VKR_SET_U64(gpu_peak_allocations, gpu.peak_allocation_count);
    const uint64_t allocations_created = vkr_renderer_metrics_cumulative_delta(
        gpu.total_allocation_count,
        &renderer_metrics->previous.gpu_allocations_created);
    if (counters_valid) {
      vkr_metrics_counter_add(metrics, ids->gpu_allocations_created,
                              allocations_created);
    } else {
      vkr_metrics_mark(metrics, ids->gpu_allocations_created,
                       VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                       VKR_METRIC_REASON_NOT_SAMPLED);
    }
    VKR_SET_U64(gpu_max_allocations, gpu.max_allocation_count);
    VKR_SET_U64(gpu_live_bytes, gpu.live_bytes);
    VKR_SET_U64(gpu_peak_bytes, gpu.peak_bytes);
    VKR_SET_U64(gpu_live_totals_exact, gpu.live_totals_exact);
    VKR_SET_U64(gpu_heap_usage_valid, gpu.heap_usage_valid);
    for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
      uint64_t values[VKR_GPU_OWNER_METRIC_ROW_COUNT];
      vkr_gpu_owner_metric_row_values(
          &gpu.owners[owner], &renderer_metrics->previous, owner, values);
      for (uint32_t row = 0; row < VKR_GPU_OWNER_METRIC_ROW_COUNT; ++row) {
        const VkrMetricId id = ids->gpu_owner[owner][row];
        if (vkr_gpu_owner_metric_rows[row].kind == VKR_METRIC_KIND_COUNTER) {
          if (counters_valid) {
            vkr_metrics_counter_add(metrics, id, values[row]);
          } else {
            vkr_metrics_mark(metrics, id, VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                             VKR_METRIC_REASON_NOT_SAMPLED);
          }
        } else {
          vkr_metrics_gauge_set_u64(metrics, id, values[row]);
        }
        if (!gpu.live_totals_exact &&
            vkr_gpu_owner_metric_rows[row].follows_handle_table_exactness) {
          vkr_metrics_mark(metrics, id, VKR_METRIC_AVAILABILITY_INEXACT,
                           VKR_METRIC_REASON_SOURCE_INEXACT);
        }
      }
    }
    renderer_metrics->previous.gpu_memory_interval_contiguous = true_v;
    if (!gpu.live_totals_exact) {
      vkr_metrics_mark(metrics, ids->gpu_live_allocations,
                       VKR_METRIC_AVAILABILITY_INEXACT,
                       VKR_METRIC_REASON_SOURCE_INEXACT);
      vkr_metrics_mark(metrics, ids->gpu_live_bytes,
                       VKR_METRIC_AVAILABILITY_INEXACT,
                       VKR_METRIC_REASON_SOURCE_INEXACT);
    }
    const uint32_t type_count =
        Min(gpu.memory_type_count, renderer_metrics->device_memory_type_count);
    for (uint32_t i = 0; i < type_count; ++i) {
      vkr_metrics_gauge_set_u64(metrics, ids->gpu_type_live_bytes[i],
                                gpu.live_bytes_by_type[i]);
      vkr_metrics_gauge_set_u64(metrics, ids->gpu_type_live_allocations[i],
                                gpu.live_count_by_type[i]);
      if (!gpu.live_totals_exact) {
        vkr_metrics_mark(metrics, ids->gpu_type_live_bytes[i],
                         VKR_METRIC_AVAILABILITY_INEXACT,
                         VKR_METRIC_REASON_SOURCE_INEXACT);
        vkr_metrics_mark(metrics, ids->gpu_type_live_allocations[i],
                         VKR_METRIC_AVAILABILITY_INEXACT,
                         VKR_METRIC_REASON_SOURCE_INEXACT);
      }
    }
    const uint32_t heap_count =
        Min(gpu.heap_count, renderer_metrics->device_memory_heap_count);
    for (uint32_t i = 0; i < heap_count; ++i) {
      vkr_metrics_gauge_set_u64(metrics, ids->gpu_heap_size_bytes[i],
                                gpu.heap_size_bytes[i]);
      if (gpu.heap_usage_valid) {
        vkr_metrics_gauge_set_u64(metrics, ids->gpu_heap_usage_bytes[i],
                                  gpu.heap_usage_bytes[i]);
        vkr_metrics_gauge_set_u64(metrics, ids->gpu_heap_budget_bytes[i],
                                  gpu.heap_budget_bytes[i]);
      } else {
        vkr_metrics_mark(metrics, ids->gpu_heap_usage_bytes[i],
                         VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                         VKR_METRIC_REASON_UNSUPPORTED);
        vkr_metrics_mark(metrics, ids->gpu_heap_budget_bytes[i],
                         VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                         VKR_METRIC_REASON_UNSUPPORTED);
      }
    }
  } else {
    renderer_metrics->previous.gpu_memory_interval_contiguous = false_v;
    vkr_metrics_mark(metrics, ids->gpu_allocations_created,
                     VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                     VKR_METRIC_REASON_NOT_SAMPLED);
    for (uint32_t owner = 0; owner < VKR_GPU_ALLOCATION_OWNER_COUNT; ++owner) {
      for (uint32_t row = 0; row < VKR_GPU_OWNER_METRIC_ROW_COUNT; ++row) {
        if (vkr_gpu_owner_metric_rows[row].kind == VKR_METRIC_KIND_COUNTER) {
          vkr_metrics_mark(metrics, ids->gpu_owner[owner][row],
                           VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                           VKR_METRIC_REASON_NOT_SAMPLED);
        }
      }
    }
  }

  vkr_renderer_metrics_collect_impl_memory(renderer_metrics, renderer);

  // Vulkan constructs its complete immutable pipeline set during
  // renderer initialization. No vkCreate*Pipelines call is reachable from a
  // prepared frame, so the per-frame creation counter is valid and zero.
  if (vkr_renderer_get_backend_type(context->renderer) ==
      VKR_RENDERER_BACKEND_TYPE_VULKAN) {
    vkr_metrics_counter_add(metrics, ids->pipelines_created, 0u);
  } else {
    vkr_metrics_mark(metrics, ids->pipelines_created,
                     VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                     VKR_METRIC_REASON_UNSUPPORTED);
  }
  vkr_metrics_mark(metrics, ids->pipeline_binds,
                   VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);
  vkr_metrics_mark(metrics, ids->redundant_binds_avoided,
                   VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);
  vkr_metrics_mark(metrics, ids->meshes_batched,
                   VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);
  vkr_metrics_mark(metrics, ids->frame_pipeline_changes,
                   VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);
  vkr_metrics_mark(metrics, ids->frame_redundant_binds_avoided,
                   VKR_METRIC_AVAILABILITY_UNAVAILABLE,
                   VKR_METRIC_REASON_UNSUPPORTED);

  VkrAllocatorStatistics cpu = vkr_allocator_get_global_statistics();
  VKR_SET_U64(cpu_live_bytes, cpu.total_allocated);
  for (uint32_t i = 0; i < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++i) {
    vkr_metrics_gauge_set_u64(metrics, ids->cpu_tag_bytes[i],
                              cpu.tagged_allocs[i]);
  }
  vkr_renderer_metrics_collect_passes(renderer_metrics, renderer,
                                      context->cpu_frame_index);
#undef VKR_SET_U64
#undef VKR_SET_F64
#undef VKR_SET_DELTA
#endif
}

const VkrRendererMetricsPassTable *vkr_renderer_metrics_get_pass_table(
    const VkrRendererMetrics *renderer_metrics) {
  return renderer_metrics ? &renderer_metrics->passes : NULL;
}

bool8_t
vkr_renderer_metrics_read_frame(const VkrRendererMetrics *renderer_metrics,
                                const VkrMetricsFrame *frame,
                                VkrRendererFrameMetrics *out_frame_metrics,
                                VkrVisibilityStats *out_visibility,
                                VkrRenderGraphResourceStats *out_rg_stats) {
  if (!renderer_metrics || !frame) {
    return false_v;
  }
  const VkrRendererMetricIds *ids = &renderer_metrics->ids;

  // Each field is written only when its slot carried a sample, so an absent
  // metric leaves the caller's value alone instead of forcing a zero.
#define VKR_READ_U32(TARGET, ID)                                               \
  do {                                                                         \
    uint64_t value_ = 0;                                                       \
    if (vkr_metrics_frame_read_u64(frame, (ID), &value_)) {                    \
      (TARGET) = (uint32_t)value_;                                             \
    }                                                                          \
  } while (0)
#define VKR_READ_U64(TARGET, ID)                                               \
  do {                                                                         \
    uint64_t value_ = 0;                                                       \
    if (vkr_metrics_frame_read_u64(frame, (ID), &value_)) {                    \
      (TARGET) = value_;                                                       \
    }                                                                          \
  } while (0)

  bool8_t read_any = false_v;
  if (out_frame_metrics) {
    VkrWorldBatchMetrics *world = &out_frame_metrics->world;
    uint64_t collected = 0;
    read_any = vkr_metrics_frame_read_u64(frame, ids->world_draws_collected,
                                          &collected);
    if (read_any) {
      world->draws_collected = (uint32_t)collected;
    }
    VKR_READ_U32(world->opaque_draws, ids->world_opaque_draws);
    VKR_READ_U32(world->transmission_draws, ids->world_transmission_draws);
    VKR_READ_U32(world->transparent_draws, ids->world_transparent_draws);
    VKR_READ_U32(world->opaque_batches, ids->world_opaque_batches);
    VKR_READ_U32(world->draws_issued, ids->world_draws_issued);
    VKR_READ_U32(world->draw_calls_issued, ids->world_draw_calls_issued);
    VKR_READ_U32(world->batches_created, ids->world_batches_created);
    VKR_READ_U32(world->draws_merged, ids->world_draws_merged);
    VKR_READ_U32(world->indirect_draws_issued,
                 ids->world_indirect_draws_issued);
    VKR_READ_U32(world->indirect_calls_issued,
                 ids->world_indirect_calls_issued);
    VKR_READ_U32(world->max_batch_size, ids->world_max_batch_size);
    float64_t avg_batch_size = 0.0;
    if (vkr_metrics_frame_read_f64(frame, ids->world_avg_batch_size,
                                   &avg_batch_size)) {
      world->avg_batch_size = (float32_t)avg_batch_size;
    }

    VkrShadowMetrics *shadow = &out_frame_metrics->shadow;
    for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
      VKR_READ_U32(shadow->shadow_indirect_draws_opaque[i],
                   ids->shadow_indirect_draws_opaque[i]);
      VKR_READ_U32(shadow->shadow_indirect_calls_opaque[i],
                   ids->shadow_indirect_calls_opaque[i]);
      VKR_READ_U32(shadow->shadow_indirect_overflow[i],
                   ids->shadow_indirect_overflow[i]);
    }
  }

  if (out_visibility) {
    VKR_READ_U32(out_visibility->objects_tested,
                 ids->visibility_objects_tested);
    VKR_READ_U32(out_visibility->objects_culled_camera,
                 ids->visibility_culled_camera);
    VKR_READ_U32(out_visibility->objects_without_bounds,
                 ids->visibility_without_bounds);
  }

  if (out_rg_stats) {
    VKR_READ_U32(out_rg_stats->live_image_textures, ids->rg_live_images);
    VKR_READ_U32(out_rg_stats->peak_image_textures, ids->rg_peak_images);
    VKR_READ_U64(out_rg_stats->live_image_bytes, ids->rg_live_image_bytes);
    VKR_READ_U64(out_rg_stats->peak_image_bytes, ids->rg_peak_image_bytes);
    VKR_READ_U32(out_rg_stats->live_buffers, ids->rg_live_buffers);
    VKR_READ_U32(out_rg_stats->peak_buffers, ids->rg_peak_buffers);
    VKR_READ_U64(out_rg_stats->live_buffer_bytes, ids->rg_live_buffer_bytes);
    VKR_READ_U64(out_rg_stats->peak_buffer_bytes, ids->rg_peak_buffer_bytes);
  }

#undef VKR_READ_U32
#undef VKR_READ_U64
  return read_any;
}

vkr_internal const char *
vkr_metric_availability_name(VkrMetricAvailability availability) {
  switch (availability) {
  case VKR_METRIC_AVAILABILITY_VALID:
    return "valid";
  case VKR_METRIC_AVAILABILITY_INEXACT:
    return "inexact";
  case VKR_METRIC_AVAILABILITY_UNAVAILABLE:
  default:
    return "unavailable";
  }
}

vkr_internal const char *vkr_metric_kind_name(VkrMetricKind kind) {
  switch (kind) {
  case VKR_METRIC_KIND_COUNTER:
    return "counter";
  case VKR_METRIC_KIND_GAUGE:
    return "gauge";
  case VKR_METRIC_KIND_DURATION:
    return "duration";
  default:
    return "unknown";
  }
}

vkr_internal const char *vkr_metric_unit_name(VkrMetricUnit unit) {
  static const char *names[VKR_METRIC_UNIT_COUNT_MAX] = {
      "count", "bytes", "ns", "ratio", "percent", "count_per_s",
  };
  return unit < VKR_METRIC_UNIT_COUNT_MAX ? names[unit] : "unknown";
}

vkr_internal const char *vkr_metric_reason_name(VkrMetricReason reason) {
  switch (reason) {
  case VKR_METRIC_REASON_NONE:
    return "none";
  case VKR_METRIC_REASON_NOT_SAMPLED:
    return "not_sampled";
  case VKR_METRIC_REASON_DISABLED:
    return "disabled";
  case VKR_METRIC_REASON_UNSUPPORTED:
    return "unsupported";
  case VKR_METRIC_REASON_NOT_READY:
    return "not_ready";
  case VKR_METRIC_REASON_SOURCE_INEXACT:
    return "source_inexact";
  case VKR_METRIC_REASON_PUBLICATION_DROPPED:
    return "publication_dropped";
  default:
    return "unknown";
  }
}

vkr_internal bool8_t vkr_json_name(VkrJsonWriter *writer, const char *name) {
  return vkr_json_writer_name(
      writer,
      string8_create_from_cstr((const uint8_t *)name, string_length(name)));
}

vkr_internal bool8_t vkr_json_cstr(VkrJsonWriter *writer, const char *value) {
  return vkr_json_writer_string(
      writer,
      string8_create_from_cstr((const uint8_t *)value, string_length(value)));
}

vkr_internal bool8_t vkr_renderer_metric_write_sample(
    VkrJsonWriter *writer, const VkrMetricCatalogEntry *entry,
    const VkrMetricSample *sample) {
  if (!vkr_json_writer_begin_object(writer) || !vkr_json_name(writer, "name") ||
      !vkr_json_writer_string(
          writer, string8_create_from_cstr((const uint8_t *)entry->name,
                                           entry->name_length)) ||
      !vkr_json_name(writer, "kind") ||
      !vkr_json_cstr(writer, vkr_metric_kind_name(entry->kind)) ||
      !vkr_json_name(writer, "unit") ||
      !vkr_json_cstr(writer, vkr_metric_unit_name(entry->unit)) ||
      !vkr_json_name(writer, "availability") ||
      !vkr_json_cstr(writer,
                     vkr_metric_availability_name(sample->availability)) ||
      !vkr_json_name(writer, "reason") ||
      !vkr_json_cstr(writer, vkr_metric_reason_name(sample->reason)) ||
      !vkr_json_name(writer, "required") ||
      !vkr_json_writer_bool(writer, entry->required_when_enabled) ||
      !vkr_json_name(writer, "value")) {
    return false_v;
  }
  if (sample->availability == VKR_METRIC_AVAILABILITY_UNAVAILABLE) {
    if (!vkr_json_writer_null(writer)) {
      return false_v;
    }
  } else if (entry->kind == VKR_METRIC_KIND_DURATION) {
    if (!vkr_json_writer_begin_object(writer) ||
        !vkr_json_name(writer, "sum_ns") ||
        !vkr_json_writer_u64(writer, sample->value.duration.sum_ns) ||
        !vkr_json_name(writer, "count") ||
        !vkr_json_writer_u64(writer, sample->value.duration.count) ||
        !vkr_json_name(writer, "min_ns") ||
        !vkr_json_writer_u64(writer, sample->value.duration.min_ns) ||
        !vkr_json_name(writer, "max_ns") ||
        !vkr_json_writer_u64(writer, sample->value.duration.max_ns) ||
        !vkr_json_writer_end_object(writer)) {
      return false_v;
    }
  } else if (entry->scalar == VKR_METRIC_SCALAR_F64) {
    if (!vkr_json_writer_f64(writer, sample->value.f64)) {
      return false_v;
    }
  } else if (!vkr_json_writer_u64(writer, sample->value.u64)) {
    return false_v;
  }
  return vkr_json_writer_end_object(writer);
}

bool8_t vkr_renderer_metrics_write_json(VkrRendererMetrics *renderer_metrics,
                                        String8 path) {
  if (!renderer_metrics || !renderer_metrics->metrics) {
    return false_v;
  }
  VkrMetricsSnapshotView snapshot = {0};
  if (!vkr_metrics_snapshot_acquire(renderer_metrics->metrics, &snapshot)) {
    return false_v;
  }

  VkrMetricEvent event = {0};
  uint32_t event_count = 0;
  while (
      event_count < VKR_METRIC_EVENT_CAPACITY &&
      vkr_metrics_event_peek(renderer_metrics->metrics, event_count, &event)) {
    event_count++;
  }

  // The pass table is single-buffered and filled by the collecting thread,
  // while the snapshot above is a pinned publication. Recording both frame
  // indices lets a reader see whether they describe the same frame rather
  // than assume it.
  const VkrRendererMetricsPassTable *passes = &renderer_metrics->passes;
  const bool8_t passes_match_snapshot =
      passes->cpu_frame_index == snapshot.frame->cpu_frame_index;
  const uint32_t missing_required = vkr_metrics_frame_missing_required(
      renderer_metrics->metrics, snapshot.frame);

  VkrJsonFileWriter file = {0};
  bool8_t success = vkr_json_file_writer_begin(&file, path);
  VkrJsonWriter *writer = &file.writer;
#if VKR_METRICS_ENABLED
  const bool8_t compiled_enabled = true_v;
#else
  const bool8_t compiled_enabled = false_v;
#endif
  if (success) {
    success =
        vkr_json_writer_begin_object(writer) &&
        vkr_json_name(writer, "schema_version") &&
        vkr_json_writer_u64(writer, 1u) && vkr_json_name(writer, "kind") &&
        vkr_json_cstr(writer, "vkr.metrics.snapshot") &&
        vkr_json_name(writer, "instrumentation") &&
        vkr_json_writer_begin_object(writer) &&
        vkr_json_name(writer, "compiled_enabled") &&
        vkr_json_writer_bool(writer, compiled_enabled) &&
        vkr_json_name(writer, "pass_gpu_timings") &&
        vkr_json_writer_bool(
            writer, renderer_metrics->metrics->config.pass_gpu_timings) &&
        vkr_json_name(writer, "event_subjects") &&
        vkr_json_writer_bool(
            writer, renderer_metrics->metrics->config.event_subjects) &&
        vkr_json_writer_end_object(writer) &&
        vkr_json_name(writer, "cpu_frame_index") &&
        vkr_json_writer_u64(writer, snapshot.frame->cpu_frame_index) &&
        vkr_json_name(writer, "submit_serial") &&
        vkr_json_writer_u64(writer, snapshot.frame->submit_serial) &&
        vkr_json_name(writer, "publication_serial") &&
        vkr_json_writer_u64(writer, snapshot.frame->publication_serial) &&
        vkr_json_name(writer, "snapshot_publications_dropped") &&
        vkr_json_writer_u64(writer,
                            snapshot.frame->snapshot_publications_dropped) &&
        vkr_json_name(writer, "events_dropped") &&
        vkr_json_writer_u64(writer, snapshot.frame->events_dropped) &&
        vkr_json_name(writer, "missing_required_metrics") &&
        vkr_json_writer_u64(writer, missing_required) &&
        vkr_json_name(writer, "metrics") && vkr_json_writer_begin_array(writer);
  }
  uint32_t catalog_count = 0;
  const VkrMetricCatalogEntry *catalog =
      vkr_metrics_get_catalog(renderer_metrics->metrics, &catalog_count);
  for (uint32_t i = 0;
       success && i < snapshot.frame->slot_count && i < catalog_count; ++i) {
    success = vkr_renderer_metric_write_sample(writer, &catalog[i],
                                               &snapshot.frame->samples[i]);
  }
  if (success) {
    success = vkr_json_writer_end_array(writer) &&
              vkr_json_name(writer, "passes_truncated") &&
              vkr_json_writer_bool(writer, passes->truncated) &&
              vkr_json_name(writer, "passes_cpu_frame_index") &&
              vkr_json_writer_u64(writer, passes->cpu_frame_index) &&
              vkr_json_name(writer, "passes_match_snapshot") &&
              vkr_json_writer_bool(writer, passes_match_snapshot) &&
              vkr_json_name(writer, "passes") &&
              vkr_json_writer_begin_array(writer);
  }
  for (uint32_t i = 0; success && i < passes->count; ++i) {
    const VkrRendererMetricsPassSample *pass = &passes->samples[i];
    success = vkr_json_writer_begin_object(writer) &&
              vkr_json_name(writer, "name") &&
              vkr_json_writer_string(
                  writer, string8_create_from_cstr((const uint8_t *)pass->name,
                                                   pass->name_length)) &&
              vkr_json_name(writer, "cpu_ms") &&
              vkr_json_writer_f64(writer, pass->cpu_ms) &&
              vkr_json_name(writer, "cpu_frame_index") &&
              vkr_json_writer_u64(writer, pass->cpu_frame_index) &&
              vkr_json_name(writer, "gpu_ms") &&
              (pass->gpu_valid ? vkr_json_writer_f64(writer, pass->gpu_ms)
                               : vkr_json_writer_null(writer)) &&
              vkr_json_name(writer, "gpu_valid") &&
              vkr_json_writer_bool(writer, pass->gpu_valid) &&
              vkr_json_name(writer, "gpu_source_frame_index") &&
              (pass->gpu_valid
                   ? vkr_json_writer_u64(writer, pass->gpu_source_frame_index)
                   : vkr_json_writer_null(writer)) &&
              vkr_json_name(writer, "gpu_source_submit_serial") &&
              (pass->gpu_valid
                   ? vkr_json_writer_u64(writer, pass->gpu_source_submit_serial)
                   : vkr_json_writer_null(writer)) &&
              vkr_json_name(writer, "culled") &&
              vkr_json_writer_bool(writer, pass->culled) &&
              vkr_json_name(writer, "disabled") &&
              vkr_json_writer_bool(writer, pass->disabled) &&
              vkr_json_writer_end_object(writer);
  }
  if (success) {
    success =
        vkr_json_writer_end_array(writer) && vkr_json_name(writer, "events") &&
        vkr_json_writer_begin_object(writer) &&
        vkr_json_name(writer, "dropped") &&
        vkr_json_writer_u64(writer, snapshot.frame->events_dropped) &&
        vkr_json_name(writer, "subjects_truncated") &&
        vkr_json_writer_u64(writer, snapshot.frame->event_subjects_truncated) &&
        vkr_json_name(writer, "items") && vkr_json_writer_begin_array(writer);
  }
  uint32_t events_written = 0;
  for (uint32_t i = 0; success && i < event_count; ++i) {
    if (!vkr_metrics_event_peek(renderer_metrics->metrics, i, &event)) {
      success = false_v;
      break;
    }
    const uint32_t source_index = vkr_metric_id_index(event.source);
    // An event whose source no longer resolves is a defect in one row, not a
    // reason to discard the whole report; skip it and let the count of
    // written items differ from the count consumed.
    if (vkr_metric_id_generation(event.source) !=
            renderer_metrics->metrics->registry_generation ||
        source_index >= catalog_count) {
      log_warn("Dropping metrics event with unresolvable source id 0x%08x",
               event.source);
      continue;
    }
    const VkrMetricCatalogEntry *source = &catalog[source_index];
    success =
        vkr_json_writer_begin_object(writer) &&
        vkr_json_name(writer, "source") &&
        vkr_json_writer_string(
            writer, string8_create_from_cstr((const uint8_t *)source->name,
                                             source->name_length)) &&
        vkr_json_name(writer, "subject") &&
        vkr_json_writer_string(
            writer, string8_create_from_cstr((const uint8_t *)event.subject,
                                             event.subject_length)) &&
        vkr_json_name(writer, "status") &&
        vkr_json_cstr(writer, event.status == VKR_METRIC_EVENT_STATUS_SUCCESS
                                  ? "success"
                                  : "failed") &&
        vkr_json_name(writer, "start_ns") &&
        vkr_json_writer_u64(writer, event.start_ns) &&
        vkr_json_name(writer, "duration_ns") &&
        vkr_json_writer_u64(writer, event.duration_ns) &&
        vkr_json_name(writer, "bytes") &&
        vkr_json_writer_u64(writer, event.bytes) &&
        vkr_json_name(writer, "thread_id") &&
        vkr_json_writer_u64(writer, event.thread_id) &&
        vkr_json_name(writer, "subject_truncated") &&
        vkr_json_writer_bool(writer, event.subject_truncated) &&
        vkr_json_writer_end_object(writer);
    events_written += success ? 1u : 0u;
  }
  (void)events_written;
  if (success) {
    success = vkr_json_writer_end_array(writer) &&
              vkr_json_writer_end_object(writer) &&
              vkr_json_writer_end_object(writer) &&
              vkr_json_file_writer_commit(&file);
    if (success && event_count > 0) {
      success =
          vkr_metrics_event_consume(renderer_metrics->metrics, event_count);
    }
  } else if (file.active) {
    vkr_json_file_writer_abort(&file);
  }
  vkr_metrics_snapshot_release(renderer_metrics->metrics, &snapshot);
  return success;
}

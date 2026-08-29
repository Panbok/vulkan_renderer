/**
 * @file vkr_renderer_metrics.h
 * @brief Renderer-owned registration and collection for VkrMetrics.
 *
 * Naming contract: durations are stored and published in nanoseconds, so no
 * metric name carries a unit suffix. `cpu.render_submit` is nanoseconds
 * because its catalog unit says so, not because its name says otherwise.
 */
#pragma once

#include "core/vkr_metrics.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_visibility.h"

typedef enum VkrRendererAssetMetricSource {
  VKR_RENDERER_ASSET_METRIC_TEXTURE = 0,
  VKR_RENDERER_ASSET_METRIC_MESH,
  VKR_RENDERER_ASSET_METRIC_MATERIAL,
  VKR_RENDERER_ASSET_METRIC_FONT,
  VKR_RENDERER_ASSET_METRIC_SCENE,
  VKR_RENDERER_ASSET_METRIC_COUNT,
} VkrRendererAssetMetricSource;

/**
 * @brief The published rows for each `VkrGpuAllocationOwner` bucket.
 *
 * Order matches the row description table in vkr_renderer_metrics.c, which
 * owns each row's name suffix, kind, unit, and whether the handle table's
 * exactness applies to it. Adding a row is one enumerator plus one table entry;
 * registration and collection iterate this enum and need no change.
 */
typedef enum VkrGpuOwnerMetricRow {
  VKR_GPU_OWNER_METRIC_ROW_LIVE_BYTES = 0,
  VKR_GPU_OWNER_METRIC_ROW_PEAK_BYTES,
  VKR_GPU_OWNER_METRIC_ROW_ALLOCATED_BYTES,
  VKR_GPU_OWNER_METRIC_ROW_LIVE_ALLOCATIONS,
  VKR_GPU_OWNER_METRIC_ROW_PEAK_ALLOCATIONS,
  VKR_GPU_OWNER_METRIC_ROW_CREATED_ALLOCATIONS,
  VKR_GPU_OWNER_METRIC_ROW_COUNT,
} VkrGpuOwnerMetricRow;

typedef struct VkrRendererMetricsPassSample {
  char name[VKR_METRIC_NAME_MAX + 1u];
  uint8_t name_length;
  float64_t cpu_ms;
  float64_t gpu_ms;
  uint64_t cpu_frame_index;
  uint64_t gpu_source_frame_index;
  uint64_t gpu_source_submit_serial;
  bool8_t culled;
  bool8_t disabled;
  bool8_t gpu_valid;
  VkrRendererImplGpuTimingReason gpu_unavailable_reason;
} VkrRendererMetricsPassSample;

/**
 * @brief Per-pass rows for the most recently collected frame.
 *
 * This table is not part of the published snapshot triple-buffer: it is
 * single-buffered and owned by the collecting thread. `cpu_frame_index`
 * records which frame filled it so a consumer holding a pinned snapshot can
 * check the two describe the same frame instead of assuming it.
 */
typedef struct VkrRendererMetricsPassTable {
  VkrRendererMetricsPassSample *samples;
  uint32_t count;
  uint32_t capacity;
  uint64_t cpu_frame_index;
  bool8_t truncated;
} VkrRendererMetricsPassTable;

#define VKR_RENDERER_IMPL_MEMORY_METRIC_MAX 96u

typedef struct VkrRendererMetricIds {
  VkrMetricId world_draws_collected;
  VkrMetricId world_opaque_draws;
  VkrMetricId world_transmission_draws;
  VkrMetricId world_transparent_draws;
  VkrMetricId world_opaque_batches;
  VkrMetricId world_draws_issued;
  VkrMetricId world_draw_calls_issued;
  VkrMetricId world_batches_created;
  VkrMetricId world_draws_merged;
  VkrMetricId world_indirect_draws_issued;
  VkrMetricId world_indirect_calls_issued;
  VkrMetricId world_avg_batch_size;
  VkrMetricId world_max_batch_size;
  VkrMetricId lighting_ibl_probes_packed;
  VkrMetricId lighting_point_selected;
  VkrMetricId lighting_point_dropped;
  VkrMetricId lighting_point_grid_cells;
  VkrMetricId lighting_point_grid_references;
  VkrMetricId lighting_point_grid_max_lights_per_cell;
  VkrMetricId lighting_point_grid_global_lights;

  VkrMetricId visibility_objects_tested;
  VkrMetricId visibility_culled_camera;
  VkrMetricId visibility_without_bounds;
  VkrMetricId visibility_candidate_count;
  VkrMetricId visibility_static_candidate_count;
  VkrMetricId visibility_candidate_capacity;
  VkrMetricId visibility_gpu_visible_count;
  VkrMetricId visibility_gpu_bucket_opaque_single;
  VkrMetricId visibility_gpu_bucket_opaque_double;
  VkrMetricId visibility_gpu_bucket_cutout_single;
  VkrMetricId visibility_gpu_bucket_cutout_double;
  VkrMetricId visibility_gpu_compaction_overflow;
  VkrMetricId visibility_gpu_resolve_invalid;
  VkrMetricId visibility_transmission_candidate_count;
  VkrMetricId visibility_transmission_gpu_visible_count;
  VkrMetricId visibility_transmission_gpu_bucket_opaque_single;
  VkrMetricId visibility_transmission_gpu_bucket_opaque_double;
  VkrMetricId visibility_transmission_gpu_bucket_cutout_single;
  VkrMetricId visibility_transmission_gpu_bucket_cutout_double;
  VkrMetricId visibility_transmission_gpu_compaction_overflow;
  VkrMetricId visibility_transmission_pixel_compaction_overflow;
  VkrMetricId visibility_transmission_covered_pixels[4];
  VkrMetricId visibility_transmission_coverage_extent_width;
  VkrMetricId visibility_transmission_coverage_extent_height;
  VkrMetricId visibility_hzb_rejected;
  VkrMetricId visibility_transmission_hzb_rejected;
  VkrMetricId visibility_hzb_history_valid;
  VkrMetricId exposure_accepted_texels;
  VkrMetricId exposure_retained_low_bin;
  VkrMetricId exposure_retained_high_bin;
  VkrMetricId exposure_average_log_luminance;
  VkrMetricId exposure_target_ev;
  VkrMetricId exposure_adapted_ev;
  VkrMetricId exposure_multiplier;
  VkrMetricId exposure_reset_reasons;
  VkrMetricId geometry_megabuffer_vertex_capacity;
  VkrMetricId geometry_megabuffer_index_capacity;
  VkrMetricId geometry_megabuffer_vertex_live_bytes;
  VkrMetricId geometry_megabuffer_index_live_bytes;
  VkrMetricId geometry_megabuffer_live_bytes;
  VkrMetricId geometry_megabuffer_fragmentation_bytes;
  VkrMetricId geometry_megabuffer_high_water_bytes;
  VkrMetricId geometry_megabuffer_vertex_high_water_bytes;
  VkrMetricId geometry_megabuffer_index_high_water_bytes;
  VkrMetricId geometry_megabuffer_vertex_uploaded_bytes_total;
  VkrMetricId geometry_megabuffer_index_uploaded_bytes_total;
  VkrMetricId geometry_megabuffer_decode_metadata_live_bytes;
  VkrMetricId geometry_megabuffer_decode_metadata_high_water_bytes;
  VkrMetricId geometry_megabuffer_decode_metadata_uploaded_bytes_total;
  VkrMetricId geometry_megabuffer_rejected_publications;
  VkrMetricId geometry_megabuffer_generation_replacements;
  VkrMetricId geometry_megabuffer_generation;
  VkrMetricId mesh_source_bytes;
  VkrMetricId mesh_cooked_bytes;
  VkrMetricId mesh_decoded_bytes;
  VkrMetricId mesh_upload_bytes;
  VkrMetricId mesh_vertex_count;
  VkrMetricId mesh_index_count;
  VkrMetricId mesh_range_count;
  VkrMetricId mesh_live_assets;
  VkrMetricId mesh_source_assets;
  VkrMetricId mesh_cooked_assets;
  VkrMetricId mesh_runtime_optimized_assets;
  VkrMetricId mesh_cache_acmr_before;
  VkrMetricId mesh_cache_acmr_after;
  VkrMetricId mesh_cache_atvr_before;
  VkrMetricId mesh_cache_atvr_after;
  VkrMetricId mesh_fetch_overfetch_before;
  VkrMetricId mesh_fetch_overfetch_after;
  VkrMetricId texture_transcode_cache_hits;
  VkrMetricId texture_transcode_cache_misses;
  VkrMetricId texture_transcode_cache_writes;
  VkrMetricId material_texture_stream_pending;
  VkrMetricId material_texture_stream_in_flight;
  VkrMetricId material_texture_stream_resident;
  VkrMetricId material_texture_stream_evicted;
  VkrMetricId material_texture_stream_resident_bytes;
  VkrMetricId material_texture_stream_budget_bytes;
  VkrMetricId material_texture_stream_applied;
  VkrMetricId material_texture_stream_failed;
  VkrMetricId material_texture_stream_evicted_total;
  VkrMetricId material_texture_stream_pressure_stalls;
  VkrMetricId material_texture_stream_automatic_pressure_active;

  VkrMetricId shadow_indirect_draws_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_indirect_calls_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_indirect_overflow[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_rendered[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_reused[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_correctness_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_proactive_refreshed[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_dynamic_candidates_tested[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_dynamic_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrMetricId shadow_sdsm_status;
  VkrMetricId shadow_sdsm_source_lag;
  VkrMetricId shadow_sdsm_occupied_pixels;
  VkrMetricId shadow_sdsm_linear_near;
  VkrMetricId shadow_sdsm_linear_far;

  VkrMetricId rg_live_images;
  VkrMetricId rg_peak_images;
  VkrMetricId rg_live_image_bytes;
  VkrMetricId rg_peak_image_bytes;
  VkrMetricId rg_live_buffers;
  VkrMetricId rg_peak_buffers;
  VkrMetricId rg_live_buffer_bytes;
  VkrMetricId rg_peak_buffer_bytes;

  /* Packet-lowering CPU cost and the work volume it describes. Registered
     under one name set for every selected implementation so a frontend
     comparison is not backend-specific. */
  VkrMetricId packet_candidate_hash;
  VkrMetricId packet_candidate_pack;
  VkrMetricId packet_geometry_table_build;
  VkrMetricId packet_candidate_row_bytes;
  VkrMetricId packet_instance_row_bytes;
  VkrMetricId packet_static_candidate_row_bytes;
  VkrMetricId packet_dynamic_candidate_row_bytes;
  VkrMetricId packet_publication_omitted;
  VkrMetricId packet_geometry_row_bytes;

  VkrMetricId upload_fence_waits;
  VkrMetricId upload_queue_idle_waits;
  VkrMetricId upload_device_idle_waits;
  VkrMetricId frame_upload_exhaustions;
  VkrMetricId frame_command_slot_waits;
  VkrMetricId gpu_submission;
  VkrMetricId backend_present;

  VkrMetricId boot_instance;
  VkrMetricId boot_device;
  VkrMetricId boot_target;
  VkrMetricId boot_systems;
  VkrMetricId boot_graph;
  VkrMetricId boot_scene;

  VkrMetricId job_queue_depth;
  VkrMetricId job_workers_busy;
  VkrMetricId job_worker_busy_ratio;
  VkrMetricId job_completed_total;

  VkrMetricId instance_occupancy;
  VkrMetricId instance_capacity;
  VkrMetricId instance_overflows;

  VkrMetricId gpu_live_allocations;
  VkrMetricId gpu_peak_allocations;
  VkrMetricId gpu_allocations_created;
  VkrMetricId gpu_max_allocations;
  VkrMetricId gpu_live_bytes;
  VkrMetricId gpu_peak_bytes;
  VkrMetricId gpu_live_totals_exact;
  VkrMetricId gpu_heap_usage_valid;
  VkrMetricId gpu_owner[VKR_GPU_ALLOCATION_OWNER_COUNT]
                       [VKR_GPU_OWNER_METRIC_ROW_COUNT];
  VkrMetricId gpu_type_live_bytes[VKR_DEVICE_MEMORY_TYPE_MAX];
  VkrMetricId gpu_type_live_allocations[VKR_DEVICE_MEMORY_TYPE_MAX];
  VkrMetricId gpu_heap_size_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  VkrMetricId gpu_heap_usage_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  VkrMetricId gpu_heap_budget_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  /** Strategy-provided heap/suballocation rows registered at device init. */
  VkrMetricId impl_memory[VKR_RENDERER_IMPL_MEMORY_METRIC_MAX];

  VkrMetricId pipelines_created;
  VkrMetricId pipeline_binds;
  VkrMetricId redundant_binds_avoided;
  VkrMetricId meshes_batched;
  VkrMetricId frame_pipeline_changes;
  VkrMetricId frame_redundant_binds_avoided;

  VkrMetricId cpu_live_bytes;
  VkrMetricId cpu_tag_bytes[VKR_ALLOCATOR_MEMORY_TAG_MAX];

  VkrMetricId pipeline_create_event;
  VkrMetricId shader_load_event;
  VkrMetricId shader_reflection_event;
  VkrMetricId hdr_decode_event;
  VkrMetricId ibl_conversion_event;
  VkrMetricId ibl_convolution_event;
  VkrMetricId asset_load_event[VKR_RENDERER_ASSET_METRIC_COUNT];
} VkrRendererMetricIds;

typedef struct VkrRendererMetricsProducerConfig {
  VkrMetricEventProducer pipeline_create;
  VkrMetricEventProducer shader_load;
  VkrMetricEventProducer shader_reflection;
  VkrMetricEventProducer hdr_decode;
  VkrMetricEventProducer ibl_conversion;
  VkrMetricEventProducer ibl_convolution;
  VkrMetricEventProducer asset_load[VKR_RENDERER_ASSET_METRIC_COUNT];
} VkrRendererMetricsProducerConfig;

/**
 * @brief Last-seen values of cumulative pull sources.
 *
 * The pipeline registry, job system, and device-memory tracker count up for the
 * process lifetime. These baselines turn each pull into a per-frame delta so
 * the published counters describe this frame's work rather than the run's
 * running total.
 */
typedef struct VkrRendererCumulativeBaselines {
  uint64_t pipelines_created;
  uint64_t pipeline_binds;
  uint64_t redundant_binds_avoided;
  uint64_t meshes_batched;
  uint64_t jobs_completed;
  uint64_t gpu_allocations_created;
  uint64_t impl_memory[VKR_RENDERER_IMPL_MEMORY_METRIC_MAX];
  struct {
    uint64_t allocated_bytes;
    uint64_t allocations_created;
  } gpu_owner[VKR_GPU_ALLOCATION_OWNER_COUNT];
  /** False after a failed pull until one snapshot re-establishes baselines. */
  bool8_t gpu_memory_interval_contiguous;
  bool8_t impl_memory_interval_contiguous;
} VkrRendererCumulativeBaselines;

/** Advances one cumulative-source baseline and returns its interval delta. */
static INLINE uint64_t
vkr_renderer_metrics_cumulative_delta(uint64_t current, uint64_t *previous) {
  const uint64_t prior = *previous;
  *previous = current;
  return current >= prior ? current - prior : 0u;
}

typedef struct VkrRendererMetrics {
  VkrMetrics *metrics;
  VkrRendererMetricIds ids;
  VkrRendererMetricsPassTable passes;
  VkrRendererMetricsProducerConfig producers;
  VkrRendererCumulativeBaselines previous;
  uint64_t boot_scene_ns;
  uint32_t device_memory_type_count;
  uint32_t device_memory_heap_count;
  uint32_t impl_memory_metric_count;
} VkrRendererMetrics;

typedef struct VkrRendererMetricsCollectContext {
  VkrRendererFrontendHandle renderer;
  const VkrRendererFrameMetrics *frame_metrics;
  const VkrVisibilityStats *visibility;
  const VkrJobSystem *job_system;
  uint64_t cpu_frame_index;
  uint64_t submit_serial;
} VkrRendererMetricsCollectContext;

bool8_t vkr_renderer_metrics_register(VkrRendererMetrics *renderer_metrics,
                                      VkrMetrics *metrics);
/**
 * Publishes backend-produced pass samples into the bounded application table.
 * The source may alias neither the destination table nor outlive this call.
 */
bool8_t vkr_renderer_metrics_publish_pass_samples(
    VkrRendererMetrics *renderer_metrics,
    const VkrRendererMetricsPassSample *samples, uint32_t sample_count,
    uint64_t cpu_frame_index);
/**
 * @brief Registers the rows whose count only the device knows.
 *
 * Registers as many memory-type/heap rows as the catalog can still hold and
 * records how many succeeded. Running short degrades the report; it never
 * fails startup, because a renderer that will not boot on an unusual memory
 * layout is a worse outcome than a report missing per-heap rows.
 */
bool8_t vkr_renderer_metrics_register_device_memory(
    VkrRendererMetrics *renderer_metrics, VkrRendererFrontendHandle renderer);
bool8_t
vkr_renderer_metrics_prepare_pass_table(VkrRendererMetrics *renderer_metrics,
                                        VkrRendererFrontendHandle renderer,
                                        VkrAllocator *allocator);
const VkrRendererMetricsProducerConfig *
vkr_renderer_metrics_get_producers(const VkrRendererMetrics *renderer_metrics);
void vkr_renderer_metrics_set_scene_boot_ns(
    VkrRendererMetrics *renderer_metrics, uint64_t duration_ns);
void vkr_renderer_metrics_collect(
    VkrRendererMetrics *renderer_metrics,
    const VkrRendererMetricsCollectContext *context);
const VkrRendererMetricsPassTable *
vkr_renderer_metrics_get_pass_table(const VkrRendererMetrics *renderer_metrics);
bool8_t vkr_renderer_metrics_write_json(VkrRendererMetrics *renderer_metrics,
                                        String8 path);

/**
 * @brief Rebuilds the renderer's aggregate structs from a published frame.
 *
 * Consumers read the snapshot through this, not by unpacking slots by hand:
 * the mapping from slot to struct field belongs beside the registration that
 * created the slot. Fields whose slots are unavailable are left at the value
 * the caller supplied, so callers should zero-initialize rather than seed
 * from live renderer state if they want a pure snapshot view.
 *
 * @return True when at least the world batch metrics were readable.
 */
bool8_t
vkr_renderer_metrics_read_frame(const VkrRendererMetrics *renderer_metrics,
                                const VkrMetricsFrame *frame,
                                VkrRendererFrameMetrics *out_frame_metrics,
                                VkrVisibilityStats *out_visibility,
                                VkrRenderGraphResourceStats *out_rg_stats);

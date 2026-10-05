#pragma once
#include "vkr_asset_publisher.h"

#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "vkr_asset_metrics.h"
#include "vkr_dynamic_resolution.h"
#include "vkr_frame_input.h"
#include "vkr_geometry_upload.h"
#include "vkr_publication_queue.h"
#include "vkr_render_graph.h"
#include "vkr_renderer.h"
#include "vkr_renderer_impl.h"
#include "vkr_temporal.h"

typedef struct VkrMetalPacketRenderer VkrMetalPacketRenderer;
typedef struct VkrVulkanRenderer VkrVulkanRenderer;

/**
 * @brief Per-frame batching statistics for the world render path.
 *
 * Counts are reset at frame begin and updated by the world view after draw
 * collection. `draws_issued` counts logical indexed commands after CPU
 * instancing; `draw_calls_issued` counts actual direct/indirect API calls.
 */
typedef struct VkrWorldBatchMetrics {
  uint32_t draws_collected;
  uint32_t opaque_draws;
  uint32_t transmission_draws;
  uint32_t transparent_draws;
  uint32_t opaque_batches;
  /** Logical indexed commands represented by direct or indirect submission. */
  uint32_t draws_issued;
  /** Actual vkCmdDrawIndexed/vkCmdDrawIndexedIndirect calls recorded. */
  uint32_t draw_calls_issued;
  uint32_t batches_created;
  uint32_t draws_merged;
  /** Logical commands carried by multi-draw-indirect calls. */
  uint32_t indirect_draws_issued;
  /** Actual multi-draw-indirect calls recorded. */
  uint32_t indirect_calls_issued;
  float32_t avg_batch_size;
  uint32_t max_batch_size;
  uint32_t gpu_candidate_count;
  uint32_t static_gpu_candidate_count;
  uint32_t gpu_candidate_capacity;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t gpu_compaction_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  uint32_t gpu_occlusion_culled_count;
  uint32_t transmission_gpu_candidate_count;
  uint32_t transmission_gpu_visible_count;
  uint32_t transmission_gpu_bucket_counts[VKR_WORLD_DRAW_STATE_BUCKET_COUNT];
  uint32_t transmission_gpu_compaction_overflow_count;
  uint32_t transmission_gpu_resolve_invalid_count;
  uint32_t transmission_gpu_occlusion_culled_count;
  uint32_t transmission_pixel_compaction_overflow_count;
  bool8_t hzb_history_valid;
  bool8_t gpu_diagnostics_valid;
  VkrGeometryMegabufferMetrics geometry_megabuffer;
  VkrMeshManagerMetrics mesh_assets;
} VkrWorldBatchMetrics;

/** Per-frame GPU shadow-submission statistics, indexed by cascade. */
typedef struct VkrShadowMetrics {
  uint32_t shadow_indirect_draws_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_indirect_calls_opaque[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_indirect_overflow[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t rendered[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t reused[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t correctness_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t proactive_refreshed[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t dynamic_candidates_tested[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t dynamic_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t sdsm_status;
  uint32_t sdsm_source_lag;
  uint32_t sdsm_occupied_count;
  float32_t sdsm_linear_near;
  float32_t sdsm_linear_far;
} VkrShadowMetrics;

/**
 * @brief Aggregate per-frame renderer metrics.
 *
 * These values are reset at frame begin and consumed by UI/debug tooling.
 */
typedef struct VkrRendererFrameMetrics {
  VkrWorldBatchMetrics world;
  VkrShadowMetrics shadow;
  VkrPacketBuildMetrics packet_build;
  uint64_t gpu_submission_ns;
  VkrMetricReason gpu_submission_unavailable_reason;
  bool8_t gpu_submission_valid;
  uint64_t backend_present_ns;
  bool8_t backend_present_valid;
  VkrExposureDebugSample exposure;
} VkrRendererFrameMetrics;

/** Outcome of one submitted frame; see vkr_renderer_complete_frame. */
struct VkrRendererFrameResult {
  VkrRendererError error;
  VkrRendererFrameMetrics metrics;
  VkrValidationError validation;
  /** The acquisition the frame used; `acquired` is false when acquiring
   * failed. `frame.renderer` is NULL because the frame was consumed. */
  VkrFrame frame;
  bool8_t acquired;
  /** CPU time of an acquisition the renderer made itself, including the wait
   * for frame-slot reuse. */
  uint64_t acquire_ns;
  /** CPU time of validation, preparation, recording, submission and present. */
  uint64_t render_ns;
};

/* One submitted frame's work for the thread that renders it. */
typedef struct VkrRendererWork {
  /* True when the work acquires the frame itself with `config`. */
  bool8_t acquire;
  VkrFrameConfig config;
  VkrFrame frame;
  VkrFrameInput *input;
  VkrFrameHooks hooks;
  /* Publications recorded before the frame; run before its input is
     prepared. NULL when none were. */
  VkrPublicationBatch *publications;
} VkrRendererWork;

typedef struct VkrRendererWorker VkrRendererWorker;

struct VkrRenderer {
  VkrAllocator *instance_allocator;
  VkrNativeSurface surface;
  VkrPresentTargetConfig present_target;
  /** Validated scene-resolution scale and its current pixel extent. */
  float32_t render_scale;
  /** Scales vkr_renderer_set_render_scale accepts between frames: the
      upscaler is built once for this range. Equal bounds fix the scale. */
  float32_t render_scale_min;
  float32_t render_scale_max;
  VkrUpscaleMode upscale_mode;
  VkrGraphicsPipelineClass graphics_pipeline;
  /** The caller's dynamic-resolution request before normalization, kept so
      the controller can be switched on again between frames. */
  VkrDynamicResolutionConfig dynamic_resolution_request;
  VkrDynamicResolutionConfig dynamic_resolution_config;
  VkrDynamicResolutionState dynamic_resolution_state;
  /** Reconstructed Scene extent before native editor composition. */
  uint32_t scene_output_width;
  uint32_t scene_output_height;
  bool8_t scene_output_extent_overridden;
  uint32_t render_width;
  uint32_t render_height;
  VkrRendererBackendType backend_type;
  VkrRendererImpl impl;
  VkrMetalPacketRenderer *metal_renderer;
  VkrVulkanRenderer *vulkan_renderer;
  VkrAssetPublisher asset_publisher;
  VkrRendererImplSubmitResult timing_result;
  uint64_t timing_last_completed_submit_value;
  bool8_t timing_completed_ready;
  bool8_t supports_multi_draw_indirect;
  bool8_t supports_draw_indirect_first_instance;
  VkrRendererBootMetrics boot_metrics;

  VkrDMemory render_graph_dmemory;
  VkrAllocator render_graph_allocator;
  /* Last successfully submitted editor Scene image; native graph owns storage.
   */
  uint32_t editor_image_width;
  uint32_t editor_image_height;
  VkrTemporalState temporal_state;
  VkrFogGpuParams submitted_fog;
  VkrFroxelFogGpuParams submitted_froxel_fog;
  uint32_t temporal_reset_reasons;
  bool8_t temporal_enabled;
  VkrExposureState exposure_state;
  /** Newest completed automatic exposure multiplier; pre-exposure source. */
  float32_t observed_exposure;
  /** Exposure the latest prepared frame displays with; see
      vkr_renderer_get_display_exposure. */
  float32_t display_exposure;
  uint32_t exposure_reset_reasons;
  /** Diagnostic VKR_PRE_EXPOSURE_FORCE_STOPS override. */
  int32_t pre_exposure_forced_stops;
  bool8_t pre_exposure_forced;
  bool8_t bloom_forced_disabled;
  bool8_t gtao_forced_disabled;
  bool8_t ssr_forced_disabled;
  bool8_t ssgi_forced_disabled;
  /* The frame static generation volumetric fog history last saw, and the one
     it follows (vkr_froxel_fog_static_generation). */
  uint64_t froxel_static_seen;
  uint64_t froxel_static_followed;
  bool8_t fxaa_enabled;
  /* False only for the analytic reference path; a frame uses the display-linear
     target when its final pass filters (vkr_renderer_prepare_frame_data). */
  bool8_t post_transform_cache_enabled;
  bool8_t submitted_ssgi_enabled;

  // Per-frame render statistics for UI/debug use.
  VkrRendererFrameMetrics frame_metrics;

  /* Probes the last accepted packet actually packed, published as
     lighting.ibl.probes_packed so a performance case can assert the work it
     believes it is measuring. */
  uint32_t ibl_probes_packed;

  // Last target dimensions applied on the rendering thread.
  uint32_t last_window_width;
  uint32_t last_window_height;

  bool32_t frame_active;
  uint64_t frame_number;
  uint64_t target_generation;

  /* Render thread, or NULL to render submitted frames inline. While it works
     on a frame, it alone touches renderer and native state; every public
     entry point waits for it first. */
  VkrRendererWorker *worker;
  /* `asset_publisher` records into `publications`; the thread rendering the
     next frame runs the records through `native_publisher`. */
  VkrAssetPublisher native_publisher;
  VkrPublicationQueue publications;
  /* Written by the frame's work; the caller collects it after the wait. */
  VkrRendererFrameResult frame_result;
  bool8_t frame_result_ready;
};

/* Waits until no submitted frame is rendering. Public entry points call it
   before touching renderer or native state. The render thread itself never
   waits, so renderer code it runs may use public entry points. */
void vkr_renderer_join_render_thread(const VkrRenderer *renderer);

/* Embedded application instances use caller-owned storage. */
bool32_t vkr_renderer_initialize(VkrRenderer *renderer,
                                 VkrRendererBackendType type,
                                 const VkrNativeSurface *surface,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 VkrRendererError *out_error);

void vkr_renderer_destroy(VkrRenderer *renderer);

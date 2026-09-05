#pragma once

#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/vkr_dynamic_resolution.h"
#include "renderer/vkr_frame_input.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_renderer_impl.h"
#include "renderer/vkr_temporal.h"

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

struct VkrRenderer {
  VkrWindow *window;
  VkrPresentTargetConfig present_target;
  /** Validated cold scene-resolution scale and its current pixel extent. */
  float32_t render_scale;
  VkrUpscaleMode upscale_mode;
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
  VkrTemporalState temporal_state;
  uint32_t temporal_reset_reasons;
  bool8_t temporal_enabled;
  VkrExposureState exposure_state;
  uint32_t exposure_reset_reasons;
  bool8_t bloom_forced_disabled;
  bool8_t gtao_forced_disabled;

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
};

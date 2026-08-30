#pragma once

#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_exposure.h"
#include "renderer/vkr_renderer.h"

typedef enum VkrRendererImplKind {
  VKR_RENDERER_IMPL_METAL = 0,
  VKR_RENDERER_IMPL_VULKAN,
} VkrRendererImplKind;

/**
 * Implementation properties selected before frontend allocation. Device and
 * target-derived values are finalized during initialization; target values are
 * refreshed only at the recreate boundary and otherwise read as plain data.
 */
typedef struct VkrRendererImplCapabilities {
  uint64_t renderer_arena_size;
  uint64_t scratch_arena_size;
  uint64_t scratch_arena_block_size;
  uint32_t frame_in_flight_count;
  uint32_t present_target_image_count;
  VkrPresentTargetKind present_target_kind;
  VkrTextureFormat present_color_format;
  VkrTextureFormat present_depth_format;
  VkrTextureFormat shadow_depth_format;
} VkrRendererImplCapabilities;

enum {
  VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY = 64,
  VKR_RENDERER_IMPL_MAX_GRAPH_PASSES = 80,
  VKR_RENDERER_IMPL_MAX_PASS_TIMINGS = VKR_RENDERER_IMPL_MAX_GRAPH_PASSES,
  VKR_RENDERER_IMPL_DRAW_BUCKET_COUNT = 4,
  VKR_RENDERER_IMPL_SHADOW_CASCADE_COUNT = 8,
};

/** Why a requested GPU duration could not be published. */
typedef enum VkrRendererImplGpuTimingReason {
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE = 0,
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED,
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY,
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE,
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE,
  VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_ERROR,
} VkrRendererImplGpuTimingReason;

static INLINE const char *vkr_renderer_impl_gpu_timing_reason_name(
    VkrRendererImplGpuTimingReason reason) {
  switch (reason) {
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE:
    return "none";
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED:
    return "disabled";
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY:
    return "not_ready";
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE:
    return "unsupported_timestamp_scope";
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE:
    return "feedback_unavailable";
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_ERROR:
    return "feedback_error";
  default:
    return "unknown";
  }
}

typedef enum VkrRendererImplMemoryClass {
  VKR_RENDERER_IMPL_MEMORY_CLASS_UNKNOWN = 0,
  VKR_RENDERER_IMPL_MEMORY_CLASS_BUFFER,
  VKR_RENDERER_IMPL_MEMORY_CLASS_TEXTURE,
  VKR_RENDERER_IMPL_MEMORY_CLASS_COUNT,
} VkrRendererImplMemoryClass;

typedef struct VkrRendererImplMemoryClassMetrics {
  uint64_t live_requested_bytes;
  uint64_t live_reserved_bytes;
  uint64_t retired_requested_bytes;
  uint64_t retired_reserved_bytes;
  uint64_t peak_requested_bytes;
  uint64_t peak_reserved_bytes;
  uint64_t allocations_created;
  uint64_t live_allocations;
  uint64_t retired_allocations;
  uint64_t peak_allocations;
  uint64_t alignment_waste_bytes;
} VkrRendererImplMemoryClassMetrics;

typedef enum VkrRendererImplSlotTableKind {
  VKR_RENDERER_IMPL_SLOT_TABLE_SAMPLED_IMAGE = 0,
  VKR_RENDERER_IMPL_SLOT_TABLE_SAMPLER,
  VKR_RENDERER_IMPL_SLOT_TABLE_STORAGE_IMAGE,
  VKR_RENDERER_IMPL_SLOT_TABLE_MATERIAL,
  VKR_RENDERER_IMPL_SLOT_TABLE_COUNT,
} VkrRendererImplSlotTableKind;

typedef struct VkrRendererImplSlotTableMetrics {
  uint64_t live;
  uint64_t peak;
  uint64_t capacity;
  uint64_t published;
  uint64_t retired;
  uint64_t collected;
  uint64_t capacity_failures;
} VkrRendererImplSlotTableMetrics;

typedef struct VkrRendererImplMemoryMetrics {
  uint64_t heap_size;
  uint64_t free_bytes;
  uint64_t largest_free_range;
  uint64_t live_requested_bytes;
  uint64_t live_reserved_bytes;
  uint64_t retired_requested_bytes;
  uint64_t retired_reserved_bytes;
  uint64_t peak_requested_bytes;
  uint64_t peak_reserved_bytes;
  uint64_t allocations_created;
  uint64_t retirements_collected;
  uint64_t live_allocations;
  uint64_t retired_allocations;
  uint64_t peak_allocations;
  uint64_t alignment_waste_bytes;
  uint64_t byte_exhaustion_failures;
  uint64_t fragmentation_failures;
  uint64_t handle_exhaustion_failures;
  uint64_t range_metadata_failures;
  uint64_t retirement_capacity_failures;
  uint64_t stale_handle_failures;
  uint64_t native_allocation_failures;
  VkrRendererImplMemoryClassMetrics
      classes[VKR_RENDERER_IMPL_MEMORY_CLASS_COUNT];
  VkrRendererImplSlotTableMetrics
      slot_tables[VKR_RENDERER_IMPL_SLOT_TABLE_COUNT];
  uint64_t native_heap_count;
  uint64_t native_heap_peak_count;
  uint64_t native_heaps_created;
  uint64_t native_heap_capacity_failures;
  uint64_t native_heap_size;
  uint64_t native_heap_used_size;
  uint64_t native_heap_allocated_size;
  uint64_t native_heap_largest_free_range;
  uint64_t native_heap_peak_allocated_size;
  uint64_t driver_current_allocated_size;
  uint64_t driver_recommended_working_set_size;
  uint64_t residency_allocation_count;
  uint64_t native_live_resources;
  uint64_t native_resources_released;
  uint64_t upload_ring_acquires;
  uint64_t upload_ring_reuses;
  uint64_t upload_ring_busy_failures;
  uint64_t readback_ring_acquires;
  uint64_t readback_ring_reuses;
  uint64_t readback_ring_busy_failures;
} VkrRendererImplMemoryMetrics;

typedef struct VkrRendererImplMaterialMetrics {
  uint64_t rows_live;
  uint64_t rows_retired;
  uint64_t rows_peak;
  uint64_t rows_published;
  uint64_t rows_replaced;
  uint64_t rows_collected;
  uint64_t capacity_failures;
  uint64_t retirement_capacity_failures;
  uint64_t stale_handle_failures;
} VkrRendererImplMaterialMetrics;

typedef struct VkrRendererImplPassTiming {
  char name[VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY];
  float64_t cpu_ms;
  float64_t gpu_ms;
  uint32_t pass_index;
  bool8_t valid;
  VkrRendererImplGpuTimingReason unavailable_reason;
} VkrRendererImplPassTiming;

/**
 * @brief Backend-produced CPU cost of lowering one packet, per frame.
 *
 * Every selected implementation fills these with the same semantics, because a
 * cost comparison only one backend can produce cannot authorize a change to the
 * shared frontend. Durations are nanoseconds.
 *
 * The byte counts are the work volume the durations describe, and they are not
 * optional. Candidate and instance bytes count rows actually written, so a
 * publication-boundary omission changes work volume instead of looking like a
 * faster pack.
 *
 * `geometry_table_build_ns` covers the geometry-row clear and rebuild. Vulkan
 * performs that as a separate capacity walk. Metal writes rows during candidate
 * packing, so its geometry duration overlaps `candidate_pack_ns`; callers must
 * not sum those two Metal scopes.
 *
 * `valid` stays false until a submitted frame fills the block, so a missing
 * producer reads as unavailable rather than as zero cost.
 */
typedef struct VkrPacketBuildMetrics {
  uint64_t candidate_hash_ns;
  uint64_t candidate_pack_ns;
  uint64_t geometry_table_build_ns;
  uint64_t candidate_row_bytes;
  uint64_t instance_row_bytes;
  uint64_t static_candidate_row_bytes;
  uint64_t dynamic_candidate_row_bytes;
  uint32_t publication_omitted_count;
  uint64_t geometry_row_bytes;
  bool8_t valid;
} VkrPacketBuildMetrics;

/** Backend-neutral subset consumed by the frontend and shared metrics path. */
typedef struct VkrRendererImplSubmitResult {
  uint64_t submit_value;
  uint64_t source_frame_index;
  uint64_t gpu_submission_ns;
  VkrRendererImplGpuTimingReason gpu_submission_unavailable_reason;
  bool8_t gpu_submission_valid;
  uint32_t executed_pass_count;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t opaque_draw_count;
  uint32_t transmission_draw_count;
  uint32_t blend_draw_count;
  uint32_t gpu_visible_count;
  uint32_t gpu_bucket_counts[VKR_RENDERER_IMPL_DRAW_BUCKET_COUNT];
  uint32_t gpu_overflow_count;
  uint32_t gpu_resolve_invalid_count;
  uint32_t gpu_occlusion_culled_count;
  uint32_t transmission_gpu_visible_count;
  uint32_t transmission_gpu_bucket_counts[VKR_RENDERER_IMPL_DRAW_BUCKET_COUNT];
  uint32_t transmission_gpu_overflow_count;
  uint32_t transmission_gpu_resolve_invalid_count;
  uint32_t transmission_gpu_occlusion_culled_count;
  uint32_t transmission_compact_overflow_count;
  uint32_t shadow_gpu_visible_count[VKR_RENDERER_IMPL_SHADOW_CASCADE_COUNT];
  uint32_t shadow_gpu_bucket_counts[VKR_RENDERER_IMPL_SHADOW_CASCADE_COUNT]
                                   [VKR_RENDERER_IMPL_DRAW_BUCKET_COUNT];
  uint32_t shadow_gpu_overflow_count[VKR_RENDERER_IMPL_SHADOW_CASCADE_COUNT];
  bool8_t hzb_history_valid;
  VkrShadowDepthRangeSample shadow_depth_range;
  bool8_t has_gpu_draw_diagnostics;
  VkrExposureDebugSample exposure;
  uint32_t transmission_covered_pixels[5];
  uint32_t transmission_coverage_extent[2];
  bool8_t transmission_coverage_valid;
  VkrCapturePollResult capture;
  VkrRendererImplMemoryMetrics memory;
  VkrRendererImplMaterialMetrics materials;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
} VkrRendererImplSubmitResult;

/**
 * Coarse operations selected once. Normal successful frames call only
 * prepare_frame and submit_packet through this table; no entry is per pass or
 * per draw.
 */
typedef struct VkrRendererImplOps {
  bool32_t (*initialize)(void *state, VkrWindow *window, uint32_t width,
                         uint32_t height,
                         VkrDeviceRequirements *device_requirements,
                         const VkrRendererBackendConfig *backend_config,
                         VkrRendererError *out_error);
  void (*destroy)(void *state);
  void (*get_device_information)(void *state,
                                 VkrDeviceInformation *device_information,
                                 Arena *temp_arena);
  VkrRendererError (*wait_idle)(void *state);
  uint64_t (*get_submit_serial)(void *state);
  uint64_t (*get_completed_submit_serial)(void *state);
  bool8_t (*get_and_reset_upload_wait_stats)(
      void *state, VkrRendererUploadWaitStats *out_stats);
  bool8_t (*get_and_reset_command_slot_wait_count)(void *state,
                                                   uint64_t *out_wait_count);
  bool8_t (*get_device_memory_stats)(void *state,
                                     VkrDeviceMemoryStats *out_stats);
  bool8_t (*get_memory_metrics)(void *state,
                                VkrRendererImplMemoryMetrics *out_metrics);
  VkrRendererError (*prepare_frame)(void *state, VkrFrameSetup *out_setup);
  VkrRendererError (*submit_packet)(void *state, const VkrRenderPacket *packet,
                                    VkrRendererFrameMetrics *out_metrics,
                                    VkrValidationError *out_validation_error);
  VkrRendererError (*cancel_frame)(void *state);
  void (*resize)(void *state, uint32_t width, uint32_t height);
  VkrRendererError (*present_target_recreate)(void *state, uint32_t width,
                                              uint32_t height,
                                              uint32_t image_count);
  uint32_t (*frame_in_flight_index)(void *state);
  VkrCaptureStatus (*capture_poll)(void *state, VkrCaptureRequestId request_id,
                                   VkrCapturePollResult *out_result);
  bool8_t (*capture_release)(void *state, VkrCaptureRequestId request_id);
  bool8_t (*poll_submit_result)(void *state, uint64_t after_submit_value,
                                VkrRendererImplSubmitResult *out_result);
  VkrAllocator *(*get_allocator)(void *state);
} VkrRendererImplOps;

typedef struct VkrRendererImplStrategies {
  const VkrRendererImplOps *metal;
  const VkrRendererImplOps *vulkan;
} VkrRendererImplStrategies;

/** Coarse renderer implementation selected exactly once at initialization. */
typedef struct VkrRendererImpl {
  VkrRendererImplKind kind;
  VkrRendererImplCapabilities caps;
  const VkrRendererImplOps *ops;
  void *state;
  bool8_t initialization_supported;
} VkrRendererImpl;

/**
 * Selects immutable implementation properties. Vulkan is the
 * production Windows strategy and remains unavailable elsewhere.
 */
bool8_t vkr_renderer_impl_select(VkrRendererBackendType backend_type,
                                 VkrPresentTargetKind target_kind,
                                 const VkrRendererImplStrategies *strategies,
                                 VkrRendererImpl *out_impl);

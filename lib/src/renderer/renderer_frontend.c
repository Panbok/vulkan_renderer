#include "renderer/renderer_frontend.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/metal/vkr_metal_packet_renderer.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/scene_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_skybox_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_capture.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vkr_rg_json.h"
#include "renderer/vulkan/vkr_vulkan_renderer.h"

#include <math.h>

static bool8_t vkr_renderer_env_enabled(const char *name) {
  const char *value = name ? getenv(name) : NULL;
  return value && value[0] != '\0' && strcmp(value, "0") != 0 ? true_v
                                                              : false_v;
}

static VkrMetricReason
vkr_renderer_gpu_timing_metric_reason(VkrRendererImplGpuTimingReason reason) {
  switch (reason) {
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_DISABLED:
    return VKR_METRIC_REASON_DISABLED;
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_NOT_READY:
    return VKR_METRIC_REASON_NOT_READY;
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_UNSUPPORTED_TIMESTAMP_SCOPE:
    return VKR_METRIC_REASON_UNSUPPORTED;
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_UNAVAILABLE:
    return VKR_METRIC_REASON_PUBLICATION_DROPPED;
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_FEEDBACK_ERROR:
    return VKR_METRIC_REASON_NOT_SAMPLED;
  case VKR_RENDERER_IMPL_GPU_TIMING_REASON_NONE:
  default:
    return VKR_METRIC_REASON_NONE;
  }
}
#if defined(PLATFORM_APPLE)
_Static_assert(VKR_RENDERER_IMPL_DRAW_BUCKET_COUNT ==
                   VKR_WORLD_DRAW_STATE_BUCKET_COUNT,
               "backend-neutral and Metal draw bucket counts must match");
_Static_assert(VKR_RENDERER_IMPL_SHADOW_CASCADE_COUNT ==
                   VKR_SHADOW_CASCADE_COUNT_MAX,
               "backend-neutral and frontend cascade counts must match");

static void
vkr_renderer_impl_lower_metal_result(const VkrMetalPacketResult *source,
                                     VkrRendererImplSubmitResult *destination) {
  if (!source || !destination) {
    return;
  }
  *destination = (VkrRendererImplSubmitResult){
      .submit_value = source->submit_value,
      .source_frame_index = source->source_frame_index,
      .gpu_submission_ns = source->gpu_submission_ns,
      .gpu_submission_unavailable_reason =
          source->gpu_submission_unavailable_reason,
      .gpu_submission_valid = source->gpu_submission_valid,
      .executed_pass_count = source->executed_pass_count,
      .indexed_draw_count = source->indexed_draw_count,
      .shadow_draw_count = source->shadow_draw_count,
      .opaque_draw_count = source->opaque_draw_count,
      .transmission_draw_count = source->transmission_draw_count,
      .blend_draw_count = source->blend_draw_count,
      .gpu_visible_count = source->gpu_visible_count,
      .gpu_overflow_count = source->gpu_overflow_count,
      .gpu_resolve_invalid_count = source->gpu_resolve_invalid_count,
      .gpu_occlusion_culled_count = source->gpu_occlusion_culled_count,
      .transmission_gpu_visible_count = source->transmission_gpu_visible_count,
      .transmission_gpu_overflow_count =
          source->transmission_gpu_overflow_count,
      .transmission_gpu_occlusion_culled_count =
          source->transmission_gpu_occlusion_culled_count,
      .transmission_compact_overflow_count =
          source->transmission_compact_overflow_count,
      .hzb_history_valid = source->hzb_history_valid,
      .shadow_depth_range = source->shadow_depth_range,
      .has_gpu_draw_diagnostics = source->has_gpu_draw_diagnostics,
      .transmission_coverage_valid = source->has_transmission_coverage,
      .capture = source->capture,
      .materials =
          {
              .rows_live = source->materials.rows_live,
              .rows_retired = source->materials.rows_retired,
              .rows_peak = source->materials.rows_peak,
              .rows_published = source->materials.rows_published,
              .rows_replaced = source->materials.rows_replaced,
              .rows_collected = source->materials.rows_collected,
              .capacity_failures = source->materials.capacity_failures,
              .retirement_capacity_failures =
                  source->materials.retirement_capacity_failures,
              .stale_handle_failures = source->materials.stale_handle_failures,
          },
      .pass_timing_count =
          Min(source->pass_timing_count, VKR_RENDERER_IMPL_MAX_PASS_TIMINGS),
  };
  MemCopy(destination->transmission_covered_pixels,
          source->transmission_covered_pixels,
          sizeof(destination->transmission_covered_pixels));
  MemCopy(destination->transmission_coverage_extent,
          source->transmission_coverage_extent,
          sizeof(destination->transmission_coverage_extent));
  MemCopy(destination->gpu_bucket_counts, source->gpu_bucket_counts,
          sizeof(destination->gpu_bucket_counts));
  MemCopy(destination->transmission_gpu_bucket_counts,
          source->transmission_gpu_bucket_counts,
          sizeof(destination->transmission_gpu_bucket_counts));
  MemCopy(destination->shadow_gpu_visible_count,
          source->shadow_gpu_visible_count,
          sizeof(destination->shadow_gpu_visible_count));
  MemCopy(destination->shadow_gpu_bucket_counts,
          source->shadow_gpu_bucket_counts,
          sizeof(destination->shadow_gpu_bucket_counts));
  MemCopy(destination->shadow_gpu_overflow_count,
          source->shadow_gpu_overflow_count,
          sizeof(destination->shadow_gpu_overflow_count));
  const VkrMetalMemoryMetrics *source_memory = &source->memory.suballocations;
  VkrRendererImplMemoryMetrics *memory = &destination->memory;
#define VKR_LOWER_MEMORY_FIELD(FIELD) memory->FIELD = source_memory->FIELD
  VKR_LOWER_MEMORY_FIELD(heap_size);
  VKR_LOWER_MEMORY_FIELD(free_bytes);
  VKR_LOWER_MEMORY_FIELD(largest_free_range);
  VKR_LOWER_MEMORY_FIELD(live_requested_bytes);
  VKR_LOWER_MEMORY_FIELD(live_reserved_bytes);
  VKR_LOWER_MEMORY_FIELD(retired_requested_bytes);
  VKR_LOWER_MEMORY_FIELD(retired_reserved_bytes);
  VKR_LOWER_MEMORY_FIELD(peak_requested_bytes);
  VKR_LOWER_MEMORY_FIELD(peak_reserved_bytes);
  VKR_LOWER_MEMORY_FIELD(allocations_created);
  VKR_LOWER_MEMORY_FIELD(retirements_collected);
  VKR_LOWER_MEMORY_FIELD(live_allocations);
  VKR_LOWER_MEMORY_FIELD(retired_allocations);
  VKR_LOWER_MEMORY_FIELD(peak_allocations);
  VKR_LOWER_MEMORY_FIELD(alignment_waste_bytes);
  VKR_LOWER_MEMORY_FIELD(byte_exhaustion_failures);
  VKR_LOWER_MEMORY_FIELD(fragmentation_failures);
  VKR_LOWER_MEMORY_FIELD(handle_exhaustion_failures);
  VKR_LOWER_MEMORY_FIELD(range_metadata_failures);
  VKR_LOWER_MEMORY_FIELD(retirement_capacity_failures);
  VKR_LOWER_MEMORY_FIELD(stale_handle_failures);
  VKR_LOWER_MEMORY_FIELD(native_allocation_failures);
#undef VKR_LOWER_MEMORY_FIELD
  for (uint32_t i = 0; i < VKR_RENDERER_IMPL_MEMORY_CLASS_COUNT; ++i) {
    const VkrMetalMemoryClassMetrics *source_class = &source_memory->classes[i];
    VkrRendererImplMemoryClassMetrics *destination_class = &memory->classes[i];
    *destination_class = (VkrRendererImplMemoryClassMetrics){
        .live_requested_bytes = source_class->live_requested_bytes,
        .live_reserved_bytes = source_class->live_reserved_bytes,
        .retired_requested_bytes = source_class->retired_requested_bytes,
        .retired_reserved_bytes = source_class->retired_reserved_bytes,
        .peak_requested_bytes = source_class->peak_requested_bytes,
        .peak_reserved_bytes = source_class->peak_reserved_bytes,
        .allocations_created = source_class->allocations_created,
        .live_allocations = source_class->live_allocations,
        .retired_allocations = source_class->retired_allocations,
        .peak_allocations = source_class->peak_allocations,
        .alignment_waste_bytes = source_class->alignment_waste_bytes,
    };
  }
#define VKR_LOWER_DEVICE_MEMORY_FIELD(FIELD)                                   \
  memory->FIELD = source->memory.FIELD
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_heap_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_heap_used_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_heap_allocated_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_heap_largest_free_range);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_heap_peak_allocated_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(driver_current_allocated_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(driver_recommended_working_set_size);
  VKR_LOWER_DEVICE_MEMORY_FIELD(residency_allocation_count);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_live_resources);
  VKR_LOWER_DEVICE_MEMORY_FIELD(native_resources_released);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_acquires);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_reuses);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_busy_failures);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_acquires);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_reuses);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_busy_failures);
#undef VKR_LOWER_DEVICE_MEMORY_FIELD
  for (uint32_t i = 0; i < destination->pass_timing_count; ++i) {
    const VkrMetalPacketPassTiming *source_timing = &source->pass_timings[i];
    VkrRendererImplPassTiming *destination_timing =
        &destination->pass_timings[i];
    MemCopy(destination_timing->name, source_timing->name,
            sizeof(destination_timing->name));
    destination_timing->cpu_ms = source_timing->cpu_ms;
    destination_timing->gpu_ms = source_timing->gpu_ms;
    destination_timing->pass_index = source_timing->pass_index;
    destination_timing->valid = source_timing->valid;
    destination_timing->unavailable_reason = source_timing->unavailable_reason;
  }
}
#endif

static void
vkr_renderer_record_gpu_candidate_metrics(RendererFrontend *renderer,
                                          const VkrRenderPacket *packet) {
  const uint32_t count =
      packet && packet->world ? packet->world->gpu_candidate_count : 0u;
  const uint32_t transmission_count =
      packet && packet->world ? packet->world->transmission_gpu_candidate_count
                              : 0u;
  renderer->frame_metrics.world.gpu_candidate_count = count;
  renderer->frame_metrics.world.static_gpu_candidate_count =
      packet && packet->world ? packet->world->static_candidate_count : 0u;
  renderer->frame_metrics.world.transmission_gpu_candidate_count =
      transmission_count;
  renderer->frame_metrics.world.gpu_candidate_capacity =
      VKR_GPU_DRAW_CANDIDATE_CAPACITY;
  vkr_mesh_manager_get_metrics(&renderer->mesh_manager,
                               &renderer->frame_metrics.world.mesh_assets);
  VkrGeometryMegabufferMetrics *mega =
      &renderer->frame_metrics.world.geometry_megabuffer;
#if defined(PLATFORM_APPLE)
  if (renderer->metal_renderer) {
    vkr_metal_packet_renderer_geometry_megabuffer_metrics(
        renderer->metal_renderer, mega);
    return;
  }
#endif
  vkr_vulkan_renderer_geometry_megabuffer_metrics(renderer->vulkan_renderer,
                                                  mega);
}

#define VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL MB(2)
#define VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE MB(32)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL MB(8)
#define VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE MB(256)

vkr_internal bool8_t
renderer_frontend_validate_render_graph(RendererFrontend *rf);
VkrRendererError vkr_renderer_begin_frame(VkrRendererFrontendHandle renderer,
                                          float64_t delta_time);
VkrRendererError vkr_renderer_end_frame(VkrRendererFrontendHandle renderer,
                                        float64_t delta_time);
VkrRendererError vkr_renderer_cancel_frame(VkrRendererFrontendHandle renderer);
static VkrRendererError
vkr_renderer_validation_fail(VkrValidationError *out_error,
                             VkrRendererError code, const char *field_path,
                             const char *message);
static bool32_t
renderer_impl_metal_initialize(void *state, VkrWindow *window, uint32_t width,
                               uint32_t height,
                               VkrDeviceRequirements *device_requirements,
                               const VkrRendererBackendConfig *backend_config,
                               VkrRendererError *out_error);
static bool32_t
renderer_impl_vulkan_initialize(void *state, VkrWindow *window, uint32_t width,
                                uint32_t height,
                                VkrDeviceRequirements *device_requirements,
                                const VkrRendererBackendConfig *backend_config,
                                VkrRendererError *out_error);
static void renderer_impl_vulkan_destroy(void *state);
static void renderer_impl_vulkan_get_device_information(
    void *state, VkrDeviceInformation *device_information, Arena *temp_arena);
static VkrRendererError renderer_impl_vulkan_wait_idle(void *state);
static uint64_t renderer_impl_vulkan_submit_serial(void *state);
static uint64_t renderer_impl_vulkan_completed_submit_serial(void *state);
static bool8_t
renderer_impl_vulkan_upload_wait_stats(void *state,
                                       VkrRendererUploadWaitStats *out_stats);
static bool8_t
renderer_impl_vulkan_command_slot_waits(void *state, uint64_t *out_wait_count);
static VkrRendererError
renderer_impl_vulkan_prepare_frame(void *state, VkrFrameSetup *out_setup);
static VkrRendererError
renderer_impl_vulkan_submit_packet(void *state, const VkrRenderPacket *packet,
                                   VkrRendererFrameMetrics *out_metrics,
                                   VkrValidationError *out_validation_error);
static VkrRendererError renderer_impl_vulkan_cancel_frame(void *state);
static void renderer_impl_vulkan_resize(void *state, uint32_t width,
                                        uint32_t height);
static VkrRendererError renderer_impl_vulkan_present_target_recreate(
    void *state, uint32_t width, uint32_t height, uint32_t image_count);
static uint32_t renderer_impl_vulkan_frame_in_flight_index(void *state);
static bool8_t renderer_impl_vulkan_poll_submit_result(
    void *state, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result);
static VkrCaptureStatus
renderer_impl_vulkan_capture_poll(void *state, VkrCaptureRequestId request_id,
                                  VkrCapturePollResult *out_result);
static bool8_t
renderer_impl_vulkan_capture_release(void *state,
                                     VkrCaptureRequestId request_id);
static VkrAllocator *renderer_impl_vulkan_allocator(void *state);
static void renderer_impl_metal_destroy(void *state);
static void renderer_impl_metal_get_device_information(
    void *state, VkrDeviceInformation *device_information, Arena *temp_arena);
static VkrRendererError renderer_impl_metal_wait_idle(void *state);
static uint64_t renderer_impl_metal_submit_serial(void *state);
static uint64_t renderer_impl_metal_completed_submit_serial(void *state);
static bool8_t
renderer_impl_metal_upload_wait_stats(void *state,
                                      VkrRendererUploadWaitStats *out_stats);
static bool8_t renderer_impl_metal_command_slot_waits(void *state,
                                                      uint64_t *out_wait_count);
static bool8_t
renderer_impl_metal_device_memory_stats(void *state,
                                        VkrDeviceMemoryStats *out_stats);
static bool8_t
renderer_impl_vulkan_device_memory_stats(void *state,
                                         VkrDeviceMemoryStats *out_stats);
static bool8_t
renderer_impl_no_memory_metrics(void *state,
                                VkrRendererImplMemoryMetrics *out_metrics);
static bool8_t
renderer_impl_metal_memory_metrics(void *state,
                                   VkrRendererImplMemoryMetrics *out_metrics);
static bool8_t
renderer_impl_vulkan_memory_metrics(void *state,
                                    VkrRendererImplMemoryMetrics *out_metrics);
static VkrRendererError
renderer_impl_metal_prepare_frame(void *state, VkrFrameSetup *out_setup);
static VkrRendererError
renderer_impl_metal_submit_packet(void *state, const VkrRenderPacket *packet,
                                  VkrRendererFrameMetrics *out_metrics,
                                  VkrValidationError *out_validation_error);
static VkrRendererError renderer_impl_metal_cancel_frame(void *state);
static void renderer_impl_no_resize(void *state, uint32_t width,
                                    uint32_t height);
static VkrRendererError renderer_impl_metal_present_target_recreate(
    void *state, uint32_t width, uint32_t height, uint32_t image_count);
static uint32_t renderer_impl_metal_frame_in_flight_index(void *state);
static VkrCaptureStatus
renderer_impl_metal_capture_poll(void *state, VkrCaptureRequestId request_id,
                                 VkrCapturePollResult *out_result);
static bool8_t
renderer_impl_metal_capture_release(void *state,
                                    VkrCaptureRequestId request_id);
static bool8_t
renderer_impl_no_submit_result(void *state, uint64_t after_submit_value,
                               VkrRendererImplSubmitResult *out_result);
static bool8_t
renderer_impl_metal_poll_submit_result(void *state, uint64_t after_submit_value,
                                       VkrRendererImplSubmitResult *out_result);
static VkrAllocator *renderer_impl_metal_allocator(void *state);

static const VkrRendererImplOps renderer_impl_metal_ops = {
    .initialize = renderer_impl_metal_initialize,
    .destroy = renderer_impl_metal_destroy,
    .get_device_information = renderer_impl_metal_get_device_information,
    .wait_idle = renderer_impl_metal_wait_idle,
    .get_submit_serial = renderer_impl_metal_submit_serial,
    .get_completed_submit_serial = renderer_impl_metal_completed_submit_serial,
    .get_and_reset_upload_wait_stats = renderer_impl_metal_upload_wait_stats,
    .get_and_reset_command_slot_wait_count =
        renderer_impl_metal_command_slot_waits,
    .get_device_memory_stats = renderer_impl_metal_device_memory_stats,
    .get_memory_metrics = renderer_impl_metal_memory_metrics,
    .prepare_frame = renderer_impl_metal_prepare_frame,
    .submit_packet = renderer_impl_metal_submit_packet,
    .cancel_frame = renderer_impl_metal_cancel_frame,
    .resize = renderer_impl_no_resize,
    .present_target_recreate = renderer_impl_metal_present_target_recreate,
    .frame_in_flight_index = renderer_impl_metal_frame_in_flight_index,
    .capture_poll = renderer_impl_metal_capture_poll,
    .capture_release = renderer_impl_metal_capture_release,
    .poll_submit_result = renderer_impl_metal_poll_submit_result,
    .get_allocator = renderer_impl_metal_allocator,
};

static const VkrRendererImplOps renderer_impl_vulkan_ops = {
    .initialize = renderer_impl_vulkan_initialize,
    .destroy = renderer_impl_vulkan_destroy,
    .get_device_information = renderer_impl_vulkan_get_device_information,
    .wait_idle = renderer_impl_vulkan_wait_idle,
    .get_submit_serial = renderer_impl_vulkan_submit_serial,
    .get_completed_submit_serial = renderer_impl_vulkan_completed_submit_serial,
    .get_and_reset_upload_wait_stats = renderer_impl_vulkan_upload_wait_stats,
    .get_and_reset_command_slot_wait_count =
        renderer_impl_vulkan_command_slot_waits,
    .get_device_memory_stats = renderer_impl_vulkan_device_memory_stats,
    .get_memory_metrics = renderer_impl_vulkan_memory_metrics,
    .prepare_frame = renderer_impl_vulkan_prepare_frame,
    .submit_packet = renderer_impl_vulkan_submit_packet,
    .cancel_frame = renderer_impl_vulkan_cancel_frame,
    .resize = renderer_impl_vulkan_resize,
    .present_target_recreate = renderer_impl_vulkan_present_target_recreate,
    .frame_in_flight_index = renderer_impl_vulkan_frame_in_flight_index,
    .capture_poll = renderer_impl_vulkan_capture_poll,
    .capture_release = renderer_impl_vulkan_capture_release,
    .poll_submit_result = renderer_impl_vulkan_poll_submit_result,
    .get_allocator = renderer_impl_vulkan_allocator,
};

static const VkrRendererImplStrategies renderer_impl_strategies = {
    .metal = &renderer_impl_metal_ops,
    .vulkan = &renderer_impl_vulkan_ops,
};

vkr_internal void
renderer_frontend_destroy_loader_async_allocators(RendererFrontend *rf) {
  if (!rf) {
    return;
  }

  if (rf->scene_async_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->scene_async_mutex);
    rf->scene_async_mutex = NULL;
  }
  if (rf->scene_async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->scene_async_allocator);
    rf->scene_async_allocator = (VkrAllocator){0};
    rf->scene_async_memory = (VkrDMemory){0};
  }
  if (rf->mesh_loader.async_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->mesh_loader.async_mutex);
    rf->mesh_loader.async_mutex = NULL;
  }
  if (rf->mesh_loader.async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->mesh_loader.async_allocator);
    rf->mesh_loader.async_allocator = (VkrAllocator){0};
    rf->mesh_loader.async_memory = (VkrDMemory){0};
  }
}

vkr_internal bool8_t vkr_renderer_on_window_resize(Event *event,
                                                   UserData user_data) {
  assert(event != NULL && "Event is NULL");
  assert(event->type == EVENT_TYPE_WINDOW_RESIZE &&
         "Event is not a window resize event");

  RendererFrontend *rf = (RendererFrontend *)user_data;
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return false_v;
  }

  VkrWindowResizeEventData *resize = (VkrWindowResizeEventData *)event->data;
  if (!resize) {
    log_error("VkrWindowResizeEventData is NULL");
    return false_v;
  }

  if (resize->width == 0 || resize->height == 0) {
    return true_v;
  }

  uint64_t packed = ((uint64_t)resize->width << 32) | (uint64_t)resize->height;
  vkr_atomic_uint64_store(&rf->pending_resize_mailbox, packed,
                          VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

vkr_internal bool8_t
renderer_frontend_validate_render_graph(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  const char *graph_path = "assets/render_graphs/main.rendergraph.json";
  VkrAllocator *scratch = &rf->scratch_allocator;
  VkrAllocatorScope scope = {0};
  if (vkr_allocator_supports_scopes(scratch)) {
    scope = vkr_allocator_begin_scope(scratch);
  }

  VkrRgJsonGraph graph = {0};
  bool8_t ok = vkr_rg_json_load_file(scratch, graph_path, &graph);
  vkr_rg_json_destroy(&graph);

  if (vkr_allocator_scope_is_valid(&scope)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  return ok;
}

static const VkrSubsystemMask
    vkr_renderer_subsystem_dependencies[VKR_RENDERER_SUBSYSTEM_COUNT] = {
        [VKR_RENDERER_SUBSYSTEM_CAMERA] = 0u,
        [VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH] = 0u,
        [VKR_RENDERER_SUBSYSTEM_FRAME_STREAMS] = 0u,
        [VKR_RENDERER_SUBSYSTEM_RESOURCES] = 0u,
        [VKR_RENDERER_SUBSYSTEM_GEOMETRY] = 0u,
        [VKR_RENDERER_SUBSYSTEM_TEXTURES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_MATERIALS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_MESHES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
        [VKR_RENDERER_SUBSYSTEM_FONTS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_LIGHTING] = 0u,
        [VKR_RENDERER_SUBSYSTEM_SHADOWS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_WORLD] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
        [VKR_RENDERER_SUBSYSTEM_UI] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_FONTS),
        [VKR_RENDERER_SUBSYSTEM_SKYBOX] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_EDITOR] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MESHES),
        [VKR_RENDERER_SUBSYSTEM_GIZMO] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MESHES),
        [VKR_RENDERER_SUBSYSTEM_PICKING] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
};

bool8_t vkr_renderer_subsystem_plan_build(VkrBootProfile profile,
                                          VkrSubsystemMask requested_mask,
                                          VkrSubsystemMask excluded_mask,
                                          VkrSubsystemPlan *out_plan,
                                          VkrRendererError *out_error) {
  VkrRendererError discarded_error = VKR_RENDERER_ERROR_NONE;
  if (!out_error) {
    out_error = &discarded_error;
  }
  if (!out_plan || profile > VKR_BOOT_PROFILE_AUTOMATION ||
      ((requested_mask | excluded_mask) & ~VKR_RENDERER_SUBSYSTEM_ALL) != 0u ||
      (profile == VKR_BOOT_PROFILE_FULL && excluded_mask != 0u)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSubsystemMask effective =
      profile == VKR_BOOT_PROFILE_FULL
          ? VKR_RENDERER_SUBSYSTEM_ALL
          : VKR_RENDERER_SUBSYSTEM_MANDATORY | requested_mask;
  /* Iterated to a fixed point rather than swept once: the dependency table is
     ordered for readability, not topologically, so a later unit may pull in an
     earlier one that has dependencies of its own. The mask only ever grows
     within a bounded bit space, so this terminates. */
  VkrSubsystemMask prior = 0u;
  while (prior != effective) {
    prior = effective;
    for (uint32_t subsystem = 0u; subsystem < VKR_RENDERER_SUBSYSTEM_COUNT;
         ++subsystem) {
      if ((effective & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u) {
        effective |= vkr_renderer_subsystem_dependencies[subsystem];
      }
    }
  }
  if ((effective & excluded_mask) != 0u) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  *out_plan = (VkrSubsystemPlan){
      .profile = profile,
      .requested_mask = profile == VKR_BOOT_PROFILE_FULL
                            ? VKR_RENDERER_SUBSYSTEM_ALL
                            : requested_mask,
      .excluded_mask = excluded_mask,
      .effective_mask = effective,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_renderer_subsystem_plan_includes(const VkrSubsystemPlan *plan,
                                             VkrRendererSubsystem subsystem) {
  return plan && subsystem < VKR_RENDERER_SUBSYSTEM_COUNT &&
         (plan->effective_mask & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u;
}

/**
 * Narrows the plan to what actually came up.
 *
 * Editor, gizmo, and picking retain non-fatal startup behavior; picking is also
 * skipped on a zero-extent window. UI and skybox failures abort initialization
 * before this point. The reported mask is comparison identity: leaving an
 * absent unit set would make two runs that rendered different work compare as
 * the same observation.
 */
static void renderer_frontend_narrow_plan_to_initialized(RendererFrontend *rf) {
  const struct {
    VkrRendererSubsystem subsystem;
    bool8_t initialized;
  } observed[] = {
      {VKR_RENDERER_SUBSYSTEM_UI, rf->ui_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_SKYBOX, rf->skybox_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_EDITOR, rf->editor_viewport.initialized},
      {VKR_RENDERER_SUBSYSTEM_GIZMO, rf->gizmo_system.initialized},
      {VKR_RENDERER_SUBSYSTEM_PICKING, rf->picking.initialized},
  };
  for (uint32_t i = 0u; i < ArrayCount(observed); ++i) {
    const VkrSubsystemMask bit =
        VKR_RENDERER_SUBSYSTEM_BIT(observed[i].subsystem);
    if (!observed[i].initialized && (rf->subsystem_plan.effective_mask & bit)) {
      log_warn("Planned renderer subsystem %u did not initialize; the reported "
               "mask omits it",
               (uint32_t)observed[i].subsystem);
      rf->subsystem_plan.effective_mask &= ~bit;
    }
  }
}

static bool32_t
renderer_impl_metal_initialize(void *state, VkrWindow *window, uint32_t width,
                               uint32_t height,
                               VkrDeviceRequirements *device_requirements,
                               const VkrRendererBackendConfig *backend_config,
                               VkrRendererError *out_error) {
  (void)width;
  (void)height;
  (void)device_requirements;
  RendererFrontend *renderer = state;
#if defined(PLATFORM_APPLE)
  /* Full-resolution deferred intermediates scale with the completion-slot
     count. Two slots keep the placement heap plus scene assets below Metal's
     recommended working set on the supported M1 Pro without serializing the
     GPU to a single slot. Shader validation retains the same bounded topology.
   */
  const uint32_t frame_slot_count = 2u;
  const uint64_t placement_heap_size = GB(7);
  const uint32_t capture_capacity = backend_config->capture_ring_capacity > 0
                                        ? backend_config->capture_ring_capacity
                                        : frame_slot_count;
  const uint64_t capture_bytes = backend_config->capture_max_batch_bytes > 0
                                     ? backend_config->capture_max_batch_bytes
                                     : MB(32);
  const char *pipeline_archive_path = getenv("VKR_PIPELINE_CACHE_PATH");
  if (!pipeline_archive_path || pipeline_archive_path[0] == '\0')
    pipeline_archive_path = VKR_METAL_PACKET_ARCHIVE_PATH;
  VkrMetalPacketRendererConfig metal_config = {
      .allocator = &renderer->render_graph_allocator,
      .graph_path = "assets/render_graphs/main.rendergraph.json",
      .slang_msl_path = VKR_METAL_PACKET_SLANG_MSL,
      .fragment_msl_path = VKR_METAL_PACKET_FRAGMENT_MSL,
      .pipeline_archive_path = pipeline_archive_path,
      .target_kind =
          renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? VKR_METAL_PACKET_TARGET_OFFSCREEN
              : VKR_METAL_PACKET_TARGET_WINDOW,
      .metal_layer = window ? vkr_window_get_metal_layer(window) : NULL,
      .requested_present_mode = backend_config->requested_present_mode,
      .heap_size = placement_heap_size,
      .upload_ring_size = MB(768),
      .readback_ring_size = MB(96),
      .frame_slot_count = frame_slot_count,
      .capture_ring_capacity = capture_capacity,
      .capture_max_batch_bytes = capture_bytes,
      .synchronous_validation_readback =
          vkr_renderer_env_enabled("VKR_METAL_SYNCHRONOUS_VALIDATION"),
      .srgb_output = true_v,
      .convert_vulkan_clip_y = true_v,
      .transmission_compact_enabled =
          !vkr_renderer_env_enabled("VKR_TRANSMISSION_COMPACT_DISABLED"),
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
      .max_images = 128,
      .max_passes = 64,
      .max_material_rows = 8192,
      .max_meshes = 16384,
      .max_submeshes_per_mesh = 512,
      .max_textures = 16384,
      .max_draws = 262144,
      .max_instances = 262144,
  };
  if (!vkr_metal_packet_renderer_create(&metal_config,
                                        &renderer->metal_renderer)) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }
  vkr_metal_packet_renderer_get_asset_publisher(renderer->metal_renderer,
                                                &renderer->asset_publisher);
  /* Caps are seeded with a backend-neutral default before any renderer exists.
     Correct the frames-in-flight count to the number of command slots this
     renderer actually built, so callers sizing per-slot storage are not handed
     a larger count than there are slots. Present image count is unrelated and
     stays as configured. */
  renderer->impl.caps.frame_in_flight_count =
      vkr_metal_packet_renderer_frame_slot_count(renderer->metal_renderer);
  if (renderer->present_target.kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    event_manager_subscribe(renderer->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                            vkr_renderer_on_window_resize, renderer);
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Metal 4 packet renderer");
  return true_v;
#else
  (void)window;
  (void)backend_config;
  (void)renderer;
  *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  return false_v;
#endif
}

static bool32_t
renderer_impl_vulkan_initialize(void *state, VkrWindow *window, uint32_t width,
                                uint32_t height,
                                VkrDeviceRequirements *device_requirements,
                                const VkrRendererBackendConfig *backend_config,
                                VkrRendererError *out_error) {
  (void)device_requirements;
  RendererFrontend *renderer = state;
  VkrVulkanRendererConfig config = {
      .allocator = &renderer->render_graph_allocator,
      .graph_path = "assets/render_graphs/main.rendergraph.json",
      .window = window,
      .target_kind = renderer->present_target.kind,
      .requested_present_mode = backend_config->requested_present_mode,
      .width = width,
      .height = height,
      .image_count = renderer->present_target.image_count,
      .sampled_image_capacity = 16384u,
      .storage_image_capacity = 1024u,
      .sampler_capacity = 2048u,
      // Publication records are indexed directly by logical handle id, so these
      // two must cover the geometry and texture systems' whole ID spaces. They
      // are configured below at max_geometries and max_texture_count; keep the
      // three in step. Neither is a descriptor-heap bound.
      .geometry_capacity = 16384u,
      .texture_capacity = 16384u,
      .material_record_capacity = 8192u,
      .material_slot_capacity = 16385u,
      .device_buffer_block_size = 8u * 1024u * 1024u,
      // A four-layer 2048x2048 D32 shadow image is 64 MiB. Matching that
      // routine graph allocation keeps smaller targets and published textures
      // packed instead of consuming the bounded image pool one small block at
      // a time before the per-image shadow set is realized.
      .device_image_block_size = 64u * 1024u * 1024u,
      // Startup publishes several 4 MiB font atlases before the first submit.
      // Pack them into shared blocks so the bounded per-pool block count does
      // not turn the publication batch into one physical allocation per atlas.
      .upload_buffer_block_size = 32u * 1024u * 1024u,
      .readback_buffer_block_size = 1u * 1024u * 1024u,
      .capture_ring_capacity =
          backend_config->capture_enabled
              ? (backend_config->capture_ring_capacity
                     ? backend_config->capture_ring_capacity
                     : 3u)
              : 0u,
      .capture_max_batch_bytes =
          backend_config->capture_enabled
              ? (backend_config->capture_max_batch_bytes
                     ? backend_config->capture_max_batch_bytes
                     : MB(32))
              : 0u,
      .memory_block_capacity = 128u,
      .memory_blocks_per_pool = 128u,
      .memory_block_allocation_capacity = 512u,
      .publication_staging_capacity = 256u,
      .max_pending_texture_upload_bytes = MB(256),
      .max_graph_images = 128u,
      .max_graph_buffers = 128u,
      .max_graph_passes = 64u,
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
#if !defined(NDEBUG)
      .enable_validation = true_v,
#endif
      .enable_gpu_assisted = backend_config->gpu_assisted_validation,
  };
  config.enable_validation = config.enable_validation ||
                             backend_config->validation_enabled ||
                             config.enable_gpu_assisted;
  config.enable_synchronization_validation =
      config.enable_validation && !config.enable_gpu_assisted;
  if (!vkr_vulkan_renderer_create(&config, &renderer->vulkan_renderer)) {
    vkr_vulkan_renderer_destroy(renderer->vulkan_renderer);
    renderer->vulkan_renderer = NULL;
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return false_v;
  }
  vkr_vulkan_renderer_get_asset_publisher(renderer->vulkan_renderer,
                                          &renderer->asset_publisher);
  /* Stated explicitly rather than inherited from the default caps, so this
     backend's slot count and the caps it publishes cannot drift apart the way
     Metal's did. */
  renderer->impl.caps.frame_in_flight_count = VKR_VULKAN_FRAME_SLOT_COUNT;
  if (renderer->present_target.kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    event_manager_subscribe(renderer->event_manager, EVENT_TYPE_WINDOW_RESIZE,
                            vkr_renderer_on_window_resize, renderer);
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Vulkan 1.4 packet renderer");
  return true_v;
}

bool32_t vkr_renderer_initialize(VkrRendererFrontendHandle renderer,
                                 VkrRendererBackendType backend_type,
                                 VkrWindow *window, EventManager *event_manager,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 uint64_t target_frame_rate,
                                 VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(event_manager != NULL, "Event manager is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  VkrPresentTargetConfig requested_target = backend_config
                                                ? backend_config->present_target
                                                : (VkrPresentTargetConfig){0};
  if (requested_target.kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    if (requested_target.width == 0 || requested_target.height == 0 ||
        requested_target.image_count == 0) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      log_error("Offscreen target requires non-zero width, height, and image "
                "count");
      return false_v;
    }
  } else if (!window) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Windowed target requires a window");
    return false_v;
  }

  VkrRendererImpl selected_impl = {0};
  if (!vkr_renderer_impl_select(backend_type, requested_target.kind,
                                &renderer_impl_strategies, &selected_impl) ||
      !selected_impl.initialization_supported) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    log_error("Requested renderer implementation is not available");
    return false_v;
  }

  // if (!vkr_dmemory_create(MB(100), MB(500), &renderer->dmemory)) {
  //   log_fatal("Failed to create dmemory!");
  //   return false_v;
  // }

  // renderer->dmemory_allocator = (VkrAllocator){.ctx = &renderer->dmemory};
  // vkr_dmemory_allocator_create(&renderer->dmemory_allocator);

  renderer->arena = arena_create(selected_impl.caps.renderer_arena_size);
  if (!renderer->arena) {
    log_fatal("Failed to create renderer arena!");
    return false_v;
  }

  renderer->allocator = (VkrAllocator){.ctx = renderer->arena};
  if (!vkr_allocator_arena(&renderer->allocator)) {
    arena_destroy(renderer->arena);
    log_fatal("Failed to initialize renderer allocator!");
    return false_v;
  }

  renderer->scratch_arena =
      arena_create(selected_impl.caps.scratch_arena_size,
                   selected_impl.caps.scratch_arena_block_size);
  if (!renderer->scratch_arena) {
    log_fatal("Failed to create scratch_arena!");
    return false_v;
  }

  renderer->scratch_allocator = (VkrAllocator){.ctx = renderer->scratch_arena};
  if (!vkr_allocator_arena(&renderer->scratch_allocator)) {
    arena_destroy(renderer->scratch_arena);
    log_fatal("Failed to initialize scratch allocator!");
    return false_v;
  }

  // Initialize struct in-place
  renderer->backend_type = backend_type;
  renderer->impl = selected_impl;
  renderer->impl.state = renderer;
  renderer->window = window;
  renderer->present_target = requested_target;
  renderer->event_manager = event_manager;
  renderer->frame_active = false;
  renderer->metal_renderer = NULL;
  renderer->vulkan_renderer = NULL;
  renderer->asset_publisher = (VkrAssetPublisher){0};
  renderer->timing_result = (VkrRendererImplSubmitResult){0};
  renderer->timing_last_completed_submit_value = 0;
  renderer->timing_completed_ready = false_v;
  renderer->supports_multi_draw_indirect = false_v;
  renderer->supports_draw_indirect_first_instance = false_v;

  // Clear high-level state
  renderer->geometry_system = (VkrGeometrySystem){0};
  renderer->texture_system = (VkrTextureSystem){0};
  renderer->material_system = (VkrMaterialSystem){0};
  renderer->render_graph_dmemory = (VkrDMemory){0};
  renderer->render_graph_allocator = (VkrAllocator){0};
  renderer->mesh_manager = (VkrMeshManager){0};
  renderer->mesh_loader = (VkrMeshLoaderContext){0};
  renderer->scene_async_memory = (VkrDMemory){0};
  renderer->scene_async_allocator = (VkrAllocator){0};
  renderer->scene_async_mutex = NULL;
  renderer->gizmo_system = (VkrGizmoSystem){0};
  renderer->lighting_system = (VkrLightingSystem){0};
  renderer->world_resources = (VkrWorldResources){0};
  renderer->ui_system = (VkrUiSystem){0};
  renderer->skybox_system = (VkrSkyboxSystem){0};
  renderer->active_scene = NULL;
  renderer->scene_generation = 1u;
  renderer->camera_system = (VkrCameraSystem){0};
  renderer->active_camera = VKR_CAMERA_HANDLE_INVALID;
  renderer->camera_controller = (VkrCameraController){0};
  renderer->globals = (VkrGlobalMaterialState){
      .ambient_color = vec4_new(0.1, 0.1, 0.1, 1.0),
      .exposure = VKR_DEFAULT_EXPOSURE,
      .render_mode = VKR_RENDER_MODE_DEFAULT,
  };
  renderer->frame_metrics = (VkrRendererFrameMetrics){0};
  renderer->boot_metrics = (VkrRendererBootMetrics){0};
  renderer->subsystem_plan = (VkrSubsystemPlan){0};
  renderer->rf_mutex = NULL;
  renderer->frame_number = 0;

  /* The selected Vulkan strategy owns its persistent graph realization,

   * descriptor tables, and bounded pool metadata through this allocator in

   * addition to the frontend graph. Keep enough committed-growth headroom for

   * a new image-pool block to be admitted after the descriptor tables are

   * resident. */
  if (!vkr_dmemory_create(MB(2), MB(64), &renderer->render_graph_dmemory)) {
    log_fatal("Failed to create render graph allocator!");
    arena_destroy(renderer->scratch_arena);
    arena_destroy(renderer->arena);
    return false_v;
  }
  renderer->render_graph_allocator =
      (VkrAllocator){.ctx = &renderer->render_graph_dmemory};
  vkr_dmemory_allocator_create(&renderer->render_graph_allocator);
  renderer->target_frame_rate = target_frame_rate;
  vkr_atomic_uint64_store(&renderer->pending_resize_mailbox, 0,
                          VKR_MEMORY_ORDER_RELAXED);

  // Create renderer mutex and initialize size tracking
  if (!vkr_mutex_create(&renderer->allocator, &renderer->rf_mutex)) {
    log_fatal("Failed to create renderer mutex!");
    return false_v;
  }

  VkrWindowPixelSize initial = requested_target.kind ==
                                       VKR_PRESENT_TARGET_OFFSCREEN
                                   ? (VkrWindowPixelSize){
                                         .width = requested_target.width,
                                         .height = requested_target.height,
                                     }
                                   : vkr_window_get_pixel_size(window);
  renderer->last_window_width = initial.width;
  renderer->last_window_height = initial.height;
  if (renderer->window) {
    renderer->window->width = initial.width;
    renderer->window->height = initial.height;
  }
  uint32_t width = initial.width;
  uint32_t height = initial.height;

  VkrRendererBackendConfig resolved_backend_config = {
      .application_name = "vulkan_renderer",
      .boot_metrics = &renderer->boot_metrics,
      .present_target = requested_target,
  };
  if (backend_config) {
    resolved_backend_config = *backend_config;
    resolved_backend_config.boot_metrics = &renderer->boot_metrics;
  }
  const VkrRendererBackendConfig *backend_cfg = &resolved_backend_config;
  if (!renderer->impl.ops || !renderer->impl.ops->initialize ||
      !renderer->impl.ops->initialize(renderer->impl.state, window, width,
                                      height, device_requirements, backend_cfg,
                                      out_error)) {
    return false_v;
  }
  return true_v;
}

void vkr_renderer_destroy(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Destroying renderer");

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Ensure GPU idle before tearing down
  vkr_renderer_wait_idle(rf);

  // Shutdown picking system if initialized
  if (rf->picking.initialized) {
    vkr_picking_shutdown(rf, &rf->picking);
  }

  if (rf->editor_viewport.initialized) {
    vkr_editor_viewport_shutdown(rf, &rf->editor_viewport);
  }

  if (rf->ui_system.initialized) {
    vkr_ui_system_shutdown(rf, &rf->ui_system);
  }
  if (rf->world_resources.initialized) {
    vkr_world_resources_shutdown(rf, &rf->world_resources);
  }
  if (rf->skybox_system.initialized) {
    vkr_skybox_system_shutdown(rf, &rf->skybox_system);
  }

  if (rf->shadow_system.initialized) {
    vkr_shadow_system_shutdown(&rf->shadow_system, rf);
  }

  if (rf->gizmo_system.initialized) {
    vkr_gizmo_system_shutdown(&rf->gizmo_system, rf);
  }

  vkr_lighting_system_shutdown(&rf->lighting_system);
  /* Renderer-only initialization is a supported focused-backend path, and

   * system initialization can fail partway through. These two shutdown

   * routines own mandatory arenas/allocators and therefore cannot consume a

   * zero-initialized system. */
  if (rf->mesh_manager.arena) {
    vkr_mesh_manager_shutdown(&rf->mesh_manager);
  }
  vkr_font_system_shutdown(&rf->font_system);
  vkr_material_system_shutdown(&rf->material_system);
  vkr_geometry_system_shutdown(&rf->geometry_system);
  if (rf->texture_system.arena) {
    vkr_texture_system_shutdown(&rf->texture_system);
  }

  if (rf->impl.ops && rf->impl.ops->destroy) {
    rf->impl.ops->destroy(rf->impl.state);
  }

  if (rf->rf_mutex) {
    vkr_mutex_destroy(&rf->allocator, &rf->rf_mutex);
  }

  // Destroy mesh arena pool
  if (rf->mesh_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->mesh_arena_pool);
  }
  if (rf->bitmap_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->bitmap_font_arena_pool);
  }
  if (rf->system_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->system_font_arena_pool);
  }
  if (rf->mtsdf_font_arena_pool.initialized) {
    vkr_arena_pool_destroy(&rf->allocator, &rf->mtsdf_font_arena_pool);
  }

  renderer_frontend_destroy_loader_async_allocators(rf);

  if (rf->render_graph_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&rf->render_graph_allocator);
  }

  // vkr_dmemory_destroy(&renderer->dmemory);
  arena_destroy(renderer->arena);
  arena_destroy(renderer->scratch_arena);
}

static void renderer_impl_metal_destroy(void *state) {
  RendererFrontend *renderer = state;
#if defined(PLATFORM_APPLE)
  vkr_metal_packet_renderer_destroy(renderer->metal_renderer);
#endif
  renderer->metal_renderer = NULL;
}

typedef struct VkrRendererPreparedPacket {
  VkrRenderPacket packet;
  VkrWorldPassPayload world;
  VkrUiPassPayload ui;
  VkrPreparedTextDraw world_text_draws[VKR_PREPARED_TEXT_DRAW_MAX];
  VkrPreparedTextDraw ui_text_draws[VKR_PREPARED_TEXT_DRAW_MAX];
} VkrRendererPreparedPacket;

static void vkr_renderer_prepare_packet(RendererFrontend *rf,
                                        const VkrRenderPacket *packet,
                                        VkrRendererPreparedPacket *prepared) {
  prepared->packet = *packet;

  const VkrTextUpdatesPayload *updates = packet->text_updates;
  if (updates) {
    if (rf->world_resources.initialized) {
      for (uint32_t i = 0u; i < updates->world_text_update_count; ++i) {
        const VkrTextUpdate *update = &updates->world_text_updates[i];
        vkr_world_resources_text_update(rf, &rf->world_resources,
                                        update->text_id, update->content);
        if (update->transform)
          vkr_world_resources_text_set_transform(
              rf, &rf->world_resources, update->text_id, update->transform);
      }
    }
    if (rf->ui_system.initialized) {
      for (uint32_t i = 0u; i < updates->ui_text_update_count; ++i) {
        const VkrTextUpdate *update = &updates->ui_text_updates[i];
        vkr_ui_system_text_update(rf, &rf->ui_system, update->text_id,
                                  update->content);
      }
    }
  }

  if (rf->world_resources.initialized) {
    prepared->world = packet->world ? *packet->world : (VkrWorldPassPayload){0};
    const uint32_t text_draw_count = vkr_world_resources_prepare_text_draws(
        rf, &rf->world_resources, prepared->world_text_draws,
        VKR_PREPARED_TEXT_DRAW_MAX);
    if (text_draw_count) {
      prepared->world.text_draws = prepared->world_text_draws;
      prepared->world.text_draw_count = text_draw_count;
    }
    prepared->packet.world = &prepared->world;
  }
  if (rf->ui_system.initialized) {
    prepared->ui = packet->ui ? *packet->ui : (VkrUiPassPayload){0};
    const uint32_t text_draw_count = vkr_ui_system_prepare_text_draws(
        rf, &rf->ui_system, prepared->ui_text_draws,
        VKR_PREPARED_TEXT_DRAW_MAX);
    if (text_draw_count) {
      prepared->ui.text_draws = prepared->ui_text_draws;
      prepared->ui.text_draw_count = text_draw_count;
    }
    prepared->packet.ui = &prepared->ui;
  }
}

static VkrRendererError
renderer_impl_vulkan_submit_packet(void *state, const VkrRenderPacket *packet,
                                   VkrRendererFrameMetrics *out_metrics,
                                   VkrValidationError *out_validation_error) {
  RendererFrontend *rf = state;
  if (!rf->frame_active) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_FRAME_IN_PROGRESS, "frame",
        "frame is not active; call vkr_renderer_prepare_frame first");
  }
  VkrRendererPreparedPacket prepared;
  vkr_renderer_prepare_packet(rf, packet, &prepared);
  VkrVulkanResult result = {0};
  const bool8_t submitted = vkr_vulkan_renderer_submit_packet(
      rf->vulkan_renderer, &prepared.packet, &result);
  rf->frame_active = false_v;
  if (!submitted) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_SUBMISSION_FAILED, "vulkan",
        "Vulkan packet submission failed");
  }
  if (packet->picking && packet->picking->pending &&
      rf->picking.state == VKR_PICKING_STATE_RENDER_PENDING)
    rf->picking.state = VKR_PICKING_STATE_READBACK_PENDING;
  if (!rf->timing_completed_ready) {
    rf->timing_result = (VkrRendererImplSubmitResult){
        .submit_value = result.submit_value,
        .source_frame_index = packet->frame.frame_index,
        .executed_pass_count = result.pass_timing_count,
        .indexed_draw_count = result.indexed_draw_count,
        .shadow_draw_count = result.shadow_draw_count,
        .opaque_draw_count = result.opaque_draw_count,
        .transmission_draw_count = result.transmission_draw_count,
        .blend_draw_count = result.blend_draw_count,
        .pass_timing_count = result.pass_timing_count,
    };
    MemCopy(rf->timing_result.pass_timings, result.pass_timings,
            (uint64_t)result.pass_timing_count *
                sizeof(*rf->timing_result.pass_timings));
    (void)renderer_impl_vulkan_memory_metrics(rf, &rf->timing_result.memory);
  }
  const uint32_t world_draw_count = result.opaque_draw_count +
                                    result.transmission_draw_count +
                                    result.blend_draw_count;
  rf->frame_metrics.world.draws_collected = world_draw_count;
  rf->frame_metrics.world.opaque_draws = result.opaque_draw_count;
  rf->frame_metrics.world.transmission_draws = result.transmission_draw_count;
  rf->frame_metrics.world.transparent_draws = result.blend_draw_count;
  rf->frame_metrics.world.draws_issued = world_draw_count;
  rf->frame_metrics.world.draw_calls_issued = world_draw_count;
  vkr_renderer_record_gpu_candidate_metrics(rf, &prepared.packet);
  /* Packet lowering happened on this thread during this submit, so it comes
     from the call's own result rather than from `timing_result`, which may
     still describe an older completed frame. */
  rf->frame_metrics.packet_build = result.packet_build;
  const VkrRendererImplSubmitResult *observed = &rf->timing_result;
  rf->frame_metrics.world.hzb_history_valid = observed->hzb_history_valid;
  if (observed->has_gpu_draw_diagnostics) {
    rf->frame_metrics.world.opaque_draws = observed->gpu_visible_count;
    rf->frame_metrics.world.transmission_draws =
        observed->transmission_gpu_visible_count;
    rf->frame_metrics.world.draws_collected =
        rf->frame_metrics.world.gpu_candidate_count +
        rf->frame_metrics.world.transmission_gpu_candidate_count +
        observed->blend_draw_count;
    rf->frame_metrics.world.gpu_visible_count = observed->gpu_visible_count;
    MemCopy(rf->frame_metrics.world.gpu_bucket_counts,
            observed->gpu_bucket_counts,
            sizeof(rf->frame_metrics.world.gpu_bucket_counts));
    rf->frame_metrics.world.gpu_compaction_overflow_count =
        observed->gpu_overflow_count;
    rf->frame_metrics.world.gpu_resolve_invalid_count =
        observed->gpu_resolve_invalid_count;
    rf->frame_metrics.world.gpu_occlusion_culled_count =
        observed->gpu_occlusion_culled_count;
    rf->frame_metrics.world.transmission_gpu_visible_count =
        observed->transmission_gpu_visible_count;
    MemCopy(rf->frame_metrics.world.transmission_gpu_bucket_counts,
            observed->transmission_gpu_bucket_counts,
            sizeof(rf->frame_metrics.world.transmission_gpu_bucket_counts));
    rf->frame_metrics.world.transmission_gpu_compaction_overflow_count =
        observed->transmission_gpu_overflow_count;
    rf->frame_metrics.world.transmission_gpu_occlusion_culled_count =
        observed->transmission_gpu_occlusion_culled_count;
    rf->frame_metrics.world.gpu_diagnostics_valid = true_v;
    uint32_t indirect_draws = observed->gpu_visible_count;
    uint32_t indirect_calls = 0u;
    uint32_t max_batch_size = 0u;
    for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
         ++bucket) {
      indirect_calls += observed->gpu_bucket_counts[bucket] > 0u ? 1u : 0u;
      max_batch_size = Max(max_batch_size, observed->gpu_bucket_counts[bucket]);
      if (observed->transmission_gpu_bucket_counts[bucket] > 0u)
        indirect_calls += 4u;
      indirect_draws += observed->transmission_gpu_bucket_counts[bucket] * 4u;
      max_batch_size =
          Max(max_batch_size, observed->transmission_gpu_bucket_counts[bucket]);
    }
    rf->frame_metrics.world.indirect_draws_issued = indirect_draws;
    rf->frame_metrics.world.indirect_calls_issued = indirect_calls;
    rf->frame_metrics.world.draws_issued =
        indirect_draws + observed->blend_draw_count;
    rf->frame_metrics.world.draw_calls_issued =
        indirect_calls + observed->blend_draw_count;
    rf->frame_metrics.world.max_batch_size = max_batch_size;
    rf->frame_metrics.world.avg_batch_size =
        indirect_calls > 0u ? (float32_t)indirect_draws / indirect_calls : 0.0f;
    for (uint32_t cascade = 0u; cascade < VKR_SHADOW_CASCADE_COUNT_MAX;
         ++cascade) {
      rf->frame_metrics.shadow.shadow_indirect_draws_opaque[cascade] =
          observed->shadow_gpu_visible_count[cascade];
      uint32_t cascade_calls = 0u;
      for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
           ++bucket)
        cascade_calls +=
            observed->shadow_gpu_bucket_counts[cascade][bucket] > 0u ? 1u : 0u;
      rf->frame_metrics.shadow.shadow_indirect_calls_opaque[cascade] =
          cascade_calls;
      rf->frame_metrics.shadow.shadow_indirect_overflow[cascade] =
          observed->shadow_gpu_overflow_count[cascade];
    }
  }
  if (out_metrics) {
    *out_metrics = rf->frame_metrics;
  }
  return VKR_RENDERER_ERROR_NONE;
}

static void renderer_impl_vulkan_destroy(void *state) {
  RendererFrontend *renderer = state;
  vkr_vulkan_renderer_destroy(renderer->vulkan_renderer);
  renderer->vulkan_renderer = NULL;
}

static void renderer_impl_metal_get_device_information(
    void *state, VkrDeviceInformation *device_information, Arena *temp_arena) {
  (void)temp_arena;
  RendererFrontend *renderer = state;
#if defined(PLATFORM_APPLE)
  const VkrPresentMode actual_present_mode =
      vkr_metal_packet_renderer_present_mode(renderer->metal_renderer);
#else
  const VkrPresentMode actual_present_mode = VKR_PRESENT_MODE_DEFAULT;
#endif
  VkrDeviceTypeFlags device_types = bitset8_create();
  VkrDeviceQueueFlags device_queues = bitset8_create();
  VkrSamplerFilterFlags sampler_filters = bitset8_create();
  bitset8_set(&device_types, VKR_DEVICE_TYPE_INTEGRATED_BIT);
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_GRAPHICS_BIT);
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_TRANSFER_BIT);
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_PRESENT_BIT);
  bitset8_set(&sampler_filters, VKR_SAMPLER_FILTER_LINEAR_BIT);
  bitset8_set(&sampler_filters, VKR_SAMPLER_FILTER_ANISOTROPIC_BIT);
  *device_information = (VkrDeviceInformation){
      .device_name = string8_lit("Apple Metal 4 GPU"),
      .vendor_name = string8_lit("Apple"),
      .driver_version = string8_lit("Metal 4"),
      .api_version = string8_lit("Metal 4"),
      .device_types = device_types,
      .device_queues = device_queues,
      .sampler_filters = sampler_filters,
      .max_sampler_anisotropy = 16.0,
      .supports_texture_astc_4x4 = true_v,
      .supports_texture_bc7 = true_v,
      .supports_texture_bc5 = true_v,
      .supports_hdr_ibl = true_v,
      .hdr_ibl_max_cube_extent = VKR_IBL_PREFILTER_SIZE,
      .hdr_ibl_max_mip_levels = VKR_IBL_PREFILTER_MIP_COUNT,
      .actual_target_kind = renderer->present_target.kind,
      .actual_present_mode = actual_present_mode,
      .actual_target_image_count =
          renderer->impl.caps.present_target_image_count,
      .actual_target_width = renderer->last_window_width,
      .actual_target_height = renderer->last_window_height,
      .actual_color_format =
          renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
              : VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB,
      .actual_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT,
      .actual_color_space = VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR,
      .actual_world_renderer_topology = VKR_WORLD_RENDERER_TOPOLOGY_DEFERRED,
  };
}

static void renderer_impl_vulkan_get_device_information(
    void *state, VkrDeviceInformation *device_information, Arena *temp_arena) {
  (void)temp_arena;
  RendererFrontend *renderer = state;
  const VkrVulkanCapabilityProfile *profile =
      vkr_vulkan_renderer_profile(renderer->vulkan_renderer);
  const VkrVulkanCandidateReport *selected =
      profile && profile->selected_candidate_index < profile->candidate_count
          ? &profile->candidates[profile->selected_candidate_index]
          : NULL;
  VkrDeviceTypeFlags device_types = bitset8_create();
  VkrDeviceQueueFlags device_queues = bitset8_create();
  VkrSamplerFilterFlags sampler_filters = bitset8_create();
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_GRAPHICS_BIT);
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_COMPUTE_BIT);
  bitset8_set(&device_queues, VKR_DEVICE_QUEUE_TRANSFER_BIT);
  if (renderer->present_target.kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    bitset8_set(&device_queues, VKR_DEVICE_QUEUE_PRESENT_BIT);
  }
  bitset8_set(&sampler_filters, VKR_SAMPLER_FILTER_LINEAR_BIT);
  uint32_t hdr_ibl_max_cube_extent = 0u;
  uint32_t hdr_ibl_max_mip_levels = 0u;
  const bool8_t supports_hdr_ibl = vkr_vulkan_renderer_hdr_ibl_limits(
      renderer->vulkan_renderer, &hdr_ibl_max_cube_extent,
      &hdr_ibl_max_mip_levels);
  VkrPresentMode present_mode = VKR_PRESENT_MODE_DEFAULT;
  VkrSurfaceColorFormat color_format = VKR_SURFACE_COLOR_FORMAT_UNKNOWN;
  VkrSurfaceDepthFormat depth_format = VKR_SURFACE_DEPTH_FORMAT_UNKNOWN;
  VkrSurfaceColorSpace color_space = VKR_SURFACE_COLOR_SPACE_UNKNOWN;
  float32_t max_anisotropy = 1.0f;
  vkr_vulkan_renderer_target_information(
      renderer->vulkan_renderer, &present_mode, &color_format, &depth_format,
      &color_space, &max_anisotropy);
  VkrDeviceMemoryStats memory_stats = {0};
  vkr_vulkan_renderer_device_memory_stats(renderer->vulkan_renderer,
                                          &memory_stats);
  bool8_t local_heaps[VKR_DEVICE_MEMORY_HEAP_MAX] = {0};
  for (uint32_t type = 0; type < memory_stats.memory_type_count; ++type) {
    const uint32_t heap = memory_stats.heap_index_by_type[type];
    if (heap < memory_stats.heap_count &&
        (memory_stats.property_flags_by_type[type] &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      local_heaps[heap] = true_v;
    }
  }
  uint64_t vram_size = 0u;
  uint64_t vram_local_size = 0u;
  uint64_t vram_shared_size = 0u;
  for (uint32_t heap = 0; heap < memory_stats.heap_count; ++heap) {
    const uint64_t size = memory_stats.heap_size_bytes[heap];
    vram_size += size;
    if (local_heaps[heap])
      vram_local_size += size;
    else
      vram_shared_size += size;
  }
  *device_information = (VkrDeviceInformation){
      .device_name = selected ? string8_create_from_cstr(
                                    (const uint8_t *)selected->device_name,
                                    strlen(selected->device_name))
                              : string8_lit("Vulkan 1.4 GPU"),
      .vendor_name = string8_lit("Vulkan"),
      .driver_version = selected ? string8_create_from_cstr(
                                       (const uint8_t *)selected->driver_info,
                                       strlen(selected->driver_info))
                                 : string8_lit("unknown"),
      .api_version = string8_lit("Vulkan 1.4"),
      .vram_size = vram_size,
      .vram_local_size = vram_local_size,
      .vram_shared_size = vram_shared_size,
      .device_types = device_types,
      .device_queues = device_queues,
      .sampler_filters = sampler_filters,
      .max_sampler_anisotropy = max_anisotropy,
      .supports_texture_astc_4x4 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM),
      .supports_texture_bc7 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_BC7_UNORM),
      .supports_texture_etc2 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM),
      .supports_texture_bc5 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_BC5_UNORM),
      .supports_texture_eac_rg11 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM),
      .supports_hdr_ibl = supports_hdr_ibl,
      .hdr_ibl_max_cube_extent = hdr_ibl_max_cube_extent,
      .hdr_ibl_max_mip_levels = hdr_ibl_max_mip_levels,
      .actual_target_kind = renderer->present_target.kind,
      .actual_present_mode = present_mode,
      .actual_target_image_count = renderer->present_target.image_count,
      .actual_target_width = renderer->last_window_width,
      .actual_target_height = renderer->last_window_height,
      .actual_color_format = color_format,
      .actual_depth_format = depth_format,
      .actual_color_space = color_space,
      .actual_world_renderer_topology = VKR_WORLD_RENDERER_TOPOLOGY_DEFERRED,
  };
}

static VkrRendererError renderer_impl_metal_wait_idle(void *state) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_wait_idle(renderer->metal_renderer)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_DEVICE_ERROR;
#else
  (void)state;
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
}

static VkrRendererError renderer_impl_vulkan_wait_idle(void *state) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_wait_idle(renderer->vulkan_renderer)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_DEVICE_ERROR;
}

static uint64_t renderer_impl_metal_submit_serial(void *state) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_submit_value(renderer->metal_renderer);
#else
  (void)state;
  return 0;
#endif
}

static uint64_t renderer_impl_vulkan_submit_serial(void *state) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_submit_value(renderer->vulkan_renderer);
}

static uint64_t renderer_impl_metal_completed_submit_serial(void *state) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_completed_value(renderer->metal_renderer);
#else
  (void)state;
  return 0;
#endif
}

static uint64_t renderer_impl_vulkan_completed_submit_serial(void *state) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_completed_value(renderer->vulkan_renderer);
}

static bool8_t
renderer_impl_metal_upload_wait_stats(void *state,
                                      VkrRendererUploadWaitStats *out_stats) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_get_and_reset_upload_wait_count(
      renderer->metal_renderer, &out_stats->fence_wait_count);
#else
  (void)state;
  (void)out_stats;
  return false_v;
#endif
}

static bool8_t
renderer_impl_vulkan_upload_wait_stats(void *state,
                                       VkrRendererUploadWaitStats *out_stats) {
  RendererFrontend *renderer = state;
  MemZero(out_stats, sizeof(*out_stats));
  return vkr_vulkan_renderer_get_and_reset_upload_wait_count(
             renderer->vulkan_renderer, &out_stats->fence_wait_count) &&
         vkr_vulkan_renderer_get_and_reset_frame_upload_exhaustion_count(
             renderer->vulkan_renderer,
             &out_stats->frame_upload_exhaustion_count);
}

static bool8_t
renderer_impl_metal_command_slot_waits(void *state, uint64_t *out_wait_count) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_get_and_reset_command_slot_wait_count(
      renderer->metal_renderer, out_wait_count);
#else
  (void)state;
  *out_wait_count = 0;
  return false_v;
#endif
}

static bool8_t
renderer_impl_vulkan_command_slot_waits(void *state, uint64_t *out_wait_count) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_get_and_reset_command_slot_wait_count(
      renderer->vulkan_renderer, out_wait_count);
}

static bool8_t
renderer_impl_metal_device_memory_stats(void *state,
                                        VkrDeviceMemoryStats *out_stats) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  VkrMetalMemoryDeviceMetrics metrics = {0};
  if (!vkr_metal_packet_renderer_get_memory_metrics(renderer->metal_renderer,
                                                    &metrics)) {
    return false_v;
  }
  out_stats->live_allocation_count = metrics.native_heap_size > 0 ? 1u : 0u;
  out_stats->peak_allocation_count = out_stats->live_allocation_count;
  out_stats->total_allocation_count = out_stats->live_allocation_count;
  out_stats->max_allocation_count = 1u;
  out_stats->live_bytes = metrics.native_heap_allocated_size;
  out_stats->peak_bytes = metrics.native_heap_peak_allocated_size;
  out_stats->live_totals_exact = true_v;
  out_stats->memory_type_count = 1;
  out_stats->live_bytes_by_type[0] = out_stats->live_bytes;
  out_stats->live_count_by_type[0] = out_stats->live_allocation_count;
  out_stats->heap_index_by_type[0] = 0;
  out_stats->heap_count = 1;
  out_stats->heap_size_bytes[0] = metrics.native_heap_size;
  out_stats->heap_usage_bytes[0] = metrics.native_heap_used_size;
  out_stats->heap_budget_bytes[0] = metrics.driver_recommended_working_set_size;
  out_stats->heap_usage_valid = metrics.driver_recommended_working_set_size > 0;
  return true_v;
#else
  (void)state;
  (void)out_stats;
  return false_v;
#endif
}

static bool8_t
renderer_impl_vulkan_device_memory_stats(void *state,
                                         VkrDeviceMemoryStats *out_stats) {
  RendererFrontend *renderer = state;
  if (!renderer || !renderer->vulkan_renderer || !out_stats)
    return false_v;
  vkr_vulkan_renderer_device_memory_stats(renderer->vulkan_renderer, out_stats);
  return true_v;
}

static bool8_t
renderer_impl_no_memory_metrics(void *state,
                                VkrRendererImplMemoryMetrics *out_metrics) {
  (void)state;
  MemZero(out_metrics, sizeof(*out_metrics));
  return false_v;
}

static bool8_t
renderer_impl_metal_memory_metrics(void *state,
                                   VkrRendererImplMemoryMetrics *out_metrics) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  VkrMetalMemoryDeviceMetrics source = {0};
  if (!vkr_metal_packet_renderer_get_memory_metrics(renderer->metal_renderer,
                                                    &source)) {
    return false_v;
  }
  VkrMetalPacketResult rich = {.memory = source};
  VkrRendererImplSubmitResult lowered = {0};
  vkr_renderer_impl_lower_metal_result(&rich, &lowered);
  *out_metrics = lowered.memory;
  return true_v;
#else
  (void)state;
  (void)out_metrics;
  return false_v;
#endif
}

static bool8_t
renderer_impl_vulkan_memory_metrics(void *state,
                                    VkrRendererImplMemoryMetrics *out_metrics) {
  RendererFrontend *renderer = state;
  VkrVulkanMemoryMetrics metrics = {0};
  vkr_vulkan_renderer_memory_metrics(renderer->vulkan_renderer, &metrics);
  const VkrGpuMemoryMetrics *source = &metrics.aggregate;
  MemZero(out_metrics, sizeof(*out_metrics));
#define VKR_LOWER_VULKAN_MEMORY_FIELD(FIELD) out_metrics->FIELD = source->FIELD
  VKR_LOWER_VULKAN_MEMORY_FIELD(heap_size);
  VKR_LOWER_VULKAN_MEMORY_FIELD(free_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(largest_free_range);
  VKR_LOWER_VULKAN_MEMORY_FIELD(live_requested_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(live_reserved_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(retired_requested_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(retired_reserved_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(peak_requested_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(peak_reserved_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(allocations_created);
  VKR_LOWER_VULKAN_MEMORY_FIELD(retirements_collected);
  VKR_LOWER_VULKAN_MEMORY_FIELD(live_allocations);
  VKR_LOWER_VULKAN_MEMORY_FIELD(retired_allocations);
  VKR_LOWER_VULKAN_MEMORY_FIELD(peak_allocations);
  VKR_LOWER_VULKAN_MEMORY_FIELD(alignment_waste_bytes);
  VKR_LOWER_VULKAN_MEMORY_FIELD(byte_exhaustion_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(fragmentation_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(handle_exhaustion_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(range_metadata_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(retirement_capacity_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(stale_handle_failures);
  VKR_LOWER_VULKAN_MEMORY_FIELD(native_allocation_failures);
#undef VKR_LOWER_VULKAN_MEMORY_FIELD
  for (uint32_t class_index = 0;
       class_index < VKR_RENDERER_IMPL_MEMORY_CLASS_COUNT; ++class_index) {
    const VkrGpuMemoryClassMetrics *input = &source->classes[class_index];
    VkrRendererImplMemoryClassMetrics *output =
        &out_metrics->classes[class_index];
    output->live_requested_bytes = input->live_requested_bytes;
    output->live_reserved_bytes = input->live_reserved_bytes;
    output->retired_requested_bytes = input->retired_requested_bytes;
    output->retired_reserved_bytes = input->retired_reserved_bytes;
    output->peak_requested_bytes = input->peak_requested_bytes;
    output->peak_reserved_bytes = input->peak_reserved_bytes;
    output->allocations_created = input->allocations_created;
    output->live_allocations = input->live_allocations;
    output->retired_allocations = input->retired_allocations;
    output->peak_allocations = input->peak_allocations;
    output->alignment_waste_bytes = input->alignment_waste_bytes;
  }
  VkrVulkanHeapMetrics heap_metrics = {0};
  vkr_vulkan_renderer_heap_metrics(renderer->vulkan_renderer, &heap_metrics);
  const VkrGpuSlotTableMetrics *slot_sources[] = {
      &heap_metrics.sampled_images,
      &heap_metrics.samplers,
      &heap_metrics.storage_images,
      &heap_metrics.materials,
  };
  for (uint32_t table_index = 0; table_index < ArrayCount(slot_sources);
       ++table_index) {
    const VkrGpuSlotTableMetrics *input = slot_sources[table_index];
    VkrRendererImplSlotTableMetrics *output =
        &out_metrics->slot_tables[table_index];
    *output = (VkrRendererImplSlotTableMetrics){
        .live = input->slots_live,
        .peak = input->slots_peak,
        .capacity = input->slots_capacity,
        .published = input->slots_published,
        .retired = input->slots_retirements,
        .collected = input->slots_collected,
        .capacity_failures = input->capacity_failures,
    };
  }
  out_metrics->native_heap_count = metrics.physical_allocations_live;
  out_metrics->native_heap_peak_count = metrics.physical_allocations_peak;
  out_metrics->native_heaps_created = metrics.physical_allocations_created;
  out_metrics->native_heap_capacity_failures = metrics.block_capacity_failures;
  out_metrics->native_heap_size = metrics.physical_allocated_bytes;
  out_metrics->native_heap_used_size =
      source->live_reserved_bytes + source->retired_reserved_bytes;
  out_metrics->native_heap_allocated_size = metrics.physical_allocated_bytes;
  out_metrics->native_heap_largest_free_range = source->largest_free_range;
  out_metrics->native_heap_peak_allocated_size =
      metrics.physical_allocated_bytes_peak;
  out_metrics->residency_allocation_count = metrics.physical_allocations_live;
  out_metrics->native_live_resources = source->live_allocations;
  return true_v;
}

static void renderer_impl_no_resize(void *state, uint32_t width,
                                    uint32_t height) {
  (void)state;
  (void)width;
  (void)height;
}

static VkrRendererError renderer_impl_metal_present_target_recreate(
    void *state, uint32_t width, uint32_t height, uint32_t image_count) {
  (void)image_count;
  RendererFrontend *renderer = state;
  VkrRendererError idle = renderer_impl_metal_wait_idle(state);
  if (idle != VKR_RENDERER_ERROR_NONE) {
    return idle;
  }
  renderer->present_target.width = width;
  renderer->present_target.height = height;
  renderer->present_target.image_count =
      renderer->impl.caps.present_target_image_count;
  return VKR_RENDERER_ERROR_NONE;
}

static uint32_t renderer_impl_metal_frame_in_flight_index(void *state) {
  RendererFrontend *renderer = state;
#if defined(PLATFORM_APPLE)
  /* Ask the backend which slot it acquired, the way the Vulkan path does.
     Deriving the index from the frame counter assumed the slot count in caps
     matched the one the Metal renderer actually built, and it did not. */
  return vkr_metal_packet_renderer_frame_slot(renderer->metal_renderer);
#else
  (void)renderer;
  return 0u;
#endif
}

static VkrCaptureStatus
renderer_impl_metal_capture_poll(void *state, VkrCaptureRequestId request_id,
                                 VkrCapturePollResult *out_result) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_capture_poll(renderer->metal_renderer,
                                                request_id, out_result);
#else
  (void)state;
  (void)request_id;
  if (out_result) {
    MemZero(out_result, sizeof(*out_result));
    out_result->error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  }
  return VKR_CAPTURE_STATUS_NOT_FOUND;
#endif
}

static bool8_t
renderer_impl_metal_capture_release(void *state,
                                    VkrCaptureRequestId request_id) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  return vkr_metal_packet_renderer_capture_release(renderer->metal_renderer,
                                                   request_id);
#else
  (void)state;
  (void)request_id;
  return false_v;
#endif
}

static bool8_t
renderer_impl_no_submit_result(void *state, uint64_t after_submit_value,
                               VkrRendererImplSubmitResult *out_result) {
  (void)state;
  (void)after_submit_value;
  if (out_result) {
    MemZero(out_result, sizeof(*out_result));
  }
  return false_v;
}

static bool8_t renderer_impl_metal_poll_submit_result(
    void *state, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result) {
#if defined(PLATFORM_APPLE)
  RendererFrontend *renderer = state;
  VkrMetalPacketResult result = {0};
  if (!out_result ||
      !vkr_metal_packet_renderer_submit_result_poll_next(
          renderer->metal_renderer, after_submit_value, &result)) {
    return false_v;
  }
  vkr_renderer_impl_lower_metal_result(&result, out_result);
  return true_v;
#else
  (void)state;
  (void)after_submit_value;
  (void)out_result;
  return false_v;
#endif
}

static VkrAllocator *renderer_impl_metal_allocator(void *state) {
  RendererFrontend *renderer = state;
  return &renderer->allocator;
}

String8 vkr_renderer_get_error_string(VkrRendererError error) {
  switch (error) {
  case VKR_RENDERER_ERROR_NONE:
    return string8_lit("No error");
  case VKR_RENDERER_ERROR_UNKNOWN:
    return string8_lit("Unknown error");
  case VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED:
    return string8_lit("Backend not supported");
  case VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED:
    return string8_lit("Resource creation failed");
  case VKR_RENDERER_ERROR_INVALID_HANDLE:
    return string8_lit("Invalid handle");
  case VKR_RENDERER_ERROR_INVALID_PARAMETER:
    return string8_lit("Invalid parameter");
  case VKR_RENDERER_ERROR_UNSUPPORTED_INPUT:
    return string8_lit("Unsupported renderer input");
  case VKR_RENDERER_ERROR_SHADER_COMPILATION_FAILED:
    return string8_lit("Shader compilation failed");
  case VKR_RENDERER_ERROR_OUT_OF_MEMORY:
    return string8_lit("Out of memory");
  case VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED:
    return string8_lit("Command recording failed");
  case VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED:
    return string8_lit("Frame preparation failed");
  case VKR_RENDERER_ERROR_PRESENTATION_FAILED:
    return string8_lit("Presentation failed");
  case VKR_RENDERER_ERROR_FRAME_IN_PROGRESS:
    return string8_lit("Frame in progress");
  case VKR_RENDERER_ERROR_DEVICE_ERROR:
    return string8_lit("Device error");
  case VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED:
    return string8_lit("Pipeline state update failed");
  case VKR_RENDERER_ERROR_FILE_NOT_FOUND:
    return string8_lit("File not found");
  case VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED:
    return string8_lit("Resource not loaded");
  case VKR_RENDERER_ERROR_INITIALIZATION_FAILED:
    return string8_lit("Initialization failed");
  case VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE:
    return string8_lit("Incompatible signature");
  case VKR_RENDERER_ERROR_FRAME_SKIPPED:
    return string8_lit("Frame skipped");
  case VKR_RENDERER_ERROR_SUBMISSION_FAILED:
    return string8_lit("Queue submission failed");
  case VKR_RENDERER_ERROR_CAPTURE_BUSY:
    return string8_lit("Capture ring busy");
  case VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE:
    return string8_lit("Capture unavailable");
  case VKR_RENDERER_ERROR_RESOURCE_BUSY:
    return string8_lit("Resource publication busy");
  case VKR_RENDERER_ERROR_COUNT:
    break;
  }
  return string8_lit("Unknown error");
}

VkrWindow *vkr_renderer_get_window(VkrRendererFrontendHandle renderer) {
  return renderer->window;
}

uint64_t
vkr_renderer_get_target_frame_rate(VkrRendererFrontendHandle renderer) {
  return renderer->target_frame_rate;
}

VkrRendererBackendType
vkr_renderer_get_backend_type(VkrRendererFrontendHandle renderer) {
  return renderer->backend_type;
}

void vkr_renderer_get_device_information(
    VkrRendererFrontendHandle renderer,
    VkrDeviceInformation *device_information, Arena *temp_arena) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  renderer->impl.ops->get_device_information(renderer->impl.state,
                                             device_information, temp_arena);
}

bool32_t vkr_renderer_is_frame_active(VkrRendererFrontendHandle renderer) {
  return renderer->frame_active;
}

VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer) {
  return renderer->impl.ops->wait_idle(renderer->impl.state);
}

bool8_t
vkr_renderer_gpu_submission_timing_poll(VkrRendererFrontendHandle renderer,
                                        uint64_t after_submit_serial,
                                        VkrGpuSubmissionTiming *out_timing) {
  if (!renderer || !out_timing)
    return false_v;
  VkrRendererImplSubmitResult result = {0};
  if (!renderer->impl.ops->poll_submit_result(renderer->impl.state,
                                              after_submit_serial, &result))
    return false_v;
  *out_timing = (VkrGpuSubmissionTiming){
      .submit_serial = result.submit_value,
      .source_frame_index = result.source_frame_index,
      .duration_ns = result.gpu_submission_ns,
      .unavailable_reason = vkr_renderer_gpu_timing_metric_reason(
          result.gpu_submission_unavailable_reason),
      .valid = result.gpu_submission_valid,
  };
  return true_v;
}

uint64_t vkr_renderer_get_submit_serial(VkrRendererFrontendHandle renderer) {
  return renderer->impl.ops->get_submit_serial(renderer->impl.state);
}

uint64_t
vkr_renderer_get_completed_submit_serial(VkrRendererFrontendHandle renderer) {
  return renderer->impl.ops->get_completed_submit_serial(renderer->impl.state);
}

bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRendererFrontendHandle renderer, VkrRendererUploadWaitStats *out_stats) {
  out_stats->fence_wait_count = 0;
  out_stats->queue_wait_idle_count = 0;
  out_stats->device_wait_idle_count = 0;
  out_stats->frame_upload_exhaustion_count = 0;
  return renderer->impl.ops->get_and_reset_upload_wait_stats(
      renderer->impl.state, out_stats);
}

bool8_t vkr_renderer_get_and_reset_command_slot_wait_count(
    VkrRendererFrontendHandle renderer, uint64_t *out_wait_count) {
  *out_wait_count = 0;
  return renderer->impl.ops->get_and_reset_command_slot_wait_count(
      renderer->impl.state, out_wait_count);
}

bool8_t vkr_renderer_get_device_memory_stats(VkrRendererFrontendHandle renderer,
                                             VkrDeviceMemoryStats *out_stats) {
  MemZero(out_stats, sizeof(*out_stats));
  return renderer->impl.ops->get_device_memory_stats(renderer->impl.state,
                                                     out_stats);
}

bool8_t vkr_renderer_create_ui_text(VkrRendererFrontendHandle renderer,
                                    const VkrUiTextCreateData *payload,
                                    uint32_t *out_text_id) {
  if (!renderer || !payload) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->ui_system.initialized) {
    return false_v;
  }
  return vkr_ui_system_text_create(rf, &rf->ui_system, payload, out_text_id);
}

bool8_t vkr_renderer_destroy_ui_text(VkrRendererFrontendHandle renderer,
                                     uint32_t text_id) {
  if (!renderer) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->ui_system.initialized) {
    return false_v;
  }
  return vkr_ui_system_text_destroy(rf, &rf->ui_system, text_id);
}

bool8_t vkr_renderer_create_world_text(VkrRendererFrontendHandle renderer,
                                       const VkrWorldTextCreateData *payload) {
  if (!renderer || !payload) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->world_resources.initialized) {
    return false_v;
  }
  return vkr_world_resources_text_create(rf, &rf->world_resources, payload);
}

bool8_t vkr_renderer_destroy_world_text(VkrRendererFrontendHandle renderer,
                                        uint32_t text_id) {
  if (!renderer) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!rf->world_resources.initialized) {
    return false_v;
  }
  return vkr_world_resources_text_destroy(rf, &rf->world_resources, text_id);
}

uint32_t
vkr_renderer_present_target_image_count(VkrRendererFrontendHandle renderer) {
  return renderer->impl.caps.present_target_image_count;
}

VkrPresentTargetKind
vkr_renderer_present_target_kind(VkrRendererFrontendHandle renderer) {
  return renderer->impl.caps.present_target_kind;
}

void vkr_renderer_present_target_extent(VkrRendererFrontendHandle renderer,
                                        uint32_t *out_width,
                                        uint32_t *out_height) {
  if (out_width) {
    *out_width = renderer->last_window_width;
  }
  if (out_height) {
    *out_height = renderer->last_window_height;
  }
}

VkrTextureFormat
vkr_renderer_present_target_format(VkrRendererFrontendHandle renderer,
                                   VkrPresentTargetAttachment attachment) {
  return attachment == VKR_PRESENT_TARGET_ATTACHMENT_COLOR
             ? renderer->impl.caps.present_color_format
             : renderer->impl.caps.present_depth_format;
}

VkrRendererError
vkr_renderer_present_target_recreate(VkrRendererFrontendHandle renderer,
                                     uint32_t width, uint32_t height,
                                     uint32_t image_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (width == 0 || height == 0 || image_count == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VkrRendererError result = renderer->impl.ops->present_target_recreate(
      renderer->impl.state, width, height, image_count);
  if (result != VKR_RENDERER_ERROR_NONE) {
    return result;
  }
  width = renderer->present_target.width;
  height = renderer->present_target.height;
  renderer->last_window_width = width;
  renderer->last_window_height = height;
  if (renderer->ui_system.initialized) {
    vkr_ui_system_resize(renderer, &renderer->ui_system, width, height);
  }
  vkr_shadow_system_invalidate_fit_history(&renderer->shadow_system);
  renderer->timing_result.shadow_depth_range = (VkrShadowDepthRangeSample){0};
  return VKR_RENDERER_ERROR_NONE;
}

VkrTextureFormat
vkr_renderer_get_shadow_depth_format(VkrRendererFrontendHandle renderer) {
  return renderer->impl.caps.shadow_depth_format;
}

uint32_t
vkr_renderer_frame_in_flight_index(VkrRendererFrontendHandle renderer) {
  return renderer->impl.ops->frame_in_flight_index(renderer->impl.state);
}

uint32_t
vkr_renderer_frame_in_flight_count(VkrRendererFrontendHandle renderer) {
  return renderer->impl.caps.frame_in_flight_count;
}

static VkrRendererError
vkr_renderer_validation_fail(VkrValidationError *out_error,
                             VkrRendererError code, const char *field_path,
                             const char *message) {
  if (out_error) {
    out_error->code = code;
    out_error->field_path = field_path;
    out_error->message = message;
  }
  return code;
}

static VkrRendererError vkr_renderer_validate_packet_array(
    const void *data, uint32_t count, uint32_t capacity, const char *data_field,
    const char *count_field, VkrValidationError *out_error) {
  if (count > capacity)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, count_field,
        "exceeds the fixed packet capacity");
  if (count > 0u && !data)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, data_field,
        "must contain every declared row");
  return VKR_RENDERER_ERROR_NONE;
}

static VkrRendererError
vkr_renderer_validate_draw_ranges(const VkrDrawItem *draws, uint32_t draw_count,
                                  uint32_t instance_count, const char *field,
                                  VkrValidationError *out_error) {
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->first_instance > instance_count ||
        draw->instance_count > instance_count - draw->first_instance)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, field,
          "contains a draw outside the payload instance array");
  }
  return VKR_RENDERER_ERROR_NONE;
}

static VkrRendererError vkr_renderer_validate_text_draws(
    const VkrPreparedTextDraw *draws, uint32_t draw_count, const char *field,
    const char *count_field, VkrValidationError *out_error) {
  const VkrRendererError array_error = vkr_renderer_validate_packet_array(
      draws, draw_count, VKR_GPU_DRAW_CANDIDATE_CAPACITY, field, count_field,
      out_error);
  if (array_error != VKR_RENDERER_ERROR_NONE)
    return array_error;
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    if (draw->vertex_count == 0u || draw->index_count == 0u ||
        !draw->vertices || !draw->indices ||
        draw->max_index >= draw->vertex_count)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, field,
          "contains incomplete or out-of-range indexed geometry");
  }
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t vkr_renderer_texture_pressure_budget(const VkrDeviceMemoryStats *stats,
                                             bool8_t pressure_active,
                                             uint64_t *out_budget,
                                             bool8_t *out_pressure_active) {
  if (!stats || !out_budget || !out_pressure_active ||
      !stats->heap_usage_valid || stats->heap_count == 0u) {
    return false_v;
  }
  uint64_t usage = 0u;
  uint64_t budget = 0u;
  for (uint32_t i = 0u; i < stats->heap_count; ++i) {
    if (UINT64_MAX - usage < stats->heap_usage_bytes[i] ||
        UINT64_MAX - budget < stats->heap_budget_bytes[i]) {
      return false_v;
    }
    usage += stats->heap_usage_bytes[i];
    budget += stats->heap_budget_bytes[i];
  }
  if (budget == 0u) {
    return false_v;
  }
  if (usage >= budget - budget / 10u) {
    const uint64_t texture_bytes =
        stats->owners[VKR_GPU_ALLOCATION_OWNER_TEXTURE].live_bytes;
    const uint64_t non_texture_bytes =
        usage > texture_bytes ? usage - texture_bytes : 0u;
    const uint64_t target_usage = budget - budget / 5u;
    *out_budget = target_usage > non_texture_bytes
                      ? target_usage - non_texture_bytes
                      : 0u;
    *out_pressure_active = true_v;
    return true_v;
  }
  if (pressure_active && usage <= budget - budget / 4u) {
    *out_budget = UINT64_MAX;
    *out_pressure_active = false_v;
    return true_v;
  }
  return false_v;
}

static void renderer_frontend_update_texture_pressure(RendererFrontend *rf) {
  if (!rf || rf->material_system.texture_stream_budget_user_configured ||
      rf->frame_number < rf->texture_pressure_poll_frame + 60u ||
      !rf->impl.ops || !rf->impl.ops->get_device_memory_stats) {
    return;
  }
  rf->texture_pressure_poll_frame = rf->frame_number;
  VkrDeviceMemoryStats stats = {0};
  if (!rf->impl.ops->get_device_memory_stats(rf->impl.state, &stats)) {
    return;
  }
  uint64_t texture_budget = 0u;
  bool8_t pressure_active = rf->texture_pressure_active;
  if (vkr_renderer_texture_pressure_budget(&stats, rf->texture_pressure_active,
                                           &texture_budget, &pressure_active)) {
    vkr_material_system_set_automatic_texture_residency_budget(
        &rf->material_system, texture_budget);
    rf->texture_pressure_active = pressure_active;
  }
}

static bool8_t renderer_frontend_pump_assets(RendererFrontend *rf) {
  renderer_frontend_update_texture_pressure(rf);
  const VkrAssetPublisher *publisher = &rf->asset_publisher;
  const bool8_t batching = publisher->begin_texture_upload_batch != NULL;
  if (batching != (publisher->end_texture_upload_batch != NULL) ||
      (batching && !publisher->begin_texture_upload_batch(publisher->state))) {
    log_error("Renderer asset texture upload batch initialization failed");
    return false_v;
  }
  vkr_resource_system_pump(NULL);
  if (batching && !publisher->end_texture_upload_batch(publisher->state)) {
    log_error("Renderer asset texture upload batch submission failed");
    return false_v;
  }
  vkr_material_system_pump_texture_streams(&rf->material_system, 32u);
  vkr_mesh_manager_pump_async(&rf->mesh_manager);
  return true_v;
}

static VkrRendererError
renderer_impl_metal_prepare_frame(void *state, VkrFrameSetup *out_setup) {
  RendererFrontend *rf = state;
#if defined(PLATFORM_APPLE)
  if (rf->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (rf->window) {
    VkrWindowPixelSize pixels = vkr_window_get_pixel_size(rf->window);
    if (pixels.width == 0 || pixels.height == 0) {
      return VKR_RENDERER_ERROR_FRAME_SKIPPED;
    }
    rf->last_window_width = pixels.width;
    rf->last_window_height = pixels.height;
  }
  rf->frame_active = true_v;
  if (!renderer_frontend_pump_assets(rf)) {
    rf->frame_active = false_v;
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  VkrRenderGraphFrameInfo frame = {
      .frame_index = (uint32_t)(rf->frame_number + 1u),
      .image_index = 0,
      .delta_time = 1.0 / 60.0,
      .target_width = rf->last_window_width,
      .target_height = rf->last_window_height,
      .window_width = rf->last_window_width,
      .window_height = rf->last_window_height,
      .viewport_width = rf->last_window_width,
      .viewport_height = rf->last_window_height,
      .picking_pending = false_v,
      .target_color_format = rf->impl.caps.present_color_format,
      .target_depth_format = rf->impl.caps.present_depth_format,
      .target_color_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                     .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
      .target_depth_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                     .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
      .target_terminal_state = {.access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
                                .layout =
                                    VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      .shadow_depth_format = rf->impl.caps.shadow_depth_format,
      .shadow_map_size =
          rf->shadow_system.initialized
              ? vkr_shadow_config_get_max_map_size(&rf->shadow_system.config)
              : 2048,
      .shadow_map_layer_count = rf->shadow_system.initialized
                                    ? rf->shadow_system.config.cascade_count
                                    : 1u,
      .shadow_cascade_count = rf->shadow_system.initialized
                                  ? rf->shadow_system.config.cascade_count
                                  : 0,
  };
  if (!vkr_metal_packet_renderer_prepare_frame(rf->metal_renderer, &frame)) {
    rf->frame_active = false_v;
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  frame.image_index =
      vkr_metal_packet_renderer_frame_image_index(rf->metal_renderer);
  rf->timing_completed_ready = renderer_impl_metal_poll_submit_result(
      rf, rf->timing_last_completed_submit_value, &rf->timing_result);
  if (rf->timing_completed_ready) {
    rf->timing_last_completed_submit_value = rf->timing_result.submit_value;
  }
  rf->frame_number++;
  MemZero(&rf->frame_metrics, sizeof(rf->frame_metrics));
  *out_setup = (VkrFrameSetup){
      .image_index = frame.image_index,
      .window_width = rf->last_window_width,
      .window_height = rf->last_window_height,
      .swapchain_format = frame.target_color_format,
      .swapchain_depth_format = frame.target_depth_format,
  };
  vkr_metal_packet_renderer_retained_shadow_token(
      rf->metal_renderer, frame.image_index, &out_setup->retained_shadow);
  return VKR_RENDERER_ERROR_NONE;
#else
  (void)out_setup;
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
}

static VkrRendererError
renderer_impl_vulkan_prepare_frame(void *state, VkrFrameSetup *out_setup) {
  RendererFrontend *rf = state;
  if (rf->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (!vkr_vulkan_renderer_prepare_frame(
          rf->vulkan_renderer, rf->frame_number + 1u,
          rf->shadow_system.initialized
              ? vkr_shadow_config_get_max_map_size(&rf->shadow_system.config)
              : 2048u,
          rf->shadow_system.initialized ? rf->shadow_system.config.cascade_count
                                        : 1u,
          out_setup)) {
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  rf->frame_active = true_v;
  // Async finalizers may publish GPU resources only after the selected
  // implementation has activated a frame. Both selected strategies pump at
  // this lifecycle point so dependency closures can reach READY and stamp the
  // submit that carries their uploads.
  if (!renderer_frontend_pump_assets(rf)) {
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  rf->frame_number++;
  rf->last_window_width = out_setup->window_width;
  rf->last_window_height = out_setup->window_height;
  rf->timing_completed_ready = renderer_impl_vulkan_poll_submit_result(
      rf, rf->timing_last_completed_submit_value, &rf->timing_result);
  if (rf->timing_completed_ready) {
    rf->timing_last_completed_submit_value = rf->timing_result.submit_value;
  }
  MemZero(&rf->frame_metrics, sizeof(rf->frame_metrics));
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError vkr_renderer_prepare_frame(VkrRendererFrontendHandle renderer,
                                            VkrFrameSetup *out_setup) {
  if (!renderer->frame_active) {
    const uint64_t resize = vkr_atomic_uint64_exchange(
        &renderer->pending_resize_mailbox, 0u, VKR_MEMORY_ORDER_ACQ_REL);
    if (resize) {
      vkr_renderer_resize(renderer, (uint32_t)(resize >> 32), (uint32_t)resize);
    }
  }
  return renderer->impl.ops->prepare_frame(renderer->impl.state, out_setup);
}

static VkrRendererError
renderer_impl_metal_submit_packet(void *state, const VkrRenderPacket *packet,
                                  VkrRendererFrameMetrics *out_metrics,
                                  VkrValidationError *out_validation_error) {
  RendererFrontend *rf = state;
  if (!rf->frame_active) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_FRAME_IN_PROGRESS, "frame",
        "frame is not active; call vkr_renderer_prepare_frame first");
  }

#if defined(PLATFORM_APPLE)
  VkrRendererPreparedPacket prepared;
  vkr_renderer_prepare_packet(rf, packet, &prepared);

  VkrMetalPacketResult result = {0};
  const bool8_t submitted = vkr_metal_packet_renderer_submit_packet(
      rf->metal_renderer, &prepared.packet, &result);
  rf->frame_active = false_v;
  if (!submitted) {
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_SUBMISSION_FAILED, "metal",
        "Metal packet submission failed");
  }
  VkrRendererImplSubmitResult current_result = {0};
  vkr_renderer_impl_lower_metal_result(&result, &current_result);
  current_result.source_frame_index = packet->frame.frame_index;
  if (!rf->timing_completed_ready) {
    rf->timing_result = current_result;
    rf->timing_result.source_frame_index = packet->frame.frame_index;
  }
  const VkrRendererImplSubmitResult *observed = &rf->timing_result;
  rf->frame_metrics.gpu_submission_ns = observed->gpu_submission_ns;
  rf->frame_metrics.gpu_submission_valid = observed->gpu_submission_valid;
  rf->frame_metrics.gpu_submission_unavailable_reason =
      vkr_renderer_gpu_timing_metric_reason(
          observed->gpu_submission_unavailable_reason);
  rf->frame_metrics.world.draws_collected = observed->indexed_draw_count;
  rf->frame_metrics.world.opaque_draws = observed->opaque_draw_count;
  rf->frame_metrics.world.transmission_draws =
      observed->transmission_draw_count;
  rf->frame_metrics.world.transparent_draws = observed->blend_draw_count;
  rf->frame_metrics.world.draws_issued = observed->indexed_draw_count;
  rf->frame_metrics.world.draw_calls_issued = observed->indexed_draw_count;
  vkr_renderer_record_gpu_candidate_metrics(rf, &prepared.packet);
  /* From this submit's own result: packet lowering ran on this thread now,
     while `observed` may still describe an older completed frame. */
  rf->frame_metrics.packet_build = result.packet_build;
  rf->frame_metrics.world.hzb_history_valid = observed->hzb_history_valid;
  if (observed->has_gpu_draw_diagnostics) {
    rf->frame_metrics.world.opaque_draws = observed->gpu_visible_count;
    rf->frame_metrics.world.transmission_draws =
        observed->transmission_gpu_visible_count;
    rf->frame_metrics.world.draws_collected =
        rf->frame_metrics.world.gpu_candidate_count +
        rf->frame_metrics.world.transmission_gpu_candidate_count +
        observed->blend_draw_count;
    rf->frame_metrics.world.gpu_visible_count = observed->gpu_visible_count;
    MemCopy(rf->frame_metrics.world.gpu_bucket_counts,
            observed->gpu_bucket_counts,
            sizeof(rf->frame_metrics.world.gpu_bucket_counts));
    rf->frame_metrics.world.gpu_compaction_overflow_count =
        observed->gpu_overflow_count;
    rf->frame_metrics.world.gpu_resolve_invalid_count =
        observed->gpu_resolve_invalid_count;
    rf->frame_metrics.world.gpu_occlusion_culled_count =
        observed->gpu_occlusion_culled_count;
    rf->frame_metrics.world.transmission_gpu_visible_count =
        observed->transmission_gpu_visible_count;
    MemCopy(rf->frame_metrics.world.transmission_gpu_bucket_counts,
            observed->transmission_gpu_bucket_counts,
            sizeof(rf->frame_metrics.world.transmission_gpu_bucket_counts));
    rf->frame_metrics.world.transmission_gpu_compaction_overflow_count =
        observed->transmission_gpu_overflow_count;
    rf->frame_metrics.world.transmission_gpu_occlusion_culled_count =
        observed->transmission_gpu_occlusion_culled_count;
    rf->frame_metrics.world.transmission_pixel_compaction_overflow_count =
        observed->transmission_compact_overflow_count;
    rf->frame_metrics.world.gpu_diagnostics_valid = true_v;
    uint32_t opaque_indirect_calls = 0u;
    uint32_t transmission_indirect_calls = 0u;
    uint32_t max_batch_size = 0u;
    const uint32_t transmission_layers =
        ArrayCount(observed->transmission_covered_pixels);
    for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
         ++bucket) {
      opaque_indirect_calls +=
          observed->gpu_bucket_counts[bucket] > 0u ? 1u : 0u;
      max_batch_size = MAX(max_batch_size, observed->gpu_bucket_counts[bucket]);
      if (observed->transmission_gpu_bucket_counts[bucket] > 0u)
        transmission_indirect_calls += transmission_layers;
      max_batch_size =
          MAX(max_batch_size, observed->transmission_gpu_bucket_counts[bucket]);
    }
    const uint32_t indirect_draws =
        observed->gpu_visible_count +
        observed->transmission_gpu_visible_count * transmission_layers;
    const uint32_t indirect_calls =
        opaque_indirect_calls + transmission_indirect_calls;
    rf->frame_metrics.world.opaque_batches = opaque_indirect_calls;
    rf->frame_metrics.world.indirect_draws_issued = indirect_draws;
    rf->frame_metrics.world.indirect_calls_issued = indirect_calls;
    rf->frame_metrics.world.draws_issued =
        indirect_draws + observed->blend_draw_count;
    rf->frame_metrics.world.draw_calls_issued =
        indirect_calls + observed->blend_draw_count;
    rf->frame_metrics.world.max_batch_size = max_batch_size;
    rf->frame_metrics.world.avg_batch_size =
        indirect_calls > 0u ? (float32_t)indirect_draws / indirect_calls : 0.0f;
  }
  if (observed->has_gpu_draw_diagnostics) {
    for (uint32_t cascade = 0u; cascade < VKR_SHADOW_CASCADE_COUNT_MAX;
         ++cascade) {
      rf->frame_metrics.shadow.shadow_indirect_draws_opaque[cascade] =
          observed->shadow_gpu_visible_count[cascade];
      uint32_t indirect_calls = 0u;
      for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
           ++bucket) {
        indirect_calls +=
            observed->shadow_gpu_bucket_counts[cascade][bucket] > 0u ? 1u : 0u;
      }
      rf->frame_metrics.shadow.shadow_indirect_calls_opaque[cascade] =
          indirect_calls;
      rf->frame_metrics.shadow.shadow_indirect_overflow[cascade] =
          observed->shadow_gpu_overflow_count[cascade];
    }
  }
  if (out_metrics) {
    *out_metrics = rf->frame_metrics;
  }
  return VKR_RENDERER_ERROR_NONE;
#else
  rf->frame_active = false_v;
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
}

VkrRendererError
vkr_renderer_validate_packet(const VkrRenderPacket *packet,
                             VkrValidationError *out_validation_error) {
  if (!packet) {
    return vkr_renderer_validation_fail(out_validation_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        "packet", "must not be null");
  }

#define VKR_REJECT_PACKET(CODE, FIELD, MESSAGE)                                \
  do {                                                                         \
    return vkr_renderer_validation_fail(out_validation_error, CODE, FIELD,     \
                                        MESSAGE);                              \
  } while (0)

  if (packet->packet_version != VKR_RENDER_PACKET_VERSION)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE,
                      "packet.packet_version",
                      "does not match VKR_RENDER_PACKET_VERSION");

  const VkrWorldPassPayload *world = packet->world;
  if (world) {
    VkrRendererError error = vkr_renderer_validate_packet_array(
        world->gpu_candidates, world->gpu_candidate_count,
        VKR_GPU_DRAW_CANDIDATE_CAPACITY, "packet.world.gpu_candidates",
        "packet.world.gpu_candidate_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        world->transmission_gpu_candidates,
        world->transmission_gpu_candidate_count,
        VKR_GPU_DRAW_CANDIDATE_CAPACITY,
        "packet.world.transmission_gpu_candidates",
        "packet.world.transmission_gpu_candidate_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    if (world->static_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.static_candidate_count",
                        "cannot exceed the source candidate count");
    if (world->gpu_candidate_count > 0u && world->static_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.static_generation", "must be nonzero");
    if (world->gpu_candidate_count > 0u && world->dynamic_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.dynamic_generation", "must be nonzero");
    if (world->gpu_candidate_count > 0u && world->publication_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.publication_generation",
                        "must be nonzero");
    if (world->gpu_camera_opaque_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.gpu_camera_opaque_candidate_count",
                        "cannot exceed the source candidate count");
    if (world->gpu_shadow_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.gpu_shadow_candidate_count",
                        "cannot exceed the source candidate count");
    error = vkr_renderer_validate_packet_array(
        world->instances, world->instance_count,
        VKR_INSTANCE_BUFFER_MAX_INSTANCES, "packet.world.instances",
        "packet.world.instance_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        world->transparent_draws, world->transparent_draw_count,
        VKR_INSTANCE_BUFFER_MAX_INSTANCES, "packet.world.transparent_draws",
        "packet.world.transparent_draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_draw_ranges(
        world->transparent_draws, world->transparent_draw_count,
        world->instance_count, "packet.world.transparent_draws",
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_text_draws(
        world->text_draws, world->text_draw_count, "packet.world.text_draws",
        "packet.world.text_draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
  }
  const VkrShadowPassPayload *shadow = packet->shadow;
  if (shadow) {
    if (shadow->cascade_count == 0u ||
        shadow->cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.cascade_count",
                        "must be within the supported cascade range");
    const uint32_t cascade_mask = (UINT32_C(1) << shadow->cascade_count) - 1u;
    if ((shadow->cascade_render_mask & ~cascade_mask) != 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.cascade_render_mask",
                        "contains a bit outside cascade_count");
    /* The receiver hot path indexes the Poisson table and divides by the
       per-cascade depth span with no recovery branch, so every value it trusts
       is proven here instead. */
    const VkrShadowReceiverPacketData *receiver = &shadow->receiver;
    if (!vkr_shadow_pcf_sample_count_supported(receiver->pcf_sample_count))
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_sample_count",
                        "must be one of the supported tap counts");
    if (receiver->pcf_uniform_early_out > 1u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_uniform_early_out",
                        "must be zero or one");
    if (!isfinite(receiver->pcf_radius_texels) ||
        receiver->pcf_radius_texels < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_radius_texels",
                        "must be finite and non-negative");
    if (!isfinite(receiver->receiver_bias_texels) ||
        receiver->receiver_bias_texels < 0.0f ||
        !isfinite(receiver->slope_bias_texels) ||
        receiver->slope_bias_texels < 0.0f ||
        !isfinite(receiver->normal_offset_texels) ||
        receiver->normal_offset_texels < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver",
                        "bias texel counts must be finite and non-negative");
    if (!isfinite(receiver->cascade_blend_fraction) ||
        receiver->cascade_blend_fraction < 0.0f ||
        receiver->cascade_blend_fraction > 0.5f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.cascade_blend_fraction",
                        "must be finite and within [0, 0.5]");
    if (!isfinite(receiver->fade_start) || receiver->fade_start < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_start",
                        "must be finite and non-negative");
    if (!isfinite(receiver->fade_end) ||
        receiver->fade_end < receiver->fade_start)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_end",
                        "must be finite and not precede fade_start");
    for (uint32_t i = 0; i < shadow->cascade_count; ++i) {
      const Vec4 slice = shadow->cascades[i].split_near_far_texel_depth;
      if (!isfinite(slice.x) || !isfinite(slice.y) || slice.y <= slice.x)
        VKR_REJECT_PACKET(
            VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
            "packet.shadow.cascades.split_near_far_texel_depth",
            "cascade slice bounds must be finite and strictly ordered");
      /* Both must be strictly positive: the receiver divides the light-space
         origin by the texel size to build its rotation cell, and divides the
         texel-denominated bias by the depth span. */
      if (!isfinite(slice.z) || slice.z <= 0.0f || !isfinite(slice.w) ||
          slice.w <= 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.cascades.split_near_far_texel_depth",
                          "texel size and depth span must be positive");
      const Vec4 origin = shadow->cascades[i].origin_inv_size_pad;
      if (!isfinite(origin.x) || !isfinite(origin.y) || !isfinite(origin.z) ||
          origin.z <= 0.0f)
        VKR_REJECT_PACKET(
            VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
            "packet.shadow.cascades.origin_inv_size_pad",
            "light-space origin must be finite and inverse map size positive");
    }
    const float32_t final_split = shadow->cascades[shadow->cascade_count - 1u]
                                      .split_near_far_texel_depth.y;
    if (receiver->fade_end > final_split)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_end",
                        "must not extend beyond the final cascade split");
    const VkrShadowConfigOverride *bias = shadow->config_override;
    if (bias) {
      if (!isfinite(bias->depth_bias_constant) ||
          bias->depth_bias_constant < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_constant",
                          "must be finite and non-negative");
      if (!isfinite(bias->depth_bias_slope) || bias->depth_bias_slope < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_slope",
                          "must be finite and non-negative");
      if (!isfinite(bias->depth_bias_clamp) || bias->depth_bias_clamp < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_clamp",
                          "must be finite and non-negative");
    }
  }

  const VkrUiPassPayload *ui = packet->ui;
  if (ui) {
    VkrRendererError error = vkr_renderer_validate_packet_array(
        ui->instances, ui->instance_count, VKR_INSTANCE_BUFFER_MAX_INSTANCES,
        "packet.ui.instances", "packet.ui.instance_count",
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        ui->draws, ui->draw_count, VKR_INSTANCE_BUFFER_MAX_INSTANCES,
        "packet.ui.draws", "packet.ui.draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_draw_ranges(
        ui->draws, ui->draw_count, ui->instance_count, "packet.ui.draws",
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_text_draws(
        ui->text_draws, ui->text_draw_count, "packet.ui.text_draws",
        "packet.ui.text_draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
  }

  const VkrEditorPassPayload *editor = packet->editor;
  if (editor) {
    VkrRendererError error = vkr_renderer_validate_packet_array(
        editor->instances, editor->instance_count,
        VKR_INSTANCE_BUFFER_MAX_INSTANCES, "packet.editor.instances",
        "packet.editor.instance_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        editor->draws, editor->draw_count, VKR_INSTANCE_BUFFER_MAX_INSTANCES,
        "packet.editor.draws", "packet.editor.draw_count",
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_draw_ranges(
        editor->draws, editor->draw_count, editor->instance_count,
        "packet.editor.draws", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
  }

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting) {
    if (lighting->point_light_count > VKR_MAX_SCENE_POINT_LIGHTS)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_light_count",
                        "exceeds the fixed scene-light capacity");
    if (lighting->point_light_count > 0u &&
        (!lighting->point_lights || !lighting->point_light_grid))
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_lights",
                        "lights and their lookup grid must both be present");
    if (lighting->point_light_grid &&
        lighting->point_light_grid->cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_light_grid.cell_count",
                        "exceeds the fixed light-grid capacity");
    if (lighting->ibl_probe_count > VKR_FRAME_IBL_PROBE_MAX)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.ibl_probe_count",
                        "exceeds the fixed frame-probe capacity");
    if (lighting->ibl_probe_count > 0u && !lighting->ibl_probes)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.ibl_probes",
                        "must contain every declared probe");
  }

  const VkrTextUpdatesPayload *updates = packet->text_updates;
  if (updates) {
    if (updates->world_text_update_count > 0u && !updates->world_text_updates)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.text_updates.world_text_updates",
                        "must contain every declared update");
    if (updates->ui_text_update_count > 0u && !updates->ui_text_updates)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.text_updates.ui_text_updates",
                        "must contain every declared update");
  }

  if (packet->debug && packet->debug->shadow_debug_mode > 3u)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.debug.shadow_debug_mode",
                      "must be a supported debug view");

#undef VKR_REJECT_PACKET
  if (out_validation_error)
    *out_validation_error = (VkrValidationError){0};
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
vkr_renderer_submit_packet(VkrRendererFrontendHandle renderer,
                           const VkrRenderPacket *packet,
                           VkrRendererFrameMetrics *out_metrics,
                           VkrValidationError *out_validation_error) {
  if (!renderer)
    return vkr_renderer_validation_fail(out_validation_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        "renderer", "must not be null");
  const VkrRendererError validation_error =
      vkr_renderer_validate_packet(packet, out_validation_error);
  if (validation_error != VKR_RENDERER_ERROR_NONE) {
    if (renderer->frame_active) {
      const VkrRendererError cancel_error =
          renderer->impl.ops->cancel_frame(renderer->impl.state);
      if (cancel_error != VKR_RENDERER_ERROR_NONE)
        return vkr_renderer_validation_fail(
            out_validation_error, cancel_error, "frame",
            "failed to cancel the active frame after packet rejection");
    }
    return validation_error;
  }
  return renderer->impl.ops->submit_packet(renderer->impl.state, packet,
                                           out_metrics, out_validation_error);
}

void vkr_renderer_resize(VkrRendererFrontendHandle renderer, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Resizing renderer to %d %d", width, height);

  RendererFrontend *rf = (RendererFrontend *)renderer;

  rf->impl.ops->resize(rf->impl.state, width, height);

  if (rf->rf_mutex) {
    vkr_mutex_lock(rf->rf_mutex);
  }
  if (rf->window) {
    rf->window->width = width;
    rf->window->height = height;
  }
  rf->last_window_width = width;
  rf->last_window_height = height;
  if (rf->rf_mutex) {
    if (!vkr_mutex_unlock(rf->rf_mutex)) {
      log_error("Failed to unlock renderer mutex");
    }
  }

  if (rf->ui_system.initialized) {
    vkr_ui_system_resize(rf, &rf->ui_system, width, height);
  }
  /* Every resize path recreates or invalidates target state. A skipped frame
     may separate the stored fit from the next camera pose. */
  vkr_shadow_system_invalidate_fit_history(&rf->shadow_system);
  rf->timing_result.shadow_depth_range = (VkrShadowDepthRangeSample){0};
}

static VkrRendererError renderer_impl_metal_cancel_frame(void *state) {
  RendererFrontend *renderer = state;
#if defined(PLATFORM_APPLE)
  if (!vkr_metal_packet_renderer_cancel_frame(renderer->metal_renderer))
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  renderer->frame_active = false_v;
  return VKR_RENDERER_ERROR_NONE;
#else
  (void)renderer;
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
}

static VkrRendererError renderer_impl_vulkan_cancel_frame(void *state) {
  RendererFrontend *renderer = state;
  vkr_vulkan_renderer_cancel_frame(renderer->vulkan_renderer);
  renderer->frame_active = false_v;
  return VKR_RENDERER_ERROR_NONE;
}

static void renderer_impl_vulkan_resize(void *state, uint32_t width,
                                        uint32_t height) {
  RendererFrontend *renderer = state;
  if (vkr_vulkan_renderer_resize(
          renderer->vulkan_renderer, width, height,
          renderer->impl.caps.present_target_image_count)) {
    renderer->last_window_width = width;
    renderer->last_window_height = height;
    renderer->present_target.width = width;
    renderer->present_target.height = height;
  }
}

static VkrRendererError renderer_impl_vulkan_present_target_recreate(
    void *state, uint32_t width, uint32_t height, uint32_t image_count) {
  RendererFrontend *renderer = state;
  if (!vkr_vulkan_renderer_resize(renderer->vulkan_renderer, width, height,
                                  image_count)) {
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  renderer->last_window_width = width;
  renderer->last_window_height = height;
  renderer->present_target.width = width;
  renderer->present_target.height = height;
  renderer->present_target.image_count = image_count;
  renderer->impl.caps.present_target_image_count = image_count;
  return VKR_RENDERER_ERROR_NONE;
}

static uint32_t renderer_impl_vulkan_frame_in_flight_index(void *state) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_frame_slot(renderer->vulkan_renderer);
}

static bool8_t renderer_impl_vulkan_poll_submit_result(
    void *state, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result) {
  RendererFrontend *renderer = state;
  VkrVulkanResult source = {0};
  if (!vkr_vulkan_renderer_poll_result(renderer->vulkan_renderer,
                                       after_submit_value, &source)) {
    return false_v;
  }
  *out_result = (VkrRendererImplSubmitResult){
      .submit_value = source.submit_value,
      .source_frame_index = source.source_frame_index,
      .executed_pass_count = source.pass_timing_count,
      .indexed_draw_count = source.indexed_draw_count,
      .shadow_draw_count = source.shadow_draw_count,
      .opaque_draw_count = source.opaque_draw_count,
      .transmission_draw_count = source.transmission_draw_count,
      .blend_draw_count = source.blend_draw_count,
      .gpu_visible_count = source.gpu_visible_count,
      .gpu_overflow_count = source.gpu_overflow_count,
      .gpu_resolve_invalid_count = source.gpu_resolve_invalid_count,
      .gpu_occlusion_culled_count = source.gpu_occlusion_culled_count,
      .transmission_gpu_visible_count = source.transmission_gpu_visible_count,
      .transmission_gpu_overflow_count = source.transmission_gpu_overflow_count,
      .transmission_gpu_occlusion_culled_count =
          source.transmission_gpu_occlusion_culled_count,
      .transmission_coverage_valid = source.has_transmission_coverage,
      .hzb_history_valid = source.hzb_history_valid,
      .shadow_depth_range = source.shadow_depth_range,
      .has_gpu_draw_diagnostics = source.has_gpu_draw_diagnostics,
      .pass_timing_count = source.pass_timing_count,
  };
  MemCopy(out_result->gpu_bucket_counts, source.gpu_bucket_counts,
          sizeof(out_result->gpu_bucket_counts));
  MemCopy(out_result->transmission_gpu_bucket_counts,
          source.transmission_gpu_bucket_counts,
          sizeof(out_result->transmission_gpu_bucket_counts));
  MemCopy(out_result->transmission_covered_pixels,
          source.transmission_covered_pixels,
          sizeof(out_result->transmission_covered_pixels));
  MemCopy(out_result->transmission_coverage_extent,
          source.transmission_coverage_extent,
          sizeof(out_result->transmission_coverage_extent));
  MemCopy(out_result->shadow_gpu_visible_count, source.shadow_gpu_visible_count,
          sizeof(out_result->shadow_gpu_visible_count));
  MemCopy(out_result->shadow_gpu_bucket_counts, source.shadow_gpu_bucket_counts,
          sizeof(out_result->shadow_gpu_bucket_counts));
  MemCopy(out_result->shadow_gpu_overflow_count,
          source.shadow_gpu_overflow_count,
          sizeof(out_result->shadow_gpu_overflow_count));
  MemCopy(out_result->pass_timings, source.pass_timings,
          (uint64_t)source.pass_timing_count *
              sizeof(*out_result->pass_timings));
  (void)renderer_impl_vulkan_memory_metrics(renderer, &out_result->memory);
  return true_v;
}

static VkrCaptureStatus
renderer_impl_vulkan_capture_poll(void *state, VkrCaptureRequestId request_id,
                                  VkrCapturePollResult *out_result) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_capture_poll(renderer->vulkan_renderer, request_id,
                                          out_result);
}

static bool8_t
renderer_impl_vulkan_capture_release(void *state,
                                     VkrCaptureRequestId request_id) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_capture_release(renderer->vulkan_renderer,
                                             request_id);
}

static VkrAllocator *renderer_impl_vulkan_allocator(void *state) {
  RendererFrontend *renderer = state;
  return vkr_vulkan_renderer_allocator(renderer->vulkan_renderer);
}

VkrRendererError vkr_renderer_cancel_frame(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  if (!renderer->frame_active) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  return renderer->impl.ops->cancel_frame(renderer->impl.state);
}

vkr_internal bool8_t renderer_frontend_initialize_packet_systems(
    RendererFrontend *rf, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers) {
  log_debug("Initializing packet renderer resource system");
  if (!vkr_resource_system_init(&rf->allocator, rf, job_system,
                                metrics_producers)) {
    log_error("Packet renderer resource system initialization failed");
    return false_v;
  }
  log_debug("Initializing packet renderer geometry system");

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrGeometrySystemConfig geometry_config = {
      .max_geometries = 16384,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_geometry_system_init(&rf->geometry_system, &geometry_config,
                                &error)) {
    log_error("Packet renderer geometry system initialization failed");
    return false_v;
  }
  log_debug("Initializing packet renderer texture system");
  VkrTextureSystemConfig texture_config = {
      .max_texture_count = 16384,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_texture_system_init(rf, &texture_config, job_system,
                               &rf->texture_system)) {
    log_error("Packet renderer texture system initialization failed");
    return false_v;
  }
  log_debug("Initializing packet renderer material system");
  rf->texture_system.hdr_decode_metrics = rf->hdr_decode_metrics;
  VkrMaterialSystemConfig material_config = {
      .max_material_count = 8192,
      .asset_publisher = &rf->asset_publisher,
  };
  if (!vkr_material_system_init(&rf->material_system, rf->arena,
                                &rf->texture_system, &material_config)) {
    log_error("Packet renderer material system initialization failed");
    return false_v;
  }
  VkrMeshManagerConfig mesh_config = {.max_mesh_count = 16384};
  if (!vkr_mesh_manager_init(&rf->mesh_manager, &rf->geometry_system,
                             &rf->material_system, &mesh_config)) {
    return false_v;
  }

  const uint32_t pool_chunk_count =
      job_system ? job_system->worker_count + 4 : 8;
  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mesh_arena_pool)) {
    return false_v;
  }
  rf->mesh_loader =
      (VkrMeshLoaderContext){.geometry_system = &rf->geometry_system,
                             .material_system = &rf->material_system,
                             .mesh_manager = &rf->mesh_manager,
                             .job_system = job_system,
                             .arena_pool = &rf->mesh_arena_pool};
  rf->mesh_loader.allocator.ctx = rf->arena;
  vkr_allocator_arena(&rf->mesh_loader.allocator);
  if (!vkr_dmemory_create(VKR_MESH_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_MESH_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->mesh_loader.async_memory)) {
    return false_v;
  }
  rf->mesh_loader.async_allocator =
      (VkrAllocator){.ctx = &rf->mesh_loader.async_memory};
  vkr_dmemory_allocator_create(&rf->mesh_loader.async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->mesh_loader.async_mutex)) {
    return false_v;
  }
  if (!vkr_dmemory_create(VKR_SCENE_LOADER_ASYNC_DMEMORY_INITIAL,
                          VKR_SCENE_LOADER_ASYNC_DMEMORY_RESERVE,
                          &rf->scene_async_memory)) {
    return false_v;
  }
  rf->scene_async_allocator = (VkrAllocator){.ctx = &rf->scene_async_memory};
  vkr_dmemory_allocator_create(&rf->scene_async_allocator);
  if (!vkr_mutex_create(&rf->allocator, &rf->scene_async_mutex)) {
    return false_v;
  }
  rf->mesh_manager.loader_context = &rf->mesh_loader;

  if (!vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->bitmap_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->system_font_arena_pool) ||
      !vkr_arena_pool_create(MB(6), pool_chunk_count, &rf->allocator,
                             &rf->mtsdf_font_arena_pool)) {
    return false_v;
  }
  rf->bitmap_font_loader = (VkrBitmapFontLoaderContext){
      .job_system = job_system, .arena_pool = &rf->bitmap_font_arena_pool};
  rf->system_font_loader = (VkrSystemFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->system_font_arena_pool,
      .texture_system = &rf->texture_system,
  };
  rf->mtsdf_font_loader = (VkrMtsdfFontLoaderContext){
      .job_system = job_system,
      .arena_pool = &rf->mtsdf_font_arena_pool,
      .texture_system = &rf->texture_system,
  };

  vkr_resource_system_register_loader((void *)&rf->texture_system,
                                      vkr_texture_loader_create());
  vkr_resource_system_register_loader((void *)&rf->material_system,
                                      vkr_material_loader_create());
  vkr_resource_system_register_loader((void *)&rf->mesh_loader,
                                      vkr_mesh_loader_create(&rf->mesh_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->bitmap_font_loader,
      vkr_bitmap_font_loader_create(&rf->bitmap_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->system_font_loader,
      vkr_system_font_loader_create(&rf->system_font_loader));
  vkr_resource_system_register_loader(
      (void *)&rf->mtsdf_font_loader,
      vkr_mtsdf_font_loader_create(&rf->mtsdf_font_loader));
  vkr_resource_system_register_loader((void *)rf, vkr_scene_loader_create());

  VkrFontSystemConfig font_config = {
      .max_system_font_count = 16,
      .max_bitmap_font_count = 16,
      .max_mtsdf_font_count = 16,
  };
  if (!vkr_font_system_init(&rf->font_system, rf, &font_config, &error) ||
      !vkr_lighting_system_init(&rf->lighting_system) ||
      !vkr_world_resources_init(rf, &rf->world_resources)) {
    return false_v;
  }
  VkrShadowConfig shadow_config = VKR_SHADOW_CONFIG_DEFAULT;
  if (!vkr_shadow_system_init(&rf->shadow_system, rf, &shadow_config)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_UI) &&
      !vkr_ui_system_init(rf, &rf->ui_system)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_SKYBOX) &&
      !vkr_skybox_system_init(rf, &rf->skybox_system)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_EDITOR) &&
      !vkr_editor_viewport_init(rf, &rf->editor_viewport)) {
    return false_v;
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_GIZMO)) {
    VkrGizmoConfig gizmo_config = VKR_GIZMO_CONFIG_DEFAULT;
    if (!vkr_gizmo_system_init(&rf->gizmo_system, rf, &gizmo_config)) {
      return false_v;
    }
  }
  if (vkr_renderer_subsystem_plan_includes(&rf->subsystem_plan,
                                           VKR_RENDERER_SUBSYSTEM_PICKING) &&
      !vkr_picking_init(rf, &rf->picking, rf->last_window_width,
                        rf->last_window_height)) {
    return false_v;
  }
  renderer_frontend_narrow_plan_to_initialized(rf);
  return true_v;
}

bool32_t vkr_renderer_systems_initialize(
    VkrRendererFrontendHandle renderer, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers,
    const VkrSubsystemPlan *subsystem_plan) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!subsystem_plan) {
    log_error("Renderer subsystem plan is required");
    return false_v;
  }

  VkrSubsystemPlan resolved_plan = {0};
  VkrRendererError plan_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_renderer_subsystem_plan_build(
          subsystem_plan->profile, subsystem_plan->requested_mask,
          subsystem_plan->excluded_mask, &resolved_plan, &plan_error) ||
      resolved_plan.effective_mask != subsystem_plan->effective_mask) {
    log_error("Renderer subsystem plan is invalid or not dependency-closed");
    return false_v;
  }
  rf->subsystem_plan = resolved_plan;
  if (metrics_producers) {
    rf->hdr_decode_metrics = metrics_producers->hdr_decode;
    rf->ibl_conversion_metrics = metrics_producers->ibl_conversion;
    rf->ibl_convolution_metrics = metrics_producers->ibl_convolution;
  }
#if VKR_METRICS_ENABLED
  const float64_t systems_start = vkr_platform_get_absolute_time();
#endif

  VkrCameraSystemConfig camera_cfg = {.max_camera_count = 24};
  if (!vkr_camera_registry_init(&camera_cfg, &rf->camera_system)) {
    log_fatal("Failed to initialize camera system");
    return false_v;
  }
  const float32_t default_vertical_fov_degrees = 70.0f;
  const float32_t default_near_clip = 0.1f;
  const float32_t default_far_clip = 500.0f;
  VkrCameraHandle default_camera = VKR_CAMERA_HANDLE_INVALID;
  if (!vkr_camera_registry_create_perspective(
          &rf->camera_system, string8_lit("camera.default"), rf->window,
          default_vertical_fov_degrees, default_near_clip, default_far_clip,
          &default_camera)) {
    log_fatal("Failed to create default camera");
    return false_v;
  }
  vkr_camera_registry_set_active(&rf->camera_system, default_camera);
  rf->active_camera = default_camera;
  /* Creation seeds the aspect from the window, which an offscreen renderer does
     not have. Restate the lens against the actual target extent so both target
     kinds project identically. */
  VkrCamera *initial_camera =
      vkr_camera_registry_get_by_handle(&rf->camera_system, default_camera);
  if (!initial_camera ||
      !vkr_camera_set_perspective_lens(
          initial_camera, default_vertical_fov_degrees, default_near_clip,
          default_far_clip, rf->last_window_width, rf->last_window_height)) {
    log_fatal("Failed to configure default camera target extent");
    return false_v;
  }
  vkr_camera_system_update(initial_camera);

  if (!renderer_frontend_initialize_packet_systems(rf, job_system,
                                                   metrics_producers)) {
    log_fatal("Failed to initialize packet renderer systems");
    return false_v;
  }
#if VKR_METRICS_ENABLED
  rf->boot_metrics.systems_ns = vkr_metrics_elapsed_ns(systems_start);
#endif
  return true_v;
}

VkrSubsystemMask
vkr_renderer_get_subsystem_mask(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return ((RendererFrontend *)renderer)->subsystem_plan.effective_mask;
}

// =============================================================================
// Pixel Readback API (for picking and screenshots)
// =============================================================================

VkrRendererError
vkr_renderer_get_pixel_readback_result(VkrRendererFrontendHandle renderer,
                                       VkrPixelReadbackResult *out_result) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_result != NULL, "Output result is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (rf->impl.kind == VKR_RENDERER_IMPL_VULKAN) {
    return vkr_vulkan_renderer_get_pixel_readback_result(rf->vulkan_renderer,
                                                         out_result);
  }
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
}

VkrAllocator *
vkr_renderer_get_backend_allocator(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;
  return rf->impl.ops->get_allocator(rf->impl.state);
}

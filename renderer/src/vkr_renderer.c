#include "vkr_renderer.h"
#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/vkr_vfs.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/vkr_dmemory_allocator.h"
#include "metal/vkr_metal_memory.h"
#include "metal/vkr_metal_packet_renderer.h"
#include "platform/vkr_platform.h"
#include "vkr_asset_metrics.h"
#include "vkr_capture.h"
#include "vkr_dynamic_resolution.h"
#include "vkr_frame_input.h"
#include "vkr_ibl_math.h"
#include "vkr_prepared_frame.h"
#include "vkr_renderer_internal.h"
#include "vkr_renderer_metrics.h"
#include "vkr_rg_json.h"
#include "vkr_shader_catalog.h"
#include "vulkan/vkr_vulkan_renderer.h"

#include <math.h>

/* The render graph of a pipeline class (ADR-087) ships in the build tree
 * beside the renderer; a mounted bundle or an installed program carries it as
 * content instead (ADR-077). */
vkr_internal const char *
vkr_renderer_graph_path(VkrGraphicsPipelineClass graphics, char *storage,
                        uint64_t capacity) {
  const bool8_t tiled = graphics == VKR_GRAPHICS_PIPELINE_TILED;
  if (!vkr_vfs_pack_count() && vkr_content_root_is_repository()) {
    return tiled ? VKR_TILED_RENDER_GRAPH_PATH : VKR_RENDER_GRAPH_PATH;
  }
  snprintf(storage, capacity, "%sassets/render_graphs/%s.rendergraph.json",
           vkr_content_root(), tiled ? "tiled" : "main");
  return storage;
}

VkrGraphicsPipelineClass
vkr_graphics_pipeline_for_backend(VkrRendererBackendType backend) {
  return backend == VKR_RENDERER_BACKEND_TYPE_METAL
             ? VKR_GRAPHICS_PIPELINE_TILED
             : VKR_GRAPHICS_PIPELINE_DESKTOP;
}

bool8_t
vkr_graphics_pipeline_draws_render_mode(VkrGraphicsPipelineClass graphics,
                                        uint32_t mode) {
  if (mode >= VKR_RENDER_MODE_COUNT) {
    return false_v;
  }
  /* The artist views draw on the tiled pipeline only so far (ADR-099). */
  if (graphics != VKR_GRAPHICS_PIPELINE_TILED) {
    return mode < VKR_RENDER_MODE_BASE_COLOR;
  }
  return mode == VKR_RENDER_MODE_DEFAULT || mode == VKR_RENDER_MODE_UNLIT ||
         mode == VKR_RENDER_MODE_DETAIL_LIGHTING ||
         mode == VKR_RENDER_MODE_LIGHTING_ONLY ||
         mode == VKR_RENDER_MODE_WIREFRAME || mode == VKR_RENDER_MODE_NORMAL ||
         mode >= VKR_RENDER_MODE_BASE_COLOR;
}

vkr_internal bool8_t vkr_renderer_env_enabled(const char *name) {
  const char *value = name ? getenv(name) : NULL;
  return value && value[0] != '\0' && strcmp(value, "0") != 0 ? true_v
                                                              : false_v;
}

// =============================================================================
// Render thread
// =============================================================================

/* One frame at a time: the caller submits, the worker renders, and every other
   renderer entry point waits until the worker is idle again. */
typedef enum VkrRendererWorkerState {
  VKR_RENDERER_WORKER_IDLE = 0,
  VKR_RENDERER_WORKER_BUSY,
} VkrRendererWorkerState;

struct VkrRendererWorker {
  VkrThread thread;
  VkrThreadId thread_id;
  VkrMutex mutex;
  VkrCondVar cond;
  /* VkrRendererWorkerState; release on completion orders the frame's writes
     before a waiter's acquire. */
  VkrAtomicUint32 state;
  bool8_t stop;
  VkrRendererWork work;
};

vkr_internal void vkr_renderer_render_submitted(VkrRenderer *renderer,
                                                VkrRendererWork *work,
                                                VkrRendererFrameResult *result);

void vkr_renderer_join_render_thread(const VkrRenderer *renderer) {
  VkrRendererWorker *worker = renderer ? renderer->worker : NULL;
  if (!worker ||
      vkr_atomic_uint32_load(&worker->state, VKR_MEMORY_ORDER_ACQUIRE) ==
          VKR_RENDERER_WORKER_IDLE ||
      vkr_thread_current_id() == worker->thread_id) {
    return;
  }
  vkr_mutex_lock(worker->mutex);
  while (vkr_atomic_uint32_load(&worker->state, VKR_MEMORY_ORDER_ACQUIRE) !=
         VKR_RENDERER_WORKER_IDLE) {
    vkr_cond_wait(worker->cond, worker->mutex);
  }
  vkr_mutex_unlock(worker->mutex);
}

vkr_internal void *vkr_renderer_worker_main(void *arg) {
  VkrRenderer *renderer = arg;
  VkrRendererWorker *worker = renderer->worker;
  vkr_mutex_lock(worker->mutex);
  for (;;) {
    while (!worker->stop &&
           vkr_atomic_uint32_load(&worker->state, VKR_MEMORY_ORDER_ACQUIRE) !=
               VKR_RENDERER_WORKER_BUSY) {
      vkr_cond_wait(worker->cond, worker->mutex);
    }
    if (vkr_atomic_uint32_load(&worker->state, VKR_MEMORY_ORDER_ACQUIRE) !=
        VKR_RENDERER_WORKER_BUSY) {
      break;
    }
    vkr_mutex_unlock(worker->mutex);

    vkr_renderer_render_submitted(renderer, &worker->work,
                                  &renderer->frame_result);

    vkr_mutex_lock(worker->mutex);
    vkr_atomic_uint32_store(&worker->state, VKR_RENDERER_WORKER_IDLE,
                            VKR_MEMORY_ORDER_RELEASE);
    vkr_cond_broadcast(worker->cond);
  }
  vkr_mutex_unlock(worker->mutex);
  return NULL;
}

vkr_internal void vkr_renderer_worker_destroy(VkrRenderer *renderer) {
  VkrRendererWorker *worker = renderer->worker;
  if (!worker) {
    return;
  }
  VkrAllocator *allocator = &renderer->render_graph_allocator;
  if (worker->thread) {
    vkr_renderer_join_render_thread(renderer);
    vkr_mutex_lock(worker->mutex);
    worker->stop = true_v;
    vkr_cond_broadcast(worker->cond);
    vkr_mutex_unlock(worker->mutex);
    vkr_thread_join(worker->thread);
    vkr_thread_destroy(allocator, &worker->thread);
  }
  if (worker->cond) {
    vkr_cond_destroy(allocator, &worker->cond);
  }
  if (worker->mutex) {
    vkr_mutex_destroy(allocator, &worker->mutex);
  }
  vkr_allocator_free(allocator, worker, sizeof(*worker),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->worker = NULL;
}

vkr_internal bool8_t vkr_renderer_worker_create(VkrRenderer *renderer) {
  VkrAllocator *allocator = &renderer->render_graph_allocator;
  VkrRendererWorker *worker = vkr_allocator_alloc(
      allocator, sizeof(*worker), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!worker) {
    return false_v;
  }
  MemZero(worker, sizeof(*worker));
  renderer->worker = worker;
  if (!vkr_mutex_create(allocator, &worker->mutex) ||
      !vkr_cond_create(allocator, &worker->cond) ||
      /* Frame preparation and recording run with the main thread's 8 MiB
         stack (CMakeLists.txt reserves the same on Windows); a default
         secondary-thread stack overflows inside Metal image realization. */
      !vkr_thread_create_with_stack(allocator, &worker->thread,
                                    vkr_renderer_worker_main, renderer,
                                    MB(8))) {
    vkr_renderer_worker_destroy(renderer);
    return false_v;
  }
  worker->thread_id = vkr_thread_get_id(worker->thread);
  return true_v;
}

vkr_internal uint32_t vkr_renderer_scaled_extent(uint32_t extent,
                                                 float32_t render_scale) {
  const float64_t scaled = (float64_t)extent * (float64_t)render_scale;
  return ClampBot((uint32_t)floor(scaled + 0.5), 1u);
}

vkr_internal VkrMetricReason
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

vkr_internal void
vkr_renderer_impl_lower_metal_result(const VkrMetalPacketResult *source,
                                     VkrRendererImplSubmitResult *destination) {
  if (!source || !destination) {
    return;
  }
  *destination = (VkrRendererImplSubmitResult){
      .submit_value = source->submit_value,
      .source_frame_index = source->source_frame_index,
      .gpu_submission_ns = source->gpu_submission_ns,
      .source_render_scale = source->source_render_scale,
      .source_render_width = source->source_render_width,
      .source_render_height = source->source_render_height,
      .gpu_submission_unavailable_reason =
          source->gpu_submission_unavailable_reason,
      .gpu_submission_valid = source->gpu_submission_valid,
      .executed_pass_count = source->executed_pass_count,
      .indexed_draw_count = source->indexed_draw_count,
      .shadow_draw_count = source->shadow_draw_count,
      .opaque_draw_count = source->opaque_draw_count,
      .blend_draw_count = source->blend_draw_count,
      .gpu_visible_count = source->gpu_visible_count,
      .gpu_overflow_count = source->gpu_overflow_count,
      .gpu_resolve_invalid_count = source->gpu_resolve_invalid_count,
      .gpu_occlusion_culled_count = source->gpu_occlusion_culled_count,
      .custom_late_draw_count = source->custom_late_count,
      .hzb_history_valid = source->hzb_history_valid,
      .has_gpu_draw_diagnostics = source->has_gpu_draw_diagnostics,
      .exposure = source->exposure,
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
  MemCopy(destination->gpu_bucket_counts, source->gpu_bucket_counts,
          sizeof(destination->gpu_bucket_counts));
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
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_total_capacity_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_slot_capacity_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_max_requested_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(upload_ring_oversize_failures);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_acquires);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_reuses);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_busy_failures);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_total_capacity_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_slot_capacity_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_max_requested_bytes);
  VKR_LOWER_DEVICE_MEMORY_FIELD(readback_ring_oversize_failures);
#undef VKR_LOWER_DEVICE_MEMORY_FIELD
  memory->transfer_ring_demand_metrics_supported = true_v;
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

/* The local faces this submission draws, by the casters each draws. */
vkr_internal void
vkr_renderer_record_local_shadow_metrics(VkrRenderer *renderer,
                                         const VkrFrameInput *packet) {
  VkrShadowMetrics *shadow = &renderer->frame_metrics.shadow;
  shadow->local_faces_static = 0u;
  shadow->local_faces_dynamic = 0u;
  shadow->local_faces_copied = 0u;
  shadow->local_faces_full = 0u;
  shadow->local_copy_texels = 0u;
  shadow->local_dynamic_faces_wanted = 0u;
  shadow->local_dynamic_faces_dropped = 0u;
  shadow->local_dynamic_layers = 0u;
  shadow->baked_lamps_selected = 0u;
  shadow->baked_lamp_candidates = 0u;
  const VkrLocalShadowPassPayload *local = packet ? packet->local_shadow : NULL;
  if (!local)
    return;
  shadow->baked_lamps_selected = local->baked_lamp_count;
  shadow->baked_lamp_candidates = local->baked_lamp_candidates;
  const uint32_t dynamic_first = vkr_local_shadow_dynamic_render_first(local);
  for (uint32_t slot = 0u; slot < dynamic_first; ++slot) {
    if ((local->retained_opaque_mask & (UINT64_C(1) << slot)) != 0u)
      continue;
    if ((local->static_render_mask & (UINT64_C(1) << slot)) != 0u)
      shadow->local_faces_static++;
    else
      shadow->local_faces_full++;
  }
  for (uint32_t slot = dynamic_first; slot < local->render_count; ++slot) {
    const float32_t side =
        local->views[local->render_views[slot]].atlas_rect.z *
        (float32_t)VKR_LOCAL_SHADOW_ATLAS_SIZE;
    shadow->local_copy_texels += (uint64_t)side * (uint64_t)side;
  }
  shadow->local_faces_dynamic = local->dynamic_render_count;
  shadow->local_faces_copied = local->dynamic_render_count;
  shadow->local_dynamic_faces_wanted = local->dynamic_faces_wanted;
  shadow->local_dynamic_faces_dropped =
      local->dynamic_faces_wanted > local->dynamic_render_count
          ? local->dynamic_faces_wanted - local->dynamic_render_count
          : 0u;
  shadow->local_dynamic_layers = local->dynamic_layer_count;
}

vkr_internal void
vkr_renderer_record_gpu_candidate_metrics(VkrRenderer *renderer,
                                          const VkrFrameInput *packet) {
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

vkr_internal VkrRendererError vkr_renderer_validation_fail(
    VkrValidationError *out_error, VkrRendererError code,
    const char *field_path, const char *message);
vkr_internal bool32_t vkr_renderer_backend_initialize(
    VkrRenderer *renderer, const VkrNativeSurface *surface, uint32_t width,
    uint32_t height, VkrDeviceRequirements *device_requirements,
    const VkrRendererBackendConfig *backend_config,
    VkrRendererError *out_error);
vkr_internal void vkr_renderer_backend_destroy(VkrRenderer *renderer);
vkr_internal void vkr_renderer_backend_get_device_information(
    VkrRenderer *renderer, VkrDeviceInformation *device_information,
    Arena *temp_arena);
vkr_internal VkrRendererError
vkr_renderer_backend_wait_idle(VkrRenderer *renderer);
vkr_internal uint64_t vkr_renderer_backend_submit_serial(VkrRenderer *renderer);
vkr_internal uint64_t
vkr_renderer_backend_completed_submit_serial(VkrRenderer *renderer);
vkr_internal bool8_t vkr_renderer_backend_upload_wait_stats(
    VkrRenderer *renderer, VkrRendererUploadWaitStats *out_stats);
vkr_internal bool8_t vkr_renderer_backend_command_slot_waits(
    VkrRenderer *renderer, uint64_t *out_wait_count);
vkr_internal bool8_t vkr_renderer_backend_device_memory_stats(
    VkrRenderer *renderer, VkrDeviceMemoryStats *out_stats);
#if !defined(PLATFORM_APPLE)
// Metal lowers memory metrics with its submit result; only Vulkan polls them.
vkr_internal bool8_t vkr_renderer_backend_memory_metrics(
    VkrRenderer *renderer, VkrRendererImplMemoryMetrics *out_metrics);
#endif
vkr_internal void vkr_renderer_backend_resize(VkrRenderer *renderer,
                                              uint32_t width, uint32_t height);
vkr_internal VkrRendererError vkr_renderer_backend_present_target_recreate(
    VkrRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t image_count);
vkr_internal bool8_t vkr_renderer_backend_poll_submit_result(
    VkrRenderer *renderer, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result);

/* A completed automatic exposure seeds the next frames' pre-exposure. */
vkr_internal void vkr_renderer_observe_exposure(VkrRenderer *renderer) {
  const VkrExposureDebugSample *sample = &renderer->timing_result.exposure;
  if (renderer->timing_completed_ready && sample->valid &&
      isfinite(sample->state.exposure_multiplier) &&
      sample->state.exposure_multiplier > 0.0f)
    renderer->observed_exposure = sample->state.exposure_multiplier;
}

vkr_internal VkrAllocator *
vkr_renderer_backend_allocator(VkrRenderer *renderer);
vkr_internal VkrRendererError vkr_renderer_backend_prepare_frame(
    VkrRenderer *renderer, const VkrFrameConfig *config, VkrFrame *out_setup);
vkr_internal VkrRendererError vkr_renderer_backend_render_frame(
    VkrRenderer *renderer, const VkrFrameInput *packet,
    VkrRendererFrameMetrics *out_metrics,
    VkrValidationError *out_validation_error);
vkr_internal VkrRendererError
vkr_renderer_backend_cancel_frame(VkrRenderer *renderer);

vkr_internal bool32_t vkr_renderer_backend_initialize(
    VkrRenderer *renderer, const VkrNativeSurface *surface, uint32_t width,
    uint32_t height, VkrDeviceRequirements *device_requirements,
    const VkrRendererBackendConfig *backend_config,
    VkrRendererError *out_error) {
#if defined(PLATFORM_APPLE)
  (void)device_requirements;
  /* Two completion-protected slots; backing heaps grow only at resource
   * creation. */
  const uint32_t frame_slot_count = 2u;
  /* Zero lets the Metal renderer size the cap from its device. */
  uint64_t managed_budget_mb = 0u;
  const char *budget_env = getenv("VKR_METAL_MEMORY_BUDGET_MB");
  if (budget_env &&
      (!string_to_u64(budget_env, &managed_budget_mb) ||
       managed_budget_mb == 0u || managed_budget_mb > UINT64_MAX / MB(1))) {
    log_error("VKR_METAL_MEMORY_BUDGET_MB must be a positive integer in MiB");
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  const uint32_t capture_capacity = backend_config->capture_ring_capacity > 0
                                        ? backend_config->capture_ring_capacity
                                        : frame_slot_count;
  const uint64_t capture_bytes = backend_config->capture_max_batch_bytes > 0
                                     ? backend_config->capture_max_batch_bytes
                                     : MB(32);
  /* Shaders come from the catalog `vkr_bakery shaders` publishes. A metallib
   * is used only when its manifest proves it was built from the current MSL;
   * otherwise the renderer compiles the MSL at startup. */
  char slang_msl[4096];
  char slang_metallib[4096];
  char fragment_msl[4096];
  char fragment_metallib[4096];
  char archive_default[4096];
  char graph_path[4096];
  /* The project's Custom material graphs, from `vkr_bakery materials`
     (ADR-096); absent until a project has one. */
  char custom_library[4096];
  const bool8_t slang_precompiled = vkr_shader_catalog_metallib(
      "library.slang", slang_metallib, slang_msl, sizeof(slang_msl));
  const bool8_t fragment_precompiled = vkr_shader_catalog_metallib(
      "library", fragment_metallib, fragment_msl, sizeof(fragment_msl));
  if (!slang_msl[0] || !fragment_msl[0] ||
      !vkr_shader_catalog_path("metal", "vkr_application.mtlarchive",
                               archive_default, sizeof(archive_default)) ||
      !vkr_shader_catalog_path("metal", "project_materials.metallib",
                               custom_library, sizeof(custom_library))) {
    log_error("Shader catalog paths do not fit");
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }
  const bool8_t use_metallib =
      slang_precompiled && fragment_precompiled &&
      !vkr_renderer_env_enabled("VKR_METAL_COMPILE_SOURCE");
  const char *pipeline_archive_path = getenv("VKR_PIPELINE_CACHE_PATH");
  if (!pipeline_archive_path || pipeline_archive_path[0] == '\0')
    pipeline_archive_path = archive_default;
  VkrMetalPacketRendererConfig metal_config = {
      .allocator = &renderer->render_graph_allocator,
      .graph_path = vkr_renderer_graph_path(renderer->graphics_pipeline,
                                            graph_path, sizeof(graph_path)),
      .slang_msl_path = slang_msl,
      .fragment_msl_path = fragment_msl,
      .slang_metallib_path = use_metallib ? slang_metallib : NULL,
      .fragment_metallib_path = use_metallib ? fragment_metallib : NULL,
      .pipeline_archive_path = pipeline_archive_path,
      .custom_library_path = custom_library,
      .target_kind =
          renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? VKR_METAL_PACKET_TARGET_OFFSCREEN
              : VKR_METAL_PACKET_TARGET_WINDOW,
      .target_width = width,
      .target_height = height,
      .dynamic_resolution = renderer->dynamic_resolution_config,
      .metal_layer = surface ? surface->metal_layer : NULL,
      .display_output_mode = backend_config->display_output_mode,
      .display_output_context = surface ? surface->context : NULL,
      .display_output_snapshot =
          surface ? surface->display_output_snapshot : NULL,
      .requested_present_mode = backend_config->requested_present_mode,
      .managed_budget_size = managed_budget_mb * MB(1),
      .heap_chunk_size = MB(64),
      .upload_ring_size = MB(64),
      .upload_ring_max_size = MB(768),
      .frame_slot_count = frame_slot_count,
      .capture_ring_capacity = capture_capacity,
      .capture_max_batch_bytes = capture_bytes,
      .synchronous_validation_readback =
          vkr_renderer_env_enabled("VKR_METAL_SYNCHRONOUS_VALIDATION"),
      .srgb_output = true_v,
      .tonemap_enabled = !vkr_renderer_env_enabled("VKR_TONEMAP_DISABLED"),
      .convert_vulkan_clip_y = true_v,
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
      .frustum_enabled = !vkr_renderer_env_enabled("VKR_FRUSTUM_DISABLED"),
      .max_images = 128,
      .max_passes = VKR_RENDERER_IMPL_MAX_GRAPH_PASSES,
      .max_material_rows = 8192,
      .max_meshes = VKR_GEOMETRY_CAPACITY,
      .max_textures = 16384,
      .max_draws = 262144,
      .max_instances = 262144,
  };
  if (!vkr_metal_packet_renderer_create(&metal_config,
                                        &renderer->metal_renderer)) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    return false_v;
  }
  renderer->impl.caps.present_color_format =
      vkr_metal_packet_renderer_present_color_format(renderer->metal_renderer);
  vkr_metal_packet_renderer_get_asset_publisher(renderer->metal_renderer,
                                                &renderer->native_publisher);
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Metal 4 packet renderer");
  return true_v;
#else
  (void)device_requirements;
  VkrSsrQuality ssr_quality = VKR_SSR_QUALITY_HIGH;
  const char *ssr_quality_env = getenv("VKR_SSR_QUALITY");
  if (ssr_quality_env && ssr_quality_env[0] != '\0') {
    if (strcmp(ssr_quality_env, "balanced") == 0) {
      ssr_quality = VKR_SSR_QUALITY_BALANCED;
    } else if (strcmp(ssr_quality_env, "high") != 0) {
      log_error("VKR_SSR_QUALITY must be high or balanced");
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      }
      return false_v;
    }
  }
  const VkrSsrConfig ssr_config = vkr_ssr_config_for_quality(ssr_quality);
  char graph_path[4096];
  VkrVulkanRendererConfig config = {
      .ssr = ssr_config,
      .ssgi = vkr_ssgi_config_default(),
      .allocator = &renderer->render_graph_allocator,
      .graph_path = vkr_renderer_graph_path(renderer->graphics_pipeline,
                                            graph_path, sizeof(graph_path)),
      .surface = surface ? *surface : (VkrNativeSurface){0},
      .display_output_mode = backend_config->display_output_mode,
      .target_kind = renderer->present_target.kind,
      .requested_present_mode = backend_config->requested_present_mode,
      .width = width,
      .height = height,
      .image_count = renderer->present_target.image_count,
      .sampled_image_capacity =
          16384u + VKR_VULKAN_PERMANENT_SAMPLED_IMAGE_ROWS,
      .storage_image_capacity = 1024u,
      .sampler_capacity = 2048u,
      // Publication records are indexed directly by logical handle id, so these
      // two must cover the geometry and texture systems' whole ID spaces: the
      // geometry system's VKR_GEOMETRY_CAPACITY and max_texture_count. Neither
      // is a descriptor-heap bound.
      .geometry_capacity = VKR_GEOMETRY_CAPACITY,
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
      .max_graph_passes = VKR_RENDERER_IMPL_MAX_GRAPH_PASSES,
      .tonemap_enabled = !vkr_renderer_env_enabled("VKR_TONEMAP_DISABLED"),
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
      .frustum_enabled = !vkr_renderer_env_enabled("VKR_FRUSTUM_DISABLED"),
      .fsr31_enabled = renderer->upscale_mode == VKR_UPSCALE_MODE_FSR31,
      .transmission_compact_enabled =
          !vkr_renderer_env_enabled("VKR_TRANSMISSION_COMPACT_DISABLED"),
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
                                          &renderer->native_publisher);
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Vulkan 1.4 packet renderer");
  return true_v;
#endif
}

VkrRenderer *vkr_renderer_create(VkrAllocator *allocator,
                                 VkrRendererBackendType type,
                                 const VkrNativeSurface *surface,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 VkrRendererError *out_error) {
  if (!allocator || !device_requirements || !out_error) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return NULL;
  }
  VkrRenderer *renderer = vkr_allocator_alloc(
      allocator, sizeof(*renderer), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return NULL;
  }
  MemZero(renderer, sizeof(*renderer));
  if (!vkr_renderer_initialize(renderer, type, surface, device_requirements,
                               backend_config, out_error)) {
    vkr_allocator_free(allocator, renderer, sizeof(*renderer),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return NULL;
  }
  renderer->instance_allocator = allocator;
  return renderer;
}

void vkr_renderer_release(VkrRenderer *renderer) {
  if (!renderer)
    return;
  VkrAllocator *allocator = renderer->instance_allocator;
  vkr_renderer_destroy(renderer);
  vkr_allocator_free(allocator, renderer, sizeof(*renderer),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

const VkrAssetPublisher *
vkr_renderer_get_asset_publisher(VkrRenderer *renderer) {
  return renderer ? &renderer->asset_publisher : NULL;
}

bool32_t vkr_renderer_initialize(VkrRenderer *renderer,
                                 VkrRendererBackendType backend_type,
                                 const VkrNativeSurface *surface,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  VkrPresentTargetConfig requested_target = backend_config
                                                ? backend_config->present_target
                                                : (VkrPresentTargetConfig){0};
  VkrDisplayOutputMode requested_display_output =
      backend_config ? backend_config->display_output_mode
                     : VKR_DISPLAY_OUTPUT_SDR;
  const char *display_output_env = getenv("VKR_DISPLAY_OUTPUT");
  if (display_output_env && display_output_env[0] != '\0') {
    if (strcmp(display_output_env, "sdr") == 0)
      requested_display_output = VKR_DISPLAY_OUTPUT_SDR;
    else if (strcmp(display_output_env, "auto_extended_linear") == 0)
      requested_display_output = VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR;
    else {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      log_error("VKR_DISPLAY_OUTPUT must be sdr or auto_extended_linear");
      return false_v;
    }
  }
  if (requested_display_output != VKR_DISPLAY_OUTPUT_SDR &&
      requested_display_output != VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  float32_t requested_render_scale =
      backend_config && backend_config->render_scale != 0.0f
          ? backend_config->render_scale
          : 1.0f;
  const VkrUpscaleMode requested_upscale_mode =
      backend_config ? backend_config->upscale_mode : VKR_UPSCALE_MODE_SPATIAL;
  VkrDynamicResolutionConfig requested_dynamic_resolution =
      backend_config ? backend_config->dynamic_resolution
                     : (VkrDynamicResolutionConfig){0};
  if (!isfinite(requested_render_scale) || requested_render_scale <= 0.0f ||
      requested_render_scale > 1.0f) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Renderer render scale must be finite and in (0, 1]");
    return false_v;
  }
  if (backend_type != VKR_RENDERER_BACKEND_TYPE_METAL &&
      requested_upscale_mode != VKR_UPSCALE_MODE_FSR31 &&
      requested_render_scale != 1.0f) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("Internal render scale is currently supported only by Metal");
    return false_v;
  }
  if (requested_upscale_mode < VKR_UPSCALE_MODE_SPATIAL ||
      requested_upscale_mode >= VKR_UPSCALE_MODE_COUNT) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Renderer upscale mode is invalid");
    return false_v;
  }
  if (requested_upscale_mode == VKR_UPSCALE_MODE_FSR31 &&
      backend_type != VKR_RENDERER_BACKEND_TYPE_VULKAN) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("FSR 3.1 upscaling requires the Vulkan backend");
    return false_v;
  }
  if (requested_upscale_mode == VKR_UPSCALE_MODE_FSR31 &&
      requested_render_scale < (1.0f / 3.0f)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("FSR 3.1 render scale must be in [1/3, 1]");
    return false_v;
  }
  /* The class follows the backend (ADR-087). The tiled pipeline resolves
     MSAA in tile memory and upscales spatially; its adaptive quality steps
     the resolution of that upscale. */
  const VkrGraphicsPipelineClass requested_graphics =
      vkr_graphics_pipeline_for_backend(backend_type);
  if (requested_graphics == VKR_GRAPHICS_PIPELINE_TILED &&
      requested_upscale_mode != VKR_UPSCALE_MODE_SPATIAL) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("Metal's tiled graphics pipeline upscales spatially, without "
              "temporal upscaling");
    return false_v;
  }
  if (requested_dynamic_resolution.enabled &&
      requested_graphics != VKR_GRAPHICS_PIPELINE_TILED) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("Dynamic resolution requires Metal's tiled graphics pipeline");
    return false_v;
  }
  /* The live range: the FSR 3.1 context accepts any render extent up to its
     output, and the spatial Metal path resamples any extent. Vulkan spatial
     fixes unit scale. */
  const VkrDynamicResolutionConfig dynamic_resolution_request =
      requested_dynamic_resolution;
  float32_t live_min = requested_render_scale;
  float32_t live_max = requested_render_scale;
  if (requested_upscale_mode == VKR_UPSCALE_MODE_FSR31 ||
      backend_type == VKR_RENDERER_BACKEND_TYPE_METAL) {
    live_min = Min(live_min, 1.0f / 3.0f);
    live_max = 1.0f;
  }
  if (!vkr_dynamic_resolution_config_normalize(
          &requested_dynamic_resolution, requested_render_scale,
          &requested_dynamic_resolution, &requested_render_scale)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Dynamic-resolution scale bounds or frame budget are invalid");
    return false_v;
  }
  if (requested_target.kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    if (requested_target.width == 0 || requested_target.height == 0 ||
        requested_target.image_count == 0) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      log_error("Offscreen target requires non-zero width, height, and image "
                "count");
      return false_v;
    }
  } else if (!surface || !surface->pixel_size) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Windowed target requires a native surface and pixel-size query");
    return false_v;
  }

  VkrRendererImpl selected_impl = {0};
  if (!vkr_renderer_impl_select(backend_type, requested_target.kind,
                                &selected_impl) ||
      !selected_impl.initialization_supported) {
    *out_error = VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    log_error("Requested renderer implementation is not available");
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  renderer->render_graph_dmemory = (VkrDMemory){0};
  renderer->render_graph_allocator = (VkrAllocator){0};
  renderer->metal_renderer = NULL;
  renderer->vulkan_renderer = NULL;
  renderer->worker = NULL;
  renderer->frame_result_ready = false_v;
  renderer->impl = selected_impl;

  // Initialize struct in-place
  renderer->backend_type = backend_type;
  renderer->surface = surface ? *surface : (VkrNativeSurface){0};
  renderer->present_target = requested_target;
  renderer->render_scale = requested_render_scale;
  renderer->render_scale_min = live_min;
  renderer->render_scale_max = live_max;
  renderer->upscale_mode = requested_upscale_mode;
  renderer->graphics_pipeline = requested_graphics;
  renderer->dynamic_resolution_request = dynamic_resolution_request;
  renderer->dynamic_resolution_config = requested_dynamic_resolution;
  vkr_dynamic_resolution_init(&renderer->dynamic_resolution_state,
                              &requested_dynamic_resolution,
                              requested_render_scale);
  renderer->frame_active = false;
  renderer->asset_publisher = (VkrAssetPublisher){0};
  renderer->timing_result = (VkrRendererImplSubmitResult){0};
  renderer->timing_last_completed_submit_value = 0;
  renderer->timing_completed_ready = false_v;
  renderer->supports_multi_draw_indirect = false_v;
  renderer->supports_draw_indirect_first_instance = false_v;

  renderer->editor_image_width = 0u;
  renderer->editor_image_height = 0u;
  renderer->temporal_state = (VkrTemporalState){0};
  renderer->temporal_reset_reasons = 0u;
  renderer->temporal_enabled =
      requested_upscale_mode == VKR_UPSCALE_MODE_FSR31 ||
      !vkr_renderer_env_enabled("VKR_TAA_DISABLED");
  renderer->exposure_state = (VkrExposureState){0};
  renderer->exposure_reset_reasons = 0u;
  /* Diagnostic: a forced pre-exposure exponent. Pre-exposure is exact, so
     any forced scale must leave displayed output unchanged. */
  {
    const char *stops = getenv("VKR_PRE_EXPOSURE_FORCE_STOPS");
    renderer->pre_exposure_forced = stops != NULL && stops[0] != '\0';
    renderer->pre_exposure_forced_stops =
        renderer->pre_exposure_forced
            ? (int32_t)Clamp(strtol(stops, NULL, 10),
                             -VKR_PRE_EXPOSURE_MAX_STOPS,
                             VKR_PRE_EXPOSURE_MAX_STOPS)
            : 0;
  }
  /* Production enables bloom, and the environment override exists for the same
     reason `VKR_TAA_DISABLED` does: a matched capture of the same frame with
     and without it must not require a rebuild. */
  renderer->bloom_forced_disabled =
      vkr_renderer_env_enabled("VKR_BLOOM_DISABLED");
  renderer->ssr_forced_disabled = vkr_renderer_env_enabled("VKR_SSR_DISABLED");
  renderer->ssgi_forced_disabled =
      vkr_renderer_env_enabled("VKR_SSGI_DISABLED");
  renderer->fxaa_enabled = !vkr_renderer_env_enabled("VKR_FXAA_DISABLED");
  /* Filtering display-linear pixels is the default (ADR-043); only the explicit
     "0" spelling keeps the analytic per-tap transform as a reference. */
  const char *post_transform_cache = getenv("VKR_POST_TRANSFORM_CACHE");
  renderer->post_transform_cache_enabled =
      !post_transform_cache || strcmp(post_transform_cache, "0") != 0;
  renderer->gtao_forced_disabled =
      vkr_renderer_env_enabled("VKR_GTAO_DISABLED");
  renderer->frame_metrics = (VkrRendererFrameMetrics){0};
  renderer->boot_metrics = (VkrRendererBootMetrics){0};
  renderer->frame_number = 0;
  renderer->target_generation = 1u;

  /* The Vulkan implementation owns its persistent graph realization,
   * descriptor tables, and bounded pool metadata through this allocator in
   * addition to the frontend graph. A resize can retain the old graph while
   * texture publication admits more 64 MiB image blocks; each block adds a
   * fixed GPU-memory-core record. Reserve that cold-boundary overlap without
   * increasing the initial physical commit. */
  if (!vkr_dmemory_create(MB(2), MB(96), &renderer->render_graph_dmemory)) {
    log_error("Failed to create render graph allocator!");
    goto initialize_failure;
  }
  renderer->render_graph_allocator =
      (VkrAllocator){.ctx = &renderer->render_graph_dmemory};
  vkr_dmemory_allocator_create(&renderer->render_graph_allocator);

  VkrSurfaceSize initial = requested_target.kind ==
                                       VKR_PRESENT_TARGET_OFFSCREEN
                                   ? (VkrSurfaceSize){
                                         .width = requested_target.width,
                                         .height = requested_target.height,
                                     }
                                   : surface->pixel_size(surface->context);
  renderer->last_window_width = initial.width;
  renderer->last_window_height = initial.height;
  renderer->scene_output_width = initial.width;
  renderer->scene_output_height = initial.height;
  renderer->scene_output_extent_overridden = false_v;
  renderer->render_width = vkr_renderer_scaled_extent(
      renderer->scene_output_width, renderer->render_scale);
  renderer->render_height = vkr_renderer_scaled_extent(
      renderer->scene_output_height, renderer->render_scale);
  uint32_t width = initial.width;
  uint32_t height = initial.height;

  VkrRendererBackendConfig resolved_backend_config = {
      .application_name = "vulkan_renderer",
      .boot_metrics = &renderer->boot_metrics,
      .present_target = requested_target,
      .render_scale = requested_render_scale,
      .upscale_mode = requested_upscale_mode,
      .dynamic_resolution = requested_dynamic_resolution,
  };
  if (backend_config) {
    resolved_backend_config = *backend_config;
    resolved_backend_config.boot_metrics = &renderer->boot_metrics;
    resolved_backend_config.render_scale = requested_render_scale;
    resolved_backend_config.upscale_mode = requested_upscale_mode;
    resolved_backend_config.dynamic_resolution = requested_dynamic_resolution;
  }
  resolved_backend_config.display_output_mode = requested_display_output;
  const VkrRendererBackendConfig *backend_cfg = &resolved_backend_config;
  if (!vkr_renderer_backend_initialize(renderer, surface, width, height,
                                       device_requirements, backend_cfg,
                                       out_error)) {
    goto initialize_failure;
  }

  bool8_t render_thread = !backend_config || !backend_config->inline_rendering;
  const char *render_thread_env = getenv("VKR_RENDER_THREAD");
  if (render_thread_env && render_thread_env[0] != '\0') {
    render_thread = strcmp(render_thread_env, "0") != 0;
  }
  if (!vkr_publication_queue_create(&renderer->publications)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    log_error("Failed to create the asset publication queue");
    goto initialize_failure;
  }
  vkr_publication_queue_publisher(renderer, &renderer->asset_publisher);
  if (render_thread && !vkr_renderer_worker_create(renderer)) {
    *out_error = VKR_RENDERER_ERROR_INITIALIZATION_FAILED;
    log_error("Failed to start the render thread");
    goto initialize_failure;
  }
  log_info("Rendering %s", renderer->worker ? "on a render thread" : "inline");
  return true_v;

initialize_failure:
  vkr_renderer_worker_destroy(renderer);
  vkr_publication_queue_destroy(&renderer->publications);
  if (renderer->metal_renderer || renderer->vulkan_renderer)
    vkr_renderer_backend_destroy(renderer);
  if (renderer->render_graph_allocator.ctx)
    vkr_dmemory_allocator_destroy(&renderer->render_graph_allocator);
  renderer->render_graph_allocator = (VkrAllocator){0};
  return false_v;
}

void vkr_renderer_destroy(VkrRenderer *renderer) {
  vkr_renderer_wait_idle(renderer);
  vkr_renderer_worker_destroy(renderer);
  vkr_publication_queue_destroy(&renderer->publications);
  vkr_renderer_backend_destroy(renderer);
  if (renderer->render_graph_allocator.ctx)
    vkr_dmemory_allocator_destroy(&renderer->render_graph_allocator);
}

vkr_internal void vkr_renderer_backend_destroy(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  vkr_metal_packet_renderer_destroy(renderer->metal_renderer);
  renderer->metal_renderer = NULL;
#else
  vkr_vulkan_renderer_destroy(renderer->vulkan_renderer);
  renderer->vulkan_renderer = NULL;
#endif
}

typedef struct VkrFramePreparation {
  VkrPreparedFrame frame;
  VkrTemporalFrameInput temporal_input;
  VkrExposureFrameInput exposure_input;
} VkrFramePreparation;

vkr_internal void
vkr_renderer_prepare_frame_data(VkrRenderer *rf, const VkrFrameInput *packet,
                                VkrFramePreparation *prepared) {
  const bool8_t orthographic = packet->globals.projection.m32 == 0.0f;
  const bool8_t tiled = rf->graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED;
  prepared->frame.input = *packet;
  prepared->frame.tiled_pipeline = tiled;
  prepared->frame.scene_rendering =
      !packet->editor || !packet->editor->scene_rendering_stopped;
  /* Portable TAA and FSR both reconstruct edges temporally, so FXAA filters
     only frames without temporal reconstruction. The tiled pipeline's
     multisampling resolves its edges instead (ADR-087). */
  const bool8_t temporal_frame =
      rf->temporal_enabled && !tiled &&
      packet->globals.render_mode != VKR_RENDER_MODE_INDIRECT_DIFFUSE &&
      packet->globals.render_mode != VKR_RENDER_MODE_WIREFRAME && !orthographic;
  prepared->frame.fxaa_enabled = rf->fxaa_enabled && !temporal_frame && !tiled;
  /* The display-linear target only pays off when the final pass filters. */
  const bool8_t output_filtered =
      prepared->frame.fxaa_enabled || packet->globals.image_sharpness > 0.0f;
  prepared->frame.post_transform_cache_enabled =
      rf->post_transform_cache_enabled && output_filtered &&
      prepared->frame.scene_rendering &&
      packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  prepared->frame.editor_image_available = false_v;
  prepared->frame.editor_image_width = 1u;
  prepared->frame.editor_image_height = 1u;
  prepared->frame.editor_image_rect_px = (Vec4){0};
  prepared->frame.scene_output_width = rf->scene_output_width;
  prepared->frame.scene_output_height = rf->scene_output_height;
  prepared->frame.render_scale = rf->render_scale;
  prepared->frame.fsr31_enabled =
      rf->upscale_mode == VKR_UPSCALE_MODE_FSR31 && !orthographic &&
      packet->globals.render_mode != VKR_RENDER_MODE_WIREFRAME;
  if (packet->editor) {
    const Vec4 rect = packet->editor->image_rect_px;
    const bool8_t rendering = prepared->frame.scene_rendering;
    if (!rendering) {
#if defined(PLATFORM_APPLE)
      vkr_metal_packet_renderer_retained_editor_extent(
          rf->metal_renderer, &rf->editor_image_width,
          &rf->editor_image_height);
#else
      vkr_vulkan_renderer_retained_editor_extent(rf->vulkan_renderer,
                                                 &rf->editor_image_width,
                                                 &rf->editor_image_height);
#endif
    }
    const uint32_t width =
        rendering ? (uint32_t)rect.z : rf->editor_image_width;
    const uint32_t height =
        rendering ? (uint32_t)rect.w : rf->editor_image_height;
    prepared->frame.editor_image_available = width > 0u && height > 0u;
    prepared->frame.editor_image_width = Max(width, 1u);
    prepared->frame.editor_image_height = Max(height, 1u);
    Vec4 output = rect;
    if (!rendering && prepared->frame.editor_image_available) {
      const float32_t scale =
          Min(rect.z / (float32_t)width, rect.w / (float32_t)height);
      output.z = Max(1.0f, floorf((float32_t)width * scale));
      output.w = Max(1.0f, floorf((float32_t)height * scale));
      output.x += floorf((rect.z - output.z) * 0.5f);
      output.y += floorf((rect.w - output.w) * 0.5f);
    }
    prepared->frame.editor_image_rect_px = output;
  }
  if (!prepared->frame.scene_rendering) {
    if (!packet->animation_preview) {
      prepared->frame.input.world = NULL;
    }
    prepared->frame.input.shadow = NULL;
    prepared->frame.input.local_shadow = NULL;
    prepared->frame.input.lighting = NULL;
    prepared->frame.input.sky = NULL;
    prepared->frame.input.picking = NULL;
  }
  const bool8_t scaled =
      rf->scene_output_extent_overridden || rf->render_scale != 1.0f;
  const uint32_t temporal_width = scaled ? rf->render_width
                                  : packet->frame.viewport_width
                                      ? packet->frame.viewport_width
                                      : packet->frame.window_width;
  const uint32_t temporal_height = scaled ? rf->render_height
                                   : packet->frame.viewport_height
                                       ? packet->frame.viewport_height
                                       : packet->frame.window_height;
  if (scaled) {
    prepared->frame.input.frame.viewport_width = temporal_width;
    prepared->frame.input.frame.viewport_height = temporal_height;
  }
  /* The indirect-diffuse capture channel must contain the environment diffuse
     term and nothing else, so its two frame-coupled post stages are resolved
     off here at the cold boundary rather than branched on per pixel. */
  const bool8_t editor_inspection =
      packet->globals.render_mode >= VKR_RENDER_MODE_DETAIL_LIGHTING &&
      packet->globals.render_mode <= VKR_RENDER_MODE_WIREFRAME;
  const bool8_t wireframe =
      packet->globals.render_mode == VKR_RENDER_MODE_WIREFRAME;
  const bool8_t indirect_diffuse_only =
      packet->globals.render_mode == VKR_RENDER_MODE_INDIRECT_DIFFUSE;
  rf->ibl_probes_packed =
      packet->lighting ? packet->lighting->ibl_probe_count : 0u;
  prepared->frame.ssgi_enabled =
      packet->globals.ssgi_enabled && !rf->ssgi_forced_disabled &&
      !orthographic && packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  prepared->frame.fog = vkr_fog_prepare(&packet->globals.fog);
  /* The tiled graph has no froxel fog; its forward shading applies the
     analytic fog (ADR-087). */
  prepared->frame.froxel_fog =
      prepared->frame.scene_rendering && !orthographic && !tiled
          ? vkr_froxel_fog_prepare(&prepared->frame.input, temporal_width,
                                   temporal_height)
          : (VkrFroxelFogGpuParams){0};
  vkr_froxel_fog_static_generation(&prepared->frame.input,
                                   &rf->froxel_static_seen,
                                   &rf->froxel_static_followed);
  prepared->frame.froxel_fog_signature = vkr_froxel_fog_content_signature(
      &prepared->frame.input, &prepared->frame.froxel_fog,
      rf->froxel_static_followed);
  if (prepared->frame.froxel_fog.grid_dimensions_cell_pixels[0])
    prepared->frame.fog = (VkrFogGpuParams){0};
  if (packet->globals.render_mode != VKR_RENDER_MODE_DEFAULT || orthographic)
    prepared->frame.fog = (VkrFogGpuParams){0};
  /* The camera-dependent sky exists whenever an atmosphere is visible.
     Aerial perspective and clouds follow the analytic fog's render-mode rule.
     Without a published atmosphere, fog keeps its constant in-scatter
     colour. */
  prepared->frame.sky = (VkrSkyGpuParams){0};
  if (prepared->frame.input.sky &&
      prepared->frame.input.sky->atmosphere.enabled) {
    const VkrSkyPassPayload *sky = prepared->frame.input.sky;
    prepared->frame.sky = vkr_atmosphere_prepare_sky(
        &sky->atmosphere, &sky->clouds, sky->cloud_wind_offset_m,
        packet->globals.view_position,
        mat4_mul(packet->globals.projection, packet->globals.view),
        packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT &&
            !orthographic);
  } else {
    prepared->frame.fog.sky_lighting.x = 0.0f;
  }
  const bool8_t fog_changed =
      MemCompare(&rf->submitted_fog, &prepared->frame.fog,
                 sizeof(prepared->frame.fog)) != 0 ||
      MemCompare(&rf->submitted_froxel_fog.color_density,
                 &prepared->frame.froxel_fog.color_density,
                 2u * sizeof(Vec4)) != 0 ||
      MemCompare(rf->submitted_froxel_fog.boxes,
                 prepared->frame.froxel_fog.boxes,
                 sizeof(prepared->frame.froxel_fog.boxes)) != 0 ||
      MemCompare(&rf->submitted_froxel_fog.lighting,
                 &prepared->frame.froxel_fog.lighting, sizeof(Vec4)) != 0 ||
      (rf->submitted_froxel_fog.grid_dimensions_cell_pixels[0] != 0u) !=
          (prepared->frame.froxel_fog.grid_dimensions_cell_pixels[0] != 0u);
  prepared->temporal_input = (VkrTemporalFrameInput){
      .view = packet->globals.view,
      .projection = packet->globals.projection,
      .view_position = packet->globals.view_position,
      .scene_generation = packet->frame.scene_generation,
      .frame_index = packet->frame.frame_index,
      .width = temporal_width,
      .height = temporal_height,
      .render_mode = packet->globals.render_mode,
      .jitter_phase_count = prepared->frame.fsr31_enabled
                                ? vkr_temporal_upscale_sequence_length(
                                      temporal_width, rf->scene_output_width)
                                : VKR_TEMPORAL_SEQUENCE_LENGTH,
      .explicit_reset_reasons =
          rf->temporal_reset_reasons |
          ((fog_changed ||
            rf->submitted_ssgi_enabled != prepared->frame.ssgi_enabled)
               ? VKR_TEMPORAL_RESET_SCENE_CHANGE
               : 0u),
      .enabled = temporal_frame,
  };
  prepared->frame.temporal =
      vkr_temporal_prepare(&rf->temporal_state, &prepared->temporal_input);
  if (prepared->frame.froxel_fog.grid_dimensions_cell_pixels[0])
    prepared->frame.froxel_fog.inverse_raster_view_projection =
        mat4_inverse(mat4_mul(prepared->frame.temporal.jittered_projection,
                              packet->globals.view));

  /* Exposure reuses the discontinuities temporal already derived at this same
     boundary rather than re-deriving them from the same inputs. */
  /* A completed exposure from another mode's chain must not seed the
     pre-exposure. */
  if (rf->exposure_state.mode != packet->globals.exposure_mode)
    rf->observed_exposure = 0.0f;
  prepared->exposure_input = (VkrExposureFrameInput){
      .mode = packet->globals.exposure_mode,
      .manual_exposure = packet->globals.manual_exposure,
      .compensation_ev = packet->globals.exposure_compensation_ev,
      .min_ev = packet->globals.exposure_min_ev,
      .max_ev = packet->globals.exposure_max_ev,
      .delta_time = packet->frame.delta_time,
      .observed_exposure = rf->observed_exposure,
      .temporal_reset_reasons = prepared->frame.temporal.reset_reasons,
      .explicit_reset_reasons = rf->exposure_reset_reasons,
  };
  prepared->frame.exposure =
      vkr_exposure_prepare(&rf->exposure_state, &prepared->exposure_input);
  rf->display_exposure =
      packet->globals.exposure_mode == VKR_EXPOSURE_MODE_AUTOMATIC &&
              rf->observed_exposure > 0.0f
          ? rf->observed_exposure
          : packet->globals.manual_exposure;
  if (rf->pre_exposure_forced) {
    prepared->frame.exposure.pre_exposure_stops = rf->pre_exposure_forced_stops;
    prepared->frame.exposure.pre_exposure =
        ldexpf(1.0f, rf->pre_exposure_forced_stops);
  }
  /* Inspection modes write albedo, normals and other non-radiance values
     into the HDR target, which a 1/P tonemap would distort. */
  if (packet->globals.render_mode != VKR_RENDER_MODE_DEFAULT) {
    prepared->frame.exposure.pre_exposure_stops = 0;
    prepared->frame.exposure.pre_exposure = 1.0f;
  }
  /* The sky record is lowered pre-exposed; the frame input keeps the physical
     sun the temporal signature hashes. Its RGBA16F limits then bound the
     stored values, which is what they protect. */
  const float32_t pre_exposure = prepared->frame.exposure.pre_exposure;
  prepared->frame.sky.atmosphere.solar.x *= pre_exposure;
  prepared->frame.sky.atmosphere.solar.y *= pre_exposure;
  prepared->frame.sky.atmosphere.solar.z *= pre_exposure;
  prepared->frame.sky.atmosphere.lunar.x *= pre_exposure;
  prepared->frame.sky.atmosphere.lunar.y *= pre_exposure;
  prepared->frame.sky.atmosphere.lunar.z *= pre_exposure;
  prepared->frame.sky.star_pole.w *= pre_exposure;

  prepared->frame.color_grading = vkr_color_grading_prepare(
      packet->globals.white_balance_temperature,
      packet->globals.white_balance_tint, packet->globals.color_contrast,
      packet->globals.color_saturation);

  /* Bloom carries no history, so it has no prepare/commit chain: the frame
     block is a pure function of the validated packet fields. */
  prepared->frame.bloom = vkr_bloom_prepare(
      packet->globals.bloom_enabled && !rf->bloom_forced_disabled &&
          !indirect_diffuse_only && !editor_inspection,
      packet->globals.bloom_threshold, packet->globals.bloom_knee,
      packet->globals.bloom_intensity, prepared->frame.exposure.pre_exposure);
  prepared->frame.subsurface_enabled =
      prepared->frame.scene_rendering && !orthographic && packet->lighting &&
      packet->lighting->subsurface.profile_count > 0u &&
      packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  if (prepared->frame.subsurface_enabled) {
    const Mat4 projection = packet->globals.projection;
    prepared->frame.subsurface = (VkrSubsurfaceGpuParams){
        .projection = {projection.m23 / projection.m22,
                       projection.m23 / (projection.m22 + 1.0f),
                       0.5f * fabsf(projection.m00) * temporal_width,
                       0.5f * fabsf(projection.m11) * temporal_height},
        .dimensions = {temporal_width, temporal_height,
                       packet->lighting->subsurface.profile_count, 0u}};
  }
  prepared->frame.dof_enabled =
      prepared->frame.scene_rendering && packet->globals.dof_enabled &&
      packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  if (prepared->frame.dof_enabled)
    prepared->frame.dof = vkr_dof_prepare(
        packet->globals.dof_focus_distance, packet->globals.dof_f_stop,
        packet->globals.projection, prepared->frame.temporal.jitter_pixels,
        prepared->frame.scene_output_width, prepared->frame.scene_output_height,
        temporal_width, temporal_height);
  prepared->frame.motion_blur_delta_seconds = packet->frame.delta_time;
  rf->clock_seconds =
      fmod(rf->clock_seconds + Max(packet->frame.delta_time, 0.0), 3600.0);
  prepared->frame.clock_seconds = (float32_t)rf->clock_seconds;
  prepared->frame.motion_blur_enabled =
      prepared->frame.scene_rendering && packet->globals.motion_blur_enabled &&
      packet->globals.motion_blur_shutter_angle > 0.0f &&
      packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  if (prepared->frame.motion_blur_enabled)
    prepared->frame.motion_blur = vkr_motion_blur_prepare(
        packet->globals.motion_blur_shutter_angle, packet->globals.projection,
        prepared->frame.temporal.jitter_pixels,
        prepared->frame.scene_output_width, prepared->frame.scene_output_height,
        temporal_width, temporal_height);
  /* GTAO is current-frame spatial state: like bloom, it has no prepare/commit
     history chain. */
  prepared->frame.ssr_enabled =
      packet->globals.ssr_enabled && !rf->ssr_forced_disabled &&
      !orthographic && packet->globals.render_mode == VKR_RENDER_MODE_DEFAULT;
  prepared->frame.gtao = vkr_gtao_prepare(
      packet->globals.gtao_enabled && !rf->gtao_forced_disabled &&
          !orthographic && !wireframe,
      packet->globals.gtao_radius, packet->globals.gtao_power);
  /* The tiled graph has none of the desktop pipeline's screen-space effects,
     surface diffusion or lens effects (ADR-087). */
  if (tiled) {
    prepared->frame.subsurface_enabled = false_v;
    prepared->frame.dof_enabled = false_v;
    prepared->frame.motion_blur_enabled = false_v;
    prepared->frame.ssr_enabled = false_v;
    prepared->frame.ssgi_enabled = false_v;
    prepared->frame.gtao = vkr_gtao_prepare(
        false_v, packet->globals.gtao_radius, packet->globals.gtao_power);
  }
}

vkr_internal void vkr_renderer_backend_get_device_information(
    VkrRenderer *renderer, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
#if defined(PLATFORM_APPLE)
  (void)temp_arena;
  const VkrDisplayOutputParams display_output =
      vkr_metal_packet_renderer_display_output(renderer->metal_renderer);
  const VkrPresentMode actual_present_mode =
      vkr_metal_packet_renderer_present_mode(renderer->metal_renderer);
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
      .device_name =
          vkr_metal_packet_renderer_device_name(renderer->metal_renderer),
      .vendor_name = string8_lit("Apple"),
      .driver_version = string8_lit("Metal 4"),
      .api_version = string8_lit("Metal 4"),
      .device_types = device_types,
      .device_queues = device_queues,
      .sampler_filters = sampler_filters,
      .max_sampler_anisotropy = 16.0,
      .supports_texture_astc_4x4 = true_v,
      /* Metal 4 devices are Apple7 or later; Apple6 adds ASTC HDR. */
      .supports_texture_astc_hdr = true_v,
      .supports_texture_bc7 = true_v,
      .supports_texture_bc5 = true_v,
      .graphics_pipeline = renderer->graphics_pipeline,
      .actual_target_kind = renderer->present_target.kind,
      .actual_present_mode = actual_present_mode,
      .actual_target_image_count =
          renderer->impl.caps.present_target_image_count,
      .actual_target_width = renderer->last_window_width,
      .actual_target_height = renderer->last_window_height,
      .actual_render_width = renderer->render_width,
      .actual_render_height = renderer->render_height,
      .actual_color_format =
          display_output.extended_linear
              ? VKR_SURFACE_COLOR_FORMAT_RGBA16_SFLOAT
          : renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
              : VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB,
      .actual_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT,
      .actual_color_space = display_output.extended_linear
                                ? VKR_SURFACE_COLOR_SPACE_EXTENDED_SRGB_LINEAR
                                : VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR,
      .display_output = display_output,
      .actual_world_renderer_topology = VKR_WORLD_RENDERER_TOPOLOGY_DEFERRED,
  };
#else
  (void)temp_arena;
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
      .supports_texture_astc_hdr = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_ASTC_4x4_HDR),
      .supports_texture_bc7 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_BC7_UNORM),
      .supports_texture_bc5 = vkr_vulkan_renderer_texture_format_supported(
          renderer->vulkan_renderer, VKR_TEXTURE_FORMAT_BC5_UNORM),
      .graphics_pipeline = renderer->graphics_pipeline,
      .actual_target_kind = renderer->present_target.kind,
      .actual_present_mode = present_mode,
      .actual_target_image_count = renderer->present_target.image_count,
      .actual_target_width = renderer->last_window_width,
      .actual_target_height = renderer->last_window_height,
      .actual_render_width = renderer->render_width,
      .actual_render_height = renderer->render_height,
      .actual_color_format = color_format,
      .actual_depth_format = depth_format,
      .actual_color_space = color_space,
      .display_output =
          vkr_vulkan_renderer_display_output(renderer->vulkan_renderer),
      .actual_world_renderer_topology = VKR_WORLD_RENDERER_TOPOLOGY_DEFERRED,
  };
#endif
}

vkr_internal VkrRendererError
vkr_renderer_backend_wait_idle(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_wait_idle(renderer->metal_renderer)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_DEVICE_ERROR;
#else
  return vkr_vulkan_renderer_wait_idle(renderer->vulkan_renderer)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_DEVICE_ERROR;
#endif
}

vkr_internal uint64_t
vkr_renderer_backend_submit_serial(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_submit_value(renderer->metal_renderer);
#else
  return vkr_vulkan_renderer_submit_value(renderer->vulkan_renderer);
#endif
}

vkr_internal uint64_t
vkr_renderer_backend_completed_submit_serial(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_completed_value(renderer->metal_renderer);
#else
  return vkr_vulkan_renderer_completed_value(renderer->vulkan_renderer);
#endif
}

vkr_internal bool8_t vkr_renderer_backend_upload_wait_stats(
    VkrRenderer *renderer, VkrRendererUploadWaitStats *out_stats) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_get_and_reset_upload_wait_count(
      renderer->metal_renderer, &out_stats->fence_wait_count);
#else
  MemZero(out_stats, sizeof(*out_stats));
  return vkr_vulkan_renderer_get_and_reset_upload_wait_count(
             renderer->vulkan_renderer, &out_stats->fence_wait_count) &&
         vkr_vulkan_renderer_get_and_reset_frame_upload_exhaustion_count(
             renderer->vulkan_renderer,
             &out_stats->frame_upload_exhaustion_count);
#endif
}

vkr_internal bool8_t vkr_renderer_backend_command_slot_waits(
    VkrRenderer *renderer, uint64_t *out_wait_count) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_get_and_reset_command_slot_wait_count(
      renderer->metal_renderer, out_wait_count);
#else
  return vkr_vulkan_renderer_get_and_reset_command_slot_wait_count(
      renderer->vulkan_renderer, out_wait_count);
#endif
}

vkr_internal bool8_t vkr_renderer_backend_device_memory_stats(
    VkrRenderer *renderer, VkrDeviceMemoryStats *out_stats) {
#if defined(PLATFORM_APPLE)
  if (!renderer || !renderer->metal_renderer || !out_stats)
    return false_v;
  MemZero(out_stats, sizeof(*out_stats));
  VkrMetalMemoryDeviceMetrics metrics = {0};
  if (!vkr_metal_packet_renderer_get_memory_metrics(renderer->metal_renderer,
                                                    &metrics)) {
    return false_v;
  }
  if (metrics.suballocations.free_bytes > metrics.suballocations.heap_size)
    return false_v;
  out_stats->live_allocation_count = (uint32_t)metrics.native_heap_count;
  out_stats->peak_allocation_count = (uint32_t)metrics.native_heap_peak_count;
  out_stats->total_allocation_count = metrics.native_heap_total_count;
  out_stats->max_allocation_count = (uint32_t)metrics.max_native_heaps;
  out_stats->live_bytes = metrics.native_heap_allocated_size;
  out_stats->peak_bytes = metrics.native_heap_peak_allocated_size;
  out_stats->live_totals_exact = true_v;
  MemCopy(out_stats->owners, metrics.owners, sizeof(out_stats->owners));
  out_stats->memory_type_count = 1;
  out_stats->live_bytes_by_type[0] = out_stats->live_bytes;
  out_stats->live_count_by_type[0] = out_stats->live_allocation_count;
  out_stats->heap_index_by_type[0] = 0;
  out_stats->heap_count = 1;
  out_stats->heap_size_bytes[0] = metrics.managed_budget_size;
  out_stats->heap_usage_bytes[0] = metrics.managed_allocated_size;
  out_stats->heap_budget_bytes[0] = metrics.managed_budget_size;
  out_stats->texture_heap_capacity_bytes = metrics.texture_heap_capacity_bytes;
  out_stats->texture_heap_capacity_valid = true_v;
  out_stats->heap_usage_valid = metrics.managed_budget_size > 0u;
  return true_v;
#else
  if (!renderer || !renderer->vulkan_renderer || !out_stats)
    return false_v;
  vkr_vulkan_renderer_device_memory_stats(renderer->vulkan_renderer, out_stats);
  return true_v;
#endif
}

#if !defined(PLATFORM_APPLE)
vkr_internal bool8_t vkr_renderer_backend_memory_metrics(
    VkrRenderer *renderer, VkrRendererImplMemoryMetrics *out_metrics) {
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
#endif

vkr_internal void vkr_renderer_backend_resize(VkrRenderer *renderer,
                                              uint32_t width, uint32_t height) {
#if defined(PLATFORM_APPLE)
  /* Metal realizes its targets from each prepared frame's extent. */
  (void)renderer;
  (void)width;
  (void)height;
#else
  if (vkr_vulkan_renderer_resize(
          renderer->vulkan_renderer, width, height,
          renderer->impl.caps.present_target_image_count)) {
    renderer->last_window_width = width;
    renderer->last_window_height = height;
    renderer->present_target.width = width;
    renderer->present_target.height = height;
  }
#endif
}

vkr_internal VkrRendererError vkr_renderer_backend_present_target_recreate(
    VkrRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t image_count) {
#if defined(PLATFORM_APPLE)
  (void)image_count;
  VkrRendererError idle = vkr_renderer_backend_wait_idle(renderer);
  if (idle != VKR_RENDERER_ERROR_NONE) {
    return idle;
  }
  renderer->present_target.width = width;
  renderer->present_target.height = height;
  renderer->present_target.image_count =
      renderer->impl.caps.present_target_image_count;
  return VKR_RENDERER_ERROR_NONE;
#else
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
#endif
}

VkrCaptureStatus
vkr_renderer_backend_capture_poll(VkrRenderer *renderer,
                                  VkrCaptureRequestId request_id,
                                  VkrCapturePollResult *out_result) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_capture_poll(renderer->metal_renderer,
                                                request_id, out_result);
#else
  return vkr_vulkan_renderer_capture_poll(renderer->vulkan_renderer, request_id,
                                          out_result);
#endif
}

bool8_t vkr_renderer_backend_capture_release(VkrRenderer *renderer,
                                             VkrCaptureRequestId request_id) {
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_capture_release(renderer->metal_renderer,
                                                   request_id);
#else
  return vkr_vulkan_renderer_capture_release(renderer->vulkan_renderer,
                                             request_id);
#endif
}

vkr_internal bool8_t vkr_renderer_backend_poll_submit_result(
    VkrRenderer *renderer, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result) {
#if defined(PLATFORM_APPLE)
  VkrMetalPacketResult result = {0};
  if (!out_result ||
      !vkr_metal_packet_renderer_submit_result_poll_next(
          renderer->metal_renderer, after_submit_value, &result)) {
    return false_v;
  }
  vkr_renderer_impl_lower_metal_result(&result, out_result);
  return true_v;
#else
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
      .transmission_gpu_resolve_invalid_count =
          source.transmission_gpu_resolve_invalid_count,
      .transmission_gpu_occlusion_culled_count =
          source.transmission_gpu_occlusion_culled_count,
      .transmission_compact_overflow_count =
          source.transmission_compact_overflow_count,
      .transmission_coverage_valid = source.has_transmission_coverage,
      .hzb_history_valid = source.hzb_history_valid,
      .shadow_depth_range = source.shadow_depth_range,
      .local_light_contribution = source.local_light_contribution,
      .has_gpu_draw_diagnostics = source.has_gpu_draw_diagnostics,
      .exposure = source.exposure,
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
  (void)vkr_renderer_backend_memory_metrics(renderer, &out_result->memory);
  return true_v;
#endif
}

vkr_internal VkrAllocator *
vkr_renderer_backend_allocator(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  return &renderer->render_graph_allocator;
#else
  return vkr_vulkan_renderer_allocator(renderer->vulkan_renderer);
#endif
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

void vkr_renderer_get_device_information(
    VkrRenderer *renderer, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  vkr_renderer_backend_get_device_information(renderer, device_information,
                                              temp_arena);
}

bool32_t vkr_renderer_is_frame_active(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  return renderer->frame_active;
}

VkrRendererError vkr_renderer_wait_idle(VkrRenderer *renderer) {
  vkr_renderer_flush_publications(renderer);
  return vkr_renderer_backend_wait_idle(renderer);
}

bool8_t
vkr_renderer_gpu_submission_timing_poll(VkrRenderer *renderer,
                                        uint64_t after_submit_serial,
                                        VkrGpuSubmissionTiming *out_timing) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer || !out_timing)
    return false_v;
  VkrRendererImplSubmitResult result = {0};
  if (!vkr_renderer_backend_poll_submit_result(renderer, after_submit_serial,
                                               &result))
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

uint64_t vkr_renderer_get_submit_serial(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  return vkr_renderer_backend_submit_serial(renderer);
}

uint64_t vkr_renderer_get_completed_submit_serial(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  return vkr_renderer_backend_completed_submit_serial(renderer);
}

VkrRendererPipelineStats
vkr_renderer_get_pipeline_stats(const VkrRenderer *renderer) {
  VkrRendererPipelineStats stats = {0};
#if defined(PLATFORM_APPLE)
  if (renderer && renderer->metal_renderer)
    vkr_metal_packet_renderer_pipeline_stats(
        renderer->metal_renderer, &stats.pending_graphs, &stats.late_draws);
#endif
  (void)renderer;
  return stats;
}

void vkr_renderer_reload_custom_materials(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  if (renderer && renderer->metal_renderer)
    vkr_metal_packet_renderer_request_custom_reload(renderer->metal_renderer);
#endif
  (void)renderer;
}

float32_t vkr_renderer_get_display_exposure(const VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  return renderer->display_exposure;
}

bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRenderer *renderer, VkrRendererUploadWaitStats *out_stats) {
  vkr_renderer_join_render_thread(renderer);
  out_stats->fence_wait_count = 0;
  out_stats->queue_wait_idle_count = 0;
  out_stats->device_wait_idle_count = 0;
  out_stats->frame_upload_exhaustion_count = 0;
  return vkr_renderer_backend_upload_wait_stats(renderer, out_stats);
}

bool8_t
vkr_renderer_get_and_reset_command_slot_wait_count(VkrRenderer *renderer,
                                                   uint64_t *out_wait_count) {
  vkr_renderer_join_render_thread(renderer);
  *out_wait_count = 0;
  return vkr_renderer_backend_command_slot_waits(renderer, out_wait_count);
}

bool8_t vkr_renderer_get_device_memory_stats(VkrRenderer *renderer,
                                             VkrDeviceMemoryStats *out_stats) {
  vkr_renderer_join_render_thread(renderer);
  MemZero(out_stats, sizeof(*out_stats));
  return vkr_renderer_backend_device_memory_stats(renderer, out_stats);
}

void vkr_renderer_present_target_extent(VkrRenderer *renderer,
                                        uint32_t *out_width,
                                        uint32_t *out_height) {
  vkr_renderer_join_render_thread(renderer);
  if (out_width) {
    *out_width = renderer->last_window_width;
  }
  if (out_height) {
    *out_height = renderer->last_window_height;
  }
}

VkrTextureFormat
vkr_renderer_present_target_format(VkrRenderer *renderer,
                                   VkrPresentTargetAttachment attachment) {
  vkr_renderer_join_render_thread(renderer);
  return attachment == VKR_PRESENT_TARGET_ATTACHMENT_COLOR
             ? renderer->impl.caps.present_color_format
             : renderer->impl.caps.present_depth_format;
}

VkrRendererError vkr_renderer_present_target_recreate(VkrRenderer *renderer,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t image_count) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (width == 0 || height == 0 || image_count == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  VkrRendererError result = vkr_renderer_backend_present_target_recreate(
      renderer, width, height, image_count);
  if (result != VKR_RENDERER_ERROR_NONE) {
    return result;
  }
  renderer->target_generation++;
  width = renderer->present_target.width;
  height = renderer->present_target.height;
  renderer->last_window_width = width;
  renderer->last_window_height = height;
  if (!renderer->scene_output_extent_overridden) {
    if (renderer->scene_output_width != width ||
        renderer->scene_output_height != height)
      vkr_dynamic_resolution_reset_feedback(
          &renderer->dynamic_resolution_state);
    renderer->scene_output_width = width;
    renderer->scene_output_height = height;
  }
  renderer->render_width = vkr_renderer_scaled_extent(
      renderer->scene_output_width, renderer->render_scale);
  renderer->render_height = vkr_renderer_scaled_extent(
      renderer->scene_output_height, renderer->render_scale);
  renderer->timing_result.shadow_depth_range = (VkrShadowDepthRangeSample){0};
  return VKR_RENDERER_ERROR_NONE;
}

VkrTextureFormat vkr_renderer_get_shadow_depth_format(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  return renderer->impl.caps.shadow_depth_format;
}

vkr_internal VkrRendererError vkr_renderer_validation_fail(
    VkrValidationError *out_error, VkrRendererError code,
    const char *field_path, const char *message) {
  if (out_error) {
    out_error->code = code;
    out_error->field_path = field_path;
    out_error->message = message;
  }
  return code;
}

vkr_internal VkrRendererError vkr_renderer_backend_prepare_frame(
    VkrRenderer *renderer, const VkrFrameConfig *config, VkrFrame *out_setup) {
#if defined(PLATFORM_APPLE)
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  bool8_t output_format_changed = false_v;
  if (!vkr_metal_packet_renderer_refresh_display_output(
          renderer->metal_renderer, &output_format_changed))
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  renderer->impl.caps.present_color_format =
      vkr_metal_packet_renderer_present_color_format(renderer->metal_renderer);
  if (output_format_changed) {
    ++renderer->target_generation;
    renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
  }
  if (renderer->present_target.kind == VKR_PRESENT_TARGET_WINDOWED) {
    VkrSurfaceSize pixels =
        renderer->surface.pixel_size(renderer->surface.context);
    if (pixels.width == 0 || pixels.height == 0) {
      return VKR_RENDERER_ERROR_FRAME_SKIPPED;
    }
    if (renderer->last_window_width != pixels.width ||
        renderer->last_window_height != pixels.height)
      renderer->target_generation++;
    renderer->last_window_width = pixels.width;
    renderer->last_window_height = pixels.height;
    if (!renderer->scene_output_extent_overridden) {
      if (renderer->scene_output_width != pixels.width ||
          renderer->scene_output_height != pixels.height)
        vkr_dynamic_resolution_reset_feedback(
            &renderer->dynamic_resolution_state);
      renderer->scene_output_width = pixels.width;
      renderer->scene_output_height = pixels.height;
    }
  }
  renderer->timing_completed_ready = vkr_renderer_backend_poll_submit_result(
      renderer, renderer->timing_last_completed_submit_value,
      &renderer->timing_result);
  vkr_renderer_observe_exposure(renderer);
  if (renderer->timing_completed_ready) {
    renderer->timing_last_completed_submit_value =
        renderer->timing_result.submit_value;
    float32_t next_scale = renderer->render_scale;
    /* A completed sample from the previous output size cannot train the
       current tier, even when its scale happens to match. */
    if (renderer->timing_result.gpu_submission_valid &&
        renderer->timing_result.source_render_width ==
            vkr_renderer_scaled_extent(
                renderer->scene_output_width,
                renderer->timing_result.source_render_scale) &&
        renderer->timing_result.source_render_height ==
            vkr_renderer_scaled_extent(
                renderer->scene_output_height,
                renderer->timing_result.source_render_scale) &&
        vkr_dynamic_resolution_update(
            &renderer->dynamic_resolution_state,
            renderer->timing_result.submit_value,
            renderer->timing_result.gpu_submission_ns,
            renderer->timing_result.source_render_scale, &next_scale)) {
      renderer->render_scale = next_scale;
      renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
    }
  }
  renderer->render_width = vkr_renderer_scaled_extent(
      renderer->scene_output_width, renderer->render_scale);
  renderer->render_height = vkr_renderer_scaled_extent(
      renderer->scene_output_height, renderer->render_scale);
  renderer->frame_active = true_v;
  VkrRenderGraphFrameInfo frame = {
      .frame_index = (uint32_t)(renderer->frame_number + 1u),
      .image_index = 0,
      .delta_time = 1.0 / 60.0,
      .target_width = renderer->last_window_width,
      .target_height = renderer->last_window_height,
      .window_width = renderer->last_window_width,
      .window_height = renderer->last_window_height,
      .scene_output_width = renderer->scene_output_width,
      .scene_output_height = renderer->scene_output_height,
      .viewport_width = renderer->render_width,
      .viewport_height = renderer->render_height,
      .render_scale = renderer->render_scale,
      .fsr31_enabled = renderer->upscale_mode == VKR_UPSCALE_MODE_FSR31,
      .picking_pending = false_v,
      .target_color_format = renderer->impl.caps.present_color_format,
      .target_depth_format = renderer->impl.caps.present_depth_format,
      .target_color_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                     .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
      .target_depth_initial_state = {.access = VKR_IMAGE_ACCESS_NONE,
                                     .layout = VKR_TEXTURE_LAYOUT_UNDEFINED},
      .target_terminal_state = {.access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
                                .layout =
                                    VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL},
      .shadow_depth_format = renderer->impl.caps.shadow_depth_format,
      .shadow_map_size = config->shadow_map_size,
      .local_shadow_map_size = config->local_shadow_map_size,
      .local_shadow_transmission_map_size =
          vkr_local_shadow_transmission_map_size(
              config->local_shadow_map_size, config->local_shadow_face_budget),
      .local_shadow_map_layer_count = config->local_shadow_face_budget,
      .shadow_map_layer_count = config->shadow_cascade_count,
      .shadow_cascade_count = config->shadow_cascade_count,
  };
  if (!vkr_metal_packet_renderer_prepare_frame(renderer->metal_renderer,
                                               &frame)) {
    renderer->frame_active = false_v;
    return VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  frame.image_index =
      vkr_metal_packet_renderer_frame_image_index(renderer->metal_renderer);
  renderer->frame_number++;
  MemZero(&renderer->frame_metrics, sizeof(renderer->frame_metrics));
  *out_setup = (VkrFrame){
      .image_index = frame.image_index,
      .window_width = renderer->last_window_width,
      .window_height = renderer->last_window_height,
      .swapchain_format = frame.target_color_format,
      .swapchain_depth_format = frame.target_depth_format,
  };
  vkr_metal_packet_renderer_retained_shadow_token(
      renderer->metal_renderer, frame.image_index, &out_setup->retained_shadow);
  vkr_metal_packet_renderer_retained_local_shadow_token(
      renderer->metal_renderer, &out_setup->retained_local_shadow);
  return VKR_RENDERER_ERROR_NONE;
#else
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (!vkr_vulkan_renderer_prepare_frame(
          renderer->vulkan_renderer, renderer->frame_number + 1u,
          config->shadow_map_size, config->shadow_cascade_count,
          config->local_shadow_map_size, config->local_shadow_face_budget,
          out_setup)) {
    const VkrRendererError error =
        vkr_vulkan_renderer_get_error(renderer->vulkan_renderer);
    return error != VKR_RENDERER_ERROR_NONE
               ? error
               : VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  renderer->frame_active = true_v;
  renderer->frame_number++;
  const bool8_t output_format_changed =
      renderer->impl.caps.present_color_format != out_setup->swapchain_format;
  if (renderer->last_window_width != out_setup->window_width ||
      renderer->last_window_height != out_setup->window_height ||
      output_format_changed)
    renderer->target_generation++;
  renderer->impl.caps.present_color_format = out_setup->swapchain_format;
  if (output_format_changed)
    renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
  renderer->last_window_width = out_setup->window_width;
  renderer->last_window_height = out_setup->window_height;
  if (!renderer->scene_output_extent_overridden) {
    renderer->scene_output_width = out_setup->window_width;
    renderer->scene_output_height = out_setup->window_height;
  }
  renderer->render_width = vkr_renderer_scaled_extent(
      renderer->scene_output_width, renderer->render_scale);
  renderer->render_height = vkr_renderer_scaled_extent(
      renderer->scene_output_height, renderer->render_scale);
  renderer->timing_completed_ready = vkr_renderer_backend_poll_submit_result(
      renderer, renderer->timing_last_completed_submit_value,
      &renderer->timing_result);
  vkr_renderer_observe_exposure(renderer);
  if (renderer->timing_completed_ready) {
    renderer->timing_last_completed_submit_value =
        renderer->timing_result.submit_value;
  }
  MemZero(&renderer->frame_metrics, sizeof(renderer->frame_metrics));
  return VKR_RENDERER_ERROR_NONE;
#endif
}

vkr_internal VkrRendererError vkr_renderer_acquire_frame(
    VkrRenderer *renderer, const VkrFrameConfig *config, VkrFrame *out_frame) {
  if (!renderer || !out_frame || !config || config->shadow_map_size == 0u ||
      config->shadow_cascade_count == 0u ||
      config->shadow_cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX ||
      config->local_shadow_face_budget >
          VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX ||
      (config->local_shadow_face_budget != 0u &&
       config->local_shadow_map_size == 0u))
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (renderer->frame_active)
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  *out_frame = (VkrFrame){0};
  const VkrRendererError error =
      vkr_renderer_backend_prepare_frame(renderer, config, out_frame);
  if (error == VKR_RENDERER_ERROR_NONE) {
    out_frame->renderer = renderer;
    out_frame->number = renderer->frame_number;
    out_frame->target_generation = renderer->target_generation;
    out_frame->render_width = renderer->render_width;
    out_frame->render_height = renderer->render_height;
  }
  return error;
}

VkrRendererError vkr_renderer_begin_frame(VkrRenderer *renderer,
                                          const VkrFrameConfig *config,
                                          VkrFrame *out_frame) {
  vkr_renderer_join_render_thread(renderer);
  const VkrRendererError error =
      vkr_renderer_acquire_frame(renderer, config, out_frame);
  /* No frame will run the recorded publications; run them now, so assets
     still progress while frames are skipped. A frame already in progress
     runs them itself. */
  if (error != VKR_RENDERER_ERROR_NONE &&
      error != VKR_RENDERER_ERROR_FRAME_IN_PROGRESS) {
    vkr_renderer_flush_publications(renderer);
  }
  return error;
}

/* A submitted frame becomes the producer of the state its successor reuses. */
vkr_internal void
vkr_renderer_commit_submitted_frame(VkrRenderer *renderer,
                                    const VkrFrameInput *packet,
                                    const VkrFramePreparation *prepared) {
  if (prepared->frame.scene_rendering) {
    vkr_temporal_commit(&renderer->temporal_state, &prepared->temporal_input);
    renderer->submitted_ssgi_enabled = prepared->frame.ssgi_enabled;
    renderer->submitted_fog = prepared->frame.fog;
    renderer->submitted_froxel_fog = prepared->frame.froxel_fog;
    renderer->temporal_reset_reasons = 0u;
    vkr_exposure_commit(&renderer->exposure_state, &prepared->exposure_input,
                        &prepared->frame.exposure);
    renderer->exposure_reset_reasons = 0u;
    if (packet->editor) {
      renderer->editor_image_width = prepared->frame.editor_image_width;
      renderer->editor_image_height = prepared->frame.editor_image_height;
    }
  } else {
    /* UI submissions do not publish scene histories. Resume resets even if
       the caller uses scene-local frame indices without a submission gap. */
    renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
  }
}

vkr_internal VkrRendererError vkr_renderer_backend_render_frame(
    VkrRenderer *renderer, const VkrFrameInput *packet,
    VkrRendererFrameMetrics *out_metrics,
    VkrValidationError *out_validation_error) {
#if defined(PLATFORM_APPLE)
  VkrFramePreparation prepared;
  vkr_renderer_prepare_frame_data(renderer, packet, &prepared);
  VkrMetalPacketResult result = {0};
  const bool8_t submitted = vkr_metal_packet_renderer_submit_packet(
      renderer->metal_renderer, &prepared.frame, &result);
  renderer->frame_active = false_v;
  if (!submitted) {
    /* A post-submit failure cannot roll back native histories or GPU uses.
     * Stop the device path instead of retrying with uncommitted CPU history. */
    const bool8_t committed = result.submit_value != 0u;
    const VkrRendererError error = committed ? VKR_RENDERER_ERROR_DEVICE_ERROR
                                   : result.error != VKR_RENDERER_ERROR_NONE
                                       ? result.error
                                       : VKR_RENDERER_ERROR_SUBMISSION_FAILED;
    return vkr_renderer_validation_fail(
        out_validation_error, error, "metal",
        committed ? "Metal frame failed after queue submission"
        : result.error == VKR_RENDERER_ERROR_OUT_OF_MEMORY
            ? "Metal frame resource allocation exhausted GPU memory"
            : "Metal frame rendering failed");
  }
  vkr_renderer_commit_submitted_frame(renderer, packet, &prepared);
  VkrRendererImplSubmitResult current_result = {0};
  vkr_renderer_impl_lower_metal_result(&result, &current_result);
  current_result.source_frame_index = packet->frame.frame_index;
  if (!renderer->timing_completed_ready) {
    renderer->timing_result = current_result;
    renderer->timing_result.source_frame_index = packet->frame.frame_index;
  }
  const VkrRendererImplSubmitResult *observed = &renderer->timing_result;
  renderer->frame_metrics.world.pipelines_late =
      observed->custom_late_draw_count;
  renderer->frame_metrics.gpu_submission_ns = observed->gpu_submission_ns;
  renderer->frame_metrics.gpu_submission_valid = observed->gpu_submission_valid;
  renderer->frame_metrics.gpu_submission_unavailable_reason =
      vkr_renderer_gpu_timing_metric_reason(
          observed->gpu_submission_unavailable_reason);
  renderer->frame_metrics.world.draws_collected = observed->indexed_draw_count;
  renderer->frame_metrics.world.opaque_draws = observed->opaque_draw_count;
  renderer->frame_metrics.world.transmission_draws =
      observed->transmission_draw_count;
  renderer->frame_metrics.world.transparent_draws = observed->blend_draw_count;
  renderer->frame_metrics.world.draws_issued = observed->indexed_draw_count;
  renderer->frame_metrics.world.draw_calls_issued =
      observed->indexed_draw_count;
  vkr_renderer_record_gpu_candidate_metrics(renderer, &prepared.frame.input);
  vkr_renderer_record_local_shadow_metrics(renderer, &prepared.frame.input);
  /* From this submit's own result: packet lowering ran on this thread now,
     while `observed` may still describe an older completed frame. */
  renderer->frame_metrics.packet_build = result.packet_build;
  renderer->frame_metrics.world.hzb_history_valid = observed->hzb_history_valid;
  renderer->frame_metrics.exposure = observed->exposure;
  if (observed->has_gpu_draw_diagnostics) {
    renderer->frame_metrics.world.opaque_draws = observed->gpu_visible_count;
    renderer->frame_metrics.world.transmission_draws =
        observed->transmission_gpu_visible_count;
    renderer->frame_metrics.world.draws_collected =
        renderer->frame_metrics.world.gpu_candidate_count +
        renderer->frame_metrics.world.transmission_gpu_candidate_count +
        observed->blend_draw_count;
    renderer->frame_metrics.world.gpu_visible_count =
        observed->gpu_visible_count;
    MemCopy(renderer->frame_metrics.world.gpu_bucket_counts,
            observed->gpu_bucket_counts,
            sizeof(renderer->frame_metrics.world.gpu_bucket_counts));
    renderer->frame_metrics.world.gpu_compaction_overflow_count =
        observed->gpu_overflow_count;
    renderer->frame_metrics.world.gpu_resolve_invalid_count =
        observed->gpu_resolve_invalid_count;
    renderer->frame_metrics.world.gpu_occlusion_culled_count =
        observed->gpu_occlusion_culled_count;
    renderer->frame_metrics.world.transmission_gpu_visible_count =
        observed->transmission_gpu_visible_count;
    MemCopy(
        renderer->frame_metrics.world.transmission_gpu_bucket_counts,
        observed->transmission_gpu_bucket_counts,
        sizeof(renderer->frame_metrics.world.transmission_gpu_bucket_counts));
    renderer->frame_metrics.world.transmission_gpu_compaction_overflow_count =
        observed->transmission_gpu_overflow_count;
    renderer->frame_metrics.world.transmission_gpu_resolve_invalid_count =
        observed->transmission_gpu_resolve_invalid_count;
    renderer->frame_metrics.world.transmission_gpu_occlusion_culled_count =
        observed->transmission_gpu_occlusion_culled_count;
    renderer->frame_metrics.world.transmission_pixel_compaction_overflow_count =
        observed->transmission_compact_overflow_count;
    renderer->frame_metrics.world.gpu_diagnostics_valid = true_v;
    uint32_t opaque_indirect_calls = 0u;
    uint32_t transmission_indirect_calls = 0u;
    uint32_t max_batch_size = 0u;
    const uint32_t transmission_layers =
        ArrayCount(observed->transmission_covered_pixels);
    for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
         ++bucket) {
      opaque_indirect_calls +=
          observed->gpu_bucket_counts[bucket] > 0u ? 1u : 0u;
      max_batch_size = Max(max_batch_size, observed->gpu_bucket_counts[bucket]);
      if (observed->transmission_gpu_bucket_counts[bucket] > 0u)
        transmission_indirect_calls += transmission_layers;
      max_batch_size =
          Max(max_batch_size, observed->transmission_gpu_bucket_counts[bucket]);
    }
    const uint32_t indirect_draws =
        observed->gpu_visible_count +
        observed->transmission_gpu_visible_count * transmission_layers;
    const uint32_t indirect_calls =
        opaque_indirect_calls + transmission_indirect_calls;
    renderer->frame_metrics.world.opaque_batches = opaque_indirect_calls;
    renderer->frame_metrics.world.indirect_draws_issued = indirect_draws;
    renderer->frame_metrics.world.indirect_calls_issued = indirect_calls;
    renderer->frame_metrics.world.draws_issued =
        indirect_draws + observed->blend_draw_count;
    renderer->frame_metrics.world.draw_calls_issued =
        indirect_calls + observed->blend_draw_count;
    renderer->frame_metrics.world.max_batch_size = max_batch_size;
    renderer->frame_metrics.world.avg_batch_size =
        indirect_calls > 0u ? (float32_t)indirect_draws / indirect_calls : 0.0f;
  }
  if (observed->has_gpu_draw_diagnostics) {
    for (uint32_t cascade = 0u; cascade < VKR_SHADOW_CASCADE_COUNT_MAX;
         ++cascade) {
      renderer->frame_metrics.shadow.shadow_indirect_draws_opaque[cascade] =
          observed->shadow_gpu_visible_count[cascade];
      uint32_t indirect_calls = 0u;
      for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
           ++bucket) {
        indirect_calls +=
            observed->shadow_gpu_bucket_counts[cascade][bucket] > 0u ? 1u : 0u;
      }
      renderer->frame_metrics.shadow.shadow_indirect_calls_opaque[cascade] =
          indirect_calls;
      renderer->frame_metrics.shadow.shadow_indirect_overflow[cascade] =
          observed->shadow_gpu_overflow_count[cascade];
    }
  }
  if (out_metrics) {
    *out_metrics = renderer->frame_metrics;
  }
  return VKR_RENDERER_ERROR_NONE;
#else
  VkrFramePreparation prepared;
  vkr_renderer_prepare_frame_data(renderer, packet, &prepared);
  VkrVulkanResult result = {0};
  const bool8_t submitted = vkr_vulkan_renderer_submit_packet(
      renderer->vulkan_renderer, &prepared.frame, &result);
  renderer->frame_active = false_v;
  if (!submitted) {
    const VkrRendererError error =
        vkr_vulkan_renderer_get_error(renderer->vulkan_renderer);
    return vkr_renderer_validation_fail(
        out_validation_error,
        error != VKR_RENDERER_ERROR_NONE ? error
                                         : VKR_RENDERER_ERROR_SUBMISSION_FAILED,
        "vulkan", "Vulkan frame rendering failed");
  }
  vkr_renderer_commit_submitted_frame(renderer, packet, &prepared);
  if (!renderer->timing_completed_ready) {
    renderer->timing_result = (VkrRendererImplSubmitResult){
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
    MemCopy(renderer->timing_result.pass_timings, result.pass_timings,
            (uint64_t)result.pass_timing_count *
                sizeof(*renderer->timing_result.pass_timings));
    (void)vkr_renderer_backend_memory_metrics(renderer,
                                              &renderer->timing_result.memory);
  }
  const uint32_t world_draw_count = result.opaque_draw_count +
                                    result.transmission_draw_count +
                                    result.blend_draw_count;
  renderer->frame_metrics.world.draws_collected = world_draw_count;
  renderer->frame_metrics.world.opaque_draws = result.opaque_draw_count;
  renderer->frame_metrics.world.transmission_draws =
      result.transmission_draw_count;
  renderer->frame_metrics.world.transparent_draws = result.blend_draw_count;
  renderer->frame_metrics.world.draws_issued = world_draw_count;
  renderer->frame_metrics.world.draw_calls_issued = world_draw_count;
  vkr_renderer_record_gpu_candidate_metrics(renderer, &prepared.frame.input);
  vkr_renderer_record_local_shadow_metrics(renderer, &prepared.frame.input);
  /* Packet lowering happened on this thread during this submit, so it comes
     from the call's own result rather than from `timing_result`, which may
     still describe an older completed frame. */
  renderer->frame_metrics.packet_build = result.packet_build;
  const VkrRendererImplSubmitResult *observed = &renderer->timing_result;
  renderer->frame_metrics.world.hzb_history_valid = observed->hzb_history_valid;
  renderer->frame_metrics.exposure = observed->exposure;
  if (observed->has_gpu_draw_diagnostics) {
    renderer->frame_metrics.world.opaque_draws = observed->gpu_visible_count;
    renderer->frame_metrics.world.transmission_draws =
        observed->transmission_gpu_visible_count;
    renderer->frame_metrics.world.draws_collected =
        renderer->frame_metrics.world.gpu_candidate_count +
        renderer->frame_metrics.world.transmission_gpu_candidate_count +
        observed->blend_draw_count;
    renderer->frame_metrics.world.gpu_visible_count =
        observed->gpu_visible_count;
    MemCopy(renderer->frame_metrics.world.gpu_bucket_counts,
            observed->gpu_bucket_counts,
            sizeof(renderer->frame_metrics.world.gpu_bucket_counts));
    renderer->frame_metrics.world.gpu_compaction_overflow_count =
        observed->gpu_overflow_count;
    renderer->frame_metrics.world.gpu_resolve_invalid_count =
        observed->gpu_resolve_invalid_count;
    renderer->frame_metrics.world.gpu_occlusion_culled_count =
        observed->gpu_occlusion_culled_count;
    renderer->frame_metrics.world.transmission_gpu_visible_count =
        observed->transmission_gpu_visible_count;
    MemCopy(
        renderer->frame_metrics.world.transmission_gpu_bucket_counts,
        observed->transmission_gpu_bucket_counts,
        sizeof(renderer->frame_metrics.world.transmission_gpu_bucket_counts));
    renderer->frame_metrics.world.transmission_gpu_compaction_overflow_count =
        observed->transmission_gpu_overflow_count;
    renderer->frame_metrics.world.transmission_gpu_resolve_invalid_count =
        observed->transmission_gpu_resolve_invalid_count;
    renderer->frame_metrics.world.transmission_gpu_occlusion_culled_count =
        observed->transmission_gpu_occlusion_culled_count;
    renderer->frame_metrics.world.gpu_diagnostics_valid = true_v;
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
    renderer->frame_metrics.world.indirect_draws_issued = indirect_draws;
    renderer->frame_metrics.world.indirect_calls_issued = indirect_calls;
    renderer->frame_metrics.world.draws_issued =
        indirect_draws + observed->blend_draw_count;
    renderer->frame_metrics.world.draw_calls_issued =
        indirect_calls + observed->blend_draw_count;
    renderer->frame_metrics.world.max_batch_size = max_batch_size;
    renderer->frame_metrics.world.avg_batch_size =
        indirect_calls > 0u ? (float32_t)indirect_draws / indirect_calls : 0.0f;
    for (uint32_t cascade = 0u; cascade < VKR_SHADOW_CASCADE_COUNT_MAX;
         ++cascade) {
      renderer->frame_metrics.shadow.shadow_indirect_draws_opaque[cascade] =
          observed->shadow_gpu_visible_count[cascade];
      uint32_t cascade_calls = 0u;
      for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
           ++bucket)
        cascade_calls +=
            observed->shadow_gpu_bucket_counts[cascade][bucket] > 0u ? 1u : 0u;
      renderer->frame_metrics.shadow.shadow_indirect_calls_opaque[cascade] =
          cascade_calls;
      renderer->frame_metrics.shadow.shadow_indirect_overflow[cascade] =
          observed->shadow_gpu_overflow_count[cascade];
    }
  }
  if (out_metrics) {
    *out_metrics = renderer->frame_metrics;
  }
  return VKR_RENDERER_ERROR_NONE;
#endif
}

/* The frame's work on whichever thread renders it: acquire when the caller
   did not, complete the input through the hooks, validate, then render or
   cancel the acquisition. */
vkr_internal void
vkr_renderer_render_submitted(VkrRenderer *renderer, VkrRendererWork *work,
                              VkrRendererFrameResult *result) {
  const float64_t start = vkr_platform_get_absolute_time();
  *result = (VkrRendererFrameResult){0};
  VkrFrameInput *packet = work->input;
  VkrRendererError acquire_error = VKR_RENDERER_ERROR_NONE;
  if (work->acquire) {
    acquire_error =
        vkr_renderer_acquire_frame(renderer, &work->config, &work->frame);
    const float64_t acquired_at = vkr_platform_get_absolute_time();
    result->acquire_ns = (uint64_t)((acquired_at - start) * 1e9);
  }
  /* Publications recorded before this frame run first, inside it when it was
     acquired, so its input sees them. */
  if (work->publications) {
    vkr_publication_queue_run(renderer, work->publications);
  }
  if (acquire_error != VKR_RENDERER_ERROR_NONE) {
    vkr_publication_queue_observe(renderer);
    result->error = acquire_error;
    return;
  }
  result->frame = work->frame;
  result->frame.renderer = NULL;
  result->acquired = true_v;

  /* An input built for another target extent cannot render into this one;
     the caller rebuilds it for the extent this result reports. */
  if (work->acquire &&
      (packet->frame.window_width != work->frame.window_width ||
       packet->frame.window_height != work->frame.window_height)) {
    const VkrRendererError cancel_error =
        vkr_renderer_backend_cancel_frame(renderer);
    result->error = cancel_error != VKR_RENDERER_ERROR_NONE
                        ? cancel_error
                        : VKR_RENDERER_ERROR_FRAME_SKIPPED;
    vkr_publication_queue_observe(renderer);
    return;
  }

  const VkrFrameHooks *hooks = &work->hooks;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (hooks->prepare && !hooks->prepare(hooks->state, &work->frame, packet)) {
    error = vkr_renderer_validation_fail(
        &result->validation, VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED,
        "frame", "the caller could not complete the frame input");
  }
  if (error == VKR_RENDERER_ERROR_NONE) {
    error = vkr_frame_input_validate(packet, &result->validation);
  }
  if (error == VKR_RENDERER_ERROR_NONE &&
      (packet->frame.window_width != work->frame.window_width ||
       packet->frame.window_height != work->frame.window_height)) {
    error = vkr_renderer_validation_fail(
        &result->validation, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "frame_input.frame", "target extent must match the acquired frame");
  }
  if (error != VKR_RENDERER_ERROR_NONE) {
    const VkrRendererError cancel_error =
        vkr_renderer_backend_cancel_frame(renderer);
    if (cancel_error != VKR_RENDERER_ERROR_NONE) {
      error = vkr_renderer_validation_fail(
          &result->validation, cancel_error, "frame",
          "failed to cancel the acquired frame after input rejection");
    }
  } else {
    error = vkr_renderer_backend_render_frame(
        renderer, packet, &result->metrics, &result->validation);
  }
  if (hooks->finish) {
    hooks->finish(hooks->state, &work->frame, error);
  }
  vkr_publication_queue_observe(renderer);
  result->error = error;
  result->render_ns =
      (uint64_t)((vkr_platform_get_absolute_time() - start) * 1e9) -
      result->acquire_ns;
}

/* Runs `work` inline or hands it to the render thread. The caller already
   waited for the previous frame. */
vkr_internal void vkr_renderer_start_work(VkrRenderer *renderer,
                                          VkrRendererWork *work) {
  work->publications = vkr_publication_queue_take(&renderer->publications);
  /* A result nobody collected belongs to an older frame; this one replaces
     it. */
  renderer->frame_result_ready = true_v;
  VkrRendererWorker *worker = renderer->worker;
  if (!worker) {
    VkrRendererWork inline_work = *work;
    vkr_renderer_render_submitted(renderer, &inline_work,
                                  &renderer->frame_result);
    return;
  }
  vkr_mutex_lock(worker->mutex);
  worker->work = *work;
  vkr_atomic_uint32_store(&worker->state, VKR_RENDERER_WORKER_BUSY,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_cond_broadcast(worker->cond);
  vkr_mutex_unlock(worker->mutex);
}

VkrRendererError vkr_renderer_submit_frame(VkrFrame *frame,
                                           VkrFrameInput *packet,
                                           const VkrFrameHooks *hooks,
                                           VkrValidationError *out_validation) {
  VkrRenderer *renderer = frame ? frame->renderer : NULL;
  vkr_renderer_join_render_thread(renderer);
  if (!renderer || !renderer->frame_active ||
      frame->number != renderer->frame_number) {
    return vkr_renderer_validation_fail(
        out_validation, VKR_RENDERER_ERROR_INVALID_PARAMETER, "frame",
        "must identify the current acquired frame");
  }
  VkrRendererWork work = {
      .frame = *frame,
      .input = packet,
      .hooks = hooks ? *hooks : (VkrFrameHooks){0},
  };
  frame->renderer = NULL;
  vkr_renderer_start_work(renderer, &work);
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError vkr_renderer_submit_unacquired_frame(
    VkrRenderer *renderer, const VkrFrameConfig *config, VkrFrameInput *packet,
    const VkrFrameHooks *hooks) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer || !config || !packet) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  VkrRendererWork work = {
      .acquire = true_v,
      .config = *config,
      .input = packet,
      .hooks = hooks ? *hooks : (VkrFrameHooks){0},
  };
  vkr_renderer_start_work(renderer, &work);
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t vkr_renderer_complete_frame(VkrRenderer *renderer,
                                    VkrRendererFrameResult *out_result) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer || !renderer->frame_result_ready) {
    return false_v;
  }
  vkr_publication_queue_deliver(&renderer->publications);
  renderer->frame_result_ready = false_v;
  if (out_result) {
    *out_result = renderer->frame_result;
  }
  return true_v;
}

void vkr_renderer_flush_publications(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer) {
    return;
  }
  VkrPublicationBatch *batch =
      vkr_publication_queue_take(&renderer->publications);
  if (!batch) {
    return;
  }
  vkr_publication_queue_run(renderer, batch);
  vkr_publication_queue_observe(renderer);
  vkr_publication_queue_deliver(&renderer->publications);
}

bool8_t vkr_renderer_render_thread_enabled(const VkrRenderer *renderer) {
  return renderer && renderer->worker;
}

VkrRendererError
vkr_renderer_render_frame(VkrFrame *frame, const VkrFrameInput *packet,
                          VkrRendererFrameMetrics *out_metrics,
                          VkrValidationError *out_validation_error) {
  VkrRenderer *renderer = frame ? frame->renderer : NULL;
  /* The work only reads the input; without hooks nothing changes it. */
  VkrFrameInput input = packet ? *packet : (VkrFrameInput){0};
  const VkrRendererError submit_error = vkr_renderer_submit_frame(
      frame, packet ? &input : NULL, NULL, out_validation_error);
  if (submit_error != VKR_RENDERER_ERROR_NONE) {
    return submit_error;
  }
  VkrRendererFrameResult result = {0};
  vkr_renderer_complete_frame(renderer, &result);
  if (result.error == VKR_RENDERER_ERROR_NONE) {
    if (out_metrics) {
      *out_metrics = result.metrics;
    }
  } else if (out_validation_error) {
    *out_validation_error = result.validation;
  }
  return result.error;
}

void vkr_renderer_resize(VkrRenderer *renderer, uint32_t width,
                         uint32_t height) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");

  VkrRenderer *rf = (VkrRenderer *)renderer;

  vkr_renderer_backend_resize(rf, width, height);
  rf->target_generation++;

  rf->last_window_width = width;
  rf->last_window_height = height;
  if (!rf->scene_output_extent_overridden) {
    if (rf->scene_output_width != width || rf->scene_output_height != height)
      vkr_dynamic_resolution_reset_feedback(&rf->dynamic_resolution_state);
    rf->scene_output_width = width;
    rf->scene_output_height = height;
  }
  rf->render_width =
      vkr_renderer_scaled_extent(rf->scene_output_width, rf->render_scale);
  rf->render_height =
      vkr_renderer_scaled_extent(rf->scene_output_height, rf->render_scale);

  /* Every resize path recreates or invalidates target state. A skipped frame
     may separate the stored fit from the next camera pose. */
  rf->timing_result.shadow_depth_range = (VkrShadowDepthRangeSample){0};
  rf->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
}

vkr_internal VkrRendererError vkr_renderer_configure_scene_output_extent(
    VkrRenderer *renderer, uint32_t width, uint32_t height,
    bool8_t overridden) {
  if (!renderer || width == 0u || height == 0u)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (renderer->frame_active)
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  if (renderer->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL &&
      renderer->upscale_mode != VKR_UPSCALE_MODE_FSR31)
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  if (renderer->scene_output_extent_overridden == overridden &&
      renderer->scene_output_width == width &&
      renderer->scene_output_height == height)
    return VKR_RENDERER_ERROR_NONE;

  if (renderer->scene_output_width != width ||
      renderer->scene_output_height != height)
    vkr_dynamic_resolution_reset_feedback(&renderer->dynamic_resolution_state);
  renderer->scene_output_width = width;
  renderer->scene_output_height = height;
  renderer->scene_output_extent_overridden = overridden;
  renderer->render_width =
      vkr_renderer_scaled_extent(width, renderer->render_scale);
  renderer->render_height =
      vkr_renderer_scaled_extent(height, renderer->render_scale);
  renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError vkr_renderer_set_scene_output_extent(VkrRenderer *renderer,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      bool8_t memory_relief) {
  vkr_renderer_join_render_thread(renderer);
#if defined(PLATFORM_APPLE)
  if (renderer && renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL &&
      memory_relief)
    vkr_metal_packet_renderer_request_memory_relief(renderer->metal_renderer);
#else
  (void)memory_relief;
#endif
  return vkr_renderer_configure_scene_output_extent(renderer, width, height,
                                                    true_v);
}

VkrRendererError
vkr_renderer_restore_scene_output_extent(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (!renderer->scene_output_extent_overridden)
    return VKR_RENDERER_ERROR_NONE;
  return vkr_renderer_configure_scene_output_extent(
      renderer, renderer->last_window_width, renderer->last_window_height,
      false_v);
}

void vkr_renderer_invalidate_temporal_history(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");
  ((VkrRenderer *)renderer)->temporal_reset_reasons |=
      VKR_TEMPORAL_RESET_EXPLICIT;
}

void vkr_renderer_set_temporal_enabled(VkrRenderer *renderer, bool8_t enabled) {
  assert_log(renderer != NULL, "Renderer is NULL");
  vkr_renderer_join_render_thread(renderer);
  renderer->temporal_enabled = enabled;
}

void vkr_renderer_render_scale_range(const VkrRenderer *renderer,
                                     float32_t *out_min, float32_t *out_max) {
  *out_min = renderer ? renderer->render_scale_min : 1.0f;
  *out_max = renderer ? renderer->render_scale_max : 1.0f;
}

VkrRendererError vkr_renderer_set_present_mode(VkrRenderer *renderer,
                                               VkrPresentMode mode) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer ||
      (mode != VKR_PRESENT_MODE_FIFO && mode != VKR_PRESENT_MODE_IMMEDIATE))
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (renderer->present_target.kind != VKR_PRESENT_TARGET_WINDOWED)
    return VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_set_present_mode(renderer->metal_renderer,
                                                    mode)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
#else
  return vkr_vulkan_renderer_set_present_mode(renderer->vulkan_renderer, mode)
             ? VKR_RENDERER_ERROR_NONE
             : VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
#endif
}

bool8_t
vkr_renderer_dynamic_resolution_switchable(const VkrRenderer *renderer) {
  return renderer && renderer->graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED;
}

VkrRendererError vkr_renderer_set_render_scale(VkrRenderer *renderer,
                                               float32_t render_scale,
                                               bool8_t dynamic_resolution) {
  vkr_renderer_join_render_thread(renderer);
  if (!renderer || !isfinite(render_scale) ||
      render_scale < renderer->render_scale_min - 1e-4f ||
      render_scale > renderer->render_scale_max + 1e-4f)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (renderer->frame_active)
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  if (dynamic_resolution &&
      !vkr_renderer_dynamic_resolution_switchable(renderer))
    return VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
  render_scale = vkr_clamp_f32(render_scale, renderer->render_scale_min,
                               renderer->render_scale_max);
  VkrDynamicResolutionConfig request = renderer->dynamic_resolution_request;
  /* The scale caps the controller; a cap at its floor fixes the scale. */
  const float32_t floor_scale = request.min_scale > 0.0f
                                    ? request.min_scale
                                    : VKR_DYNAMIC_RESOLUTION_DEFAULT_MIN_SCALE;
  request.enabled =
      dynamic_resolution &&
      render_scale > floor_scale + VKR_DYNAMIC_RESOLUTION_SCALE_STEP;
  request.max_scale = render_scale;
  VkrDynamicResolutionConfig config = {0};
  float32_t scale = render_scale;
  if (!vkr_dynamic_resolution_config_normalize(&request, render_scale, &config,
                                               &scale))
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (scale == renderer->render_scale &&
      config.enabled == renderer->dynamic_resolution_config.enabled &&
      config.max_scale == renderer->dynamic_resolution_config.max_scale)
    return VKR_RENDERER_ERROR_NONE;
  renderer->dynamic_resolution_config = config;
  vkr_dynamic_resolution_init(&renderer->dynamic_resolution_state, &config,
                              scale);
  renderer->render_scale = scale;
  renderer->temporal_reset_reasons |= VKR_TEMPORAL_RESET_EXPLICIT;
#if defined(PLATFORM_APPLE)
  if (renderer->backend_type == VKR_RENDERER_BACKEND_TYPE_METAL)
    vkr_metal_packet_renderer_set_dynamic_resolution(renderer->metal_renderer,
                                                     config.enabled);
#endif
  return VKR_RENDERER_ERROR_NONE;
}

void vkr_renderer_invalidate_exposure_history(VkrRenderer *renderer) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");
  ((VkrRenderer *)renderer)->exposure_reset_reasons |=
      VKR_TEMPORAL_RESET_EXPLICIT;
}

vkr_internal VkrRendererError
vkr_renderer_backend_cancel_frame(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  if (!vkr_metal_packet_renderer_cancel_frame(renderer->metal_renderer))
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  renderer->frame_active = false_v;
  return VKR_RENDERER_ERROR_NONE;
#else
  vkr_vulkan_renderer_cancel_frame(renderer->vulkan_renderer);
  renderer->frame_active = false_v;
  /* Cancellation may follow a rejected submission. Its allocation error is
     still available to that submit caller, but is not a cancellation failure.
   */
  const VkrRendererError error =
      vkr_vulkan_renderer_get_error(renderer->vulkan_renderer);
  return error == VKR_RENDERER_ERROR_DEVICE_ERROR ? error
                                                  : VKR_RENDERER_ERROR_NONE;
#endif
}

VkrRendererError vkr_renderer_cancel_frame(VkrFrame *frame) {
  vkr_renderer_join_render_thread(frame ? frame->renderer : NULL);
  if (!frame || !frame->renderer || !frame->renderer->frame_active ||
      frame->number != frame->renderer->frame_number)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  const VkrRendererError error =
      vkr_renderer_backend_cancel_frame(frame->renderer);
  frame->renderer = NULL;
  return error;
}

// =============================================================================
// Pixel Readback API (for picking and screenshots)
// =============================================================================

VkrRendererError
vkr_renderer_get_pixel_readback_result(VkrRenderer *renderer,
                                       VkrPixelReadbackResult *out_result) {
  vkr_renderer_join_render_thread(renderer);
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_result != NULL, "Output result is NULL");

#if defined(PLATFORM_APPLE)
  return vkr_metal_packet_renderer_get_pixel_readback_result(
      renderer->metal_renderer, out_result);
#else
  return vkr_vulkan_renderer_get_pixel_readback_result(
      renderer->vulkan_renderer, out_result);
#endif
}

VkrAllocator *vkr_renderer_get_backend_allocator(VkrRenderer *renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  VkrRenderer *rf = (VkrRenderer *)renderer;
  return vkr_renderer_backend_allocator(rf);
}

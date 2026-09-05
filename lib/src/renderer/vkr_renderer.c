#include "renderer/vkr_renderer.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/metal/vkr_metal_memory.h"
#include "renderer/metal/vkr_metal_packet_renderer.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/vkr_capture.h"
#include "renderer/vkr_dynamic_resolution.h"
#include "renderer/vkr_frame_input.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_prepared_frame.h"
#include "renderer/vkr_renderer_internal.h"
#include "renderer/vkr_renderer_metrics.h"
#include "renderer/vkr_rg_json.h"
#include "renderer/vulkan/vkr_vulkan_renderer.h"

#include <math.h>

vkr_internal bool8_t vkr_renderer_env_enabled(const char *name) {
  const char *value = name ? getenv(name) : NULL;
  return value && value[0] != '\0' && strcmp(value, "0") != 0 ? true_v
                                                              : false_v;
}

vkr_internal uint32_t vkr_renderer_scaled_extent(uint32_t extent,
                                                 float32_t render_scale) {
  return ClampBot((uint32_t)((float64_t)extent * (float64_t)render_scale + 0.5),
                  1u);
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
      .transmission_draw_count = source->transmission_draw_count,
      .blend_draw_count = source->blend_draw_count,
      .gpu_visible_count = source->gpu_visible_count,
      .gpu_overflow_count = source->gpu_overflow_count,
      .gpu_resolve_invalid_count = source->gpu_resolve_invalid_count,
      .gpu_occlusion_culled_count = source->gpu_occlusion_culled_count,
      .transmission_gpu_visible_count = source->transmission_gpu_visible_count,
      .transmission_gpu_overflow_count =
          source->transmission_gpu_overflow_count,
      .transmission_gpu_resolve_invalid_count =
          source->transmission_gpu_resolve_invalid_count,
      .transmission_gpu_occlusion_culled_count =
          source->transmission_gpu_occlusion_culled_count,
      .transmission_compact_overflow_count =
          source->transmission_compact_overflow_count,
      .hzb_history_valid = source->hzb_history_valid,
      .shadow_depth_range = source->shadow_depth_range,
      .has_gpu_draw_diagnostics = source->has_gpu_draw_diagnostics,
      .exposure = source->exposure,
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
    VkrRenderer *renderer, VkrWindow *window, uint32_t width, uint32_t height,
    VkrDeviceRequirements *device_requirements,
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
vkr_internal bool8_t vkr_renderer_backend_memory_metrics(
    VkrRenderer *renderer, VkrRendererImplMemoryMetrics *out_metrics);
vkr_internal void vkr_renderer_backend_resize(VkrRenderer *renderer,
                                              uint32_t width, uint32_t height);
vkr_internal VkrRendererError vkr_renderer_backend_present_target_recreate(
    VkrRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t image_count);
vkr_internal uint32_t
vkr_renderer_backend_frame_in_flight_index(VkrRenderer *renderer);
vkr_internal bool8_t vkr_renderer_backend_poll_submit_result(
    VkrRenderer *renderer, uint64_t after_submit_value,
    VkrRendererImplSubmitResult *out_result);
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
    VkrRenderer *renderer, VkrWindow *window, uint32_t width, uint32_t height,
    VkrDeviceRequirements *device_requirements,
    const VkrRendererBackendConfig *backend_config,
    VkrRendererError *out_error) {
#if defined(PLATFORM_APPLE)
  (void)device_requirements;
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
      .target_width = width,
      .target_height = height,
      .render_scale = renderer->render_scale,
      .upscale_mode = renderer->upscale_mode,
      .dynamic_resolution = renderer->dynamic_resolution_config,
      .metal_layer = window ? vkr_window_get_metal_layer(window) : NULL,
      .requested_present_mode = backend_config->requested_present_mode,
      .heap_size = placement_heap_size,
      .upload_ring_size = MB(768),
      .frame_slot_count = frame_slot_count,
      .capture_ring_capacity = capture_capacity,
      .capture_max_batch_bytes = capture_bytes,
      .synchronous_validation_readback =
          vkr_renderer_env_enabled("VKR_METAL_SYNCHRONOUS_VALIDATION"),
      .srgb_output = true_v,
      .tonemap_enabled = !vkr_renderer_env_enabled("VKR_TONEMAP_DISABLED"),
      .convert_vulkan_clip_y = true_v,
      .fxaa_enabled = !vkr_renderer_env_enabled("VKR_FXAA_DISABLED"),
      .transmission_compact_enabled =
          !vkr_renderer_env_enabled("VKR_TRANSMISSION_COMPACT_DISABLED"),
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
      .max_images = 128,
      .max_passes = VKR_RENDERER_IMPL_MAX_GRAPH_PASSES,
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
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Metal 4 packet renderer");
  return true_v;
#else
  (void)device_requirements;
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
      .max_graph_passes = VKR_RENDERER_IMPL_MAX_GRAPH_PASSES,
      .tonemap_enabled = !vkr_renderer_env_enabled("VKR_TONEMAP_DISABLED"),
      .fxaa_enabled = !vkr_renderer_env_enabled("VKR_FXAA_DISABLED"),
      .hzb_enabled = !vkr_renderer_env_enabled("VKR_HZB_DISABLED"),
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
                                          &renderer->asset_publisher);
  /* Stated explicitly rather than inherited from the default caps, so this
     backend's slot count and the caps it publishes cannot drift apart the way
     Metal's did. */
  renderer->impl.caps.frame_in_flight_count = VKR_VULKAN_FRAME_SLOT_COUNT;
  *out_error = VKR_RENDERER_ERROR_NONE;
  log_info("Selected Vulkan 1.4 packet renderer");
  return true_v;
#endif
}

bool32_t vkr_renderer_initialize(VkrRenderer *renderer,
                                 VkrRendererBackendType backend_type,
                                 VkrWindow *window,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  VkrPresentTargetConfig requested_target = backend_config
                                                ? backend_config->present_target
                                                : (VkrPresentTargetConfig){0};
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
  if (requested_upscale_mode == VKR_UPSCALE_MODE_METALFX_TEMPORAL &&
      backend_type != VKR_RENDERER_BACKEND_TYPE_METAL) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("MetalFX temporal upscaling requires the Metal backend");
    return false_v;
  }
  if (requested_dynamic_resolution.enabled &&
      requested_upscale_mode != VKR_UPSCALE_MODE_METALFX_TEMPORAL) {
    *out_error = VKR_RENDERER_ERROR_UNSUPPORTED_INPUT;
    log_error("Dynamic resolution requires MetalFX temporal upscaling");
    return false_v;
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
  } else if (!window) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    log_error("Windowed target requires a window");
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
  renderer->impl = selected_impl;

  // Initialize struct in-place
  renderer->backend_type = backend_type;
  renderer->window = window;
  renderer->present_target = requested_target;
  renderer->render_scale = requested_render_scale;
  renderer->upscale_mode = requested_upscale_mode;
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

  renderer->temporal_state = (VkrTemporalState){0};
  renderer->temporal_reset_reasons = 0u;
  renderer->temporal_enabled =
      requested_upscale_mode == VKR_UPSCALE_MODE_METALFX_TEMPORAL ||
      !vkr_renderer_env_enabled("VKR_TAA_DISABLED");
  renderer->exposure_state = (VkrExposureState){0};
  renderer->exposure_reset_reasons = 0u;
  /* Production enables bloom, and the environment override exists for the same
     reason `VKR_TAA_DISABLED` does: a matched capture of the same frame with
     and without it must not require a rebuild. */
  renderer->bloom_forced_disabled =
      vkr_renderer_env_enabled("VKR_BLOOM_DISABLED");
  renderer->gtao_forced_disabled =
      vkr_renderer_env_enabled("VKR_GTAO_DISABLED");
  renderer->frame_metrics = (VkrRendererFrameMetrics){0};
  renderer->boot_metrics = (VkrRendererBootMetrics){0};
  renderer->frame_number = 0;
  renderer->target_generation = 1u;

  /* The Vulkan implementation owns its persistent graph realization,

   * descriptor tables, and bounded pool metadata through this allocator in

   * addition to the frontend graph. Keep enough committed-growth headroom for

   * a new image-pool block to be admitted after the descriptor tables are

   * resident. */
  if (!vkr_dmemory_create(MB(2), MB(64), &renderer->render_graph_dmemory)) {
    log_error("Failed to create render graph allocator!");
    goto initialize_failure;
  }
  renderer->render_graph_allocator =
      (VkrAllocator){.ctx = &renderer->render_graph_dmemory};
  vkr_dmemory_allocator_create(&renderer->render_graph_allocator);

  VkrWindowPixelSize initial = requested_target.kind ==
                                       VKR_PRESENT_TARGET_OFFSCREEN
                                   ? (VkrWindowPixelSize){
                                         .width = requested_target.width,
                                         .height = requested_target.height,
                                     }
                                   : vkr_window_get_pixel_size(window);
  renderer->last_window_width = initial.width;
  renderer->last_window_height = initial.height;
  renderer->scene_output_width = initial.width;
  renderer->scene_output_height = initial.height;
  renderer->scene_output_extent_overridden = false_v;
  renderer->render_width = vkr_renderer_scaled_extent(
      renderer->scene_output_width, renderer->render_scale);
  renderer->render_height = vkr_renderer_scaled_extent(
      renderer->scene_output_height, renderer->render_scale);
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
  const VkrRendererBackendConfig *backend_cfg = &resolved_backend_config;
  if (!vkr_renderer_backend_initialize(renderer, window, width, height,
                                       device_requirements, backend_cfg,
                                       out_error)) {
    goto initialize_failure;
  }
  return true_v;

initialize_failure:
  if (renderer->metal_renderer || renderer->vulkan_renderer)
    vkr_renderer_backend_destroy(renderer);
  if (renderer->render_graph_allocator.ctx)
    vkr_dmemory_allocator_destroy(&renderer->render_graph_allocator);
  renderer->render_graph_allocator = (VkrAllocator){0};
  return false_v;
}

void vkr_renderer_destroy(VkrRenderer *renderer) {
  vkr_renderer_wait_idle(renderer);
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
  prepared->frame.input = *packet;
  const bool8_t scaled = rf->render_scale != 1.0f;
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
  const bool8_t indirect_diffuse_only =
      packet->globals.render_mode == VKR_RENDER_MODE_INDIRECT_DIFFUSE;
  rf->ibl_probes_packed =
      packet->lighting ? packet->lighting->ibl_probe_count : 0u;
  prepared->temporal_input = (VkrTemporalFrameInput){
      .view = packet->globals.view,
      .projection = packet->globals.projection,
      .view_position = packet->globals.view_position,
      .scene_generation = packet->frame.scene_generation,
      .frame_index = packet->frame.frame_index,
      .width = temporal_width,
      .height = temporal_height,
      .render_mode = packet->globals.render_mode,
      .explicit_reset_reasons = rf->temporal_reset_reasons,
      .enabled = rf->temporal_enabled && !indirect_diffuse_only,
  };
  prepared->frame.temporal =
      vkr_temporal_prepare(&rf->temporal_state, &prepared->temporal_input);

  /* Exposure reuses the discontinuities temporal already derived at this same
     boundary rather than re-deriving them from the same inputs. */
  prepared->exposure_input = (VkrExposureFrameInput){
      .mode = packet->globals.exposure_mode,
      .manual_exposure = packet->globals.manual_exposure,
      .compensation_ev = packet->globals.exposure_compensation_ev,
      .delta_time = packet->frame.delta_time,
      .temporal_reset_reasons = prepared->frame.temporal.reset_reasons,
      .explicit_reset_reasons = rf->exposure_reset_reasons,
  };
  prepared->frame.exposure =
      vkr_exposure_prepare(&rf->exposure_state, &prepared->exposure_input);

  /* Bloom carries no history, so it has no prepare/commit chain: the frame
     block is a pure function of the validated packet fields. */
  prepared->frame.bloom = vkr_bloom_prepare(
      packet->globals.bloom_enabled && !rf->bloom_forced_disabled &&
          !indirect_diffuse_only,
      packet->globals.bloom_threshold, packet->globals.bloom_knee,
      packet->globals.bloom_intensity);
  /* GTAO is current-frame spatial state: like bloom, it has no prepare/commit
     history chain. */
  prepared->frame.gtao = vkr_gtao_prepare(
      packet->globals.gtao_enabled && !rf->gtao_forced_disabled,
      packet->globals.gtao_radius, packet->globals.gtao_power);
}

vkr_internal void vkr_renderer_backend_get_device_information(
    VkrRenderer *renderer, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
#if defined(PLATFORM_APPLE)
  (void)temp_arena;
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
      .actual_render_width = renderer->render_width,
      .actual_render_height = renderer->render_height,
      .actual_color_format =
          renderer->present_target.kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
              : VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB,
      .actual_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT,
      .actual_color_space = VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR,
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
      .actual_render_width = renderer->last_window_width,
      .actual_render_height = renderer->last_window_height,
      .actual_color_format = color_format,
      .actual_depth_format = depth_format,
      .actual_color_space = color_space,
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
  const uint64_t placement_capacity = metrics.suballocations.heap_size;
  const uint64_t driver_budget = metrics.driver_recommended_working_set_size;
  const uint64_t effective_budget =
      vkr_metal_memory_effective_budget(placement_capacity, driver_budget);
  const uint64_t placement_usage =
      placement_capacity - metrics.suballocations.free_bytes;
  out_stats->live_allocation_count = metrics.native_heap_size > 0 ? 1u : 0u;
  out_stats->peak_allocation_count = out_stats->live_allocation_count;
  out_stats->total_allocation_count = out_stats->live_allocation_count;
  out_stats->max_allocation_count = 1u;
  out_stats->live_bytes = metrics.native_heap_allocated_size;
  out_stats->peak_bytes = metrics.native_heap_peak_allocated_size;
  out_stats->live_totals_exact = true_v;
  MemCopy(out_stats->owners, metrics.owners, sizeof(out_stats->owners));
  out_stats->memory_type_count = 1;
  out_stats->live_bytes_by_type[0] = out_stats->live_bytes;
  out_stats->live_count_by_type[0] = out_stats->live_allocation_count;
  out_stats->heap_index_by_type[0] = 0;
  out_stats->heap_count = 1;
  out_stats->heap_size_bytes[0] = placement_capacity;
  out_stats->heap_usage_bytes[0] = placement_usage;
  out_stats->heap_budget_bytes[0] = effective_budget;
  out_stats->pending_texture_upload_bytes =
      metrics.pending_texture_upload_bytes;
  out_stats->heap_usage_valid = effective_budget > 0u;
  return true_v;
#else
  if (!renderer || !renderer->vulkan_renderer || !out_stats)
    return false_v;
  vkr_vulkan_renderer_device_memory_stats(renderer->vulkan_renderer, out_stats);
  return true_v;
#endif
}

vkr_internal bool8_t vkr_renderer_backend_memory_metrics(
    VkrRenderer *renderer, VkrRendererImplMemoryMetrics *out_metrics) {
#if defined(PLATFORM_APPLE)
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
#endif
}

vkr_internal void vkr_renderer_backend_resize(VkrRenderer *renderer,
                                              uint32_t width, uint32_t height) {
#if defined(PLATFORM_APPLE)
  if (!renderer ||
      renderer->upscale_mode != VKR_UPSCALE_MODE_METALFX_TEMPORAL ||
      width == 0u || height == 0u)
    return;
  if (renderer->scene_output_extent_overridden)
    return;
  if (vkr_renderer_backend_wait_idle(renderer) != VKR_RENDERER_ERROR_NONE ||
      !vkr_metal_packet_renderer_resize_metalfx(renderer->metal_renderer, width,
                                                height))
    log_error("MetalFX temporal scaler resize failed for %ux%u", width, height);
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
  const uint32_t scene_width = renderer->scene_output_extent_overridden
                                   ? renderer->scene_output_width
                                   : width;
  const uint32_t scene_height = renderer->scene_output_extent_overridden
                                    ? renderer->scene_output_height
                                    : height;
  if (!vkr_metal_packet_renderer_resize_metalfx(renderer->metal_renderer,
                                                scene_width, scene_height))
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
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

vkr_internal uint32_t
vkr_renderer_backend_frame_in_flight_index(VkrRenderer *renderer) {
#if defined(PLATFORM_APPLE)
  /* Ask the backend which slot it acquired, the way the Vulkan path does.
     Deriving the index from the frame counter assumed the slot count in caps
     matched the one the Metal renderer actually built, and it did not. */
  return vkr_metal_packet_renderer_frame_slot(renderer->metal_renderer);
#else
  return vkr_vulkan_renderer_frame_slot(renderer->vulkan_renderer);
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

VkrWindow *vkr_renderer_get_window(VkrRenderer *renderer) {
  return renderer->window;
}

VkrRendererBackendType vkr_renderer_get_backend_type(VkrRenderer *renderer) {
  return renderer->backend_type;
}

void vkr_renderer_get_device_information(
    VkrRenderer *renderer, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  vkr_renderer_backend_get_device_information(renderer, device_information,
                                              temp_arena);
}

bool32_t vkr_renderer_is_frame_active(VkrRenderer *renderer) {
  return renderer->frame_active;
}

VkrRendererError vkr_renderer_wait_idle(VkrRenderer *renderer) {
  return vkr_renderer_backend_wait_idle(renderer);
}

bool8_t
vkr_renderer_gpu_submission_timing_poll(VkrRenderer *renderer,
                                        uint64_t after_submit_serial,
                                        VkrGpuSubmissionTiming *out_timing) {
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
  return vkr_renderer_backend_submit_serial(renderer);
}

uint64_t vkr_renderer_get_completed_submit_serial(VkrRenderer *renderer) {
  return vkr_renderer_backend_completed_submit_serial(renderer);
}

bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRenderer *renderer, VkrRendererUploadWaitStats *out_stats) {
  out_stats->fence_wait_count = 0;
  out_stats->queue_wait_idle_count = 0;
  out_stats->device_wait_idle_count = 0;
  out_stats->frame_upload_exhaustion_count = 0;
  return vkr_renderer_backend_upload_wait_stats(renderer, out_stats);
}

bool8_t
vkr_renderer_get_and_reset_command_slot_wait_count(VkrRenderer *renderer,
                                                   uint64_t *out_wait_count) {
  *out_wait_count = 0;
  return vkr_renderer_backend_command_slot_waits(renderer, out_wait_count);
}

bool8_t vkr_renderer_get_device_memory_stats(VkrRenderer *renderer,
                                             VkrDeviceMemoryStats *out_stats) {
  MemZero(out_stats, sizeof(*out_stats));
  return vkr_renderer_backend_device_memory_stats(renderer, out_stats);
}

uint32_t vkr_renderer_present_target_image_count(VkrRenderer *renderer) {
  return renderer->impl.caps.present_target_image_count;
}

VkrPresentTargetKind vkr_renderer_present_target_kind(VkrRenderer *renderer) {
  return renderer->impl.caps.present_target_kind;
}

void vkr_renderer_present_target_extent(VkrRenderer *renderer,
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
vkr_renderer_present_target_format(VkrRenderer *renderer,
                                   VkrPresentTargetAttachment attachment) {
  return attachment == VKR_PRESENT_TARGET_ATTACHMENT_COLOR
             ? renderer->impl.caps.present_color_format
             : renderer->impl.caps.present_depth_format;
}

VkrRendererError vkr_renderer_present_target_recreate(VkrRenderer *renderer,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t image_count) {
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
  return renderer->impl.caps.shadow_depth_format;
}

uint32_t vkr_renderer_frame_in_flight_index(VkrRenderer *renderer) {
  return vkr_renderer_backend_frame_in_flight_index(renderer);
}

uint32_t vkr_renderer_frame_in_flight_count(VkrRenderer *renderer) {
  return renderer->impl.caps.frame_in_flight_count;
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
  if (renderer->window) {
    VkrWindowPixelSize pixels = vkr_window_get_pixel_size(renderer->window);
    if (pixels.width == 0 || pixels.height == 0) {
      return VKR_RENDERER_ERROR_FRAME_SKIPPED;
    }
    if (renderer->last_window_width != pixels.width ||
        renderer->last_window_height != pixels.height)
      renderer->target_generation++;
    renderer->last_window_width = pixels.width;
    renderer->last_window_height = pixels.height;
    if (!renderer->scene_output_extent_overridden) {
      renderer->scene_output_width = pixels.width;
      renderer->scene_output_height = pixels.height;
    }
  }
  renderer->timing_completed_ready = vkr_renderer_backend_poll_submit_result(
      renderer, renderer->timing_last_completed_submit_value,
      &renderer->timing_result);
  if (renderer->timing_completed_ready) {
    renderer->timing_last_completed_submit_value =
        renderer->timing_result.submit_value;
    float32_t next_scale = renderer->render_scale;
    if (renderer->timing_result.gpu_submission_valid &&
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
      .metalfx_enabled =
          renderer->upscale_mode == VKR_UPSCALE_MODE_METALFX_TEMPORAL,
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
  return VKR_RENDERER_ERROR_NONE;
#else
  if (renderer->frame_active) {
    return VKR_RENDERER_ERROR_FRAME_IN_PROGRESS;
  }
  if (!vkr_vulkan_renderer_prepare_frame(
          renderer->vulkan_renderer, renderer->frame_number + 1u,
          config->shadow_map_size, config->shadow_cascade_count, out_setup)) {
    const VkrRendererError error =
        vkr_vulkan_renderer_get_error(renderer->vulkan_renderer);
    return error != VKR_RENDERER_ERROR_NONE
               ? error
               : VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED;
  }
  renderer->frame_active = true_v;
  renderer->frame_number++;
  renderer->last_window_width = out_setup->window_width;
  renderer->last_window_height = out_setup->window_height;
  renderer->timing_completed_ready = vkr_renderer_backend_poll_submit_result(
      renderer, renderer->timing_last_completed_submit_value,
      &renderer->timing_result);
  if (renderer->timing_completed_ready) {
    renderer->timing_last_completed_submit_value =
        renderer->timing_result.submit_value;
  }
  MemZero(&renderer->frame_metrics, sizeof(renderer->frame_metrics));
  return VKR_RENDERER_ERROR_NONE;
#endif
}

VkrRendererError vkr_renderer_begin_frame(VkrRenderer *renderer,
                                          const VkrFrameConfig *config,
                                          VkrFrame *out_frame) {
  if (!renderer || !out_frame || !config || config->shadow_map_size == 0u ||
      config->shadow_cascade_count == 0u ||
      config->shadow_cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX)
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
    return vkr_renderer_validation_fail(
        out_validation_error,
        committed ? VKR_RENDERER_ERROR_DEVICE_ERROR
                  : VKR_RENDERER_ERROR_SUBMISSION_FAILED,
        "metal",
        committed ? "Metal frame failed after queue submission"
                  : "Metal frame rendering failed");
  }
  vkr_temporal_commit(&renderer->temporal_state, &prepared.temporal_input);
  renderer->temporal_reset_reasons = 0u;
  vkr_exposure_commit(&renderer->exposure_state, &prepared.exposure_input);
  renderer->exposure_reset_reasons = 0u;
  VkrRendererImplSubmitResult current_result = {0};
  vkr_renderer_impl_lower_metal_result(&result, &current_result);
  current_result.source_frame_index = packet->frame.frame_index;
  if (!renderer->timing_completed_ready) {
    renderer->timing_result = current_result;
    renderer->timing_result.source_frame_index = packet->frame.frame_index;
  }
  const VkrRendererImplSubmitResult *observed = &renderer->timing_result;
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
  vkr_temporal_commit(&renderer->temporal_state, &prepared.temporal_input);
  renderer->temporal_reset_reasons = 0u;
  vkr_exposure_commit(&renderer->exposure_state, &prepared.exposure_input);
  renderer->exposure_reset_reasons = 0u;
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

VkrRendererError
vkr_renderer_render_frame(VkrFrame *frame, const VkrFrameInput *packet,
                          VkrRendererFrameMetrics *out_metrics,
                          VkrValidationError *out_validation_error) {
  if (!frame || !frame->renderer || !frame->renderer->frame_active ||
      frame->number != frame->renderer->frame_number)
    return vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER, "frame",
        "must identify the current acquired frame");
  VkrRenderer *renderer = frame->renderer;
  VkrRendererError error =
      vkr_frame_input_validate(packet, out_validation_error);
  if (error == VKR_RENDERER_ERROR_NONE &&
      (packet->frame.window_width != frame->window_width ||
       packet->frame.window_height != frame->window_height))
    error = vkr_renderer_validation_fail(
        out_validation_error, VKR_RENDERER_ERROR_INVALID_PARAMETER,
        "frame_input.frame", "target extent must match the acquired frame");
  if (error != VKR_RENDERER_ERROR_NONE) {
    const VkrRendererError cancel_error = vkr_renderer_cancel_frame(frame);
    if (cancel_error != VKR_RENDERER_ERROR_NONE)
      return vkr_renderer_validation_fail(
          out_validation_error, cancel_error, "frame",
          "failed to cancel the acquired frame after input rejection");
    return error;
  }
  error = vkr_renderer_backend_render_frame(renderer, packet, out_metrics,
                                            out_validation_error);
  frame->renderer = NULL;
  return error;
}

void vkr_renderer_resize(VkrRenderer *renderer, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  VkrRenderer *rf = (VkrRenderer *)renderer;

  vkr_renderer_backend_resize(rf, width, height);
  rf->target_generation++;

  if (rf->window) {
    rf->window->width = width;
    rf->window->height = height;
  }
  rf->last_window_width = width;
  rf->last_window_height = height;
  if (!rf->scene_output_extent_overridden) {
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
  if (renderer->backend_type != VKR_RENDERER_BACKEND_TYPE_METAL)
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
  if (renderer->scene_output_extent_overridden == overridden &&
      renderer->scene_output_width == width &&
      renderer->scene_output_height == height)
    return VKR_RENDERER_ERROR_NONE;

  if (renderer->upscale_mode == VKR_UPSCALE_MODE_METALFX_TEMPORAL &&
      (renderer->scene_output_width != width ||
       renderer->scene_output_height != height)) {
#if defined(PLATFORM_APPLE)
    const VkrRendererError idle = vkr_renderer_wait_idle(renderer);
    if (idle != VKR_RENDERER_ERROR_NONE)
      return idle;
    if (!vkr_metal_packet_renderer_resize_metalfx(renderer->metal_renderer,
                                                  width, height))
      return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
#else
    return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
#endif
  }

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
                                                      uint32_t height) {
  return vkr_renderer_configure_scene_output_extent(renderer, width, height,
                                                    true_v);
}

VkrRendererError
vkr_renderer_restore_scene_output_extent(VkrRenderer *renderer) {
  if (!renderer)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (!renderer->scene_output_extent_overridden)
    return VKR_RENDERER_ERROR_NONE;
  return vkr_renderer_configure_scene_output_extent(
      renderer, renderer->last_window_width, renderer->last_window_height,
      false_v);
}

void vkr_renderer_invalidate_temporal_history(VkrRenderer *renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  ((VkrRenderer *)renderer)->temporal_reset_reasons |=
      VKR_TEMPORAL_RESET_EXPLICIT;
}

void vkr_renderer_invalidate_exposure_history(VkrRenderer *renderer) {
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
  return vkr_vulkan_renderer_get_error(renderer->vulkan_renderer);
#endif
}

VkrRendererError vkr_renderer_cancel_frame(VkrFrame *frame) {
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
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_result != NULL, "Output result is NULL");

  VkrRenderer *rf = (VkrRenderer *)renderer;
  if (rf->impl.kind == VKR_RENDERER_IMPL_VULKAN) {
    return vkr_vulkan_renderer_get_pixel_readback_result(rf->vulkan_renderer,
                                                         out_result);
  }
  return VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
}

VkrAllocator *vkr_renderer_get_backend_allocator(VkrRenderer *renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  VkrRenderer *rf = (VkrRenderer *)renderer;
  return vkr_renderer_backend_allocator(rf);
}

#include "renderer/vkr_render_graph_internal.h"

#include "core/logger.h"
#include "platform/vkr_platform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_render_packet.h"
#include "renderer/vkr_renderer.h"

vkr_internal void vkr_rg_prepare_pass_timings(VkrRenderGraph *graph) {
  assert_log(graph != NULL, "graph is NULL");

  vector_clear_VkrRgPassTiming(&graph->pass_timings);
  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    VkrRgPassTiming timing = {0};
    if (pass) {
      timing.name = pass->desc.name;
      timing.culled = pass->culled;
      timing.disabled = (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) != 0;
    }
    vector_push_VkrRgPassTiming(&graph->pass_timings, timing);
  }
}

vkr_internal void vkr_rg_apply_gpu_timings(VkrRenderGraph *graph,
                                           RendererFrontend *rf) {
  assert_log(graph != NULL, "graph is NULL");
  assert_log(rf != NULL, "rf is NULL");

  uint32_t pass_count = 0;
  const float64_t *pass_ms = NULL;
  const bool8_t *pass_valid = NULL;
  uint64_t source_frame_index = 0;
  uint64_t source_submit_serial = 0;
  if (!vkr_renderer_rg_timing_get_results(rf, &pass_count, &pass_ms,
                                          &pass_valid, &source_frame_index,
                                          &source_submit_serial)) {
    return;
  }

  if (!pass_ms || pass_count == 0) {
    return;
  }

  uint32_t max_count = (uint32_t)graph->pass_timings.length;
  uint32_t copy_count = pass_count < max_count ? pass_count : max_count;
  for (uint32_t i = 0; i < copy_count; ++i) {
    VkrRgPassTiming *timing =
        vector_get_VkrRgPassTiming(&graph->pass_timings, i);
    timing->gpu_ms = pass_ms[i];
    timing->gpu_valid = pass_valid ? pass_valid[i] : true_v;
    timing->gpu_source_frame_index = source_frame_index;
    timing->gpu_source_submit_serial = source_submit_serial;
  }
}

vkr_internal VkrRendererError vkr_rg_apply_image_barriers(
    VkrRenderGraph *graph, RendererFrontend *rf, const VkrRgPass *pass) {
  assert_log(graph != NULL, "graph is NULL");
  assert_log(rf != NULL, "rf is NULL");
  assert_log(pass != NULL, "pass is NULL");

  uint32_t image_index = graph->frame_info.image_index;
  for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
    VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, barrier->image);
    if (!image) {
      log_error("RenderGraph pass '%.*s' references an invalid image barrier",
                (int)pass->desc.name.length, pass->desc.name.str);
      return VKR_RENDERER_ERROR_INVALID_HANDLE;
    }

    VkrTextureOpaqueHandle tex = vkr_rg_pick_image_texture(image, image_index);
    if (!tex) {
      log_error("RenderGraph pass '%.*s' has no texture for image '%.*s' at "
                "swapchain image %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (int)image->name.length, image->name.str, image_index);
      return VKR_RENDERER_ERROR_INVALID_HANDLE;
    }

    VkrRendererError err = vkr_renderer_image_barrier(
        rf, tex, barrier->src_access, barrier->dst_access, barrier->src_layout,
        barrier->dst_layout, &barrier->range);
    if (err != VKR_RENDERER_ERROR_NONE) {
      // A dropped barrier means the next pass reads a resource in the wrong
      // layout or races a still-running write. There is no "continue anyway"
      // that is ever right here.
      String8 err_str = vkr_renderer_get_error_string(err);
      log_error("RenderGraph barrier failed for '%.*s': %s",
                (int)image->name.length, image->name.str,
                string8_cstr(&err_str));
      return err;
    }
  }
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkrRendererError vkr_rg_apply_buffer_barriers(
    VkrRenderGraph *graph, RendererFrontend *rf, const VkrRgPass *pass) {
  assert_log(graph != NULL, "graph is NULL");
  assert_log(rf != NULL, "rf is NULL");
  assert_log(pass != NULL, "pass is NULL");

  for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
    VkrRgBufferBarrier *barrier =
        vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, barrier->buffer);
    if (!buffer) {
      log_error("RenderGraph pass '%.*s' references an invalid buffer barrier",
                (int)pass->desc.name.length, pass->desc.name.str);
      return VKR_RENDERER_ERROR_INVALID_HANDLE;
    }

    VkrBufferHandle handle =
        vkr_rg_pick_buffer_handle(buffer, graph->frame_info.image_index);
    if (!handle) {
      log_error("RenderGraph pass '%.*s' has no handle for buffer '%.*s' at "
                "swapchain image %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (int)buffer->name.length, buffer->name.str,
                graph->frame_info.image_index);
      return VKR_RENDERER_ERROR_INVALID_HANDLE;
    }

    VkrRendererError err = vkr_renderer_buffer_barrier(
        rf, handle, barrier->src_access, barrier->dst_access);
    if (err != VKR_RENDERER_ERROR_NONE) {
      String8 err_str = vkr_renderer_get_error_string(err);
      log_error("RenderGraph buffer barrier failed for '%.*s': %s",
                (int)buffer->name.length, buffer->name.str,
                string8_cstr(&err_str));
      return err;
    }
  }
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError vkr_rg_execute(VkrRenderGraph *graph,
                                struct s_RendererFrontend *rf) {
  if (!graph) {
    log_error("RenderGraph execute failed: graph is NULL");
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  graph->renderer = rf;

  if (!graph->compiled) {
    if (!vkr_rg_compile(graph)) {
      log_error("RenderGraph execute failed: compile failed");
      graph->packet = NULL;
      return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    }
  }

  const VkrGpuDebugPayload *debug =
      (graph->packet && graph->packet->debug) ? graph->packet->debug : NULL;
  bool8_t capture_gpu_timings =
      debug && debug->enable_timing && debug->capture_pass_timestamps;
  bool8_t gpu_timing_requested = debug && debug->enable_timing;

  vkr_rg_prepare_pass_timings(graph);
  if (rf && capture_gpu_timings) {
    vkr_rg_apply_gpu_timings(graph, rf);
  }
  bool8_t gpu_timing_active =
      gpu_timing_requested && (rf != NULL) &&
      vkr_renderer_rg_timing_begin_frame(rf, (uint32_t)graph->passes.length,
                                         graph->frame_info.frame_index);

  VkrRendererError result = VKR_RENDERER_ERROR_NONE;
  uint32_t failed_pass_index = 0;
  bool8_t timing_pass_open = false_v;

  for (uint64_t order_index = 0; order_index < graph->execution_order.length;
       ++order_index) {
    uint32_t pass_index = graph->execution_order.data[order_index];
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);
    if (!pass) {
      continue;
    }
    VkrRgPassTiming *timing = NULL;
    if (pass_index < graph->pass_timings.length) {
      timing = vector_get_VkrRgPassTiming(&graph->pass_timings, pass_index);
    }

    if (pass->culled || (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED)) {
      continue;
    }

    float64_t start_time = vkr_platform_get_absolute_time();
    if (gpu_timing_active) {
      vkr_renderer_rg_timing_begin_pass(rf, pass_index);
      timing_pass_open = true_v;
    }
    failed_pass_index = pass_index;

    if (rf) {
      result = vkr_rg_apply_image_barriers(graph, (RendererFrontend *)rf, pass);
      if (result != VKR_RENDERER_ERROR_NONE) {
        goto execute_failed;
      }
      result =
          vkr_rg_apply_buffer_barriers(graph, (RendererFrontend *)rf, pass);
      if (result != VKR_RENDERER_ERROR_NONE) {
        goto execute_failed;
      }
    }

    VkrRenderTargetHandle target = NULL;
    if (pass->render_targets && pass->render_target_count > 0) {
      // A pass whose attachments are all single-buffered builds one target, not
      // one per swapchain image, so indexing by image_index would leave every
      // frame that is not on image 0 with no target at all. Mirror
      // vkr_rg_pick_image_texture's convention: one entry serves every image.
      // Ordering is still safe -- the single target is written and consumed
      // within one frame's command buffer, and submissions execute in order.
      const uint32_t target_index =
          pass->render_target_count == 1u ? 0u : graph->frame_info.image_index;
      if (target_index < pass->render_target_count) {
        target = pass->render_targets[target_index];
      }
    }

    VkrRgPassContext ctx = {
        .graph = graph,
        .pass_desc = &pass->desc,
        .pass_index = pass_index,
        .renderer = rf,
        .renderpass = pass->renderpass,
        .render_target = target,
        .render_targets = pass->render_targets,
        .render_target_count = pass->render_target_count,
        .frame_index = graph->frame_info.frame_index,
        .image_index = graph->frame_info.image_index,
        .delta_time = graph->frame_info.delta_time,
        .error = VKR_RENDERER_ERROR_NONE,
    };

    if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS && rf) {
      if (!pass->renderpass || !target) {
        log_error("RenderGraph graphics pass '%.*s' is missing its %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  !pass->renderpass ? "render pass" : "render target");
        result = VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
        goto execute_failed;
      }
      result = vkr_renderer_begin_render_pass(rf, pass->renderpass, target);
      if (result != VKR_RENDERER_ERROR_NONE) {
        // Nothing was begun, so nothing must be ended.
        log_error("RenderGraph failed to begin render pass '%.*s'",
                  (int)pass->desc.name.length, pass->desc.name.str);
        goto execute_failed;
      }
      if (pass->desc.execute) {
        pass->desc.execute(&ctx, pass->desc.user_data);
      }
      // End the pass even when the executor failed: the render pass is open and
      // leaving it open would corrupt every later recording.
      VkrRendererError end_err = vkr_renderer_end_render_pass(rf);
      result = (ctx.error != VKR_RENDERER_ERROR_NONE) ? ctx.error : end_err;
      if (result != VKR_RENDERER_ERROR_NONE) {
        log_error("RenderGraph pass '%.*s' failed", (int)pass->desc.name.length,
                  pass->desc.name.str);
        goto execute_failed;
      }
    } else {
      if (pass->desc.execute) {
        pass->desc.execute(&ctx, pass->desc.user_data);
      }
      if (ctx.error != VKR_RENDERER_ERROR_NONE) {
        result = ctx.error;
        log_error("RenderGraph pass '%.*s' failed", (int)pass->desc.name.length,
                  pass->desc.name.str);
        goto execute_failed;
      }
    }

    if (timing) {
      float64_t end_time = vkr_platform_get_absolute_time();
      timing->cpu_ms = (end_time - start_time) * 1000.0;
    }
    if (gpu_timing_active) {
      vkr_renderer_rg_timing_end_pass(rf, pass_index);
      timing_pass_open = false_v;
    }
  }

  vkr_rg_end_frame(graph);
  return VKR_RENDERER_ERROR_NONE;

execute_failed:
  // Close the timestamp query that was opened for the failing pass, or the
  // query pool is left half-written and the next frame's results are garbage.
  if (gpu_timing_active && timing_pass_open) {
    vkr_renderer_rg_timing_end_pass(rf, failed_pass_index);
  }
  vkr_rg_end_frame(graph);
  return result;
}

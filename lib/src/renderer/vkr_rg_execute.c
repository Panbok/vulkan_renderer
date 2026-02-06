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
  if (!vkr_renderer_rg_timing_get_results(rf, &pass_count, &pass_ms,
                                          &pass_valid)) {
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
  }
}

vkr_internal void vkr_rg_apply_image_barriers(VkrRenderGraph *graph,
                                              RendererFrontend *rf,
                                              const VkrRgPass *pass) {
  assert_log(graph != NULL, "graph is NULL");
  assert_log(rf != NULL, "rf is NULL");
  assert_log(pass != NULL, "pass is NULL");

  uint32_t image_index = graph->frame_info.image_index;
  for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
    VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, barrier->image);
    if (!image) {
      continue;
    }

    VkrTextureOpaqueHandle tex = vkr_rg_pick_image_texture(image, image_index);
    if (!tex || barrier->src_layout == barrier->dst_layout) {
      continue;
    }

    VkrRendererError err = vkr_renderer_transition_texture_layout(
        rf, tex, barrier->src_layout, barrier->dst_layout);
    if (err != VKR_RENDERER_ERROR_NONE) {
      String8 err_str = vkr_renderer_get_error_string(err);
      log_warn("RenderGraph barrier failed for '%.*s': %s",
               (int)image->name.length, image->name.str,
               string8_cstr(&err_str));
    }
  }
}

vkr_internal void vkr_rg_apply_buffer_barriers(VkrRenderGraph *graph,
                                               RendererFrontend *rf,
                                               const VkrRgPass *pass) {
  assert_log(graph != NULL, "graph is NULL");
  assert_log(rf != NULL, "rf is NULL");
  assert_log(pass != NULL, "pass is NULL");

  for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
    VkrRgBufferBarrier *barrier =
        vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, barrier->buffer);
    if (!buffer) {
      continue;
    }

    VkrBufferHandle handle =
        vkr_rg_pick_buffer_handle(buffer, graph->frame_info.image_index);
    if (!handle || barrier->src_access == barrier->dst_access) {
      continue;
    }

    VkrRendererError err = vkr_renderer_buffer_barrier(
        rf, handle, barrier->src_access, barrier->dst_access);
    if (err != VKR_RENDERER_ERROR_NONE) {
      String8 err_str = vkr_renderer_get_error_string(err);
      log_warn("RenderGraph buffer barrier failed for '%.*s': %s",
               (int)buffer->name.length, buffer->name.str,
               string8_cstr(&err_str));
    }
  }
}

void vkr_rg_execute(VkrRenderGraph *graph, struct s_RendererFrontend *rf) {
  if (!graph) {
    log_error("RenderGraph execute failed: graph is NULL");
    return;
  }

  graph->renderer = rf;

  if (!graph->compiled) {
    if (!vkr_rg_compile(graph)) {
      log_error("RenderGraph execute failed: compile failed");
      return;
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
      vkr_renderer_rg_timing_begin_frame(rf, (uint32_t)graph->passes.length);

  for (uint64_t order_index = 0; order_index < graph->execution_order.length;
       ++order_index) {
    uint32_t pass_index = graph->execution_order.data[order_index];
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);
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
    }

    vkr_rg_apply_image_barriers(graph, (RendererFrontend *)rf, pass);
    vkr_rg_apply_buffer_barriers(graph, (RendererFrontend *)rf, pass);

    VkrRenderTargetHandle target = NULL;
    if (pass->render_targets &&
        graph->frame_info.image_index < pass->render_target_count) {
      target = pass->render_targets[graph->frame_info.image_index];
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
    };

    if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS && rf &&
        pass->renderpass && target) {
      vkr_renderer_begin_render_pass(rf, pass->renderpass, target);
      if (pass->desc.execute) {
        pass->desc.execute(&ctx, pass->desc.user_data);
      }
      vkr_renderer_end_render_pass(rf);
    } else {
      if (pass->desc.execute) {
        pass->desc.execute(&ctx, pass->desc.user_data);
      }
    }

    if (timing) {
      float64_t end_time = vkr_platform_get_absolute_time();
      timing->cpu_ms = (end_time - start_time) * 1000.0;
    }
    if (gpu_timing_active) {
      vkr_renderer_rg_timing_end_pass(rf, pass_index);
    }
  }

  graph->packet = NULL;
}

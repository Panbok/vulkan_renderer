#include "renderer/systems/vkr_view_system.h"

#include "core/logger.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_pipeline_registry.h"

typedef struct VkrLayerSortEntry {
  VkrLayer *layer;
  int32_t order; // Must be signed to handle negative order values
  uint32_t index;
} VkrLayerSortEntry;

vkr_internal void vkr_view_system_rebuild_sorted(VkrViewSystem *vs);

vkr_internal uint32_t vkr_view_system_layer_width(const VkrViewSystem *vs,
                                                  const VkrLayer *layer) {
  return layer->width ? layer->width : vs->window_width;
}

vkr_internal uint32_t vkr_view_system_layer_height(const VkrViewSystem *vs,
                                                   const VkrLayer *layer) {
  return layer->height ? layer->height : vs->window_height;
}

vkr_internal VkrLayerContext vkr_view_system_make_context(VkrViewSystem *vs,
                                                          VkrLayer *layer,
                                                          VkrLayerPass *pass) {
  VkrLayerContext ctx = {
      .view_system = vs,
      .layer = layer,
      .pass = pass,
  };
  return ctx;
}

vkr_internal VkrLayer *vkr_view_system_get_layer(VkrViewSystem *vs,
                                                 VkrLayerHandle handle) {
  if (!vs || !vs->initialized || handle.id == 0) {
    return NULL;
  }

  if (handle.id - 1 >= vs->layers.length) {
    return NULL;
  }

  VkrLayer *layer = array_get_VkrLayer(&vs->layers, handle.id - 1);
  if (!layer->active) {
    return NULL;
  }

  if (layer->handle.generation != handle.generation) {
    return NULL;
  }

  return layer;
}

vkr_internal void vkr_view_system_destroy_pass_targets(RendererFrontend *rf,
                                                       VkrLayerPass *pass) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!pass) {
    return;
  }

  if (!pass->render_targets || pass->render_target_count == 0) {
    pass->render_target_count = 0;
    pass->render_targets = NULL;
    return;
  }

  for (uint32_t i = 0; i < pass->render_target_count; ++i) {
    if (pass->render_targets[i]) {
      vkr_renderer_render_target_destroy(rf, pass->render_targets[i], false_v);
    }
  }

  pass->render_target_count = 0;
  pass->render_targets = NULL;
}

vkr_internal void vkr_view_system_destroy_layer(RendererFrontend *rf,
                                                VkrLayer *layer) {
  if (!layer) {
    return;
  }

  // Invoke detach callback before tearing down resources
  if (layer->active && layer->callbacks.on_detach) {
    VkrLayerContext ctx =
        vkr_view_system_make_context(&rf->view_system, layer, NULL);
    layer->callbacks.on_detach(&ctx);
  }

  if (layer->active && layer->callbacks.on_destroy) {
    VkrLayerContext ctx =
        vkr_view_system_make_context(&rf->view_system, layer, NULL);
    layer->callbacks.on_destroy(&ctx);
  }

  // Destroy render targets per pass
  for (uint32_t i = 0; i < layer->pass_count; ++i) {
    VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, i);
    vkr_view_system_destroy_pass_targets(rf, pass);
  }

  if (!array_is_null_VkrLayerPass(&layer->passes)) {
    array_destroy_VkrLayerPass(&layer->passes);
  }

  uint32_t old_generation = layer->handle.generation;
  MemZero(layer, sizeof(*layer));
  layer->handle.generation = old_generation + 1;
}

vkr_internal void vkr_view_system_copy_passes(VkrViewSystem *vs,
                                              VkrLayer *layer,
                                              const VkrLayerConfig *cfg) {
  layer->passes = array_create_VkrLayerPass(vs->arena, layer->pass_count);
  MemZero(layer->passes.data,
          sizeof(VkrLayerPass) * (uint64_t)layer->pass_count);

  for (uint32_t i = 0; i < layer->pass_count; ++i) {
    VkrLayerPass *dst = array_get_VkrLayerPass(&layer->passes, i);
    const VkrLayerPassConfig *src = &cfg->passes[i];
    dst->renderpass_name = string8_duplicate(vs->arena, &src->renderpass_name);
    dst->use_depth = src->use_depth;
    dst->use_swapchain_color = src->use_swapchain_color;
  }
}

vkr_internal int vkr_view_system_layer_compare(const void *a, const void *b) {
  const VkrLayerSortEntry *ea = (const VkrLayerSortEntry *)a;
  const VkrLayerSortEntry *eb = (const VkrLayerSortEntry *)b;
  if (ea->order < eb->order)
    return -1;
  if (ea->order > eb->order)
    return 1;
  if (ea->index < eb->index)
    return -1;
  if (ea->index > eb->index)
    return 1;
  return 0;
}

vkr_internal void vkr_view_system_rebuild_sorted(VkrViewSystem *vs) {
  if (!vs || !vs->initialized) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)vs->renderer;

  if (!vs->sorted_indices) {
    vs->sorted_indices = vkr_allocator_alloc(
        &vs->allocator, sizeof(uint32_t) * (uint64_t)vs->layer_capacity,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!vs->sorted_indices) {
    log_error("Failed to allocate sorted index array for view system");
    return;
  }

  Scratch scratch = {0};
  VkrLayerSortEntry *entries = NULL;
  if (rf && rf->scratch_arena) {
    scratch = scratch_create(rf->scratch_arena);
    entries = (VkrLayerSortEntry *)vkr_allocator_alloc(
        &vs->allocator, sizeof(VkrLayerSortEntry) * vs->layers.length,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!entries) {
    log_error("Failed to allocate scratch entries for view sorting");
    if (scratch.arena) {
      vkr_allocator_free(&vs->allocator, entries,
                         sizeof(VkrLayerSortEntry) * vs->layers.length,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    }
    return;
  }

  uint32_t sorted_count = 0;
  for (uint32_t i = 0; i < vs->layers.length; ++i) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, i);
    if (!layer->active) {
      continue;
    }
    entries[sorted_count++] =
        (VkrLayerSortEntry){.layer = layer, .order = layer->order, .index = i};
  }

  qsort(entries, sorted_count, sizeof(VkrLayerSortEntry),
        vkr_view_system_layer_compare);

  for (uint32_t i = 0; i < sorted_count; ++i) {
    vs->sorted_indices[i] = entries[i].index;
  }
  vs->sorted_count = sorted_count;
  vs->order_dirty = false_v;

  vkr_allocator_free(&vs->allocator, entries,
                     sizeof(VkrLayerSortEntry) * vs->layers.length,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
}

bool32_t vkr_view_system_init(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  MemZero(vs, sizeof(*vs));

  vs->arena = arena_create(MB(3));
  if (!vs->arena) {
    log_fatal("Failed to create view system arena");
    return false_v;
  }

  vs->allocator = (VkrAllocator){.ctx = vs->arena};
  vkr_allocator_arena(&vs->allocator);

  vs->renderer = renderer;
  vs->layer_capacity = VKR_VIEW_SYSTEM_MAX_LAYERS;
  vs->window_width = rf->last_window_width;
  vs->window_height = rf->last_window_height;
  vs->render_target_count = vkr_renderer_window_attachment_count(renderer);
  vs->sorted_indices = NULL;
  vs->sorted_count = 0;
  vs->order_dirty = true_v;

  vs->layers = array_create_VkrLayer(vs->arena, vs->layer_capacity);
  MemZero(vs->layers.data, sizeof(VkrLayer) * vs->layers.length);

  vs->initialized = true_v;
  return true_v;
}

void vkr_view_system_shutdown(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  for (uint32_t i = 0; i < vs->layers.length; ++i) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, i);
    if (!layer->active) {
      continue;
    }
    vkr_view_system_destroy_layer(rf, layer);
  }

  if (!array_is_null_VkrLayer(&vs->layers)) {
    array_destroy_VkrLayer(&vs->layers);
  }
  vs->sorted_indices = NULL;
  vs->sorted_count = 0;

  arena_destroy(vs->arena);
  MemZero(vs, sizeof(*vs));
}

bool32_t vkr_view_system_register_layer(VkrRendererFrontendHandle renderer,
                                        const VkrLayerConfig *cfg,
                                        VkrLayerHandle *out_handle,
                                        VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(cfg != NULL, "Layer config is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;

  if (!vs->initialized) {
    log_error("View system not initialized");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (cfg->pass_count == 0 ||
      cfg->pass_count > VKR_VIEW_SYSTEM_MAX_LAYER_PASSES) {
    log_error("Invalid pass count %u for layer %s", cfg->pass_count,
              cfg->name.str ? (const char *)cfg->name.str : "<unnamed>");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (!cfg->passes) {
    log_error("Layer %s has no pass configurations",
              cfg->name.str ? (const char *)cfg->name.str : "<unnamed>");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Locate a free slot
  VkrLayer *slot = NULL;
  uint32_t slot_index = 0;
  for (uint32_t i = 0; i < vs->layers.length; ++i) {
    VkrLayer *candidate = array_get_VkrLayer(&vs->layers, i);
    if (!candidate->active) {
      slot = candidate;
      slot_index = i;
      break;
    }
  }

  if (!slot) {
    log_error("View system layer capacity reached (%u)", vs->layer_capacity);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  uint32_t old_generation = slot->handle.generation;
  MemZero(slot, sizeof(*slot));

  slot->handle.id = slot_index + 1;
  slot->handle.generation = old_generation + 1;
  slot->active = true_v;
  slot->order = cfg->order;
  slot->sync_to_window = (cfg->width == 0 || cfg->height == 0);
  slot->width = cfg->width ? cfg->width : vs->window_width;
  slot->height = cfg->height ? cfg->height : vs->window_height;
  slot->view = cfg->view;
  slot->projection = cfg->projection;
  slot->callbacks = cfg->callbacks;
  slot->user_data = cfg->user_data;
  slot->pass_count = cfg->pass_count;
  slot->name = string8_duplicate(vs->arena, &cfg->name);

  vkr_view_system_copy_passes(vs, slot, cfg);

  // Invoke create callback before attachment/target build
  if (slot->callbacks.on_create) {
    VkrLayerContext ctx = vkr_view_system_make_context(vs, slot, NULL);
    if (!slot->callbacks.on_create(&ctx)) {
      log_error("Layer %s on_create failed",
                slot->name.str ? (const char *)slot->name.str : "<unnamed>");
      vkr_view_system_destroy_layer(rf, slot);
      slot->active = false_v;
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return false_v;
    }
  }

  *out_handle = slot->handle;
  *out_error = VKR_RENDERER_ERROR_NONE;

  // Invoke attach callback
  if (slot->callbacks.on_attach) {
    VkrLayerContext ctx = vkr_view_system_make_context(vs, slot, NULL);
    slot->callbacks.on_attach(&ctx);
  }

  // Build render targets for the new layer
  vkr_view_system_rebuild_targets(renderer);

  vs->order_dirty = true_v;
  return true_v;
}

void vkr_view_system_unregister_layer(VkrRendererFrontendHandle renderer,
                                      VkrLayerHandle handle) {
  assert_log(renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, handle);
  if (!layer) {
    return;
  }

  vkr_view_system_destroy_layer(rf, layer);
  vs->order_dirty = true_v;
}

bool32_t vkr_view_system_set_layer_camera(VkrRendererFrontendHandle renderer,
                                          VkrLayerHandle handle,
                                          const Mat4 *view,
                                          const Mat4 *projection) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return false_v;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, handle);
  if (!layer) {
    return false_v;
  }

  if (view) {
    layer->view = *view;
  }
  if (projection) {
    layer->projection = *projection;
  }

  vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);
  return true_v;
}

void vkr_view_system_on_resize(VkrRendererFrontendHandle renderer,
                               uint32_t width, uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  vs->window_width = width;
  vs->window_height = height;

  for (uint32_t i = 0; i < vs->layers.length; ++i) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, i);
    if (!layer->active) {
      continue;
    }

    if (layer->sync_to_window) {
      layer->width = width;
      layer->height = height;
    }

    if (layer->callbacks.on_resize) {
      VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);
      layer->callbacks.on_resize(&ctx, layer->width, layer->height);
    }
  }
}

void vkr_view_system_rebuild_targets(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  if (rf->rf_mutex) {
    vkr_mutex_lock(rf->rf_mutex);
  }

  uint32_t count = vkr_renderer_window_attachment_count(renderer);
  if (count == 0) {
    if (rf->rf_mutex) {
      vkr_mutex_unlock(rf->rf_mutex);
    }
    return;
  }

  VkrTextureOpaqueHandle depth = vkr_renderer_depth_attachment_get(renderer);

  for (uint32_t layer_index = 0; layer_index < vs->layers.length;
       ++layer_index) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, layer_index);
    if (!layer->active) {
      continue;
    }

    for (uint32_t pass_index = 0; pass_index < layer->pass_count;
         ++pass_index) {
      VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, pass_index);
      vkr_view_system_destroy_pass_targets(rf, pass);

      pass->renderpass =
          vkr_renderer_renderpass_get(renderer, pass->renderpass_name);
      if (!pass->renderpass) {
        log_error(
            "Renderpass %s unavailable for layer %s",
            pass->renderpass_name.str ? (const char *)pass->renderpass_name.str
                                      : "<unnamed>",
            layer->name.str ? (const char *)layer->name.str : "<unnamed>");
        continue;
      }

      if (!pass->render_targets || pass->render_target_count < count) {
        pass->render_targets = (VkrRenderTargetHandle *)vkr_allocator_alloc(
            &vs->allocator, sizeof(VkrRenderTargetHandle) * count,
            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (!pass->render_targets) {
        log_error("Failed to allocate render target array for layer %s",
                  layer->name.str ? (const char *)layer->name.str
                                  : "<unnamed>");
        pass->render_target_count = 0;
        continue;
      }
      MemZero(pass->render_targets,
              sizeof(VkrRenderTargetHandle) * (uint64_t)count);
      pass->render_target_count = count;

      for (uint32_t image_index = 0; image_index < count; ++image_index) {
        VkrTextureOpaqueHandle attachments[2] = {0};
        uint8_t attachment_count = 0;

        if (pass->use_swapchain_color) {
          attachments[attachment_count++] =
              vkr_renderer_window_attachment_get(renderer, image_index);
        }

        if (pass->use_depth) {
          if (!depth) {
            log_error("Depth attachment unavailable for layer %s",
                      layer->name.str ? (const char *)layer->name.str
                                      : "<unnamed>");
            continue;
          }
          attachments[attachment_count++] = depth;
        }

        if (attachment_count == 0) {
          log_error("No attachments configured for layer %s pass %u",
                    layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>",
                    pass_index);
          continue;
        }

        VkrRenderTargetDesc desc = {
            .sync_to_window_size = true_v,
            .attachment_count = attachment_count,
            .attachments = attachments,
            .width = vkr_view_system_layer_width(vs, layer),
            .height = vkr_view_system_layer_height(vs, layer),
        };

        pass->render_targets[image_index] = vkr_renderer_render_target_create(
            renderer, &desc, pass->renderpass);

        if (!pass->render_targets[image_index]) {
          log_error("Failed to create render target for layer %s image %u",
                    layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>",
                    image_index);
        }
      }
    }
  }

  vs->render_target_count = count;

  if (rf->rf_mutex) {
    vkr_mutex_unlock(rf->rf_mutex);
  }
}

void vkr_view_system_draw_all(VkrRendererFrontendHandle renderer,
                              uint32_t image_index) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  if (rf->rf_mutex) {
    vkr_mutex_lock(rf->rf_mutex);
  }

  if (vs->render_target_count == 0 || image_index >= vs->render_target_count) {
    log_error("Invalid render target index %u (count=%u)", image_index,
              vs->render_target_count);
    if (rf->rf_mutex) {
      vkr_mutex_unlock(rf->rf_mutex);
    }
    return;
  }

  if (vs->order_dirty || vs->sorted_count == 0) {
    vkr_view_system_rebuild_sorted(vs);
  }

  for (uint32_t i = 0; i < vs->sorted_count; ++i) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, vs->sorted_indices[i]);
    for (uint32_t pass_index = 0; pass_index < layer->pass_count;
         ++pass_index) {
      VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, pass_index);
      if (!pass->renderpass || !pass->render_targets ||
          image_index >= pass->render_target_count) {
        continue;
      }

      VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, pass);
      VkrLayerRenderInfo info = {.image_index = image_index,
                                 .renderpass_name = pass->renderpass_name};

      VkrRendererError begin_err = vkr_renderer_begin_render_pass(
          renderer, pass->renderpass, pass->render_targets[image_index]);
      if (begin_err != VKR_RENDERER_ERROR_NONE) {
        String8 err_str = vkr_renderer_get_error_string(begin_err);
        log_error("Failed to begin render pass for layer %s: %s",
                  layer->name.str ? (const char *)layer->name.str : "<unnamed>",
                  string8_cstr(&err_str));
        continue;
      }

      if (layer->callbacks.on_render) {
        layer->callbacks.on_render(&ctx, &info);
      }

      VkrRendererError end_err = vkr_renderer_end_render_pass(renderer);
      if (end_err != VKR_RENDERER_ERROR_NONE) {
        String8 err_str = vkr_renderer_get_error_string(end_err);
        log_error("Failed to end render pass for layer %s: %s",
                  layer->name.str ? (const char *)layer->name.str : "<unnamed>",
                  string8_cstr(&err_str));
        continue;
      }
    }
  }

  if (rf->rf_mutex) {
    vkr_mutex_unlock(rf->rf_mutex);
  }
}

// ============================================================================
// Layer context accessors
// ============================================================================

VkrRendererFrontendHandle vkr_layer_context_get_renderer(VkrLayerContext *ctx) {
  if (!ctx || !ctx->view_system) {
    return NULL;
  }
  return ctx->view_system->renderer;
}

uint32_t vkr_layer_context_get_width(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->view_system || !ctx->layer) {
    return 0;
  }
  return vkr_view_system_layer_width(ctx->view_system, ctx->layer);
}

uint32_t vkr_layer_context_get_height(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->view_system || !ctx->layer) {
    return 0;
  }
  return vkr_view_system_layer_height(ctx->view_system, ctx->layer);
}

const Mat4 *vkr_layer_context_get_view(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->layer) {
    return NULL;
  }
  return &ctx->layer->view;
}

const Mat4 *vkr_layer_context_get_projection(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->layer) {
    return NULL;
  }
  return &ctx->layer->projection;
}

void vkr_layer_context_set_camera(VkrLayerContext *ctx, const Mat4 *view,
                                  const Mat4 *projection) {
  if (!ctx || !ctx->view_system || !ctx->layer) {
    return;
  }

  if (view) {
    ctx->layer->view = *view;
  }
  if (projection) {
    ctx->layer->projection = *projection;
  }

  if (ctx->view_system->renderer) {
    RendererFrontend *rf = (RendererFrontend *)ctx->view_system->renderer;
    vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);
  }
}

void *vkr_layer_context_get_user_data(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->layer) {
    return NULL;
  }
  return ctx->layer->user_data;
}

VkrRenderPassHandle
vkr_layer_context_get_renderpass(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->pass) {
    return NULL;
  }
  return ctx->pass->renderpass;
}

VkrRenderTargetHandle
vkr_layer_context_get_render_target(const VkrLayerContext *ctx,
                                    uint32_t image_index) {
  if (!ctx || !ctx->pass || !ctx->pass->render_targets) {
    return NULL;
  }
  if (image_index >= ctx->pass->render_target_count) {
    return NULL;
  }
  return ctx->pass->render_targets[image_index];
}

uint32_t vkr_layer_context_get_render_target_count(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->pass) {
    return 0;
  }
  return ctx->pass->render_target_count;
}

uint32_t vkr_layer_context_get_pass_index(const VkrLayerContext *ctx) {
  if (!ctx || !ctx->layer || !ctx->pass) {
    return 0;
  }
  for (uint32_t i = 0; i < ctx->layer->pass_count; ++i) {
    VkrLayerPass *candidate = array_get_VkrLayerPass(&ctx->layer->passes, i);
    if (candidate == ctx->pass) {
      return i;
    }
  }
  return 0;
}

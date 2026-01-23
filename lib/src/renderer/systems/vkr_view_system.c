#include "renderer/systems/vkr_view_system.h"

#include "core/logger.h"
#include "defines.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_layer_messages.h"
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

vkr_internal VkrLayerBehaviorSlot *
vkr_view_system_get_behavior_slot(VkrLayer *layer,
                                  VkrLayerBehaviorHandle handle) {
  if (!layer || handle.id == 0) {
    return NULL;
  }

  if (array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
    return NULL;
  }

  if (handle.id - 1 >= layer->behaviors.length) {
    return NULL;
  }

  VkrLayerBehaviorSlot *slot =
      array_get_VkrLayerBehaviorSlot(&layer->behaviors, handle.id - 1);
  if (!slot->active) {
    return NULL;
  }

  if (slot->handle.generation != handle.generation) {
    return NULL;
  }

  return slot;
}

vkr_internal void vkr_view_system_destroy_pass_targets(RendererFrontend *rf,
                                                       VkrLayerPass *pass) {
  assert_log(rf != NULL, "Renderer frontend is NULL");
  assert_log(pass != NULL, "Pass is NULL");

  if (pass->use_custom_render_targets) {
    pass->render_target_count = 0;
    pass->render_targets = NULL;
    pass->custom_color_attachments = NULL;
    pass->custom_color_layouts = NULL;
    return;
  }

  if (!pass->render_targets || pass->render_target_count == 0) {
    pass->render_target_count = 0;
    pass->render_targets = NULL;
    return;
  }

  for (uint32_t i = 0; i < pass->render_target_count; ++i) {
    if (pass->render_targets[i]) {
      vkr_renderer_render_target_destroy(rf, pass->render_targets[i]);
    }
  }

  vkr_allocator_free(&rf->view_system.allocator, pass->render_targets,
                     sizeof(VkrRenderTargetHandle) *
                         (uint64_t)pass->render_target_count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  pass->render_target_count = 0;
  pass->render_targets = NULL;
}

vkr_internal void vkr_view_system_destroy_layer(RendererFrontend *rf,
                                                VkrLayer *layer) {
  assert_log(rf != NULL, "Renderer frontend is NULL");
  assert_log(layer != NULL, "Layer is NULL");

  VkrViewSystem *vs = &rf->view_system;
  if (vs->modal_focus_layer.id != 0 &&
      vs->modal_focus_layer.id == layer->handle.id &&
      vs->modal_focus_layer.generation == layer->handle.generation) {
    vs->modal_focus_layer = VKR_LAYER_HANDLE_INVALID;
  }

  // Invoke detach callback before tearing down resources
  if (layer->active && layer->callbacks.on_detach) {
    VkrLayerContext ctx =
        vkr_view_system_make_context(&rf->view_system, layer, NULL);
    layer->callbacks.on_detach(&ctx);
  }

  if (layer->active && layer->behavior_count > 0 &&
      !array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
    VkrLayerContext ctx =
        vkr_view_system_make_context(&rf->view_system, layer, NULL);
    for (uint32_t i = 0; i < layer->behaviors.length; ++i) {
      VkrLayerBehaviorSlot *slot =
          array_get_VkrLayerBehaviorSlot(&layer->behaviors, i);
      if (!slot->active || !slot->behavior.on_detach) {
        continue;
      }
      slot->behavior.on_detach(&ctx, slot->behavior.behavior_data);
    }
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

  if (!array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
    array_destroy_VkrLayerBehaviorSlot(&layer->behaviors);
  }

  uint32_t old_generation = layer->handle.generation;
  MemZero(layer, sizeof(*layer));
  layer->handle.generation = old_generation + 1;
}

vkr_internal void vkr_view_system_copy_passes(VkrViewSystem *vs,
                                              VkrLayer *layer,
                                              const VkrLayerConfig *cfg) {
  assert_log(vs != NULL, "View system is NULL");
  assert_log(layer != NULL, "Layer is NULL");
  assert_log(cfg != NULL, "Layer config is NULL");

  layer->passes = array_create_VkrLayerPass(&vs->allocator, layer->pass_count);
  MemZero(layer->passes.data,
          sizeof(VkrLayerPass) * (uint64_t)layer->pass_count);

  for (uint32_t i = 0; i < layer->pass_count; ++i) {
    VkrLayerPass *dst = array_get_VkrLayerPass(&layer->passes, i);
    const VkrLayerPassConfig *src = &cfg->passes[i];
    dst->renderpass_name =
        string8_duplicate(&vs->allocator, &src->renderpass_name);
    dst->use_depth = src->use_depth;
    dst->use_swapchain_color = src->use_swapchain_color;
  }
}

vkr_internal int vkr_view_system_layer_compare(const void *a, const void *b) {
  assert_log(a != NULL, "A is NULL");
  assert_log(b != NULL, "B is NULL");

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
  assert_log(vs != NULL, "View system is NULL");
  assert_log(vs->initialized, "View system is not initialized");

  RendererFrontend *rf = (RendererFrontend *)vs->renderer;
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!vs->sorted_indices) {
    vs->sorted_indices = vkr_allocator_alloc(
        &vs->allocator, sizeof(uint32_t) * (uint64_t)vs->layer_capacity,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (!vs->sorted_indices) {
    log_error("Failed to allocate sorted index array for view system");
    return;
  }

  VkrAllocator temp_alloc = {0};
  VkrAllocatorScope temp_scope = {0};
  VkrAllocator *entry_alloc = &vs->allocator;
  bool use_scope = false;
  if (rf && rf->scratch_arena) {
    temp_alloc = rf->allocator;
    temp_scope = vkr_allocator_begin_scope(&temp_alloc);
    if (vkr_allocator_scope_is_valid(&temp_scope)) {
      entry_alloc = &temp_alloc;
      use_scope = true;
    }
  }

  VkrLayerSortEntry *entries = (VkrLayerSortEntry *)vkr_allocator_alloc(
      entry_alloc, sizeof(VkrLayerSortEntry) * vs->layers.length,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!entries) {
    log_error("Failed to allocate scratch entries for view sorting");
    if (use_scope) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
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

  if (use_scope) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    vkr_allocator_free(&vs->allocator, entries,
                       sizeof(VkrLayerSortEntry) * vs->layers.length,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
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
  vs->input_state = rf->window ? &rf->window->input_state : NULL;
  vs->modal_focus_layer = VKR_LAYER_HANDLE_INVALID;

  vs->layers = array_create_VkrLayer(&vs->allocator, vs->layer_capacity);
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
  slot->name = string8_duplicate(&vs->allocator, &cfg->name);
  slot->enabled = cfg->enabled;
  slot->flags = cfg->flags;
  slot->behavior_count = 0;
  slot->behaviors = (Array_VkrLayerBehaviorSlot){0};

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

  if (slot->enabled && slot->callbacks.on_enable) {
    VkrLayerContext ctx = vkr_view_system_make_context(vs, slot, NULL);
    slot->callbacks.on_enable(&ctx);
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

      if (pass->use_custom_render_targets) {
        if (!pass->render_targets || pass->render_target_count == 0) {
          log_error("Custom render targets missing for layer %s pass %u",
                    layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>",
                    pass_index);
        }
        continue;
      }

      vkr_view_system_destroy_pass_targets(rf, pass);

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
        VkrRenderTargetAttachmentRef attachments[2] = {0};
        uint8_t attachment_count = 0;

        if (pass->use_swapchain_color) {
          attachments[attachment_count++] = (VkrRenderTargetAttachmentRef){
              .texture =
                  vkr_renderer_window_attachment_get(renderer, image_index),
              .mip_level = 0,
              .base_layer = 0,
              .layer_count = 1,
          };
        }

        if (pass->use_depth) {
          if (!depth) {
            log_error("Depth attachment unavailable for layer %s",
                      layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>");
            continue;
          }
          attachments[attachment_count++] = (VkrRenderTargetAttachmentRef){
              .texture = depth,
              .mip_level = 0,
              .base_layer = 0,
              .layer_count = 1,
          };
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

        VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
        pass->render_targets[image_index] = vkr_renderer_render_target_create(
            renderer, &desc, pass->renderpass, &rt_err);

        if (!pass->render_targets[image_index]) {
          String8 err = vkr_renderer_get_error_string(rt_err);
          log_error("Failed to create render target for layer %s image %u",
                    layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>",
                    image_index);
          log_error("Render target error: %s", string8_cstr(&err));
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
                              float64_t delta_time, uint32_t image_index) {
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
    if (!layer->enabled) {
      continue;
    }
    for (uint32_t pass_index = 0; pass_index < layer->pass_count;
         ++pass_index) {
      VkrLayerPass *pass = array_get_VkrLayerPass(&layer->passes, pass_index);
      if (!pass->renderpass || !pass->render_targets ||
          image_index >= pass->render_target_count) {
        continue;
      }

      bool8_t has_custom_color = pass->use_custom_render_targets &&
                                 pass->custom_color_attachment_count > 0 &&
                                 pass->custom_color_attachments &&
                                 pass->custom_color_layouts &&
                                 image_index < pass->render_target_count;
      if (has_custom_color) {
        VkrTextureOpaqueHandle color_tex =
            pass->custom_color_attachments[image_index];
        if (!color_tex) {
          log_error("Missing custom color attachment for layer %s pass %u",
                    layer->name.str ? (const char *)layer->name.str
                                    : "<unnamed>",
                    pass_index);
          continue;
        }

        VkrTextureLayout current_layout =
            pass->custom_color_layouts[image_index];
        if (current_layout != VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT) {
          VkrRendererError trans_err = vkr_renderer_transition_texture_layout(
              renderer, color_tex, current_layout,
              VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT);
          if (trans_err != VKR_RENDERER_ERROR_NONE) {
            String8 err_str = vkr_renderer_get_error_string(trans_err);
            log_error(
                "Failed to transition custom color attachment for layer %s: %s",
                layer->name.str ? (const char *)layer->name.str : "<unnamed>",
                string8_cstr(&err_str));
            continue;
          }
          pass->custom_color_layouts[image_index] =
              VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT;
        }
      }

      VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, pass);
      VkrLayerRenderInfo info = {.image_index = image_index,
                                 .delta_time = delta_time,
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

      if (layer->behavior_count > 0 &&
          !array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
        for (uint32_t b = 0; b < layer->behaviors.length; ++b) {
          VkrLayerBehaviorSlot *slot =
              array_get_VkrLayerBehaviorSlot(&layer->behaviors, b);
          if (!slot->active || !slot->behavior.on_render) {
            continue;
          }
          slot->behavior.on_render(&ctx, slot->behavior.behavior_data, &info);
        }
      }

      VkrRendererError end_err = vkr_renderer_end_render_pass(renderer);
      if (end_err != VKR_RENDERER_ERROR_NONE) {
        String8 err_str = vkr_renderer_get_error_string(end_err);
        log_error("Failed to end render pass for layer %s: %s",
                  layer->name.str ? (const char *)layer->name.str : "<unnamed>",
                  string8_cstr(&err_str));
        continue;
      }

      if (has_custom_color) {
        VkrTextureOpaqueHandle color_tex =
            pass->custom_color_attachments[image_index];
        VkrTextureLayout current_layout =
            pass->custom_color_layouts[image_index];
        if (current_layout != VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY) {
          VkrRendererError trans_err = vkr_renderer_transition_texture_layout(
              renderer, color_tex, current_layout,
              VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY);
          if (trans_err != VKR_RENDERER_ERROR_NONE) {
            String8 err_str = vkr_renderer_get_error_string(trans_err);
            log_error(
                "Failed to transition custom color attachment for layer %s: %s",
                layer->name.str ? (const char *)layer->name.str : "<unnamed>",
                string8_cstr(&err_str));
            continue;
          }
          pass->custom_color_layouts[image_index] =
              VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY;
        }
      }
    }
  }

  if (rf->rf_mutex) {
    vkr_mutex_unlock(rf->rf_mutex);
  }
}

void vkr_view_system_update_all(VkrRendererFrontendHandle renderer,
                                float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  if (vs->order_dirty || vs->sorted_count == 0) {
    vkr_view_system_rebuild_sorted(vs);
  }

  bool8_t input_consumed = false_v;
  bool8_t has_modal_focus = (vs->modal_focus_layer.id != 0);

  for (int32_t i = (int32_t)vs->sorted_count - 1; i >= 0; --i) {
    VkrLayer *layer = array_get_VkrLayer(&vs->layers, vs->sorted_indices[i]);
    if (!layer->enabled && !(layer->flags & VKR_LAYER_FLAG_ALWAYS_UPDATE)) {
      continue;
    }

    bool8_t can_receive_input = !input_consumed && layer->enabled;
    if (has_modal_focus) {
      can_receive_input =
          layer->enabled &&
          (layer->handle.id == vs->modal_focus_layer.id &&
           layer->handle.generation == vs->modal_focus_layer.generation);
    }

    VkrLayerUpdateInfo info = {
        .delta_time = delta_time,
        .input_state = can_receive_input ? vs->input_state : NULL,
        .camera_system = &rf->camera_system,
        .active_camera = rf->active_camera,
        .frame_number = (uint32_t)rf->frame_number,
    };

    VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);

    bool8_t consumed = false_v;
    if (layer->callbacks.on_update) {
      bool8_t layer_consumed = layer->callbacks.on_update(&ctx, &info);
      if (info.input_state) {
        consumed |= layer_consumed;
      }
    }

    if (layer->behavior_count > 0 &&
        !array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
      for (uint32_t b = 0; b < layer->behaviors.length; ++b) {
        VkrLayerBehaviorSlot *slot =
            array_get_VkrLayerBehaviorSlot(&layer->behaviors, b);
        if (!slot->active || !slot->behavior.on_update) {
          continue;
        }
        bool8_t behavior_consumed =
            slot->behavior.on_update(&ctx, slot->behavior.behavior_data, &info);
        if (info.input_state) {
          consumed |= behavior_consumed;
        }
      }
    }

    if (consumed && info.input_state) {
      input_consumed = true_v;
    }
  }
}

void vkr_view_system_set_layer_enabled(VkrRendererFrontendHandle renderer,
                                       VkrLayerHandle handle, bool8_t enabled) {
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

  bool8_t next_state = enabled ? true_v : false_v;
  if (layer->enabled == next_state) {
    return;
  }

  layer->enabled = next_state;
  VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);

  if (next_state) {
    if (layer->callbacks.on_enable) {
      layer->callbacks.on_enable(&ctx);
    }
  } else {
    if (layer->callbacks.on_disable) {
      layer->callbacks.on_disable(&ctx);
    }

    if (vs->modal_focus_layer.id == layer->handle.id &&
        vs->modal_focus_layer.generation == layer->handle.generation) {
      vs->modal_focus_layer = VKR_LAYER_HANDLE_INVALID;
    }
  }
}

bool8_t vkr_view_system_is_layer_enabled(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle handle) {
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

  return layer->enabled;
}

void vkr_view_system_set_modal_focus(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle handle) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, handle);
  if (!layer) {
    vs->modal_focus_layer = VKR_LAYER_HANDLE_INVALID;
    return;
  }

  vs->modal_focus_layer = layer->handle;
}

void vkr_view_system_clear_modal_focus(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  vs->modal_focus_layer = VKR_LAYER_HANDLE_INVALID;
}

VkrLayerHandle
vkr_view_system_get_modal_focus(VkrRendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return VKR_LAYER_HANDLE_INVALID;
  }

  return vs->modal_focus_layer;
}

// ============================================================================
// Typed Message API
// ============================================================================

bool32_t vkr_view_system_send_msg(VkrRendererFrontendHandle renderer,
                                  VkrLayerHandle target,
                                  const VkrLayerMsgHeader *msg, void *out_rsp,
                                  uint64_t out_rsp_capacity,
                                  uint64_t *out_rsp_size) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(msg != NULL, "Message is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return false_v;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, target);
  if (!layer) {
    return false_v;
  }

#ifndef NDEBUG
  // Debug-only validation
  const VkrLayerMsgMeta *meta = vkr_layer_msg_get_meta(msg->kind);
  if (!meta) {
    log_error("Unknown message kind: %u", msg->kind);
    return false_v;
  }

  if (msg->version != meta->expected_version) {
    log_error("Message version mismatch for %s: expected %u, got %u",
              meta->name, meta->expected_version, msg->version);
    return false_v;
  }

  if (msg->payload_size != meta->payload_size) {
    log_error("Payload size mismatch for %s: expected %u, got %u", meta->name,
              meta->payload_size, msg->payload_size);
    return false_v;
  }

  if ((msg->flags & VKR_LAYER_MSG_FLAG_EXPECTS_RESPONSE) &&
      meta->rsp_kind == VKR_LAYER_RSP_NONE) {
    log_warn("Message %s flagged as expecting response but has no response "
             "type defined",
             meta->name);
  }

  if (meta->rsp_kind != VKR_LAYER_RSP_NONE && out_rsp != NULL &&
      out_rsp_capacity < meta->rsp_size) {
    log_error("Response buffer too small for %s: need %u, have %llu",
              meta->name, meta->rsp_size, (unsigned long long)out_rsp_capacity);
    return false_v;
  }
#endif

  VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);

  if (out_rsp_size) {
    *out_rsp_size = 0;
  }

  if (layer->callbacks.on_data_received) {
    layer->callbacks.on_data_received(&ctx, msg, out_rsp, out_rsp_capacity,
                                      out_rsp_size);
  }

  if (layer->behavior_count > 0 &&
      !array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
    bool8_t allow_behavior_out = out_rsp != NULL && out_rsp_size != NULL;
    if (allow_behavior_out && out_rsp_size && *out_rsp_size > 0) {
      allow_behavior_out = false_v;
    }
    for (uint32_t i = 0; i < layer->behaviors.length; ++i) {
      VkrLayerBehaviorSlot *slot =
          array_get_VkrLayerBehaviorSlot(&layer->behaviors, i);
      if (!slot->active || !slot->behavior.on_data_received) {
        continue;
      }
      slot->behavior.on_data_received(&ctx, slot->behavior.behavior_data, msg,
                                      allow_behavior_out ? out_rsp : NULL,
                                      allow_behavior_out ? out_rsp_capacity : 0,
                                      allow_behavior_out ? out_rsp_size : NULL);
      if (allow_behavior_out && out_rsp_size && *out_rsp_size > 0) {
        allow_behavior_out = false_v;
      }
    }
  }

  return true_v;
}

bool32_t vkr_view_system_send_msg_no_rsp(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle target,
                                         const VkrLayerMsgHeader *msg) {
  return vkr_view_system_send_msg(renderer, target, msg, NULL, 0, NULL);
}

void vkr_view_system_broadcast_msg(VkrRendererFrontendHandle renderer,
                                   const VkrLayerMsgHeader *msg,
                                   uint32_t flags_filter) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(msg != NULL, "Message is NULL");

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

    if (flags_filter != 0 && (layer->flags & flags_filter) == 0) {
      continue;
    }

    vkr_view_system_send_msg_no_rsp(renderer, layer->handle, msg);
  }
}

VkrLayerBehaviorHandle vkr_view_system_attach_behavior(
    VkrRendererFrontendHandle renderer, VkrLayerHandle layer_handle,
    const VkrLayerBehavior *behavior, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(behavior != NULL, "Behavior is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return VKR_LAYER_BEHAVIOR_HANDLE_INVALID;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, layer_handle);
  if (!layer) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return VKR_LAYER_BEHAVIOR_HANDLE_INVALID;
  }

  if (array_is_null_VkrLayerBehaviorSlot(&layer->behaviors)) {
    layer->behaviors = array_create_VkrLayerBehaviorSlot(
        &vs->allocator, VKR_VIEW_SYSTEM_MAX_LAYER_BEHAVIORS);
    MemZero(layer->behaviors.data,
            sizeof(VkrLayerBehaviorSlot) * (uint64_t)layer->behaviors.length);
  }

  if (layer->behavior_count >= layer->behaviors.length) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return VKR_LAYER_BEHAVIOR_HANDLE_INVALID;
  }

  VkrLayerBehaviorSlot *slot = NULL;
  uint32_t slot_index = 0;
  for (uint32_t i = 0; i < layer->behaviors.length; ++i) {
    VkrLayerBehaviorSlot *candidate =
        array_get_VkrLayerBehaviorSlot(&layer->behaviors, i);
    if (!candidate->active) {
      slot = candidate;
      slot_index = i;
      break;
    }
  }

  if (!slot) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return VKR_LAYER_BEHAVIOR_HANDLE_INVALID;
  }

  uint32_t next_generation = slot->handle.generation + 1;
  MemZero(slot, sizeof(*slot));
  slot->active = true_v;
  slot->handle.id = slot_index + 1;
  slot->handle.generation = next_generation ? next_generation : 1;
  slot->behavior = *behavior;
  slot->behavior.name = string8_duplicate(&vs->allocator, &behavior->name);
  layer->behavior_count++;

  VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);
  if (slot->behavior.on_attach) {
    slot->behavior.on_attach(&ctx, slot->behavior.behavior_data);
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return slot->handle;
}

void vkr_view_system_detach_behavior(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle layer_handle,
                                     VkrLayerBehaviorHandle behavior_handle) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, layer_handle);
  if (!layer) {
    return;
  }

  VkrLayerBehaviorSlot *slot =
      vkr_view_system_get_behavior_slot(layer, behavior_handle);
  if (!slot) {
    return;
  }

  VkrLayerContext ctx = vkr_view_system_make_context(vs, layer, NULL);
  if (slot->behavior.on_detach) {
    slot->behavior.on_detach(&ctx, slot->behavior.behavior_data);
  }

  slot->active = false_v;
  slot->handle.generation++;
  if (layer->behavior_count > 0) {
    layer->behavior_count--;
  }
}

void *
vkr_view_system_get_behavior_data(VkrRendererFrontendHandle renderer,
                                  VkrLayerHandle layer_handle,
                                  VkrLayerBehaviorHandle behavior_handle) {
  assert_log(renderer != NULL, "Renderer is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrViewSystem *vs = &rf->view_system;
  if (!vs->initialized) {
    return NULL;
  }

  VkrLayer *layer = vkr_view_system_get_layer(vs, layer_handle);
  if (!layer) {
    return NULL;
  }

  VkrLayerBehaviorSlot *slot =
      vkr_view_system_get_behavior_slot(layer, behavior_handle);
  if (!slot) {
    return NULL;
  }

  return slot->behavior.behavior_data;
}

// ============================================================================
// Layer context accessors
// ============================================================================

VkrRendererFrontendHandle vkr_layer_context_get_renderer(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  return ctx->view_system->renderer;
}

uint32_t vkr_layer_context_get_width(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return vkr_view_system_layer_width(ctx->view_system, ctx->layer);
}

uint32_t vkr_layer_context_get_height(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return vkr_view_system_layer_height(ctx->view_system, ctx->layer);
}

const Mat4 *vkr_layer_context_get_view(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return &ctx->layer->view;
}

const Mat4 *vkr_layer_context_get_projection(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return &ctx->layer->projection;
}

void vkr_layer_context_set_camera(VkrLayerContext *ctx, const Mat4 *view,
                                  const Mat4 *projection) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");

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
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return ctx->layer->user_data;
}

VkrRenderPassHandle
vkr_layer_context_get_renderpass(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->pass != NULL, "Pass is NULL");
  return ctx->pass->renderpass;
}

VkrRenderTargetHandle
vkr_layer_context_get_render_target(const VkrLayerContext *ctx,
                                    uint32_t image_index) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->pass != NULL, "Pass is NULL");
  assert_log(ctx->pass->render_targets != NULL, "Render targets are NULL");
  assert_log(image_index < ctx->pass->render_target_count,
             "Image index out of bounds");
  return ctx->pass->render_targets[image_index];
}

uint32_t vkr_layer_context_get_render_target_count(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->pass != NULL, "Pass is NULL");
  assert_log(ctx->pass->render_targets != NULL, "Render targets are NULL");
  return ctx->pass->render_target_count;
}

uint32_t vkr_layer_context_get_pass_index(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  assert_log(ctx->pass != NULL, "Pass is NULL");
  assert_log(ctx->layer->pass_count > 0, "Layer has no passes");
  for (uint32_t i = 0; i < ctx->layer->pass_count; ++i) {
    VkrLayerPass *candidate = array_get_VkrLayerPass(&ctx->layer->passes, i);
    if (candidate == ctx->pass) {
      return i;
    }
  }
  return 0;
}

VkrLayerHandle vkr_layer_context_get_handle(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return ctx->layer->handle;
}

uint32_t vkr_layer_context_get_flags(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return ctx->layer->flags;
}

bool8_t vkr_layer_context_has_modal_focus(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->layer != NULL, "Layer is NULL");
  return (ctx->view_system->modal_focus_layer.id == ctx->layer->handle.id &&
          ctx->view_system->modal_focus_layer.generation ==
              ctx->layer->handle.generation);
}

VkrCameraSystem *
vkr_layer_context_get_camera_system(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->view_system->renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)ctx->view_system->renderer;
  return &rf->camera_system;
}

VkrCameraHandle
vkr_layer_context_get_active_camera(const VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(ctx->view_system != NULL, "View system is NULL");
  assert_log(ctx->view_system->renderer != NULL, "Renderer is NULL");
  RendererFrontend *rf = (RendererFrontend *)ctx->view_system->renderer;
  return rf->active_camera;
}

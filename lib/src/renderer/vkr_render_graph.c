#include "renderer/vkr_render_graph_internal.h"

#include "core/logger.h"
#include "defines.h"
#include "renderer/vkr_render_packet.h"

static int32_t vkr_rg_find_image_index(VkrRenderGraph *graph, String8 name) {
  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (string8_equals(&image->name, &name)) {
      return (int32_t)i;
    }
  }
  return -1;
}

static int32_t vkr_rg_find_buffer_index(VkrRenderGraph *graph, String8 name) {
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (string8_equals(&buffer->name, &name)) {
      return (int32_t)i;
    }
  }
  return -1;
}

static bool8_t vkr_rg_image_desc_equal(const VkrRgImageDesc *a,
                                       const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type &&
         a->flags == b->flags;
}

static bool8_t vkr_rg_buffer_desc_equal(const VkrRgBufferDesc *a,
                                        const VkrRgBufferDesc *b) {
  return a->size == b->size && a->usage.set == b->usage.set &&
         a->flags == b->flags;
}

VkrRgImage *vkr_rg_image_from_handle(VkrRenderGraph *graph,
                                     VkrRgImageHandle handle) {
  if (!graph || !vkr_rg_image_handle_valid(handle)) {
    return NULL;
  }

  uint32_t index = handle.id - 1;
  if (index >= graph->images.length) {
    return NULL;
  }

  VkrRgImage *image = vector_get_VkrRgImage(&graph->images, index);
  if (image->generation != handle.generation) {
    return NULL;
  }

  return image;
}

VkrRgBuffer *vkr_rg_buffer_from_handle(VkrRenderGraph *graph,
                                       VkrRgBufferHandle handle) {
  if (!graph || !vkr_rg_buffer_handle_valid(handle)) {
    return NULL;
  }

  uint32_t index = handle.id - 1;
  if (index >= graph->buffers.length) {
    return NULL;
  }

  VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, index);
  if (buffer->generation != handle.generation) {
    return NULL;
  }

  return buffer;
}

VkrTextureOpaqueHandle vkr_rg_get_image_texture(const VkrRenderGraph *graph,
                                                VkrRgImageHandle image,
                                                uint32_t image_index) {
  if (!graph) {
    return NULL;
  }

  VkrRgImage *entry =
      vkr_rg_image_from_handle((VkrRenderGraph *)graph, image);
  if (!entry) {
    return NULL;
  }

  return vkr_rg_pick_image_texture(entry, image_index);
}

VkrRgImageHandle vkr_rg_find_image(const VkrRenderGraph *graph, String8 name) {
  if (!graph || name.length == 0) {
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (string8_equals(&image->name, &name)) {
      return (VkrRgImageHandle){(uint32_t)i + 1, image->generation};
    }
  }

  return VKR_RG_IMAGE_HANDLE_INVALID;
}

VkrBufferHandle vkr_rg_get_buffer_handle(const VkrRenderGraph *graph,
                                         VkrRgBufferHandle buffer,
                                         uint32_t image_index) {
  if (!graph) {
    return NULL;
  }

  VkrRgBuffer *entry =
      vkr_rg_buffer_from_handle((VkrRenderGraph *)graph, buffer);
  if (!entry) {
    return NULL;
  }

  return vkr_rg_pick_buffer_handle(entry, image_index);
}

VkrTextureOpaqueHandle
vkr_rg_pass_get_image_texture(const VkrRgPassContext *ctx,
                              VkrRgImageHandle image) {
  if (!ctx) {
    return NULL;
  }
  return vkr_rg_get_image_texture(ctx->graph, image, ctx->image_index);
}

VkrBufferHandle vkr_rg_pass_get_buffer_handle(const VkrRgPassContext *ctx,
                                              VkrRgBufferHandle buffer) {
  if (!ctx) {
    return NULL;
  }
  return vkr_rg_get_buffer_handle(ctx->graph, buffer, ctx->image_index);
}

void vkr_rg_set_packet(VkrRenderGraph *graph, const VkrRenderPacket *packet) {
  if (!graph) {
    return;
  }

  graph->packet = packet;
}

static const VkrRenderPacket *
vkr_rg_get_packet_from_ctx(const VkrRgPassContext *ctx) {
  if (!ctx || !ctx->graph) {
    return NULL;
  }
  return ctx->graph->packet;
}

const VkrWorldPassPayload *
vkr_rg_pass_get_world_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->world : NULL;
}

const VkrShadowPassPayload *
vkr_rg_pass_get_shadow_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->shadow : NULL;
}

const VkrSkyboxPassPayload *
vkr_rg_pass_get_skybox_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->skybox : NULL;
}

const VkrUiPassPayload *vkr_rg_pass_get_ui_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->ui : NULL;
}

const VkrEditorPassPayload *
vkr_rg_pass_get_editor_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->editor : NULL;
}

const VkrPickingPassPayload *
vkr_rg_pass_get_picking_payload(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? packet->picking : NULL;
}

const VkrRenderPacket *vkr_rg_pass_get_packet(const VkrRgPassContext *ctx) {
  return vkr_rg_get_packet_from_ctx(ctx);
}

const VkrFrameInfo *vkr_rg_pass_get_frame_info(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? &packet->frame : NULL;
}

const VkrFrameGlobals *
vkr_rg_pass_get_frame_globals(const VkrRgPassContext *ctx) {
  const VkrRenderPacket *packet = vkr_rg_get_packet_from_ctx(ctx);
  return packet ? &packet->globals : NULL;
}

bool8_t vkr_rg_get_resource_stats(const VkrRenderGraph *graph,
                                  VkrRenderGraphResourceStats *out_stats) {
  if (!graph || !out_stats) {
    return false_v;
  }

  *out_stats = graph->resource_stats;
  return true_v;
}

bool8_t vkr_rg_get_pass_timings(const VkrRenderGraph *graph,
                                const VkrRgPassTiming **out_timings,
                                uint32_t *out_count) {
  if (out_timings) {
    *out_timings = NULL;
  }
  if (out_count) {
    *out_count = 0;
  }
  if (!graph || !out_timings || !out_count) {
    return false_v;
  }

  *out_timings = graph->pass_timings.data;
  *out_count = (uint32_t)graph->pass_timings.length;
  return true_v;
}

void vkr_rg_log_resource_stats(const VkrRenderGraph *graph,
                               const char *label) {
  if (!graph) {
    return;
  }

  const char *tag =
      (label && label[0] != '\0') ? label : "RenderGraph";
  const VkrRenderGraphResourceStats *stats = &graph->resource_stats;

  log_debug("%s resources: images=%u (peak=%u), image_bytes=%llu (peak=%llu), "
            "buffers=%u (peak=%u), buffer_bytes=%llu (peak=%llu)",
            tag, (uint32_t)stats->live_image_textures,
            (uint32_t)stats->peak_image_textures,
            (unsigned long long)stats->live_image_bytes,
            (unsigned long long)stats->peak_image_bytes,
            (uint32_t)stats->live_buffers, (uint32_t)stats->peak_buffers,
            (unsigned long long)stats->live_buffer_bytes,
            (unsigned long long)stats->peak_buffer_bytes);
}

void vkr_rg_reset_passes(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    vector_destroy_VkrRgAttachment(&pass->desc.color_attachments);
    vector_destroy_VkrRgImageUse(&pass->desc.image_reads);
    vector_destroy_VkrRgImageUse(&pass->desc.image_writes);
    vector_destroy_VkrRgBufferUse(&pass->desc.buffer_reads);
    vector_destroy_VkrRgBufferUse(&pass->desc.buffer_writes);

    vector_destroy_uint32_t(&pass->out_edges);
    vector_destroy_uint32_t(&pass->in_edges);
    vector_destroy_VkrRgImageBarrier(&pass->pre_image_barriers);
    vector_destroy_VkrRgBufferBarrier(&pass->pre_buffer_barriers);

    if (pass->desc.name.str) {
      vkr_allocator_free(graph->allocator, pass->desc.name.str,
                         pass->desc.name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    if (pass->desc.execute_name.str) {
      vkr_allocator_free(graph->allocator, pass->desc.execute_name.str,
                         pass->desc.execute_name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  vector_clear_VkrRgPass(&graph->passes);
  vector_clear_VkrRgPassTiming(&graph->pass_timings);
}

void vkr_rg_reset_exports(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    image->exported = false_v;
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    buffer->exported = false_v;
  }

  vector_clear_VkrRgImageHandle(&graph->export_images);
  vector_clear_VkrRgBufferHandle(&graph->export_buffers);
  graph->present_image = VKR_RG_IMAGE_HANDLE_INVALID;
}

void vkr_rg_clear_compiled(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  vector_clear_uint32_t(&graph->execution_order);
  graph->compiled = false_v;
}

bool8_t vkr_rg_executor_registry_init(VkrRgExecutorRegistry *reg,
                                      VkrAllocator *allocator) {
  if (!reg || !allocator) {
    log_error("RenderGraph executor registry init failed: invalid args");
    return false_v;
  }

  *reg = (VkrRgExecutorRegistry){0};
  reg->allocator = allocator;
  reg->entries = vector_create_VkrRgPassExecutor(allocator);
  reg->initialized = true_v;
  return true_v;
}

void vkr_rg_executor_registry_destroy(VkrRgExecutorRegistry *reg) {
  if (!reg || !reg->initialized) {
    return;
  }

  for (uint64_t i = 0; i < reg->entries.length; ++i) {
    VkrRgPassExecutor *entry = vector_get_VkrRgPassExecutor(&reg->entries, i);
    if (entry->name.str) {
      vkr_allocator_free(reg->allocator, entry->name.str,
                         entry->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  vector_destroy_VkrRgPassExecutor(&reg->entries);
  *reg = (VkrRgExecutorRegistry){0};
}

bool8_t vkr_rg_executor_registry_register(VkrRgExecutorRegistry *reg,
                                          const VkrRgPassExecutor *entry) {
  if (!reg || !reg->initialized || !entry) {
    log_error("RenderGraph executor registry register failed: invalid args");
    return false_v;
  }

  if (!entry->execute) {
    log_error("RenderGraph executor '%.*s' has NULL execute callback",
              (int)entry->name.length, entry->name.str);
    return false_v;
  }

  if (entry->name.length == 0 || !entry->name.str) {
    log_error("RenderGraph executor registration requires non-empty name");
    return false_v;
  }

  for (uint64_t i = 0; i < reg->entries.length; ++i) {
    VkrRgPassExecutor *existing =
        vector_get_VkrRgPassExecutor(&reg->entries, i);
    if (string8_equals(&existing->name, &entry->name)) {
      log_error("RenderGraph executor '%.*s' already registered",
                (int)entry->name.length, entry->name.str);
      return false_v;
    }
  }

  VkrRgPassExecutor stored = *entry;
  stored.name = string8_duplicate(reg->allocator, &entry->name);
  if (!stored.name.str) {
    log_error("RenderGraph executor registry out of memory for '%.*s'",
              (int)entry->name.length, entry->name.str);
    return false_v;
  }

  vector_push_VkrRgPassExecutor(&reg->entries, stored);
  return true_v;
}

VkrRgPassExecuteFn
vkr_rg_executor_registry_find(const VkrRgExecutorRegistry *reg, String8 name,
                              void **out_user_data) {
  if (out_user_data) {
    *out_user_data = NULL;
  }

  if (!reg || !reg->initialized || name.length == 0) {
    return NULL;
  }

  for (uint64_t i = 0; i < reg->entries.length; ++i) {
    VkrRgPassExecutor *entry =
        vector_get_VkrRgPassExecutor(&reg->entries, i);
    if (string8_equals(&entry->name, &name)) {
      if (out_user_data) {
        *out_user_data = entry->user_data;
      }
      return entry->execute;
    }
  }

  return NULL;
}

// =============================================================================
// Render Graph Core
// =============================================================================

VkrRenderGraph *vkr_rg_create(VkrAllocator *allocator) {
  if (!allocator) {
    log_error("RenderGraph create failed: allocator is NULL");
    return NULL;
  }

  VkrRenderGraph *graph = vkr_allocator_alloc(
      allocator, sizeof(VkrRenderGraph), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!graph) {
    log_error("RenderGraph create failed: out of memory");
    return NULL;
  }

  *graph = (VkrRenderGraph){0};
  graph->allocator = allocator;
  graph->renderer = NULL;
  graph->images = vector_create_VkrRgImage(allocator);
  graph->buffers = vector_create_VkrRgBuffer(allocator);
  graph->passes = vector_create_VkrRgPass(allocator);
  graph->pass_timings = vector_create_VkrRgPassTiming(allocator);
  graph->export_images = vector_create_VkrRgImageHandle(allocator);
  graph->export_buffers = vector_create_VkrRgBufferHandle(allocator);
  graph->execution_order = vector_create_uint32_t(allocator);
  graph->renderpass_hashes = vector_create_uint64_t(allocator);
  graph->render_target_cache =
      vector_create_VkrRgRenderTargetCacheEntry(allocator);
  graph->present_image = VKR_RG_IMAGE_HANDLE_INVALID;
  return graph;
}

void vkr_rg_destroy(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  vkr_rg_reset_passes(graph);

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    vkr_rg_release_image_textures(graph, image);
    if (image->name.str) {
      vkr_allocator_free(graph->allocator, image->name.str,
                         image->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    vkr_rg_release_buffer_handles(graph, buffer);
    if (buffer->name.str) {
      vkr_allocator_free(graph->allocator, buffer->name.str,
                         buffer->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  for (uint64_t i = 0; i < graph->render_target_cache.length; ++i) {
    VkrRgRenderTargetCacheEntry *entry =
        vector_get_VkrRgRenderTargetCacheEntry(&graph->render_target_cache, i);
    if (graph->renderer && entry->renderpass) {
      vkr_renderer_renderpass_destroy(graph->renderer, entry->renderpass);
      entry->renderpass = NULL;
    }
    if (graph->renderer && entry->targets && entry->target_count > 0) {
      for (uint32_t t = 0; t < entry->target_count; ++t) {
        if (entry->targets[t]) {
          vkr_renderer_render_target_destroy(graph->renderer, entry->targets[t]);
        }
      }
    }
    if (entry->targets && entry->target_count > 0) {
      vkr_allocator_free(graph->allocator, entry->targets,
                         sizeof(VkrRenderTargetHandle) *
                             (uint64_t)entry->target_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }
    if (entry->pass_name.str) {
      vkr_allocator_free(graph->allocator, entry->pass_name.str,
                         entry->pass_name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  if (graph->resource_stats.live_image_textures > 0 ||
      graph->resource_stats.live_image_bytes > 0 ||
      graph->resource_stats.live_buffers > 0 ||
      graph->resource_stats.live_buffer_bytes > 0) {
    log_warn("RenderGraph destroy leaked resources: images=%u image_bytes=%llu "
             "buffers=%u buffer_bytes=%llu",
             (uint32_t)graph->resource_stats.live_image_textures,
             (unsigned long long)graph->resource_stats.live_image_bytes,
             (uint32_t)graph->resource_stats.live_buffers,
             (unsigned long long)graph->resource_stats.live_buffer_bytes);
  }

  vector_destroy_VkrRgImage(&graph->images);
  vector_destroy_VkrRgBuffer(&graph->buffers);
  vector_destroy_VkrRgPass(&graph->passes);
  vector_destroy_VkrRgPassTiming(&graph->pass_timings);
  vector_destroy_VkrRgImageHandle(&graph->export_images);
  vector_destroy_VkrRgBufferHandle(&graph->export_buffers);
  vector_destroy_uint32_t(&graph->execution_order);
  vector_destroy_uint64_t(&graph->renderpass_hashes);
  vector_destroy_VkrRgRenderTargetCacheEntry(&graph->render_target_cache);

  vkr_allocator_free(graph->allocator, graph, sizeof(VkrRenderGraph),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

void vkr_rg_begin_frame(VkrRenderGraph *graph,
                        const VkrRenderGraphFrameInfo *frame) {
  if (!graph || !frame) {
    log_error("RenderGraph begin frame failed: invalid args");
    return;
  }

  graph->frame_info = *frame;
  graph->packet = NULL;

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    image->declared_this_frame = false_v;
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    buffer->declared_this_frame = false_v;
  }

  vkr_rg_reset_passes(graph);
  vkr_rg_reset_exports(graph);
  vkr_rg_clear_compiled(graph);
}

void vkr_rg_end_frame(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  graph->packet = NULL;
}

bool8_t vkr_rg_get_frame_info(const VkrRenderGraph *graph,
                              VkrRenderGraphFrameInfo *out_frame) {
  if (!graph || !out_frame) {
    return false_v;
  }

  *out_frame = graph->frame_info;
  return true_v;
}

VkrRgImageHandle vkr_rg_create_image(VkrRenderGraph *graph, String8 name,
                                     const VkrRgImageDesc *desc) {
  if (!graph || !desc || name.length == 0) {
    log_error("RenderGraph create image failed: invalid args");
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  int32_t index = vkr_rg_find_image_index(graph, name);
  if (index >= 0) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, (uint32_t)index);
    if (!vkr_rg_image_desc_equal(&image->desc, desc)) {
      if (!(image->desc.flags & VKR_RG_RESOURCE_FLAG_RESIZABLE)) {
        log_warn("RenderGraph image '%.*s' resized without RESIZABLE flag",
                 (int)name.length, name.str);
      }
      image->desc = *desc;
      image->generation++;
    }
    image->declared_this_frame = true_v;
    image->imported = false_v;
    return (VkrRgImageHandle){(uint32_t)index + 1, image->generation};
  }

  String8 stored = string8_duplicate(graph->allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph create image failed: name alloc failed");
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  VkrRgImage image = {0};
  image.name = stored;
  image.desc = *desc;
  image.generation = 1;
  image.declared_this_frame = true_v;
  vector_push_VkrRgImage(&graph->images, image);

  uint32_t id = (uint32_t)graph->images.length;
  return (VkrRgImageHandle){id, image.generation};
}

VkrRgImageHandle vkr_rg_import_image(VkrRenderGraph *graph, String8 name,
                                     VkrTextureOpaqueHandle handle,
                                     VkrRgImageAccessFlags current_access,
                                     VkrTextureLayout current_layout,
                                     const VkrRgImageDesc *desc) {
  if (!graph || name.length == 0) {
    log_error("RenderGraph import image failed: invalid args");
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  VkrRgImageDesc resolved_desc = VKR_RG_IMAGE_DESC_DEFAULT;
  bool8_t has_desc = desc != NULL;
  if (has_desc) {
    resolved_desc = *desc;
  }
  resolved_desc.flags |= VKR_RG_RESOURCE_FLAG_EXTERNAL;

  int32_t index = vkr_rg_find_image_index(graph, name);
  if (index >= 0) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, (uint32_t)index);
    if (has_desc) {
      if (!vkr_rg_image_desc_equal(&image->desc, &resolved_desc)) {
        image->desc = resolved_desc;
        image->generation++;
      }
    } else {
      image->desc.flags |= VKR_RG_RESOURCE_FLAG_EXTERNAL;
    }
    image->imported = true_v;
    image->imported_handle = handle;
    image->imported_access = current_access;
    image->imported_layout = current_layout;
    image->declared_this_frame = true_v;
    return (VkrRgImageHandle){(uint32_t)index + 1, image->generation};
  }

  String8 stored = string8_duplicate(graph->allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph import image failed: name alloc failed");
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  VkrRgImage image = {0};
  image.name = stored;
  image.desc = resolved_desc;
  image.generation = 1;
  image.declared_this_frame = true_v;
  image.imported = true_v;
  image.imported_handle = handle;
  image.imported_access = current_access;
  image.imported_layout = current_layout;
  vector_push_VkrRgImage(&graph->images, image);

  uint32_t id = (uint32_t)graph->images.length;
  return (VkrRgImageHandle){id, image.generation};
}

VkrRgImageHandle vkr_rg_import_swapchain(VkrRenderGraph *graph) {
  return vkr_rg_import_image(graph, string8_lit("swapchain"), NULL,
                             VKR_RG_IMAGE_ACCESS_PRESENT,
                             VKR_TEXTURE_LAYOUT_UNDEFINED, NULL);
}

VkrRgImageHandle vkr_rg_import_depth(VkrRenderGraph *graph) {
  return vkr_rg_import_image(graph, string8_lit("swapchain_depth"), NULL,
                             VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT,
                             VKR_TEXTURE_LAYOUT_UNDEFINED,
                             NULL);
}

VkrRgBufferHandle vkr_rg_create_buffer(VkrRenderGraph *graph, String8 name,
                                       const VkrRgBufferDesc *desc) {
  if (!graph || !desc || name.length == 0) {
    log_error("RenderGraph create buffer failed: invalid args");
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  int32_t index = vkr_rg_find_buffer_index(graph, name);
  if (index >= 0) {
    VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&graph->buffers, (uint32_t)index);
    if (!vkr_rg_buffer_desc_equal(&buffer->desc, desc)) {
      if (!(buffer->desc.flags & VKR_RG_RESOURCE_FLAG_RESIZABLE)) {
        log_warn("RenderGraph buffer '%.*s' resized without RESIZABLE flag",
                 (int)name.length, name.str);
      }
      buffer->desc = *desc;
      buffer->generation++;
    }
    if (buffer->imported) {
      vkr_rg_release_buffer_handles(graph, buffer);
    }
    buffer->declared_this_frame = true_v;
    buffer->imported = false_v;
    buffer->imported_handle = NULL;
    return (VkrRgBufferHandle){(uint32_t)index + 1, buffer->generation};
  }

  String8 stored = string8_duplicate(graph->allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph create buffer failed: name alloc failed");
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  VkrRgBuffer buffer = {0};
  buffer.name = stored;
  buffer.desc = *desc;
  buffer.generation = 1;
  buffer.declared_this_frame = true_v;
  vector_push_VkrRgBuffer(&graph->buffers, buffer);

  uint32_t id = (uint32_t)graph->buffers.length;
  return (VkrRgBufferHandle){id, buffer.generation};
}

VkrRgBufferHandle vkr_rg_import_buffer(VkrRenderGraph *graph, String8 name,
                                       VkrBufferHandle handle,
                                       VkrRgBufferAccessFlags current_access) {
  if (!graph || name.length == 0) {
    log_error("RenderGraph import buffer failed: invalid args");
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  int32_t index = vkr_rg_find_buffer_index(graph, name);
  if (index >= 0) {
    VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&graph->buffers, (uint32_t)index);
    if (!buffer->imported) {
      vkr_rg_release_buffer_handles(graph, buffer);
    }
    buffer->desc.flags |= VKR_RG_RESOURCE_FLAG_EXTERNAL;
    buffer->imported = true_v;
    buffer->imported_handle = handle;
    buffer->imported_access = current_access;
    buffer->declared_this_frame = true_v;
    return (VkrRgBufferHandle){(uint32_t)index + 1, buffer->generation};
  }

  String8 stored = string8_duplicate(graph->allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph import buffer failed: name alloc failed");
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  VkrRgBuffer buffer = {0};
  buffer.name = stored;
  buffer.desc = (VkrRgBufferDesc){0};
  buffer.desc.flags |= VKR_RG_RESOURCE_FLAG_EXTERNAL;
  buffer.generation = 1;
  buffer.declared_this_frame = true_v;
  buffer.imported = true_v;
  buffer.imported_handle = handle;
  buffer.imported_access = current_access;
  vector_push_VkrRgBuffer(&graph->buffers, buffer);

  uint32_t id = (uint32_t)graph->buffers.length;
  return (VkrRgBufferHandle){id, buffer.generation};
}

VkrRgPassBuilder vkr_rg_add_pass(VkrRenderGraph *graph, VkrRgPassType type,
                                 String8 name) {
  if (!graph || name.length == 0) {
    log_error("RenderGraph add pass failed: invalid args");
    return (VkrRgPassBuilder){0};
  }

  String8 stored = string8_duplicate(graph->allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph add pass failed: name alloc failed");
    return (VkrRgPassBuilder){0};
  }

  VkrRgPass pass = {0};
  pass.desc = (VkrRgPassDesc){0};
  pass.desc.name = stored;
  pass.desc.type = type;
  pass.desc.color_attachments = vector_create_VkrRgAttachment(graph->allocator);
  pass.desc.image_reads = vector_create_VkrRgImageUse(graph->allocator);
  pass.desc.image_writes = vector_create_VkrRgImageUse(graph->allocator);
  pass.desc.buffer_reads = vector_create_VkrRgBufferUse(graph->allocator);
  pass.desc.buffer_writes = vector_create_VkrRgBufferUse(graph->allocator);

  pass.out_edges = vector_create_uint32_t(graph->allocator);
  pass.in_edges = vector_create_uint32_t(graph->allocator);
  pass.pre_image_barriers = vector_create_VkrRgImageBarrier(graph->allocator);
  pass.pre_buffer_barriers = vector_create_VkrRgBufferBarrier(graph->allocator);

  vector_push_VkrRgPass(&graph->passes, pass);

  return (VkrRgPassBuilder){.graph = graph,
                            .pass_index = (uint32_t)graph->passes.length - 1};
}

static VkrRgPass *vkr_rg_builder_get_pass(VkrRgPassBuilder *pb) {
  if (!pb || !pb->graph) {
    return NULL;
  }
  if (pb->pass_index >= pb->graph->passes.length) {
    return NULL;
  }
  return vector_get_VkrRgPass(&pb->graph->passes, pb->pass_index);
}

void vkr_rg_pass_set_execute(VkrRgPassBuilder *pb,
                             VkrRgPassExecuteFn execute, void *user_data) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  pass->desc.execute = execute;
  pass->desc.user_data = user_data;
}

void vkr_rg_pass_set_flags(VkrRgPassBuilder *pb, VkrRgPassFlags flags) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  pass->desc.flags = flags;
}

void vkr_rg_pass_set_domain(VkrRgPassBuilder *pb, VkrPipelineDomain domain) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  pass->desc.domain = domain;
}

void vkr_rg_pass_add_color_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }

  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' color attachment has invalid image",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }

  VkrRgAttachmentDesc local_desc =
      desc ? *desc : (VkrRgAttachmentDesc){.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  VkrRgAttachment attachment = {.image = image, .desc = local_desc};
  vector_push_VkrRgAttachment(&pass->desc.color_attachments, attachment);
}

void vkr_rg_pass_set_depth_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc,
                                      bool8_t read_only) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }

  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' depth attachment has invalid image",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }

  VkrRgAttachmentDesc local_desc =
      desc ? *desc : (VkrRgAttachmentDesc){.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  pass->desc.depth_attachment =
      (VkrRgAttachment){.image = image,
                        .desc = local_desc,
                        .read_only = read_only};
  pass->desc.has_depth_attachment = true_v;
}

void vkr_rg_pass_read_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                            VkrRgImageAccessFlags access, uint32_t binding,
                            uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' read has invalid image handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .binding = binding,
                       .array_index = array_index};
  vector_push_VkrRgImageUse(&pass->desc.image_reads, use);
}

void vkr_rg_pass_write_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                             VkrRgImageAccessFlags access, uint32_t binding,
                             uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' write has invalid image handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .binding = binding,
                       .array_index = array_index};
  vector_push_VkrRgImageUse(&pass->desc.image_writes, use);
}

void vkr_rg_pass_read_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                             VkrRgBufferAccessFlags access, uint32_t binding,
                             uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  if (!vkr_rg_buffer_from_handle(pb->graph, buffer)) {
    log_error("RenderGraph pass '%.*s' read has invalid buffer handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }
  VkrRgBufferUse use = {.buffer = buffer,
                        .access = access,
                        .binding = binding,
                        .array_index = array_index};
  vector_push_VkrRgBufferUse(&pass->desc.buffer_reads, use);
}

void vkr_rg_pass_write_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                              VkrRgBufferAccessFlags access, uint32_t binding,
                              uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return;
  }
  if (!vkr_rg_buffer_from_handle(pb->graph, buffer)) {
    log_error("RenderGraph pass '%.*s' write has invalid buffer handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return;
  }
  VkrRgBufferUse use = {.buffer = buffer,
                        .access = access,
                        .binding = binding,
                        .array_index = array_index};
  vector_push_VkrRgBufferUse(&pass->desc.buffer_writes, use);
}

void vkr_rg_set_present_image(VkrRenderGraph *graph, VkrRgImageHandle image) {
  if (!graph) {
    return;
  }
  graph->present_image = image;
}

void vkr_rg_export_image(VkrRenderGraph *graph, VkrRgImageHandle image) {
  if (!graph) {
    return;
  }
  VkrRgImage *image_entry = vkr_rg_image_from_handle(graph, image);
  if (!image_entry) {
    log_error("RenderGraph export image has invalid handle");
    return;
  }
  if (!image_entry->exported) {
    image_entry->exported = true_v;
    vector_push_VkrRgImageHandle(&graph->export_images, image);
  }
}

void vkr_rg_export_buffer(VkrRenderGraph *graph, VkrRgBufferHandle buffer) {
  if (!graph) {
    return;
  }
  VkrRgBuffer *buffer_entry = vkr_rg_buffer_from_handle(graph, buffer);
  if (!buffer_entry) {
    log_error("RenderGraph export buffer has invalid handle");
    return;
  }
  if (!buffer_entry->exported) {
    buffer_entry->exported = true_v;
    vector_push_VkrRgBufferHandle(&graph->export_buffers, buffer);
  }
}

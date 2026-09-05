#include "renderer/vkr_render_graph_internal.h"

#include "core/logger.h"
#include "defines.h"
#include "renderer/vkr_render_packet.h"

vkr_internal int64_t vkr_rg_find_image_index(VkrRenderGraph *graph,
                                             String8 name) {
  if (!graph) {
    return (int64_t)-1;
  }
  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (string8_equals(&image->name, &name)) {
      return (int64_t)i;
    }
  }
  return (int64_t)-1;
}

vkr_internal int64_t vkr_rg_find_buffer_index(VkrRenderGraph *graph,
                                              String8 name) {
  if (!graph) {
    return (int64_t)-1;
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (string8_equals(&buffer->name, &name)) {
      return (int64_t)i;
    }
  }
  return (int64_t)-1;
}

vkr_internal bool8_t vkr_rg_image_desc_equal(const VkrRgImageDesc *a,
                                             const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type &&
         a->flags == b->flags;
}

vkr_internal bool8_t vkr_rg_buffer_desc_equal(const VkrRgBufferDesc *a,
                                              const VkrRgBufferDesc *b) {
  return a->size == b->size && a->usage.set == b->usage.set &&
         a->flags == b->flags;
}

vkr_internal bool8_t
vkr_rg_retained_image_flags_valid(String8 name, VkrRgResourceFlags flags) {
  if ((flags & VKR_RG_RESOURCE_FLAG_RETAINED) == 0u)
    return true_v;
  if ((flags & VKR_RG_RESOURCE_RETAINED_EXCLUSIONS) == 0u)
    return true_v;
  log_error("RenderGraph image '%.*s': RETAINED cannot combine with "
            "TRANSIENT, EXTERNAL, HISTORY, or PER_FRAME_SLOT",
            (int)name.length, name.str);
  return false_v;
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

const VkrRgImageUse *vkr_rg_pass_find_image_use(const VkrRgPassDesc *pass,
                                                uint32_t binding,
                                                uint32_t array_index) {
  if (!pass) {
    return NULL;
  }
  for (uint64_t i = 0; i < pass->image_reads.length; ++i) {
    const VkrRgImageUse *use =
        vector_get_VkrRgImageUse((Vector_VkrRgImageUse *)&pass->image_reads, i);
    if (use && use->binding == binding && use->array_index == array_index) {
      return use;
    }
  }
  for (uint64_t i = 0; i < pass->image_writes.length; ++i) {
    const VkrRgImageUse *use = vector_get_VkrRgImageUse(
        (Vector_VkrRgImageUse *)&pass->image_writes, i);
    if (use && use->binding == binding && use->array_index == array_index) {
      return use;
    }
  }
  return NULL;
}

const VkrRgBufferUse *vkr_rg_pass_find_buffer_use(const VkrRgPassDesc *pass,
                                                  uint32_t binding,
                                                  uint32_t array_index) {
  if (!pass) {
    return NULL;
  }
  for (uint64_t i = 0; i < pass->buffer_reads.length; ++i) {
    const VkrRgBufferUse *use = vector_get_VkrRgBufferUse(
        (Vector_VkrRgBufferUse *)&pass->buffer_reads, i);
    if (use && use->binding == binding && use->array_index == array_index) {
      return use;
    }
  }
  for (uint64_t i = 0; i < pass->buffer_writes.length; ++i) {
    const VkrRgBufferUse *use = vector_get_VkrRgBufferUse(
        (Vector_VkrRgBufferUse *)&pass->buffer_writes, i);
    if (use && use->binding == binding && use->array_index == array_index) {
      return use;
    }
  }
  return NULL;
}

VkrRgResourceInstanceDomain
vkr_rg_resource_instance_domain(VkrRgResourceFlags flags) {
  if (flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE)
    return VKR_RG_RESOURCE_INSTANCE_PER_IMAGE;
  if (flags &
      (VKR_RG_RESOURCE_FLAG_TRANSIENT | VKR_RG_RESOURCE_FLAG_PER_FRAME_SLOT |
       VKR_RG_RESOURCE_FLAG_HISTORY))
    return VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT;
  return VKR_RG_RESOURCE_INSTANCE_SINGLE;
}

void vkr_rg_set_packet(VkrRenderGraph *graph, const VkrRenderPacket *packet) {
  graph->packet = packet;
}

void vkr_rg_set_retained_state_provider(
    VkrRenderGraph *graph, const VkrRgRetainedStateProvider *provider) {
  if (!graph)
    return;
  graph->retained_provider =
      provider ? *provider : (VkrRgRetainedStateProvider){0};
}

void vkr_rg_commit_retained_state(VkrRenderGraph *graph) {
  if (!graph || !graph->retained_provider.commit || !graph->subresource_states)
    return;
  const uint32_t image_count = (uint32_t)graph->images.length;
  for (uint32_t i = 0; i < image_count; ++i) {
    const VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (!image || !(image->desc.flags & VKR_RG_RESOURCE_FLAG_RETAINED))
      continue;
    const uint32_t offset = graph->image_state_offsets[i];
    const uint32_t count = vkr_rg_image_subresource_count(image);
    const uint32_t instance = graph->retained_instance_indices
                                  ? graph->retained_instance_indices[i]
                                  : 0u;
    for (uint32_t s = 0; s < count; ++s) {
      const VkrRgSubresourceState *terminal =
          &graph->subresource_states[offset + s];
      const VkrRgRetainedState state = {
          .access = terminal->access,
          .stages = terminal->stages,
          .layout = terminal->layout,
          .content_valid = graph->retained_content_valid
                               ? graph->retained_content_valid[offset + s]
                               : false_v,
      };
      graph->retained_provider.commit(graph->retained_provider.context, i,
                                      instance, s, &state);
    }
  }
}

void vkr_rg_reset_passes(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  VkrAllocator *allocator =
      graph->frame_allocator ? graph->frame_allocator : graph->allocator;
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
      vkr_allocator_free(allocator, pass->desc.name.str,
                         pass->desc.name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  vector_clear_VkrRgPass(&graph->passes);
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
}

bool8_t vkr_rg_executor_registry_init(VkrRgExecutorRegistry *reg,
                                      VkrAllocator *allocator) {
  if (!reg || !allocator) {
    log_error("RenderGraph executor registry init failed: invalid args");
    return false_v;
  }

  *reg = (VkrRgExecutorRegistry){0};
  reg->allocator = allocator;
  reg->entries = (Vector_VkrRgPassExecutor){.allocator = allocator};
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

  if (entry->id == 0u) {
    log_error("RenderGraph executor '%.*s' requires a non-zero id",
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
    if (existing->id == entry->id) {
      log_error("RenderGraph executor id %u is already registered as '%.*s'",
                entry->id, (int)existing->name.length, existing->name.str);
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

  if (!vector_push_VkrRgPassExecutor(&reg->entries, stored)) {
    vkr_allocator_free(reg->allocator, stored.name.str, stored.name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  return true_v;
}

const VkrRgPassExecutor *
vkr_rg_executor_registry_find(const VkrRgExecutorRegistry *reg, String8 name) {
  if (!reg || !reg->initialized || name.length == 0) {
    return NULL;
  }

  for (uint64_t i = 0; i < reg->entries.length; ++i) {
    VkrRgPassExecutor *entry = vector_get_VkrRgPassExecutor(&reg->entries, i);
    if (string8_equals(&entry->name, &name)) {
      return entry;
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
  graph->frame_allocator = allocator;
  graph->images = (Vector_VkrRgImage){.allocator = allocator};
  graph->buffers = (Vector_VkrRgBuffer){.allocator = allocator};
  graph->passes = (Vector_VkrRgPass){.allocator = allocator};
  graph->export_images = (Vector_VkrRgImageHandle){.allocator = allocator};
  graph->export_buffers = (Vector_VkrRgBufferHandle){.allocator = allocator};
  graph->execution_order = (Vector_uint32_t){.allocator = allocator};
  graph->terminal_image_barriers =
      (Vector_VkrRgImageBarrier){.allocator = allocator};
  graph->present_image = VKR_RG_IMAGE_HANDLE_INVALID;
  return graph;
}

bool8_t vkr_rg_set_frame_allocator(VkrRenderGraph *graph,
                                   VkrAllocator *allocator) {
  if (!graph || !allocator || graph->passes.length > 0 ||
      graph->frame_scope_active || !vkr_allocator_supports_scopes(allocator)) {
    log_error("RenderGraph frame allocator setup failed: invalid state or "
              "allocator without scopes");
    return false_v;
  }

  vector_destroy_VkrRgPass(&graph->passes);
  graph->frame_allocator = allocator;
  graph->passes = (Vector_VkrRgPass){.allocator = allocator};
  return true_v;
}

void vkr_rg_destroy(VkrRenderGraph *graph) {
  if (!graph) {
    return;
  }

  vkr_rg_reset_passes(graph);
  if (graph->frame_scope_active) {
    vkr_allocator_end_scope(&graph->frame_scope,
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    graph->frame_scope_active = false_v;
    graph->passes = (Vector_VkrRgPass){0};
  }

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (image->name.str) {
      vkr_allocator_free(graph->allocator, image->name.str,
                         image->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (buffer->name.str) {
      vkr_allocator_free(graph->allocator, buffer->name.str,
                         buffer->name.length + 1,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }

  // Persistent barrier-generation storage; reused across frames, so it is only
  // released here.
  if (graph->subresource_states) {
    vkr_allocator_free(graph->allocator, graph->subresource_states,
                       sizeof(VkrRgSubresourceState) *
                           graph->subresource_state_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->retained_content_valid) {
    vkr_allocator_free(graph->allocator, graph->retained_content_valid,
                       sizeof(bool8_t) * graph->retained_content_valid_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->retained_instance_indices) {
    vkr_allocator_free(graph->allocator, graph->retained_instance_indices,
                       sizeof(uint32_t) *
                           graph->retained_instance_index_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->image_state_offsets) {
    vkr_allocator_free(graph->allocator, graph->image_state_offsets,
                       sizeof(uint32_t) * graph->image_state_offset_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->image_touch_tokens) {
    vkr_allocator_free(graph->allocator, graph->image_touch_tokens,
                       sizeof(uint32_t) * graph->touched_image_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->touched_image_indices) {
    vkr_allocator_free(graph->allocator, graph->touched_image_indices,
                       sizeof(uint32_t) * graph->touched_image_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->buffer_states) {
    vkr_allocator_free(graph->allocator, graph->buffer_states,
                       sizeof(VkrRgBufferState) * graph->buffer_state_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (graph->touched_buffer_indices) {
    vkr_allocator_free(graph->allocator, graph->touched_buffer_indices,
                       sizeof(uint32_t) * graph->touched_buffer_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vector_destroy_VkrRgImage(&graph->images);
  vector_destroy_VkrRgBuffer(&graph->buffers);
  vector_destroy_VkrRgPass(&graph->passes);
  vector_destroy_VkrRgImageHandle(&graph->export_images);
  vector_destroy_VkrRgBufferHandle(&graph->export_buffers);
  vector_destroy_uint32_t(&graph->execution_order);
  vector_destroy_VkrRgImageBarrier(&graph->terminal_image_barriers);

  vkr_allocator_free(graph->allocator, graph, sizeof(VkrRenderGraph),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}

bool8_t vkr_rg_begin_frame(VkrRenderGraph *graph,
                           const VkrRenderGraphFrameInfo *frame) {
  graph->frame_info = *frame;
  graph->packet = NULL;
  vkr_rg_clear_compiled(graph);

  if (graph->frame_allocator != graph->allocator) {
    if (graph->frame_scope_active) {
      vkr_allocator_end_scope(&graph->frame_scope,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      graph->frame_scope_active = false_v;
    }
    graph->frame_scope = vkr_allocator_begin_scope(graph->frame_allocator);
    if (!vkr_allocator_scope_is_valid(&graph->frame_scope)) {
      log_error("RenderGraph begin frame failed: frame allocation scope");
      graph->passes = (Vector_VkrRgPass){.allocator = graph->frame_allocator};
      return false_v;
    }
    graph->frame_scope_active = true_v;
    graph->passes = (Vector_VkrRgPass){.allocator = graph->frame_allocator};
  }

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    image->declared_this_frame = false_v;
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    buffer->declared_this_frame = false_v;
  }

  if (graph->frame_allocator == graph->allocator) {
    vkr_rg_reset_passes(graph);
  }
  vkr_rg_reset_exports(graph);
  return true_v;
}

void vkr_rg_end_frame(VkrRenderGraph *graph) { graph->packet = NULL; }

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
  if (!vkr_rg_retained_image_flags_valid(name, desc->flags))
    return VKR_RG_IMAGE_HANDLE_INVALID;

  int64_t index = vkr_rg_find_image_index(graph, name);
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

  if (graph->images.length == UINT32_MAX) {
    return VKR_RG_IMAGE_HANDLE_INVALID;
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
  if (!vector_push_VkrRgImage(&graph->images, image)) {
    vkr_allocator_free(graph->allocator, stored.str, stored.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

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
  if (!vkr_rg_retained_image_flags_valid(name, resolved_desc.flags))
    return VKR_RG_IMAGE_HANDLE_INVALID;

  int64_t index = vkr_rg_find_image_index(graph, name);
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

  if (graph->images.length == UINT32_MAX) {
    return VKR_RG_IMAGE_HANDLE_INVALID;
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
  if (!vector_push_VkrRgImage(&graph->images, image)) {
    vkr_allocator_free(graph->allocator, stored.str, stored.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  uint32_t id = (uint32_t)graph->images.length;
  return (VkrRgImageHandle){id, image.generation};
}

bool8_t vkr_rg_imported_image_add_usage(VkrRenderGraph *graph,
                                        VkrRgImageHandle handle,
                                        VkrTextureUsageBits usage) {
  if (!graph || !vkr_rg_image_handle_valid(handle) ||
      handle.id > graph->images.length) {
    return false_v;
  }
  VkrRgImage *image = vector_get_VkrRgImage(&graph->images, handle.id - 1u);
  if (image->generation != handle.generation || !image->imported) {
    return false_v;
  }
  bitset8_set(&image->desc.usage, usage);
  return true_v;
}

VkrRgBufferHandle vkr_rg_create_buffer(VkrRenderGraph *graph, String8 name,
                                       const VkrRgBufferDesc *desc) {
  if (!graph || !desc || name.length == 0) {
    log_error("RenderGraph create buffer failed: invalid args");
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }
  if (desc->flags & VKR_RG_RESOURCE_FLAG_RETAINED) {
    log_error("RenderGraph buffer '%.*s': RETAINED currently supports images "
              "only",
              (int)name.length, name.str);
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  int64_t index = vkr_rg_find_buffer_index(graph, name);
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
    buffer->declared_this_frame = true_v;
    buffer->imported = false_v;
    buffer->imported_handle = NULL;
    return (VkrRgBufferHandle){(uint32_t)index + 1, buffer->generation};
  }

  if (graph->buffers.length == UINT32_MAX) {
    return VKR_RG_BUFFER_HANDLE_INVALID;
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
  if (!vector_push_VkrRgBuffer(&graph->buffers, buffer)) {
    vkr_allocator_free(graph->allocator, stored.str, stored.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

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

  int64_t index = vkr_rg_find_buffer_index(graph, name);
  if (index >= 0) {
    VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&graph->buffers, (uint32_t)index);
    buffer->desc.flags |= VKR_RG_RESOURCE_FLAG_EXTERNAL;
    buffer->imported = true_v;
    buffer->imported_handle = handle;
    buffer->imported_access = current_access;
    buffer->declared_this_frame = true_v;
    return (VkrRgBufferHandle){(uint32_t)index + 1, buffer->generation};
  }

  if (graph->buffers.length == UINT32_MAX) {
    return VKR_RG_BUFFER_HANDLE_INVALID;
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
  if (!vector_push_VkrRgBuffer(&graph->buffers, buffer)) {
    vkr_allocator_free(graph->allocator, stored.str, stored.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  uint32_t id = (uint32_t)graph->buffers.length;
  return (VkrRgBufferHandle){id, buffer.generation};
}

bool8_t vkr_rg_imported_buffer_add_usage(VkrRenderGraph *graph,
                                         VkrRgBufferHandle handle,
                                         VkrBufferUsageBits usage) {
  if (!graph || !vkr_rg_buffer_handle_valid(handle) ||
      handle.id > graph->buffers.length) {
    return false_v;
  }
  VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, handle.id - 1u);
  if (buffer->generation != handle.generation || !buffer->imported) {
    return false_v;
  }
  bitset8_set(&buffer->desc.usage, usage);
  return true_v;
}

VkrRgPassBuilder vkr_rg_add_pass(VkrRenderGraph *graph, VkrRgPassType type,
                                 String8 name) {
  if (!graph || name.length == 0 || graph->passes.length == INT32_MAX) {
    log_error("RenderGraph add pass failed: invalid args");
    return (VkrRgPassBuilder){0};
  }

  VkrAllocator *allocator =
      graph->frame_allocator ? graph->frame_allocator : graph->allocator;
  String8 stored = string8_duplicate(allocator, &name);
  if (!stored.str) {
    log_error("RenderGraph add pass failed: name alloc failed");
    return (VkrRgPassBuilder){0};
  }

  VkrRgPass pass = {0};
  pass.desc = (VkrRgPassDesc){0};
  pass.desc.name = stored;
  pass.desc.type = type;
  pass.desc.color_attachments =
      (Vector_VkrRgAttachment){.allocator = allocator};
  pass.desc.image_reads = (Vector_VkrRgImageUse){.allocator = allocator};
  pass.desc.image_writes = (Vector_VkrRgImageUse){.allocator = allocator};
  pass.desc.buffer_reads = (Vector_VkrRgBufferUse){.allocator = allocator};
  pass.desc.buffer_writes = (Vector_VkrRgBufferUse){.allocator = allocator};

  pass.out_edges = (Vector_uint32_t){.allocator = allocator};
  pass.in_edges = (Vector_uint32_t){.allocator = allocator};
  pass.pre_image_barriers = (Vector_VkrRgImageBarrier){.allocator = allocator};
  pass.pre_buffer_barriers =
      (Vector_VkrRgBufferBarrier){.allocator = allocator};

  if (!vector_push_VkrRgPass(&graph->passes, pass)) {
    vkr_allocator_free(allocator, stored.str, stored.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return (VkrRgPassBuilder){0};
  }

  return (VkrRgPassBuilder){.graph = graph,
                            .pass_index = (uint32_t)graph->passes.length - 1};
}

vkr_internal VkrRgPass *vkr_rg_builder_get_pass(VkrRgPassBuilder *pb) {
  if (!pb || !pb->graph) {
    return NULL;
  }
  if (pb->pass_index >= pb->graph->passes.length) {
    return NULL;
  }
  return vector_get_VkrRgPass(&pb->graph->passes, pb->pass_index);
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

bool8_t vkr_rg_pass_add_color_attachment(VkrRgPassBuilder *pb,
                                         VkrRgImageHandle image,
                                         const VkrRgAttachmentDesc *desc) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }

  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' color attachment has invalid image",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  VkrRgAttachmentDesc local_desc =
      desc ? *desc : (VkrRgAttachmentDesc){.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  VkrRgAttachment attachment = {.image = image, .desc = local_desc};
  return vector_push_VkrRgAttachment(&pass->desc.color_attachments, attachment);
}

bool8_t vkr_rg_pass_set_depth_attachment(VkrRgPassBuilder *pb,
                                         VkrRgImageHandle image,
                                         const VkrRgAttachmentDesc *desc,
                                         bool8_t read_only) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }

  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' depth attachment has invalid image",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  VkrRgAttachmentDesc local_desc =
      desc ? *desc : (VkrRgAttachmentDesc){.slice = VKR_RG_IMAGE_SLICE_DEFAULT};
  pass->desc.depth_attachment = (VkrRgAttachment){
      .image = image, .desc = local_desc, .read_only = read_only};
  pass->desc.has_depth_attachment = true_v;
  return true_v;
}

bool8_t vkr_rg_pass_read_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                               VkrRgImageAccessFlags access, uint32_t binding,
                               uint32_t array_index) {
  return vkr_rg_pass_read_image_at_stages(
      pb, image, access, vkr_gpu_stages_for_image_access(access, false_v),
      binding, array_index);
}

bool8_t vkr_rg_pass_read_image_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }
  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' read has invalid image handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .stages = stages,
                       .binding = binding,
                       .array_index = array_index};
  return vector_push_VkrRgImageUse(&pass->desc.image_reads, use);
}

bool8_t vkr_rg_pass_read_image_slice(VkrRgPassBuilder *pb,
                                     VkrRgImageHandle image,
                                     VkrRgImageAccessFlags access,
                                     uint32_t binding, uint32_t array_index,
                                     VkrRgImageSlice slice) {
  return vkr_rg_pass_read_image_slice_at_stages(
      pb, image, access, vkr_gpu_stages_for_image_access(access, false_v),
      binding, array_index, slice);
}

bool8_t vkr_rg_pass_read_image_slice_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index,
    VkrRgImageSlice slice) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass || !vkr_rg_image_from_handle(pb->graph, image)) {
    return false_v;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .stages = stages,
                       .binding = binding,
                       .array_index = array_index,
                       .slice = slice,
                       .has_slice = true_v};
  return vector_push_VkrRgImageUse(&pass->desc.image_reads, use);
}

bool8_t vkr_rg_pass_write_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                                VkrRgImageAccessFlags access, uint32_t binding,
                                uint32_t array_index) {
  return vkr_rg_pass_write_image_at_stages(
      pb, image, access, vkr_gpu_stages_for_image_access(access, false_v),
      binding, array_index);
}

bool8_t vkr_rg_pass_write_image_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }
  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' write has invalid image handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .stages = stages,
                       .binding = binding,
                       .array_index = array_index};
  return vector_push_VkrRgImageUse(&pass->desc.image_writes, use);
}

bool8_t vkr_rg_pass_write_image_slice_at_stages(
    VkrRgPassBuilder *pb, VkrRgImageHandle image, VkrRgImageAccessFlags access,
    VkrGpuStageFlags stages, uint32_t binding, uint32_t array_index,
    VkrRgImageSlice slice) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass)
    return false_v;
  if (!vkr_rg_image_from_handle(pb->graph, image)) {
    log_error("RenderGraph pass '%.*s' write has invalid image handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  VkrRgImageUse use = {.image = image,
                       .access = access,
                       .stages = stages,
                       .binding = binding,
                       .array_index = array_index,
                       .slice = slice,
                       .has_slice = true_v};
  return vector_push_VkrRgImageUse(&pass->desc.image_writes, use);
}

bool8_t vkr_rg_pass_read_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                                VkrRgBufferAccessFlags access, uint32_t binding,
                                uint32_t array_index) {
  return vkr_rg_pass_read_buffer_at_stages(
      pb, buffer, access, vkr_gpu_stages_for_buffer_access(access, false_v),
      binding, array_index);
}

bool8_t vkr_rg_pass_read_buffer_at_stages(VkrRgPassBuilder *pb,
                                          VkrRgBufferHandle buffer,
                                          VkrRgBufferAccessFlags access,
                                          VkrGpuStageFlags stages,
                                          uint32_t binding,
                                          uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }
  if (!vkr_rg_buffer_from_handle(pb->graph, buffer)) {
    log_error("RenderGraph pass '%.*s' read has invalid buffer handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  VkrRgBufferUse use = {.buffer = buffer,
                        .access = access,
                        .stages = stages,
                        .binding = binding,
                        .array_index = array_index};
  return vector_push_VkrRgBufferUse(&pass->desc.buffer_reads, use);
}

bool8_t vkr_rg_pass_write_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                                 VkrRgBufferAccessFlags access,
                                 uint32_t binding, uint32_t array_index) {
  return vkr_rg_pass_write_buffer_at_stages(
      pb, buffer, access, vkr_gpu_stages_for_buffer_access(access, false_v),
      binding, array_index);
}

bool8_t vkr_rg_pass_write_buffer_at_stages(VkrRgPassBuilder *pb,
                                           VkrRgBufferHandle buffer,
                                           VkrRgBufferAccessFlags access,
                                           VkrGpuStageFlags stages,
                                           uint32_t binding,
                                           uint32_t array_index) {
  VkrRgPass *pass = vkr_rg_builder_get_pass(pb);
  if (!pass) {
    return false_v;
  }
  if (!vkr_rg_buffer_from_handle(pb->graph, buffer)) {
    log_error("RenderGraph pass '%.*s' write has invalid buffer handle",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  VkrRgBufferUse use = {.buffer = buffer,
                        .access = access,
                        .stages = stages,
                        .binding = binding,
                        .array_index = array_index};
  return vector_push_VkrRgBufferUse(&pass->desc.buffer_writes, use);
}

bool8_t vkr_rg_set_present_image(VkrRenderGraph *graph,
                                 VkrRgImageHandle image) {
  if (!graph) {
    return false_v;
  }

  if (!vkr_rg_image_from_handle(graph, image)) {
    log_error("RenderGraph set present image has invalid handle");
    return false_v;
  }

  graph->present_image = image;
  return true_v;
}

bool8_t vkr_rg_export_image(VkrRenderGraph *graph, VkrRgImageHandle image) {
  if (!graph) {
    return false_v;
  }
  VkrRgImage *image_entry = vkr_rg_image_from_handle(graph, image);
  if (!image_entry) {
    log_error("RenderGraph export image has invalid handle");
    return false_v;
  }
  if (!image_entry->exported) {
    if (!vector_push_VkrRgImageHandle(&graph->export_images, image)) {
      return false_v;
    }
    image_entry->exported = true_v;
  }
  return true_v;
}

bool8_t vkr_rg_export_buffer(VkrRenderGraph *graph, VkrRgBufferHandle buffer) {
  if (!graph) {
    return false_v;
  }
  VkrRgBuffer *buffer_entry = vkr_rg_buffer_from_handle(graph, buffer);
  if (!buffer_entry) {
    log_error("RenderGraph export buffer has invalid handle");
    return false_v;
  }
  if (!buffer_entry->exported) {
    if (!vector_push_VkrRgBufferHandle(&graph->export_buffers, buffer)) {
      return false_v;
    }
    buffer_entry->exported = true_v;
  }
  return true_v;
}

#include "renderer/vkr_render_graph_internal.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/vkr_math.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"

typedef struct VkrRgDependencyState {
  int32_t last_writer;
  Vector_uint32_t last_readers;
} VkrRgDependencyState;

vkr_internal bool8_t vkr_rg_usage_has(const VkrTextureUsageFlags *usage,
                                      VkrTextureUsageBits bit);
vkr_internal bool8_t vkr_rg_buffer_usage_has(const VkrBufferUsageFlags *usage,
                                             VkrBufferUsageBits bit);
vkr_internal void
vkr_rg_warn_read_before_write_images(VkrRenderGraph *graph,
                                     const VkrRgDependencyState *states,
                                     uint32_t image_count);
vkr_internal void
vkr_rg_warn_read_before_write_buffers(VkrRenderGraph *graph,
                                      const VkrRgDependencyState *states,
                                      uint32_t buffer_count);

vkr_internal void vkr_rg_dependency_state_init(VkrRgDependencyState *state,
                                               VkrAllocator *allocator) {
  state->last_writer = -1;
  state->last_readers = vector_create_uint32_t(allocator);
}

vkr_internal void vkr_rg_dependency_state_destroy(VkrRgDependencyState *state) {
  vector_destroy_uint32_t(&state->last_readers);
}

vkr_internal bool8_t vkr_rg_edge_exists(const Vector_uint32_t *edges,
                                        uint32_t to) {
  for (uint64_t i = 0; i < edges->length; ++i) {
    if (edges->data[i] == to) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_rg_add_edge(VkrRenderGraph *graph, uint32_t from,
                                  uint32_t to) {
  if (from == to) {
    log_error("RenderGraph add edge failed: from == to");
    return;
  }

  VkrRgPass *from_pass = vector_get_VkrRgPass(&graph->passes, from);
  if (!vkr_rg_edge_exists(&from_pass->out_edges, to)) {
    vector_push_uint32_t(&from_pass->out_edges, to);
    VkrRgPass *to_pass = vector_get_VkrRgPass(&graph->passes, to);
    vector_push_uint32_t(&to_pass->in_edges, from);
  }
}

vkr_internal void vkr_rg_add_reader_unique(Vector_uint32_t *readers,
                                           uint32_t pass) {
  for (uint64_t i = 0; i < readers->length; ++i) {
    if (readers->data[i] == pass) {
      return;
    }
  }
  vector_push_uint32_t(readers, pass);
}

vkr_internal bool8_t vkr_rg_image_is_depth(const VkrRgImage *image) {
  if (!image) {
    return false_v;
  }
  return vkr_rg_usage_has(&image->desc.usage,
                          VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT);
}

vkr_internal VkrTextureLayout vkr_rg_layout_for_image_access(
    const VkrRgImage *image, VkrRgImageAccessFlags access) {
  bool8_t is_depth = vkr_rg_image_is_depth(image);
  if (access & VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT) {
    return VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT) {
    return VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY) {
    return VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  }
  if (access &
      (VKR_RG_IMAGE_ACCESS_STORAGE_READ | VKR_RG_IMAGE_ACCESS_STORAGE_WRITE)) {
    return VKR_TEXTURE_LAYOUT_GENERAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_SAMPLED) {
    return is_depth ? VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                    : VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_TRANSFER_DST) {
    return VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_TRANSFER_SRC) {
    return VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }
  if (access & VKR_RG_IMAGE_ACCESS_PRESENT) {
    return VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR;
  }
  return VKR_TEXTURE_LAYOUT_UNDEFINED;
}

vkr_internal bool8_t vkr_rg_validate_image_usage_bit(const VkrRgPass *pass,
                                                     const VkrRgImage *image,
                                                     VkrTextureUsageBits bit,
                                                     const char *access_label,
                                                     const char *usage_label) {
  if (!pass || !image) {
    return false_v;
  }

  if (vkr_rg_usage_has(&image->desc.usage, bit)) {
    return true_v;
  }

  log_error("RenderGraph pass '%.*s' uses image '%.*s' as %s without %s usage",
            (int)pass->desc.name.length, pass->desc.name.str,
            (int)image->name.length, image->name.str, access_label,
            usage_label);
  return false_v;
}

vkr_internal bool8_t vkr_rg_validate_image_access_usage(
    const VkrRgPass *pass, const VkrRgImage *image,
    VkrRgImageAccessFlags access) {
  bool8_t ok = true_v;
  if (access & VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT) {
    ok &= vkr_rg_validate_image_usage_bit(
        pass, image, VKR_TEXTURE_USAGE_COLOR_ATTACHMENT, "color attachment",
        "COLOR_ATTACHMENT");
  }
  if (access & (VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT |
                VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY)) {
    ok &= vkr_rg_validate_image_usage_bit(
        pass, image, VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT,
        "depth attachment", "DEPTH_STENCIL_ATTACHMENT");
  }
  if (access & VKR_RG_IMAGE_ACCESS_SAMPLED) {
    ok &= vkr_rg_validate_image_usage_bit(
        pass, image, VKR_TEXTURE_USAGE_SAMPLED, "sampled", "SAMPLED");
  }
  if (access & VKR_RG_IMAGE_ACCESS_TRANSFER_SRC) {
    ok &= vkr_rg_validate_image_usage_bit(pass, image,
                                          VKR_TEXTURE_USAGE_TRANSFER_SRC,
                                          "transfer src", "TRANSFER_SRC");
  }
  if (access & VKR_RG_IMAGE_ACCESS_TRANSFER_DST) {
    ok &= vkr_rg_validate_image_usage_bit(pass, image,
                                          VKR_TEXTURE_USAGE_TRANSFER_DST,
                                          "transfer dst", "TRANSFER_DST");
  }
  return ok;
}

vkr_internal bool8_t vkr_rg_validate_attachment_slice(
    const VkrRgPass *pass, const VkrRgImage *image,
    const VkrRgAttachmentDesc *desc, const char *label) {
  assert_log(pass != NULL, "pass is NULL");
  assert_log(image != NULL, "image is NULL");
  assert_log(desc != NULL, "desc is NULL");
  assert_log(label != NULL, "label is NULL");

  if (desc->slice.layer_count == 0) {
    log_error(
        "RenderGraph pass '%.*s' %s attachment for '%.*s' has layer_count=0",
        (int)pass->desc.name.length, pass->desc.name.str, label,
        (int)image->name.length, image->name.str);
    return false_v;
  }

  if (image->desc.mip_levels > 0 &&
      desc->slice.mip_level >= image->desc.mip_levels) {
    log_error("RenderGraph pass '%.*s' %s attachment for '%.*s' uses mip %u "
              "but image has %u mip levels",
              (int)pass->desc.name.length, pass->desc.name.str, label,
              (int)image->name.length, image->name.str, desc->slice.mip_level,
              image->desc.mip_levels);
    return false_v;
  }

  if (image->desc.layers > 0) {
    uint64_t end =
        (uint64_t)desc->slice.base_layer + (uint64_t)desc->slice.layer_count;
    if (desc->slice.base_layer >= image->desc.layers ||
        end > image->desc.layers) {
      log_error("RenderGraph pass '%.*s' %s attachment for '%.*s' uses layers "
                "[%u..%u) but image has %u layers",
                (int)pass->desc.name.length, pass->desc.name.str, label,
                (int)image->name.length, image->name.str,
                desc->slice.base_layer, (uint32_t)end, image->desc.layers);
      return false_v;
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_validate_buffer_usage_bit(const VkrRgPass *pass,
                                                      const VkrRgBuffer *buffer,
                                                      VkrBufferUsageBits bit,
                                                      const char *access_label,
                                                      const char *usage_label) {
  if (!pass || !buffer) {
    return false_v;
  }

  if (vkr_rg_buffer_usage_has(&buffer->desc.usage, bit)) {
    return true_v;
  }

  log_error("RenderGraph pass '%.*s' uses buffer '%.*s' as %s without %s usage",
            (int)pass->desc.name.length, pass->desc.name.str,
            (int)buffer->name.length, buffer->name.str, access_label,
            usage_label);
  return false_v;
}

vkr_internal bool8_t vkr_rg_validate_buffer_access_usage(
    const VkrRgPass *pass, const VkrRgBuffer *buffer,
    VkrRgBufferAccessFlags access) {
  bool8_t ok = true_v;
  if (access & VKR_BUFFER_ACCESS_VERTEX) {
    ok &= vkr_rg_validate_buffer_usage_bit(pass, buffer,
                                           VKR_BUFFER_USAGE_VERTEX_BUFFER,
                                           "vertex", "VERTEX_BUFFER");
  }
  if (access & VKR_BUFFER_ACCESS_INDEX) {
    ok &= vkr_rg_validate_buffer_usage_bit(
        pass, buffer, VKR_BUFFER_USAGE_INDEX_BUFFER, "index", "INDEX_BUFFER");
  }
  if (access & VKR_BUFFER_ACCESS_UNIFORM) {
    bool8_t has_uniform =
        vkr_rg_buffer_usage_has(&buffer->desc.usage,
                                VKR_BUFFER_USAGE_UNIFORM) ||
        vkr_rg_buffer_usage_has(&buffer->desc.usage,
                                VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER);
    if (!has_uniform) {
      log_error("RenderGraph pass '%.*s' uses buffer '%.*s' as uniform without "
                "UNIFORM usage",
                (int)pass->desc.name.length, pass->desc.name.str,
                (int)buffer->name.length, buffer->name.str);
      ok = false_v;
    }
  }
  if (access &
      (VKR_BUFFER_ACCESS_STORAGE_READ | VKR_BUFFER_ACCESS_STORAGE_WRITE)) {
    ok &= vkr_rg_validate_buffer_usage_bit(
        pass, buffer, VKR_BUFFER_USAGE_STORAGE, "storage", "STORAGE");
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_SRC) {
    ok &= vkr_rg_validate_buffer_usage_bit(pass, buffer,
                                           VKR_BUFFER_USAGE_TRANSFER_SRC,
                                           "transfer src", "TRANSFER_SRC");
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_DST) {
    ok &= vkr_rg_validate_buffer_usage_bit(pass, buffer,
                                           VKR_BUFFER_USAGE_TRANSFER_DST,
                                           "transfer dst", "TRANSFER_DST");
  }
  return ok;
}

vkr_internal bool8_t vkr_rg_validate_pass(VkrRenderGraph *graph,
                                          VkrRgPass *pass) {
  if (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) {
    return true_v;
  }

  if (pass->desc.type == VKR_RG_PASS_TYPE_GRAPHICS) {
    if (!pass->desc.has_depth_attachment &&
        pass->desc.color_attachments.length == 0) {
      log_error("RenderGraph pass '%.*s' missing attachments",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    VkrRgAttachment *att =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      log_error("RenderGraph pass '%.*s' has invalid color attachment",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_rg_validate_image_access_usage(
            pass, image, VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT) ||
        !vkr_rg_validate_attachment_slice(pass, image, &att->desc, "color")) {
      return false_v;
    }
  }

  if (pass->desc.has_depth_attachment) {
    VkrRgImage *image =
        vkr_rg_image_from_handle(graph, pass->desc.depth_attachment.image);
    if (!image) {
      log_error("RenderGraph pass '%.*s' has invalid depth attachment",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    VkrRgImageAccessFlags access = pass->desc.depth_attachment.read_only
                                       ? VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY
                                       : VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT;
    if (!vkr_rg_validate_image_access_usage(pass, image, access) ||
        !vkr_rg_validate_attachment_slice(
            pass, image, &pass->desc.depth_attachment.desc, "depth")) {
      return false_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
    VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      log_error("RenderGraph pass '%.*s' has invalid image read",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_rg_validate_image_access_usage(pass, image, use->access)) {
      return false_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      log_error("RenderGraph pass '%.*s' has invalid image write",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_rg_validate_image_access_usage(pass, image, use->access)) {
      return false_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
    VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      log_error("RenderGraph pass '%.*s' has invalid buffer read",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_rg_validate_buffer_access_usage(pass, buffer, use->access)) {
      return false_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      log_error("RenderGraph pass '%.*s' has invalid buffer write",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_rg_validate_buffer_access_usage(pass, buffer, use->access)) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal void vkr_rg_process_image_read(VkrRenderGraph *graph,
                                            VkrRgDependencyState *states,
                                            uint32_t pass_index,
                                            VkrRgImageHandle image) {
  uint32_t idx = image.id - 1;
  VkrRgDependencyState *state = &states[idx];
  if (state->last_writer >= 0) {
    vkr_rg_add_edge(graph, (uint32_t)state->last_writer, pass_index);
  }
  vkr_rg_add_reader_unique(&state->last_readers, pass_index);
}

vkr_internal void vkr_rg_process_image_write(VkrRenderGraph *graph,
                                             VkrRgDependencyState *states,
                                             uint32_t pass_index,
                                             VkrRgImageHandle image) {
  uint32_t idx = image.id - 1;
  VkrRgDependencyState *state = &states[idx];
  if (state->last_writer >= 0) {
    vkr_rg_add_edge(graph, (uint32_t)state->last_writer, pass_index);
  }
  for (uint64_t i = 0; i < state->last_readers.length; ++i) {
    vkr_rg_add_edge(graph, state->last_readers.data[i], pass_index);
  }
  vector_clear_uint32_t(&state->last_readers);
  state->last_writer = (int32_t)pass_index;
}

vkr_internal void vkr_rg_process_buffer_read(VkrRenderGraph *graph,
                                             VkrRgDependencyState *states,
                                             uint32_t pass_index,
                                             VkrRgBufferHandle buffer) {
  uint32_t idx = buffer.id - 1;
  VkrRgDependencyState *state = &states[idx];
  if (state->last_writer >= 0) {
    vkr_rg_add_edge(graph, (uint32_t)state->last_writer, pass_index);
  }
  vkr_rg_add_reader_unique(&state->last_readers, pass_index);
}

vkr_internal void vkr_rg_process_buffer_write(VkrRenderGraph *graph,
                                              VkrRgDependencyState *states,
                                              uint32_t pass_index,
                                              VkrRgBufferHandle buffer) {
  uint32_t idx = buffer.id - 1;
  VkrRgDependencyState *state = &states[idx];
  if (state->last_writer >= 0) {
    vkr_rg_add_edge(graph, (uint32_t)state->last_writer, pass_index);
  }
  for (uint64_t i = 0; i < state->last_readers.length; ++i) {
    vkr_rg_add_edge(graph, state->last_readers.data[i], pass_index);
  }
  vector_clear_uint32_t(&state->last_readers);
  state->last_writer = (int32_t)pass_index;
}

vkr_internal bool8_t vkr_rg_pass_writes_image(const VkrRgPass *pass,
                                              VkrRgImageHandle image) {
  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
    if (use->image.id == image.id &&
        use->image.generation == image.generation) {
      return true_v;
    }
  }

  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    VkrRgAttachment *att =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (att->image.id == image.id &&
        att->image.generation == image.generation) {
      return true_v;
    }
  }

  if (pass->desc.has_depth_attachment &&
      !pass->desc.depth_attachment.read_only) {
    const VkrRgAttachment *att = &pass->desc.depth_attachment;
    if (att->image.id == image.id &&
        att->image.generation == image.generation) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_rg_pass_writes_buffer(const VkrRgPass *pass,
                                               VkrRgBufferHandle buffer) {
  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    if (use->buffer.id == buffer.id &&
        use->buffer.generation == buffer.generation) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_rg_usage_has(const VkrTextureUsageFlags *usage,
                                      VkrTextureUsageBits bit) {
  if (!usage) {
    return false_v;
  }
  return bitset8_is_set(usage, (uint8_t)bit);
}

vkr_internal bool8_t vkr_rg_buffer_usage_has(const VkrBufferUsageFlags *usage,
                                             VkrBufferUsageBits bit) {
  if (!usage) {
    return false_v;
  }
  return bitset8_is_set(usage, (uint8_t)bit);
}

vkr_internal bool8_t
vkr_rg_image_allows_read_without_write(const VkrRgImage *image) {
  if (!image) {
    return false_v;
  }

  if (image->imported) {
    return true_v;
  }

  return (image->desc.flags & (VKR_RG_RESOURCE_FLAG_EXTERNAL |
                               VKR_RG_RESOURCE_FLAG_PERSISTENT)) != 0;
}

vkr_internal bool8_t
vkr_rg_buffer_allows_read_without_write(const VkrRgBuffer *buffer) {
  if (!buffer) {
    return false_v;
  }

  if (buffer->imported) {
    return true_v;
  }

  return (buffer->desc.flags & (VKR_RG_RESOURCE_FLAG_EXTERNAL |
                                VKR_RG_RESOURCE_FLAG_PERSISTENT)) != 0;
}

vkr_internal void
vkr_rg_warn_read_before_write_images(VkrRenderGraph *graph,
                                     const VkrRgDependencyState *states,
                                     uint32_t image_count) {
  if (!graph || !states) {
    return;
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    const VkrRgDependencyState *state = &states[i];
    if (state->last_writer >= 0 || state->last_readers.length == 0) {
      continue;
    }

    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (!image || vkr_rg_image_allows_read_without_write(image)) {
      continue;
    }

    uint32_t reader_index = state->last_readers.data[0];
    VkrRgPass *reader = vector_get_VkrRgPass(&graph->passes, reader_index);
    String8 reader_name = reader ? reader->desc.name : string8_lit("<unknown>");
    log_warn(
        "RenderGraph image '%.*s' is read by pass '%.*s' before any writes",
        (int)image->name.length, image->name.str, (int)reader_name.length,
        reader_name.str);
  }
}

vkr_internal void
vkr_rg_warn_read_before_write_buffers(VkrRenderGraph *graph,
                                      const VkrRgDependencyState *states,
                                      uint32_t buffer_count) {
  if (!graph || !states) {
    return;
  }

  for (uint32_t i = 0; i < buffer_count; ++i) {
    const VkrRgDependencyState *state = &states[i];
    if (state->last_writer >= 0 || state->last_readers.length == 0) {
      continue;
    }

    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (!buffer || vkr_rg_buffer_allows_read_without_write(buffer)) {
      continue;
    }

    uint32_t reader_index = state->last_readers.data[0];
    VkrRgPass *reader = vector_get_VkrRgPass(&graph->passes, reader_index);
    String8 reader_name = reader ? reader->desc.name : string8_lit("<unknown>");
    log_warn(
        "RenderGraph buffer '%.*s' is read by pass '%.*s' before any writes",
        (int)buffer->name.length, buffer->name.str, (int)reader_name.length,
        reader_name.str);
  }
}

vkr_internal uint32_t vkr_rg_resolve_image_count(const VkrRenderGraph *graph,
                                                 const VkrRgImage *image) {
  if (!graph || !image) {
    return 1;
  }

  if ((image->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE) == 0) {
    return 1;
  }

  if (!graph->renderer) {
    return 1;
  }

  uint32_t count = vkr_renderer_window_attachment_count(graph->renderer);
  return count > 0 ? count : 1;
}

vkr_internal uint32_t vkr_rg_resolve_buffer_count(const VkrRenderGraph *graph,
                                                  const VkrRgBuffer *buffer) {
  if (!graph || !buffer) {
    return 1;
  }

  if ((buffer->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE) == 0) {
    return 1;
  }

  if (!graph->renderer) {
    return 1;
  }

  uint32_t count = vkr_renderer_window_attachment_count(graph->renderer);
  return count > 0 ? count : 1;
}

vkr_internal uint32_t vkr_rg_format_bytes_per_pixel(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return 1;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return 2;
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
  case VKR_TEXTURE_FORMAT_R32_UINT:
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return 4;
  default:
    return 0;
  }
}

vkr_internal uint64_t
vkr_rg_calc_image_bytes_per_texture(const VkrRgImageDesc *desc) {
  if (!desc) {
    return 0;
  }

  uint32_t bytes_per_pixel = vkr_rg_format_bytes_per_pixel(desc->format);
  if (bytes_per_pixel == 0) {
    return 0;
  }

  uint32_t width = desc->width > 0 ? desc->width : 1;
  uint32_t height = desc->height > 0 ? desc->height : 1;
  uint32_t mip_levels = desc->mip_levels > 0 ? desc->mip_levels : 1;
  uint32_t layers = desc->layers > 0 ? desc->layers : 1;
  uint32_t samples = desc->samples > 0 ? (uint32_t)desc->samples : 1;

  if (desc->type == VKR_TEXTURE_TYPE_CUBE_MAP) {
    layers *= 6;
  }

  uint64_t texel_count = 0;
  for (uint32_t level = 0; level < mip_levels; ++level) {
    uint32_t level_width = width >> level;
    uint32_t level_height = height >> level;
    if (level_width == 0) {
      level_width = 1;
    }
    if (level_height == 0) {
      level_height = 1;
    }
    texel_count += (uint64_t)level_width * (uint64_t)level_height;
  }

  return texel_count * (uint64_t)layers * (uint64_t)samples *
         (uint64_t)bytes_per_pixel;
}

vkr_internal bool8_t vkr_rg_refresh_imported_textures(VkrRenderGraph *graph,
                                                      VkrRgImage *image,
                                                      uint32_t desired_count) {
  if (!graph || !image || !graph->renderer) {
    return false_v;
  }

  if (!image->textures || image->texture_count != desired_count) {
    if (image->textures) {
      vkr_allocator_free(graph->allocator, image->textures,
                         sizeof(VkrTextureOpaqueHandle) *
                             (uint64_t)image->texture_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }
    image->textures = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrTextureOpaqueHandle) * desired_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!image->textures) {
      image->texture_count = 0;
      return false_v;
    }
    image->texture_count = desired_count;
  }

  MemZero(image->textures,
          sizeof(VkrTextureOpaqueHandle) * (uint64_t)image->texture_count);

  if (vkr_string8_equals_cstr_i(&image->name, "swapchain")) {
    for (uint32_t i = 0; i < image->texture_count; ++i) {
      image->textures[i] =
          vkr_renderer_window_attachment_get(graph->renderer, i);
    }
  } else if (vkr_string8_equals_cstr_i(&image->name, "swapchain_depth")) {
    VkrTextureOpaqueHandle depth =
        vkr_renderer_depth_attachment_get(graph->renderer);
    for (uint32_t i = 0; i < image->texture_count; ++i) {
      image->textures[i] = depth;
    }
  } else if (image->imported_handle) {
    for (uint32_t i = 0; i < image->texture_count; ++i) {
      image->textures[i] = image->imported_handle;
    }
  } else {
    log_error("RenderGraph import '%.*s' has no source handle",
              (int)image->name.length, image->name.str);
    return false_v;
  }

  image->allocated_generation = image->generation;
  image->allocated_bytes_per_texture = 0;
  return true_v;
}

vkr_internal bool8_t vkr_rg_refresh_imported_buffers(VkrRenderGraph *graph,
                                                     VkrRgBuffer *buffer,
                                                     uint32_t desired_count) {
  if (!graph || !buffer || !graph->renderer) {
    return false_v;
  }

  if (!buffer->buffers || buffer->buffer_count != desired_count) {
    if (buffer->buffers) {
      vkr_allocator_free(graph->allocator, buffer->buffers,
                         sizeof(VkrBufferHandle) *
                             (uint64_t)buffer->buffer_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }
    buffer->buffers = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrBufferHandle) * desired_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!buffer->buffers) {
      buffer->buffer_count = 0;
      return false_v;
    }
    buffer->buffer_count = desired_count;
  }

  MemZero(buffer->buffers,
          sizeof(VkrBufferHandle) * (uint64_t)buffer->buffer_count);

  if (!buffer->imported_handle) {
    log_error("RenderGraph import '%.*s' has no source buffer handle",
              (int)buffer->name.length, buffer->name.str);
    return false_v;
  }

  for (uint32_t i = 0; i < buffer->buffer_count; ++i) {
    buffer->buffers[i] = buffer->imported_handle;
  }

  buffer->allocated_generation = buffer->generation;
  buffer->allocated_size = 0;
  return true_v;
}

vkr_internal bool8_t vkr_rg_allocate_image_textures(VkrRenderGraph *graph,
                                                    VkrRgImage *image,
                                                    uint32_t desired_count) {
  if (!graph || !image || !graph->renderer) {
    return false_v;
  }

  if (image->imported) {
    return vkr_rg_refresh_imported_textures(graph, image, desired_count);
  }

  if (image->textures && image->texture_count == desired_count &&
      image->allocated_generation == image->generation) {
    return true_v;
  }

  if (image->textures && image->texture_count == desired_count &&
      (image->desc.flags & VKR_RG_RESOURCE_FLAG_RESIZABLE) != 0) {
    bool8_t resized = true_v;
    if (image->texture_handles &&
        image->texture_handle_count == image->texture_count) {
      for (uint32_t i = 0; i < image->texture_handle_count; ++i) {
        VkrTextureHandle updated = image->texture_handles[i];
        VkrRendererError resize_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_texture_system_resize(
                &graph->renderer->texture_system, updated, image->desc.width,
                image->desc.height, false_v, &updated, &resize_err)) {
          resized = false_v;
          break;
        }
        image->texture_handles[i] = updated;
      }
    } else {
      for (uint32_t i = 0; i < image->texture_count; ++i) {
        VkrRendererError resize_err = vkr_renderer_resize_texture(
            graph->renderer, image->textures[i], image->desc.width,
            image->desc.height, false_v);
        if (resize_err != VKR_RENDERER_ERROR_NONE) {
          resized = false_v;
          break;
        }
      }
    }

    if (resized) {
      uint64_t new_bytes = vkr_rg_calc_image_bytes_per_texture(&image->desc);
      uint64_t old_bytes = image->allocated_bytes_per_texture;
      if (new_bytes > old_bytes) {
        vkr_rg_stats_add_images(graph, 0, new_bytes - old_bytes);
      } else if (old_bytes > new_bytes) {
        vkr_rg_stats_remove_images(graph, 0, old_bytes - new_bytes);
      }
      image->allocated_bytes_per_texture = new_bytes;
      image->allocated_generation = image->generation;
      return true_v;
    }
  }

  vkr_rg_release_image_textures(graph, image);

  image->textures = vkr_allocator_alloc(
      graph->allocator, sizeof(VkrTextureOpaqueHandle) * desired_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!image->textures) {
    image->texture_count = 0;
    return false_v;
  }
  image->texture_count = desired_count;
  MemZero(image->textures,
          sizeof(VkrTextureOpaqueHandle) * (uint64_t)image->texture_count);

  if (image->desc.width == 0 || image->desc.height == 0) {
    log_error("RenderGraph image '%.*s' has zero extent",
              (int)image->name.length, image->name.str);
    return false_v;
  }

  uint64_t bytes_per_texture =
      vkr_rg_calc_image_bytes_per_texture(&image->desc);
  image->allocated_bytes_per_texture = bytes_per_texture;

  bool8_t is_depth = vkr_rg_usage_has(
      &image->desc.usage, VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT);
  bool8_t is_sampled =
      vkr_rg_usage_has(&image->desc.usage, VKR_TEXTURE_USAGE_SAMPLED);
  bool8_t is_array =
      image->desc.layers > 1 ||
      ((image->desc.flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY) != 0);

  if (is_array && !is_depth) {
    log_error("RenderGraph image '%.*s' array layers only supported for depth",
              (int)image->name.length, image->name.str);
    return false_v;
  }

  for (uint32_t i = 0; i < image->texture_count; ++i) {
    VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
    if (is_depth) {
      if (is_array) {
        if (!is_sampled) {
          log_error(
              "RenderGraph image '%.*s' array depth requires SAMPLED usage",
              (int)image->name.length, image->name.str);
          return false_v;
        }
        image->textures[i] = vkr_renderer_create_sampled_depth_attachment_array(
            graph->renderer, image->desc.width, image->desc.height,
            image->desc.layers, &tex_err);
      } else if (is_sampled) {
        image->textures[i] = vkr_renderer_create_sampled_depth_attachment(
            graph->renderer, image->desc.width, image->desc.height, &tex_err);
      } else {
        image->textures[i] = vkr_renderer_create_depth_attachment(
            graph->renderer, image->desc.width, image->desc.height, &tex_err);
      }
    } else {
      VkrRenderTargetTextureDesc tex_desc = {
          .width = image->desc.width,
          .height = image->desc.height,
          .format = image->desc.format,
          .usage = image->desc.usage,
      };
      image->textures[i] = vkr_renderer_create_render_target_texture(
          graph->renderer, &tex_desc, &tex_err);
    }

    if (!image->textures[i]) {
      String8 err = vkr_renderer_get_error_string(tex_err);
      log_error("RenderGraph image '%.*s' allocation failed: %s",
                (int)image->name.length, image->name.str, string8_cstr(&err));
      vkr_rg_release_image_textures(graph, image);
      return false_v;
    }

    vkr_rg_stats_add_images(graph, 1, bytes_per_texture);
  }

  image->allocated_generation = image->generation;
  return true_v;
}

vkr_internal bool8_t vkr_rg_allocate_buffer_handles(VkrRenderGraph *graph,
                                                    VkrRgBuffer *buffer,
                                                    uint32_t desired_count) {
  if (!graph || !buffer || !graph->renderer) {
    return false_v;
  }

  if (buffer->imported) {
    return vkr_rg_refresh_imported_buffers(graph, buffer, desired_count);
  }

  if (buffer->buffers && buffer->buffer_count == desired_count &&
      buffer->allocated_generation == buffer->generation) {
    return true_v;
  }

  vkr_rg_release_buffer_handles(graph, buffer);

  if (buffer->desc.size == 0) {
    log_error("RenderGraph buffer '%.*s' has zero size",
              (int)buffer->name.length, buffer->name.str);
    return false_v;
  }

  if (bitset8_get_value(&buffer->desc.usage) == 0) {
    log_error("RenderGraph buffer '%.*s' missing usage flags",
              (int)buffer->name.length, buffer->name.str);
    return false_v;
  }

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  bool8_t needs_host_visible =
      vkr_rg_buffer_usage_has(&buffer->desc.usage, VKR_BUFFER_USAGE_UNIFORM) ||
      vkr_rg_buffer_usage_has(&buffer->desc.usage,
                              VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER);

  VkrMemoryPropertyFlags memory_props =
      needs_host_visible ? vkr_memory_property_flags_from_bits(
                               VKR_MEMORY_PROPERTY_HOST_VISIBLE |
                               VKR_MEMORY_PROPERTY_HOST_COHERENT)
                         : vkr_memory_property_flags_from_bits(
                               VKR_MEMORY_PROPERTY_DEVICE_LOCAL);

  VkrBufferDescription desc = {
      .size = buffer->desc.size,
      .usage = buffer->desc.usage,
      .memory_properties = memory_props,
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
      .persistently_mapped = false_v,
  };

  buffer->buffers = vkr_allocator_alloc(graph->allocator,
                                        sizeof(VkrBufferHandle) * desired_count,
                                        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!buffer->buffers) {
    buffer->buffer_count = 0;
    return false_v;
  }
  buffer->buffer_count = desired_count;
  MemZero(buffer->buffers,
          sizeof(VkrBufferHandle) * (uint64_t)buffer->buffer_count);

  for (uint32_t i = 0; i < buffer->buffer_count; ++i) {
    VkrRendererError buf_err = VKR_RENDERER_ERROR_NONE;
    buffer->buffers[i] =
        vkr_renderer_create_buffer(graph->renderer, &desc, NULL, &buf_err);
    if (!buffer->buffers[i]) {
      String8 err = vkr_renderer_get_error_string(buf_err);
      log_error("RenderGraph buffer '%.*s' allocation failed: %s",
                (int)buffer->name.length, buffer->name.str, string8_cstr(&err));
      vkr_rg_release_buffer_handles(graph, buffer);
      return false_v;
    }

    vkr_rg_stats_add_buffers(graph, 1, buffer->desc.size);
  }

  buffer->allocated_size = buffer->desc.size;
  buffer->allocated_generation = buffer->generation;
  return true_v;
}

vkr_internal bool8_t vkr_rg_allocate_resources(VkrRenderGraph *graph) {
  if (!graph || !graph->renderer) {
    log_error("RenderGraph allocation failed: renderer unavailable");
    return false_v;
  }

  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (!image->declared_this_frame && !image->textures) {
      continue;
    }

    uint32_t desired_count = vkr_rg_resolve_image_count(graph, image);
    if (!vkr_rg_allocate_image_textures(graph, image, desired_count)) {
      return false_v;
    }
  }

  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (!buffer->declared_this_frame && !buffer->buffers) {
      continue;
    }

    uint32_t desired_count = vkr_rg_resolve_buffer_count(graph, buffer);
    if (!vkr_rg_allocate_buffer_handles(graph, buffer, desired_count)) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal uint64_t vkr_rg_hash_bytes(const void *data, uint64_t length,
                                        uint64_t seed) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint64_t hash = seed;
  for (uint64_t i = 0; i < length; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

vkr_internal uint64_t vkr_rg_hash_u32(uint64_t seed, uint32_t value) {
  return vkr_rg_hash_bytes(&value, sizeof(value), seed);
}

vkr_internal uint64_t vkr_rg_hash_u64(uint64_t seed, uint64_t value) {
  return vkr_rg_hash_bytes(&value, sizeof(value), seed);
}

vkr_internal VkrRgRenderTargetCacheEntry *
vkr_rg_get_target_cache_entry(VkrRenderGraph *graph, String8 pass_name) {
  if (!graph) {
    return NULL;
  }

  for (uint64_t i = 0; i < graph->render_target_cache.length; ++i) {
    VkrRgRenderTargetCacheEntry *entry =
        vector_get_VkrRgRenderTargetCacheEntry(&graph->render_target_cache, i);
    if (string8_equals(&entry->pass_name, &pass_name)) {
      return entry;
    }
  }

  VkrRgRenderTargetCacheEntry entry = {0};
  entry.pass_name = string8_duplicate(graph->allocator, &pass_name);
  vector_push_VkrRgRenderTargetCacheEntry(&graph->render_target_cache, entry);

  return vector_get_VkrRgRenderTargetCacheEntry(
      &graph->render_target_cache, graph->render_target_cache.length - 1);
}

vkr_internal bool8_t vkr_rg_build_pass_targets(VkrRenderGraph *graph,
                                               VkrRgPass *pass) {
  if (!graph || !pass || !graph->renderer) {
    return false_v;
  }

  if (pass->culled || (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) ||
      pass->desc.type != VKR_RG_PASS_TYPE_GRAPHICS) {
    pass->renderpass = NULL;
    pass->render_targets = NULL;
    pass->render_target_count = 0;
    return true_v;
  }

  uint8_t color_count = (uint8_t)pass->desc.color_attachments.length;
  if (color_count > VKR_MAX_COLOR_ATTACHMENTS) {
    log_error("RenderGraph pass '%.*s' color attachments exceed max",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  VkrRenderPassAttachmentDesc color_descs[VKR_MAX_COLOR_ATTACHMENTS];
  MemZero(color_descs, sizeof(color_descs));

  uint64_t renderpass_hash = 14695981039346656037ull;
  renderpass_hash = vkr_rg_hash_u32(renderpass_hash, pass->desc.domain);
  renderpass_hash = vkr_rg_hash_u32(renderpass_hash, color_count);

  for (uint8_t i = 0; i < color_count; ++i) {
    VkrRgAttachment *att =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      return false_v;
    }

    color_descs[i] = (VkrRenderPassAttachmentDesc){
        .format = image->desc.format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = att->desc.load_op,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = att->desc.store_op,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = att->desc.clear_value,
    };

    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, image->desc.format);
    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, att->desc.load_op);
    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, att->desc.store_op);
    renderpass_hash = vkr_rg_hash_bytes(
        &att->desc.clear_value, sizeof(att->desc.clear_value), renderpass_hash);
  }

  VkrRenderPassAttachmentDesc depth_desc = {0};
  bool8_t has_depth = pass->desc.has_depth_attachment;
  if (has_depth) {
    VkrRgAttachment *att = &pass->desc.depth_attachment;
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      return false_v;
    }

    depth_desc = (VkrRenderPassAttachmentDesc){
        .format = image->desc.format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = att->desc.load_op,
        .stencil_load_op = att->desc.load_op,
        .store_op = att->desc.store_op,
        .stencil_store_op = att->desc.store_op,
        .initial_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = att->desc.clear_value,
    };

    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, image->desc.format);
    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, att->desc.load_op);
    renderpass_hash = vkr_rg_hash_u32(renderpass_hash, att->desc.store_op);
    renderpass_hash = vkr_rg_hash_bytes(
        &att->desc.clear_value, sizeof(att->desc.clear_value), renderpass_hash);
  }

  VkrRenderPassDesc pass_desc = {
      .name = pass->desc.name,
      .domain = pass->desc.domain,
      .color_attachment_count = color_count,
      .color_attachments = color_descs,
      .depth_stencil_attachment = has_depth ? &depth_desc : NULL,
      .resolve_attachment_count = 0,
      .resolve_attachments = NULL,
  };

  VkrRgRenderTargetCacheEntry *cache =
      vkr_rg_get_target_cache_entry(graph, pass->desc.name);
  if (!cache) {
    return false_v;
  }

  if (cache->renderpass && cache->renderpass_hash != renderpass_hash) {
    vkr_renderer_wait_idle(graph->renderer);
    vkr_renderer_renderpass_destroy(graph->renderer, cache->renderpass);
    cache->renderpass = NULL;
  }

  if (!cache->renderpass) {
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    cache->renderpass = vkr_renderer_renderpass_create_desc(
        graph->renderer, &pass_desc, &pass_err);
    if (!cache->renderpass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("RenderGraph pass '%.*s' renderpass create failed: %s",
                (int)pass->desc.name.length, pass->desc.name.str,
                string8_cstr(&err));
      return false_v;
    }
  }

  cache->renderpass_hash = renderpass_hash;

  bool8_t per_image = false_v;
  uint32_t target_width = 0;
  uint32_t target_height = 0;

  for (uint8_t i = 0; i < color_count; ++i) {
    VkrRgAttachment *att =
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      return false_v;
    }
    per_image |= ((image->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE) != 0);
    if (target_width == 0 && target_height == 0) {
      target_width = image->desc.width;
      target_height = image->desc.height;
    }
  }

  if (has_depth) {
    VkrRgAttachment *att = &pass->desc.depth_attachment;
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      return false_v;
    }
    per_image |= ((image->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE) != 0);
    if (target_width == 0 && target_height == 0) {
      target_width = image->desc.width;
      target_height = image->desc.height;
    }
  }

  if (target_width == 0 || target_height == 0) {
    log_error("RenderGraph pass '%.*s' missing attachment extents",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  uint32_t target_count =
      per_image ? vkr_renderer_window_attachment_count(graph->renderer) : 1;
  if (target_count == 0) {
    target_count = 1;
  }

  uint64_t target_hash = renderpass_hash;
  target_hash = vkr_rg_hash_u32(target_hash, target_count);
  target_hash = vkr_rg_hash_u32(target_hash, target_width);
  target_hash = vkr_rg_hash_u32(target_hash, target_height);

  uint8_t attachment_count = (uint8_t)(color_count + (has_depth ? 1u : 0u));
  target_hash = vkr_rg_hash_u32(target_hash, attachment_count);

  for (uint32_t image_index = 0; image_index < target_count; ++image_index) {
    for (uint8_t i = 0; i < color_count; ++i) {
      VkrRgAttachment *att =
          vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      VkrTextureOpaqueHandle tex =
          vkr_rg_pick_image_texture(image, image_index);
      target_hash = vkr_rg_hash_u64(target_hash, (uint64_t)(uintptr_t)tex);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.mip_level);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.base_layer);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.layer_count);
    }
    if (has_depth) {
      VkrRgAttachment *att = &pass->desc.depth_attachment;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      VkrTextureOpaqueHandle tex =
          vkr_rg_pick_image_texture(image, image_index);
      target_hash = vkr_rg_hash_u64(target_hash, (uint64_t)(uintptr_t)tex);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.mip_level);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.base_layer);
      target_hash = vkr_rg_hash_u32(target_hash, att->desc.slice.layer_count);
    }
  }

  if (cache->targets && (cache->target_hash != target_hash ||
                         cache->target_count != target_count)) {
    vkr_renderer_wait_idle(graph->renderer);
    for (uint32_t i = 0; i < cache->target_count; ++i) {
      if (cache->targets[i]) {
        vkr_renderer_render_target_destroy(graph->renderer, cache->targets[i]);
      }
    }
    vkr_allocator_free(graph->allocator, cache->targets,
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)cache->target_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    cache->targets = NULL;
    cache->target_count = 0;
  }

  if (!cache->targets) {
    cache->targets = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrRenderTargetHandle) * target_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!cache->targets) {
      return false_v;
    }
    cache->target_count = target_count;
    MemZero(cache->targets,
            sizeof(VkrRenderTargetHandle) * (uint64_t)target_count);

    for (uint32_t image_index = 0; image_index < target_count; ++image_index) {
      VkrRenderTargetAttachmentRef attachments[VKR_MAX_COLOR_ATTACHMENTS + 1];
      uint8_t attach_index = 0;

      for (uint8_t i = 0; i < color_count; ++i) {
        VkrRgAttachment *att =
            vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
        VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
        VkrTextureOpaqueHandle tex =
            vkr_rg_pick_image_texture(image, image_index);
        if (!tex) {
          log_error("RenderGraph pass '%.*s' missing color attachment texture",
                    (int)pass->desc.name.length, pass->desc.name.str);
          return false_v;
        }
        attachments[attach_index++] = (VkrRenderTargetAttachmentRef){
            .texture = tex,
            .mip_level = att->desc.slice.mip_level,
            .base_layer = att->desc.slice.base_layer,
            .layer_count = att->desc.slice.layer_count,
        };
      }

      if (has_depth) {
        VkrRgAttachment *att = &pass->desc.depth_attachment;
        VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
        VkrTextureOpaqueHandle tex =
            vkr_rg_pick_image_texture(image, image_index);
        if (!tex) {
          log_error("RenderGraph pass '%.*s' missing depth attachment texture",
                    (int)pass->desc.name.length, pass->desc.name.str);
          return false_v;
        }
        attachments[attach_index++] = (VkrRenderTargetAttachmentRef){
            .texture = tex,
            .mip_level = att->desc.slice.mip_level,
            .base_layer = att->desc.slice.base_layer,
            .layer_count = att->desc.slice.layer_count,
        };
      }

      VkrRenderTargetDesc target_desc = {
          .sync_to_window_size = false_v,
          .width = target_width,
          .height = target_height,
          .attachment_count = attach_index,
          .attachments = attachments,
      };

      VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
      cache->targets[image_index] = vkr_renderer_render_target_create(
          graph->renderer, &target_desc, cache->renderpass, &rt_err);
      if (!cache->targets[image_index]) {
        String8 err = vkr_renderer_get_error_string(rt_err);
        log_error("RenderGraph pass '%.*s' target create failed: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  string8_cstr(&err));
        return false_v;
      }
    }
  }

  cache->target_hash = target_hash;

  pass->renderpass = cache->renderpass;
  pass->render_targets = cache->targets;
  pass->render_target_count = cache->target_count;
  return true_v;
}

vkr_internal bool8_t vkr_rg_build_render_targets(VkrRenderGraph *graph) {
  if (!graph) {
    return false_v;
  }

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    if (!vkr_rg_build_pass_targets(graph, pass)) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal void vkr_rg_sync_scene_color_handles(VkrRenderGraph *graph) {
  if (!graph || !graph->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)graph->renderer;
  if (!graph->frame_info.editor_enabled) {
    return;
  }

  VkrRgImage *scene_color = NULL;
  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (vkr_string8_equals_cstr_i(&image->name, "scene_color")) {
      scene_color = image;
      break;
    }
  }

  if (!scene_color || !scene_color->textures ||
      scene_color->texture_count == 0) {
    return;
  }

  if (!scene_color->texture_handles ||
      scene_color->texture_handle_count != scene_color->texture_count) {
    if (scene_color->texture_handles) {
      vkr_allocator_free(graph->allocator, scene_color->texture_handles,
                         sizeof(VkrTextureHandle) *
                             (uint64_t)scene_color->texture_handle_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }

    scene_color->texture_handles = vkr_allocator_alloc(
        graph->allocator,
        sizeof(VkrTextureHandle) * (uint64_t)scene_color->texture_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!scene_color->texture_handles) {
      scene_color->texture_handle_count = 0;
      return;
    }
    scene_color->texture_handle_count = scene_color->texture_count;
    MemZero(scene_color->texture_handles,
            sizeof(VkrTextureHandle) *
                (uint64_t)scene_color->texture_handle_count);

    for (uint32_t i = 0; i < scene_color->texture_handle_count; ++i) {
      char name_buf[64];
      int name_len =
          snprintf(name_buf, sizeof(name_buf), "RenderGraph.SceneColor.%u", i);
      if (name_len <= 0) {
        continue;
      }
      String8 name = string8_create_from_cstr((const uint8_t *)name_buf,
                                              (uint64_t)name_len);

      VkrTextureDescription desc = {
          .width = scene_color->desc.width,
          .height = scene_color->desc.height,
          .channels = 4,
          .type = VKR_TEXTURE_TYPE_2D,
          .format = scene_color->desc.format,
          .sample_count = VKR_SAMPLE_COUNT_1,
          .properties = vkr_texture_property_flags_create(),
          .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
          .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
          .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
          .min_filter = VKR_FILTER_LINEAR,
          .mag_filter = VKR_FILTER_LINEAR,
          .mip_filter = VKR_MIP_FILTER_NONE,
          .anisotropy_enable = false_v,
      };
      bitset8_set(&desc.properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
      bitset8_set(&desc.properties, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
      bitset8_set(&desc.properties, VKR_TEXTURE_PROPERTY_EXTERNAL_BIT);

      if (!vkr_texture_system_register_external(
              &rf->texture_system, name, scene_color->textures[i], &desc,
              &scene_color->texture_handles[i])) {
        log_warn("RenderGraph: failed to register scene_color %u", i);
      }
    }
  }

  rf->offscreen_color_handles = scene_color->texture_handles;
  rf->offscreen_color_handle_count = scene_color->texture_handle_count;
}

vkr_internal void vkr_rg_mark_reachable(VkrRenderGraph *graph, uint32_t start,
                                        bool8_t *keep) {
  Vector_uint32_t stack = vector_create_uint32_t(graph->allocator);
  vector_push_uint32_t(&stack, start);

  while (stack.length > 0) {
    uint32_t idx = vector_pop_uint32_t(&stack);
    if (keep[idx]) {
      continue;
    }
    keep[idx] = true_v;
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, idx);
    for (uint64_t i = 0; i < pass->in_edges.length; ++i) {
      vector_push_uint32_t(&stack, pass->in_edges.data[i]);
    }
  }

  vector_destroy_uint32_t(&stack);
}

vkr_internal void vkr_rg_cull_passes(VkrRenderGraph *graph) {
  uint32_t pass_count = (uint32_t)graph->passes.length;
  if (pass_count == 0) {
    return;
  }

  bool8_t *keep =
      vkr_allocator_alloc(graph->allocator, sizeof(bool8_t) * pass_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  MemZero(keep, sizeof(bool8_t) * pass_count);

  bool8_t has_outputs = vkr_rg_image_handle_valid(graph->present_image) ||
                        graph->export_images.length > 0 ||
                        graph->export_buffers.length > 0;

  if (!has_outputs) {
    for (uint32_t i = 0; i < pass_count; ++i) {
      keep[i] = true_v;
    }
  } else {
    for (uint32_t i = 0; i < pass_count; ++i) {
      VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
      if (pass->desc.flags & VKR_RG_PASS_FLAG_NO_CULL) {
        vkr_rg_mark_reachable(graph, i, keep);
      }
    }

    if (vkr_rg_image_handle_valid(graph->present_image)) {
      for (uint32_t i = 0; i < pass_count; ++i) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
        if (vkr_rg_pass_writes_image(pass, graph->present_image)) {
          vkr_rg_mark_reachable(graph, i, keep);
        }
      }
    }

    for (uint64_t i = 0; i < graph->export_images.length; ++i) {
      VkrRgImageHandle handle = graph->export_images.data[i];
      for (uint32_t p = 0; p < pass_count; ++p) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, p);
        if (vkr_rg_pass_writes_image(pass, handle)) {
          vkr_rg_mark_reachable(graph, p, keep);
        }
      }
    }

    for (uint64_t i = 0; i < graph->export_buffers.length; ++i) {
      VkrRgBufferHandle handle = graph->export_buffers.data[i];
      for (uint32_t p = 0; p < pass_count; ++p) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, p);
        if (vkr_rg_pass_writes_buffer(pass, handle)) {
          vkr_rg_mark_reachable(graph, p, keep);
        }
      }
    }
  }

  for (uint32_t i = 0; i < pass_count; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    bool8_t disabled = (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) != 0;
    pass->culled = !keep[i] || disabled;
  }

  vkr_allocator_free(graph->allocator, keep, sizeof(bool8_t) * pass_count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

vkr_internal bool8_t vkr_rg_topo_sort(VkrRenderGraph *graph) {
  uint32_t pass_count = (uint32_t)graph->passes.length;
  if (pass_count == 0) {
    return true_v;
  }

  uint32_t *in_degree =
      vkr_allocator_alloc(graph->allocator, sizeof(uint32_t) * pass_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  MemZero(in_degree, sizeof(uint32_t) * pass_count);

  uint32_t kept_count = 0;
  for (uint32_t i = 0; i < pass_count; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    if (pass->culled) {
      continue;
    }
    kept_count++;
    for (uint64_t e = 0; e < pass->out_edges.length; ++e) {
      uint32_t to = pass->out_edges.data[e];
      VkrRgPass *to_pass = vector_get_VkrRgPass(&graph->passes, to);
      if (!to_pass->culled) {
        in_degree[to]++;
      }
    }
  }

  Vector_uint32_t queue = vector_create_uint32_t(graph->allocator);
  for (uint32_t i = 0; i < pass_count; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    if (pass->culled) {
      continue;
    }
    if (in_degree[i] == 0) {
      vector_push_uint32_t(&queue, i);
    }
  }

  vector_clear_uint32_t(&graph->execution_order);
  uint64_t head = 0;
  while (head < queue.length) {
    uint32_t pass_index = queue.data[head++];
    vector_push_uint32_t(&graph->execution_order, pass_index);

    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);
    for (uint64_t e = 0; e < pass->out_edges.length; ++e) {
      uint32_t to = pass->out_edges.data[e];
      VkrRgPass *to_pass = vector_get_VkrRgPass(&graph->passes, to);
      if (to_pass->culled) {
        continue;
      }
      if (in_degree[to] > 0) {
        in_degree[to]--;
        if (in_degree[to] == 0) {
          vector_push_uint32_t(&queue, to);
        }
      }
    }
  }

  vector_destroy_uint32_t(&queue);

  bool8_t ok = graph->execution_order.length == kept_count;
  if (!ok) {
    log_error("RenderGraph compile failed: dependency cycle detected");
  }

  vkr_allocator_free(graph->allocator, in_degree, sizeof(uint32_t) * pass_count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  return ok;
}

vkr_internal void vkr_rg_compute_lifetimes(VkrRenderGraph *graph) {
  for (uint64_t i = 0; i < graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    image->first_pass = UINT32_MAX;
    image->last_pass = 0;
  }
  for (uint64_t i = 0; i < graph->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    buffer->first_pass = UINT32_MAX;
    buffer->last_pass = 0;
  }

  for (uint64_t order_index = 0; order_index < graph->execution_order.length;
       ++order_index) {
    uint32_t pass_index = graph->execution_order.data[order_index];
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);

    for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
      VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
      VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
      if (!image) {
        continue;
      }
      image->first_pass = (uint32_t)((image->first_pass == UINT32_MAX)
                                         ? order_index
                                         : vkr_min_u32(image->first_pass,
                                                       (uint32_t)order_index));
      image->last_pass = vkr_max_u32(image->last_pass, (uint32_t)order_index);
    }

    for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
      VkrRgImageUse *use =
          vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
      VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
      if (!image) {
        continue;
      }
      image->first_pass = (uint32_t)((image->first_pass == UINT32_MAX)
                                         ? order_index
                                         : vkr_min_u32(image->first_pass,
                                                       (uint32_t)order_index));
      image->last_pass = vkr_max_u32(image->last_pass, (uint32_t)order_index);
    }

    for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
      VkrRgAttachment *att =
          vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      if (!image) {
        continue;
      }
      image->first_pass = (uint32_t)((image->first_pass == UINT32_MAX)
                                         ? order_index
                                         : vkr_min_u32(image->first_pass,
                                                       (uint32_t)order_index));
      image->last_pass = vkr_max_u32(image->last_pass, (uint32_t)order_index);
    }

    if (pass->desc.has_depth_attachment) {
      VkrRgAttachment *att = &pass->desc.depth_attachment;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      if (image) {
        image->first_pass =
            (uint32_t)((image->first_pass == UINT32_MAX)
                           ? order_index
                           : vkr_min_u32(image->first_pass,
                                         (uint32_t)order_index));
        image->last_pass = vkr_max_u32(image->last_pass, (uint32_t)order_index);
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
      VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
      if (!buffer) {
        continue;
      }
      buffer->first_pass = (uint32_t)((buffer->first_pass == UINT32_MAX)
                                          ? order_index
                                          : vkr_min_u32(buffer->first_pass,
                                                        (uint32_t)order_index));
      buffer->last_pass = vkr_max_u32(buffer->last_pass, (uint32_t)order_index);
    }

    for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
      VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
      if (!buffer) {
        continue;
      }
      buffer->first_pass = (uint32_t)((buffer->first_pass == UINT32_MAX)
                                          ? order_index
                                          : vkr_min_u32(buffer->first_pass,
                                                        (uint32_t)order_index));
      buffer->last_pass = vkr_max_u32(buffer->last_pass, (uint32_t)order_index);
    }
  }
}

vkr_internal void vkr_rg_generate_barriers(VkrRenderGraph *graph) {
  typedef struct VkrRgImageState {
    VkrRgImageAccessFlags access;
    VkrTextureLayout layout;
  } VkrRgImageState;

  typedef struct VkrRgBufferState {
    VkrRgBufferAccessFlags access;
  } VkrRgBufferState;

  uint32_t image_count = (uint32_t)graph->images.length;
  uint32_t buffer_count = (uint32_t)graph->buffers.length;

  VkrRgImageState *image_states = NULL;
  VkrRgBufferState *buffer_states = NULL;

  if (image_count > 0) {
    image_states = vkr_allocator_alloc(graph->allocator,
                                       sizeof(VkrRgImageState) * image_count,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (buffer_count > 0) {
    buffer_states = vkr_allocator_alloc(graph->allocator,
                                        sizeof(VkrRgBufferState) * buffer_count,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    image_states[i].access =
        image->imported ? image->imported_access : VKR_RG_IMAGE_ACCESS_NONE;
    image_states[i].layout =
        image->imported ? image->imported_layout : VKR_TEXTURE_LAYOUT_UNDEFINED;
  }

  for (uint32_t i = 0; i < buffer_count; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    buffer_states[i].access =
        buffer->imported ? buffer->imported_access : VKR_RG_BUFFER_ACCESS_NONE;
  }

  for (uint64_t order_index = 0; order_index < graph->execution_order.length;
       ++order_index) {
    uint32_t pass_index = graph->execution_order.data[order_index];
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);

    vector_clear_VkrRgImageBarrier(&pass->pre_image_barriers);
    vector_clear_VkrRgBufferBarrier(&pass->pre_buffer_barriers);

    for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
      VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
      uint32_t idx = use->image.id - 1;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
      VkrTextureLayout desired_layout =
          vkr_rg_layout_for_image_access(image, use->access);
      VkrRgImageState *state = &image_states[idx];
      if (state->access != use->access || state->layout != desired_layout) {
        VkrRgImageBarrier barrier = {
            .image = use->image,
            .src_access = state->access,
            .dst_access = use->access,
            .src_layout = state->layout,
            .dst_layout = desired_layout,
        };
        vector_push_VkrRgImageBarrier(&pass->pre_image_barriers, barrier);
        state->access = use->access;
        state->layout = desired_layout;
      }
    }

    for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
      VkrRgImageUse *use =
          vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
      uint32_t idx = use->image.id - 1;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
      VkrTextureLayout desired_layout =
          vkr_rg_layout_for_image_access(image, use->access);
      VkrRgImageState *state = &image_states[idx];
      if (state->access != use->access || state->layout != desired_layout) {
        VkrRgImageBarrier barrier = {
            .image = use->image,
            .src_access = state->access,
            .dst_access = use->access,
            .src_layout = state->layout,
            .dst_layout = desired_layout,
        };
        vector_push_VkrRgImageBarrier(&pass->pre_image_barriers, barrier);
        state->access = use->access;
        state->layout = desired_layout;
      }
    }

    for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
      VkrRgAttachment *att =
          vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
      uint32_t idx = att->image.id - 1;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      VkrRgImageAccessFlags access = VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT;
      VkrTextureLayout desired_layout =
          vkr_rg_layout_for_image_access(image, access);
      VkrRgImageState *state = &image_states[idx];
      if (state->access != access || state->layout != desired_layout) {
        VkrRgImageBarrier barrier = {
            .image = att->image,
            .src_access = state->access,
            .dst_access = access,
            .src_layout = state->layout,
            .dst_layout = desired_layout,
        };
        vector_push_VkrRgImageBarrier(&pass->pre_image_barriers, barrier);
        state->access = access;
        state->layout = desired_layout;
      }
    }

    if (pass->desc.has_depth_attachment) {
      VkrRgAttachment *att = &pass->desc.depth_attachment;
      uint32_t idx = att->image.id - 1;
      VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
      VkrRgImageAccessFlags access = att->read_only
                                         ? VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY
                                         : VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT;
      VkrTextureLayout desired_layout =
          vkr_rg_layout_for_image_access(image, access);
      VkrRgImageState *state = &image_states[idx];
      if (state->access != access || state->layout != desired_layout) {
        VkrRgImageBarrier barrier = {
            .image = att->image,
            .src_access = state->access,
            .dst_access = access,
            .src_layout = state->layout,
            .dst_layout = desired_layout,
        };
        vector_push_VkrRgImageBarrier(&pass->pre_image_barriers, barrier);
        state->access = access;
        state->layout = desired_layout;
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
      uint32_t idx = use->buffer.id - 1;
      VkrRgBufferState *state = &buffer_states[idx];
      if (state->access != use->access) {
        VkrRgBufferBarrier barrier = {
            .buffer = use->buffer,
            .src_access = state->access,
            .dst_access = use->access,
        };
        vector_push_VkrRgBufferBarrier(&pass->pre_buffer_barriers, barrier);
        state->access = use->access;
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
      uint32_t idx = use->buffer.id - 1;
      VkrRgBufferState *state = &buffer_states[idx];
      if (state->access != use->access) {
        VkrRgBufferBarrier barrier = {
            .buffer = use->buffer,
            .src_access = state->access,
            .dst_access = use->access,
        };
        vector_push_VkrRgBufferBarrier(&pass->pre_buffer_barriers, barrier);
        state->access = use->access;
      }
    }
  }

  if (image_states) {
    for (uint32_t i = 0; i < image_count; ++i) {
      VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
      image->final_layout = image_states[i].layout;
    }
  }

  if (image_count > 0) {
    vkr_allocator_free(graph->allocator, image_states,
                       sizeof(VkrRgImageState) * image_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (buffer_count > 0) {
    vkr_allocator_free(graph->allocator, buffer_states,
                       sizeof(VkrRgBufferState) * buffer_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
}

bool8_t vkr_rg_compile(VkrRenderGraph *graph) {
  if (!graph) {
    log_error("RenderGraph compile failed: graph is NULL");
    return false_v;
  }

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    vector_clear_uint32_t(&pass->out_edges);
    vector_clear_uint32_t(&pass->in_edges);
    vector_clear_VkrRgImageBarrier(&pass->pre_image_barriers);
    vector_clear_VkrRgBufferBarrier(&pass->pre_buffer_barriers);
    pass->culled = false_v;
  }

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    if (!vkr_rg_validate_pass(graph, pass)) {
      return false_v;
    }
  }

  uint32_t image_count = (uint32_t)graph->images.length;
  uint32_t buffer_count = (uint32_t)graph->buffers.length;

  VkrRgDependencyState *image_states = NULL;
  VkrRgDependencyState *buffer_states = NULL;
  if (image_count > 0) {
    image_states = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrRgDependencyState) * image_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (buffer_count > 0) {
    buffer_states = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrRgDependencyState) * buffer_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    vkr_rg_dependency_state_init(&image_states[i], graph->allocator);
  }
  for (uint32_t i = 0; i < buffer_count; ++i) {
    vkr_rg_dependency_state_init(&buffer_states[i], graph->allocator);
  }

  for (uint32_t pass_index = 0; pass_index < graph->passes.length;
       ++pass_index) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);
    if (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) {
      continue;
    }

    for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
      VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
      vkr_rg_process_image_read(graph, image_states, pass_index, use->image);
    }

    for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
      VkrRgImageUse *use =
          vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
      vkr_rg_process_image_write(graph, image_states, pass_index, use->image);
    }

    for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
      VkrRgAttachment *att =
          vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
      if (att->desc.load_op == VKR_ATTACHMENT_LOAD_OP_LOAD) {
        vkr_rg_process_image_read(graph, image_states, pass_index, att->image);
      }
      vkr_rg_process_image_write(graph, image_states, pass_index, att->image);
    }

    if (pass->desc.has_depth_attachment) {
      VkrRgAttachment *att = &pass->desc.depth_attachment;
      if (att->desc.load_op == VKR_ATTACHMENT_LOAD_OP_LOAD || att->read_only) {
        vkr_rg_process_image_read(graph, image_states, pass_index, att->image);
      }
      if (!att->read_only) {
        vkr_rg_process_image_write(graph, image_states, pass_index, att->image);
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
      vkr_rg_process_buffer_read(graph, buffer_states, pass_index, use->buffer);
    }

    for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
      vkr_rg_process_buffer_write(graph, buffer_states, pass_index,
                                  use->buffer);
    }
  }

  if (image_states) {
    vkr_rg_warn_read_before_write_images(graph, image_states, image_count);
  }
  if (buffer_states) {
    vkr_rg_warn_read_before_write_buffers(graph, buffer_states, buffer_count);
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    vkr_rg_dependency_state_destroy(&image_states[i]);
  }
  for (uint32_t i = 0; i < buffer_count; ++i) {
    vkr_rg_dependency_state_destroy(&buffer_states[i]);
  }

  if (image_count > 0) {
    vkr_allocator_free(graph->allocator, image_states,
                       sizeof(VkrRgDependencyState) * image_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (buffer_count > 0) {
    vkr_allocator_free(graph->allocator, buffer_states,
                       sizeof(VkrRgDependencyState) * buffer_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vkr_rg_cull_passes(graph);

  if (!vkr_rg_topo_sort(graph)) {
    return false_v;
  }

  vkr_rg_compute_lifetimes(graph);
  vkr_rg_generate_barriers(graph);
  if (!vkr_rg_allocate_resources(graph)) {
    return false_v;
  }
  if (!vkr_rg_build_render_targets(graph)) {
    return false_v;
  }
  vkr_rg_sync_scene_color_handles(graph);

  graph->compiled = true_v;
  return true_v;
}

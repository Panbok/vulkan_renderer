#include "renderer/vkr_render_graph_internal.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/vkr_math.h"
#include "memory/vkr_allocator.h"

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

vkr_internal bool8_t vkr_rg_usage_has(const VkrTextureUsageFlags *usage,
                                      VkrTextureUsageBits bit) {
  return usage && bitset8_is_set(usage, (uint8_t)bit);
}

vkr_internal bool8_t vkr_rg_buffer_usage_has(const VkrBufferUsageFlags *usage,
                                             VkrBufferUsageBits bit) {
  return usage && bitset8_is_set(usage, (uint8_t)bit);
}

vkr_internal bool8_t
vkr_rg_image_allows_read_without_write(const VkrRgImage *image) {
  return image && (image->imported || (image->desc.flags &
                                       (VKR_RG_RESOURCE_FLAG_EXTERNAL |
                                        VKR_RG_RESOURCE_FLAG_PERSISTENT)) != 0);
}

vkr_internal bool8_t
vkr_rg_buffer_allows_read_without_write(const VkrRgBuffer *buffer) {
  return buffer &&
         (buffer->imported ||
          (buffer->desc.flags & (VKR_RG_RESOURCE_FLAG_EXTERNAL |
                                 VKR_RG_RESOURCE_FLAG_PERSISTENT)) != 0);
}

vkr_internal void
vkr_rg_warn_read_before_write_images(VkrRenderGraph *graph,
                                     const VkrRgDependencyState *states,
                                     uint32_t image_count) {
  if (!graph || !states)
    return;
  for (uint32_t i = 0; i < image_count; ++i) {
    const VkrRgDependencyState *state = &states[i];
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    if (state->last_writer >= 0 || state->last_readers.length == 0 || !image ||
        vkr_rg_image_allows_read_without_write(image))
      continue;
    VkrRgPass *reader =
        vector_get_VkrRgPass(&graph->passes, state->last_readers.data[0]);
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
  if (!graph || !states)
    return;
  for (uint32_t i = 0; i < buffer_count; ++i) {
    const VkrRgDependencyState *state = &states[i];
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    if (state->last_writer >= 0 || state->last_readers.length == 0 || !buffer ||
        vkr_rg_buffer_allows_read_without_write(buffer))
      continue;
    VkrRgPass *reader =
        vector_get_VkrRgPass(&graph->passes, state->last_readers.data[0]);
    String8 reader_name = reader ? reader->desc.name : string8_lit("<unknown>");
    log_warn(
        "RenderGraph buffer '%.*s' is read by pass '%.*s' before any writes",
        (int)buffer->name.length, buffer->name.str, (int)reader_name.length,
        reader_name.str);
  }
}

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
    return; /* Self-edge is redundant; pass already depends on itself. */
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
  if (access == VKR_RG_IMAGE_ACCESS_NONE) {
    log_error("RenderGraph pass '%.*s' declares NONE access for image '%.*s'",
              (int)pass->desc.name.length, pass->desc.name.str,
              (int)image->name.length, image->name.str);
    return false_v;
  }

  static const VkrRgImageAccessFlags access_bits[] = {
      VKR_RG_IMAGE_ACCESS_SAMPLED,
      VKR_RG_IMAGE_ACCESS_STORAGE_READ,
      VKR_RG_IMAGE_ACCESS_STORAGE_WRITE,
      VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT,
      VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT,
      VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY,
      VKR_RG_IMAGE_ACCESS_TRANSFER_SRC,
      VKR_RG_IMAGE_ACCESS_TRANSFER_DST,
      VKR_RG_IMAGE_ACCESS_PRESENT,
  };
  VkrTextureLayout declared_layout = VKR_TEXTURE_LAYOUT_UNDEFINED;
  for (uint32_t i = 0;
       i < (uint32_t)(sizeof(access_bits) / sizeof(access_bits[0])); ++i) {
    if ((access & access_bits[i]) == 0) {
      continue;
    }
    VkrTextureLayout bit_layout =
        vkr_rg_layout_for_image_access(image, access_bits[i]);
    if (declared_layout != VKR_TEXTURE_LAYOUT_UNDEFINED &&
        declared_layout != bit_layout) {
      log_error("RenderGraph pass '%.*s' combines incompatible access layouts "
                "for image '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str,
                (int)image->name.length, image->name.str);
      return false_v;
    }
    declared_layout = bit_layout;
  }

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
  if (access &
      (VKR_RG_IMAGE_ACCESS_STORAGE_READ | VKR_RG_IMAGE_ACCESS_STORAGE_WRITE)) {
    ok &= vkr_rg_validate_image_usage_bit(
        pass, image, VKR_TEXTURE_USAGE_STORAGE, "storage", "STORAGE");
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
  if (desc->slice.mip_count > 1u) {
    log_error("RenderGraph pass '%.*s' %s attachment for '%.*s' spans %u "
              "mips; attachments require exactly one mip",
              (int)pass->desc.name.length, pass->desc.name.str, label,
              (int)image->name.length, image->name.str, desc->slice.mip_count);
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

vkr_internal bool8_t vkr_rg_validate_image_use_slice(const VkrRgPass *pass,
                                                     const VkrRgImage *image,
                                                     const VkrRgImageUse *use) {
  if (!use->has_slice)
    return true_v;
  const uint32_t mip_count = use->slice.mip_count ? use->slice.mip_count : 1u;
  if (!use->slice.layer_count ||
      use->slice.mip_level >= image->desc.mip_levels ||
      mip_count > image->desc.mip_levels - use->slice.mip_level ||
      use->slice.base_layer >= image->desc.layers ||
      use->slice.layer_count > image->desc.layers - use->slice.base_layer) {
    log_error("RenderGraph pass '%.*s' image use for '%.*s' has out-of-range "
              "mips [%u..%u) or layers [%u..%u)",
              (int)pass->desc.name.length, pass->desc.name.str,
              (int)image->name.length, image->name.str, use->slice.mip_level,
              use->slice.mip_level + mip_count, use->slice.base_layer,
              use->slice.base_layer + use->slice.layer_count);
    return false_v;
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
  if (access & VKR_BUFFER_ACCESS_INDIRECT_READ) {
    ok &= vkr_rg_validate_buffer_usage_bit(
        pass, buffer, VKR_BUFFER_USAGE_INDIRECT, "indirect read", "INDIRECT");
  }
  return ok;
}

vkr_internal bool8_t vkr_rg_validate_pass_bindings(const VkrRgPass *pass) {
  const Vector_VkrRgImageUse *image_vectors[] = {&pass->desc.image_reads,
                                                 &pass->desc.image_writes};
  const Vector_VkrRgBufferUse *buffer_vectors[] = {&pass->desc.buffer_reads,
                                                   &pass->desc.buffer_writes};

  for (uint32_t a = 0; a < ArrayCount(image_vectors); ++a) {
    const Vector_VkrRgImageUse *images = image_vectors[a];
    for (uint64_t i = 0; i < images->length; ++i) {
      const VkrRgImageUse *use =
          vector_get_VkrRgImageUse((Vector_VkrRgImageUse *)images, i);
      for (uint32_t b = a; b < ArrayCount(image_vectors); ++b) {
        const Vector_VkrRgImageUse *others = image_vectors[b];
        const uint64_t begin = b == a ? i + 1u : 0u;
        for (uint64_t j = begin; j < others->length; ++j) {
          const VkrRgImageUse *other =
              vector_get_VkrRgImageUse((Vector_VkrRgImageUse *)others, j);
          if (use->binding == other->binding &&
              use->array_index == other->array_index &&
              (use->image.id != other->image.id ||
               use->image.generation != other->image.generation)) {
            log_error("RenderGraph pass '%.*s' maps different images to "
                      "binding %u[%u]",
                      (int)pass->desc.name.length, pass->desc.name.str,
                      use->binding, use->array_index);
            return false_v;
          }
        }
      }
      for (uint32_t b = 0; b < ArrayCount(buffer_vectors); ++b) {
        const Vector_VkrRgBufferUse *buffers = buffer_vectors[b];
        for (uint64_t j = 0; j < buffers->length; ++j) {
          const VkrRgBufferUse *other =
              vector_get_VkrRgBufferUse((Vector_VkrRgBufferUse *)buffers, j);
          if (use->binding == other->binding &&
              use->array_index == other->array_index) {
            log_error("RenderGraph pass '%.*s' maps an image and buffer to "
                      "binding %u[%u]",
                      (int)pass->desc.name.length, pass->desc.name.str,
                      use->binding, use->array_index);
            return false_v;
          }
        }
      }
    }
  }

  for (uint32_t a = 0; a < ArrayCount(buffer_vectors); ++a) {
    const Vector_VkrRgBufferUse *buffers = buffer_vectors[a];
    for (uint64_t i = 0; i < buffers->length; ++i) {
      const VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse((Vector_VkrRgBufferUse *)buffers, i);
      for (uint32_t b = a; b < ArrayCount(buffer_vectors); ++b) {
        const Vector_VkrRgBufferUse *others = buffer_vectors[b];
        const uint64_t begin = b == a ? i + 1u : 0u;
        for (uint64_t j = begin; j < others->length; ++j) {
          const VkrRgBufferUse *other =
              vector_get_VkrRgBufferUse((Vector_VkrRgBufferUse *)others, j);
          if (use->binding == other->binding &&
              use->array_index == other->array_index &&
              (use->buffer.id != other->buffer.id ||
               use->buffer.generation != other->buffer.generation)) {
            log_error("RenderGraph pass '%.*s' maps different buffers to "
                      "binding %u[%u]",
                      (int)pass->desc.name.length, pass->desc.name.str,
                      use->binding, use->array_index);
            return false_v;
          }
        }
      }
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_rg_validate_pass(VkrRenderGraph *graph,
                                          VkrRgPass *pass) {
  if (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) {
    return true_v;
  }

  if (!vkr_rg_validate_pass_bindings(pass)) {
    return false_v;
  }

  if (pass->desc.dispatch.kind != VKR_RG_DISPATCH_NONE) {
    if (pass->desc.type != VKR_RG_PASS_TYPE_COMPUTE) {
      log_error("RenderGraph pass '%.*s': dispatch requires a compute pass",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (pass->desc.dispatch.kind == VKR_RG_DISPATCH_DIRECT &&
        (!pass->desc.dispatch.group_count_x ||
         !pass->desc.dispatch.group_count_y ||
         !pass->desc.dispatch.group_count_z)) {
      log_error("RenderGraph pass '%.*s': direct dispatch counts must be > 0",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (pass->desc.dispatch.kind == VKR_RG_DISPATCH_INDIRECT) {
      const VkrRgBufferUse *use = vkr_rg_pass_find_buffer_use(
          &pass->desc, pass->desc.dispatch.indirect_binding,
          pass->desc.dispatch.indirect_array_index);
      VkrRgBuffer *buffer =
          use ? vkr_rg_buffer_from_handle(graph, use->buffer) : NULL;
      if (!use || !buffer ||
          !(use->access & VKR_RG_BUFFER_ACCESS_INDIRECT_READ) ||
          pass->desc.dispatch.indirect_offset > buffer->desc.size ||
          buffer->desc.size - pass->desc.dispatch.indirect_offset < 12u) {
        log_error("RenderGraph pass '%.*s': invalid indirect dispatch source "
                  "at binding %u[%u] offset %llu",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  pass->desc.dispatch.indirect_binding,
                  pass->desc.dispatch.indirect_array_index,
                  (unsigned long long)pass->desc.dispatch.indirect_offset);
        return false_v;
      }
    } else if (pass->desc.dispatch.kind != VKR_RG_DISPATCH_DIRECT) {
      log_error("RenderGraph pass '%.*s': unknown dispatch kind %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (uint32_t)pass->desc.dispatch.kind);
      return false_v;
    }
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
    if (!vkr_rg_validate_image_access_usage(pass, image, use->access) ||
        !vkr_rg_validate_image_use_slice(pass, image, use)) {
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
    if (!vkr_rg_validate_image_access_usage(pass, image, use->access) ||
        !vkr_rg_validate_image_use_slice(pass, image, use)) {
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
  if (image.id == 0) {
    return;
  }
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
  if (image.id == 0) {
    return;
  }
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
  if (buffer.id == 0) {
    return;
  }
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
  if (buffer.id == 0) {
    return;
  }
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

vkr_internal void vkr_rg_mark_reachable(VkrRenderGraph *graph, uint32_t start,
                                        bool8_t *keep,
                                        VkrAllocator *scratch_allocator) {
  Vector_uint32_t stack = vector_create_uint32_t(scratch_allocator);
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

  VkrAllocator *scratch_allocator =
      graph->frame_allocator ? graph->frame_allocator : graph->allocator;
  bool8_t *keep =
      vkr_allocator_alloc(scratch_allocator, sizeof(bool8_t) * pass_count,
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
        vkr_rg_mark_reachable(graph, i, keep, scratch_allocator);
      }
    }

    if (vkr_rg_image_handle_valid(graph->present_image)) {
      for (uint32_t i = 0; i < pass_count; ++i) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
        if (vkr_rg_pass_writes_image(pass, graph->present_image)) {
          vkr_rg_mark_reachable(graph, i, keep, scratch_allocator);
        }
      }
    }

    for (uint64_t i = 0; i < graph->export_images.length; ++i) {
      VkrRgImageHandle handle = graph->export_images.data[i];
      for (uint32_t p = 0; p < pass_count; ++p) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, p);
        if (vkr_rg_pass_writes_image(pass, handle)) {
          vkr_rg_mark_reachable(graph, p, keep, scratch_allocator);
        }
      }
    }

    for (uint64_t i = 0; i < graph->export_buffers.length; ++i) {
      VkrRgBufferHandle handle = graph->export_buffers.data[i];
      for (uint32_t p = 0; p < pass_count; ++p) {
        VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, p);
        if (vkr_rg_pass_writes_buffer(pass, handle)) {
          vkr_rg_mark_reachable(graph, p, keep, scratch_allocator);
        }
      }
    }
  }

  for (uint32_t i = 0; i < pass_count; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    bool8_t disabled = (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) != 0;
    pass->culled = !keep[i] || disabled;
  }

  vkr_allocator_free(scratch_allocator, keep, sizeof(bool8_t) * pass_count,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

vkr_internal bool8_t vkr_rg_topo_sort(VkrRenderGraph *graph) {
  uint32_t pass_count = (uint32_t)graph->passes.length;
  if (pass_count == 0) {
    return true_v;
  }

  VkrAllocator *scratch_allocator =
      graph->frame_allocator ? graph->frame_allocator : graph->allocator;
  uint32_t *in_degree =
      vkr_allocator_alloc(scratch_allocator, sizeof(uint32_t) * pass_count,
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

  Vector_uint32_t queue = vector_create_uint32_t(scratch_allocator);
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

  vkr_allocator_free(scratch_allocator, in_degree,
                     sizeof(uint32_t) * pass_count,
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

/** @brief Subresource count an image's barrier state occupies. */
vkr_internal uint32_t vkr_rg_image_subresource_count(const VkrRgImage *image) {
  uint32_t mips = image->desc.mip_levels ? image->desc.mip_levels : 1;
  uint32_t layers = image->desc.layers ? image->desc.layers : 1;
  return mips * layers;
}

/**
 * @brief Grows the persistent barrier-state arrays to fit the current graph.
 *
 * Barrier generation runs every frame (vkr_rg_begin_frame clears the compiled
 * flag), so this storage is reused rather than allocated per compile. It only
 * reallocates when the graph's shape changes -- a new resource, a different
 * cascade count -- and never in steady state.
 */
vkr_internal bool8_t vkr_rg_ensure_barrier_state(VkrRenderGraph *graph) {
  uint32_t image_count = (uint32_t)graph->images.length;
  uint32_t buffer_count = (uint32_t)graph->buffers.length;

  uint32_t subresource_total = 0;
  for (uint32_t i = 0; i < image_count; ++i) {
    subresource_total += vkr_rg_image_subresource_count(
        vector_get_VkrRgImage(&graph->images, i));
  }

  if (image_count > graph->image_state_offset_capacity) {
    uint32_t *offsets =
        vkr_allocator_alloc(graph->allocator, sizeof(uint32_t) * image_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!offsets) {
      log_error("RenderGraph barrier state: offset allocation failed");
      return false_v;
    }
    if (graph->image_state_offsets) {
      vkr_allocator_free(graph->allocator, graph->image_state_offsets,
                         sizeof(uint32_t) * graph->image_state_offset_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    graph->image_state_offsets = offsets;
    graph->image_state_offset_capacity = image_count;
  }

  if (image_count > graph->touched_image_capacity) {
    uint32_t *tokens =
        vkr_allocator_alloc(graph->allocator, sizeof(uint32_t) * image_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *indices =
        vkr_allocator_alloc(graph->allocator, sizeof(uint32_t) * image_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!tokens || !indices) {
      if (tokens) {
        vkr_allocator_free(graph->allocator, tokens,
                           sizeof(uint32_t) * image_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (indices) {
        vkr_allocator_free(graph->allocator, indices,
                           sizeof(uint32_t) * image_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      log_error("RenderGraph barrier state: image touch allocation failed");
      return false_v;
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
    graph->image_touch_tokens = tokens;
    graph->touched_image_indices = indices;
    graph->touched_image_capacity = image_count;
  }

  if (subresource_total > graph->subresource_state_capacity) {
    VkrRgSubresourceState *states = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrRgSubresourceState) * subresource_total,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!states) {
      log_error("RenderGraph barrier state: subresource allocation failed");
      return false_v;
    }
    if (graph->subresource_states) {
      vkr_allocator_free(graph->allocator, graph->subresource_states,
                         sizeof(VkrRgSubresourceState) *
                             graph->subresource_state_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    graph->subresource_states = states;
    graph->subresource_state_capacity = subresource_total;
  }

  if (buffer_count > graph->buffer_state_capacity) {
    VkrRgBufferState *states = vkr_allocator_alloc(
        graph->allocator, sizeof(VkrRgBufferState) * buffer_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!states) {
      log_error("RenderGraph barrier state: buffer allocation failed");
      return false_v;
    }
    if (graph->buffer_states) {
      vkr_allocator_free(graph->allocator, graph->buffer_states,
                         sizeof(VkrRgBufferState) *
                             graph->buffer_state_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    graph->buffer_states = states;
    graph->buffer_state_capacity = buffer_count;
  }

  if (buffer_count > graph->touched_buffer_capacity) {
    uint32_t *indices =
        vkr_allocator_alloc(graph->allocator, sizeof(uint32_t) * buffer_count,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!indices) {
      log_error("RenderGraph barrier state: buffer touch allocation failed");
      return false_v;
    }
    if (graph->touched_buffer_indices) {
      vkr_allocator_free(graph->allocator, graph->touched_buffer_indices,
                         sizeof(uint32_t) * graph->touched_buffer_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    graph->touched_buffer_indices = indices;
    graph->touched_buffer_capacity = buffer_count;
  }

  // Seed: imported resources start in the state they were imported with,
  // graph-owned ones start undefined.
  uint32_t offset = 0;
  if (image_count > 0) {
    MemZero(graph->image_touch_tokens, sizeof(uint32_t) * image_count);
  }
  for (uint32_t i = 0; i < image_count; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    graph->image_state_offsets[i] = offset;
    uint32_t count = vkr_rg_image_subresource_count(image);
    VkrRgSubresourceState seed = {
        .access =
            image->imported ? image->imported_access : VKR_RG_IMAGE_ACCESS_NONE,
        .stages = vkr_gpu_stages_for_image_access(
            image->imported ? image->imported_access : VKR_RG_IMAGE_ACCESS_NONE,
            true_v),
        .layout = image->imported ? image->imported_layout
                                  : VKR_TEXTURE_LAYOUT_UNDEFINED,
    };
    for (uint32_t s = 0; s < count; ++s) {
      graph->subresource_states[offset + s] = seed;
    }
    offset += count;
  }

  for (uint32_t i = 0; i < buffer_count; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, i);
    graph->buffer_states[i] = (VkrRgBufferState){
        .access = buffer->imported ? buffer->imported_access
                                   : VKR_RG_BUFFER_ACCESS_NONE,
        .stages = vkr_gpu_stages_for_buffer_access(
            buffer->imported ? buffer->imported_access
                             : VKR_RG_BUFFER_ACCESS_NONE,
            true_v),
    };
  }

  return true_v;
}

/**
 * @brief Accumulates one image use into this pass's desired subresource state.
 *
 * Every pre-barrier executes before the pass, so multiple declarations in one
 * pass cannot be treated as a sequence. Compatible accesses are unioned and
 * lowered once; incompatible layouts are rejected at compile time.
 */
vkr_internal bool8_t vkr_rg_declare_image_access(
    VkrRenderGraph *graph, const VkrRgPass *pass, VkrRgImageHandle handle,
    VkrRgImageAccessFlags access, VkrGpuStageFlags stages,
    const VkrRgImageSlice *slice, uint32_t token, uint32_t *touched_count) {
  VkrRgImage *image = vkr_rg_image_from_handle(graph, handle);
  if (!image) {
    return false_v;
  }
  if (stages == VKR_GPU_STAGE_NONE) {
    log_error("RenderGraph pass '%.*s' declares no execution stage for image",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  uint32_t image_index = handle.id - 1;
  if (image_index >= graph->image_state_offset_capacity ||
      image_index >= graph->touched_image_capacity) {
    return false_v;
  }

  uint32_t mips = image->desc.mip_levels ? image->desc.mip_levels : 1;
  uint32_t layers = image->desc.layers ? image->desc.layers : 1;

  VkrImageSubresourceRange requested = {0};
  if (slice) {
    requested.base_mip = slice->mip_level;
    requested.mip_count = slice->mip_count ? slice->mip_count : 1u;
    requested.base_layer = slice->base_layer;
    requested.layer_count = slice->layer_count;
  }

  uint32_t base_mip = 0;
  uint32_t mip_count = 0;
  uint32_t base_layer = 0;
  uint32_t layer_count = 0;
  vkr_image_subresource_range_resolve(slice ? &requested : NULL, mips, layers,
                                      &base_mip, &mip_count, &base_layer,
                                      &layer_count);

  VkrTextureLayout desired_layout =
      vkr_rg_layout_for_image_access(image, access);
  if (desired_layout == VKR_TEXTURE_LAYOUT_UNDEFINED) {
    log_error("RenderGraph pass '%.*s' declares no usable access for image "
              "'%.*s'",
              (int)pass->desc.name.length, pass->desc.name.str,
              (int)image->name.length, image->name.str);
    return false_v;
  }
  VkrRgSubresourceState *states =
      &graph->subresource_states[graph->image_state_offsets[image_index]];

  if (graph->image_touch_tokens[image_index] != token) {
    graph->image_touch_tokens[image_index] = token;
    graph->touched_image_indices[(*touched_count)++] = image_index;
  }

  for (uint32_t m = base_mip; m < base_mip + mip_count; ++m) {
    for (uint32_t layer = base_layer; layer < base_layer + layer_count;
         ++layer) {
      VkrRgSubresourceState *state = &states[m * layers + layer];
      if (state->pending_token == token) {
        if (state->pending_layout != desired_layout) {
          log_error("RenderGraph pass '%.*s' uses image '%.*s' subresource "
                    "in incompatible layouts (%d and %d)",
                    (int)pass->desc.name.length, pass->desc.name.str,
                    (int)image->name.length, image->name.str,
                    state->pending_layout, desired_layout);
          return false_v;
        }
        state->pending_access =
            (VkrRgImageAccessFlags)(state->pending_access | access);
        state->pending_stages =
            (VkrGpuStageFlags)(state->pending_stages | stages);
      } else {
        state->pending_access = access;
        state->pending_stages = stages;
        state->pending_layout = desired_layout;
        state->pending_token = token;
      }
    }
  }

  return true_v;
}

/** @brief Accumulates one buffer use into this pass's desired access. */
vkr_internal bool8_t vkr_rg_declare_buffer_access(VkrRenderGraph *graph,
                                                  VkrRgBufferHandle handle,
                                                  VkrRgBufferAccessFlags access,
                                                  VkrGpuStageFlags stages,
                                                  uint32_t token,
                                                  uint32_t *touched_count) {
  uint32_t index = handle.id - 1;
  if (index >= graph->buffer_state_capacity ||
      index >= graph->touched_buffer_capacity) {
    return false_v;
  }
  if (stages == VKR_GPU_STAGE_NONE) {
    return false_v;
  }
  VkrRgBufferState *state = &graph->buffer_states[index];
  if (state->pending_token == token) {
    state->pending_access =
        (VkrRgBufferAccessFlags)(state->pending_access | access);
    state->pending_stages = (VkrGpuStageFlags)(state->pending_stages | stages);
  } else {
    state->pending_access = access;
    state->pending_stages = stages;
    state->pending_token = token;
    graph->touched_buffer_indices[(*touched_count)++] = index;
  }
  return true_v;
}

/** @brief Emits the combined pre-barriers for one pass and commits its state.
 */
vkr_internal void vkr_rg_commit_pass_barriers(VkrRenderGraph *graph,
                                              VkrRgPass *pass, uint32_t token,
                                              uint32_t touched_image_count,
                                              uint32_t touched_buffer_count) {
  for (uint32_t i = 0; i < touched_image_count; ++i) {
    uint32_t image_index = graph->touched_image_indices[i];
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, image_index);
    VkrRgSubresourceState *states =
        &graph->subresource_states[graph->image_state_offsets[image_index]];
    uint32_t mips = image->desc.mip_levels ? image->desc.mip_levels : 1;
    uint32_t layers = image->desc.layers ? image->desc.layers : 1;
    VkrRgImageHandle handle = {.id = image_index + 1,
                               .generation = image->generation};

    for (uint32_t mip = 0; mip < mips; ++mip) {
      uint32_t layer = 0;
      while (layer < layers) {
        VkrRgSubresourceState *first = &states[mip * layers + layer];
        if (first->pending_token != token) {
          layer++;
          continue;
        }

        uint32_t run_end = layer + 1;
        while (run_end < layers) {
          VkrRgSubresourceState *next = &states[mip * layers + run_end];
          if (next->pending_token != token || next->access != first->access ||
              next->stages != first->stages || next->layout != first->layout ||
              next->pending_access != first->pending_access ||
              next->pending_stages != first->pending_stages ||
              next->pending_layout != first->pending_layout) {
            break;
          }
          run_end++;
        }

        const bool8_t needed = first->layout != first->pending_layout ||
                               first->access != first->pending_access ||
                               vkr_image_access_is_write(first->access);
        if (needed) {
          VkrRgImageBarrier barrier = {
              .image = handle,
              .src_access = first->access,
              .dst_access = first->pending_access,
              .src_layout = first->layout,
              .dst_layout = first->pending_layout,
              .dependency =
                  {
                      .src_stages = first->stages,
                      .dst_stages = first->pending_stages,
                      .visibility = vkr_image_access_is_write(first->access)
                                        ? VKR_GPU_VISIBILITY_DEVICE
                                        : VKR_GPU_VISIBILITY_NONE,
                  },
              .range =
                  {
                      .base_mip = mip,
                      .mip_count = 1,
                      .base_layer = layer,
                      .layer_count = run_end - layer,
                  },
          };
          vector_push_VkrRgImageBarrier(&pass->pre_image_barriers, barrier);
        }

        for (uint32_t l = layer; l < run_end; ++l) {
          VkrRgSubresourceState *state = &states[mip * layers + l];
          state->access = state->pending_access;
          state->stages = state->pending_stages;
          state->layout = state->pending_layout;
        }
        layer = run_end;
      }
    }
  }

  for (uint32_t i = 0; i < touched_buffer_count; ++i) {
    uint32_t index = graph->touched_buffer_indices[i];
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&graph->buffers, index);
    VkrRgBufferState *state = &graph->buffer_states[index];
    const bool8_t prior_writes =
        (state->access & (VKR_RG_BUFFER_ACCESS_STORAGE_WRITE |
                          VKR_RG_BUFFER_ACCESS_TRANSFER_DST)) != 0;
    if (state->access != state->pending_access || prior_writes) {
      VkrRgBufferBarrier barrier = {
          .buffer = {.id = index + 1, .generation = buffer->generation},
          .src_access = state->access,
          .dst_access = state->pending_access,
          .dependency =
              {
                  .src_stages = state->stages,
                  .dst_stages = state->pending_stages,
                  .visibility = prior_writes ? VKR_GPU_VISIBILITY_DEVICE
                                             : VKR_GPU_VISIBILITY_NONE,
              },
      };
      vector_push_VkrRgBufferBarrier(&pass->pre_buffer_barriers, barrier);
    }
    state->access = state->pending_access;
    state->stages = state->pending_stages;
  }
}

vkr_internal bool8_t vkr_rg_generate_barriers(VkrRenderGraph *graph) {
  if (!vkr_rg_ensure_barrier_state(graph)) {
    return false_v;
  }
  vector_clear_VkrRgImageBarrier(&graph->terminal_image_barriers);

  for (uint64_t order_index = 0; order_index < graph->execution_order.length;
       ++order_index) {
    uint32_t pass_index = graph->execution_order.data[order_index];
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, pass_index);

    vector_clear_VkrRgImageBarrier(&pass->pre_image_barriers);
    vector_clear_VkrRgBufferBarrier(&pass->pre_buffer_barriers);
    uint32_t token = pass_index + 1;
    uint32_t touched_image_count = 0;
    uint32_t touched_buffer_count = 0;

    for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
      VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
      if (!vkr_rg_declare_image_access(graph, pass, use->image, use->access,
                                       use->stages,
                                       use->has_slice ? &use->slice : NULL,
                                       token, &touched_image_count)) {
        return false_v;
      }
    }

    for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
      VkrRgImageUse *use =
          vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
      if (!vkr_rg_declare_image_access(graph, pass, use->image, use->access,
                                       use->stages,
                                       use->has_slice ? &use->slice : NULL,
                                       token, &touched_image_count)) {
        return false_v;
      }
    }

    for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
      VkrRgAttachment *att =
          vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
      if (!vkr_rg_declare_image_access(
              graph, pass, att->image, VKR_RG_IMAGE_ACCESS_COLOR_ATTACHMENT,
              VKR_GPU_STAGE_COLOR_OUTPUT, &att->desc.slice, token,
              &touched_image_count)) {
        return false_v;
      }
    }

    if (pass->desc.has_depth_attachment) {
      VkrRgAttachment *att = &pass->desc.depth_attachment;
      VkrRgImageAccessFlags access = att->read_only
                                         ? VKR_RG_IMAGE_ACCESS_DEPTH_READ_ONLY
                                         : VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT;
      if (!vkr_rg_declare_image_access(
              graph, pass, att->image, access,
              vkr_gpu_stages_for_image_access(access, false_v),
              &att->desc.slice, token, &touched_image_count)) {
        return false_v;
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
      if (!vkr_rg_declare_buffer_access(graph, use->buffer, use->access,
                                        use->stages, token,
                                        &touched_buffer_count)) {
        return false_v;
      }
    }

    for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
      VkrRgBufferUse *use =
          vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
      if (!vkr_rg_declare_buffer_access(graph, use->buffer, use->access,
                                        use->stages, token,
                                        &touched_buffer_count)) {
        return false_v;
      }
    }

    vkr_rg_commit_pass_barriers(graph, pass, token, touched_image_count,
                                touched_buffer_count);
  }

  /* The graph owns the target's completion transition, so the backend never
     injects a hidden one at end_frame. The present image is single-mip and
     single-layer, so subresource 0 describes all of it. */
  const VkrPresentTargetImageState terminal =
      graph->frame_info.target_terminal_state;
  if (vkr_rg_image_handle_valid(graph->present_image) &&
      terminal.access != VKR_IMAGE_ACCESS_NONE &&
      terminal.layout != VKR_TEXTURE_LAYOUT_UNDEFINED) {
    uint32_t target_index = graph->present_image.id - 1u;
    VkrRgSubresourceState *state =
        &graph->subresource_states[graph->image_state_offsets[target_index]];
    VkrRgImageBarrier barrier = {
        .image = graph->present_image,
        .src_access = state->access,
        .dst_access = (VkrRgImageAccessFlags)terminal.access,
        .src_layout = state->layout,
        .dst_layout = terminal.layout,
        .dependency =
            {
                .src_stages = state->stages,
                .dst_stages = vkr_gpu_stages_for_image_access(
                    (VkrImageAccessFlags)terminal.access, false_v),
                .visibility = vkr_image_access_is_write(state->access)
                                  ? VKR_GPU_VISIBILITY_DEVICE
                                  : VKR_GPU_VISIBILITY_NONE,
            },
    };
    // A write must still be made visible even when the layout already matches.
    if (barrier.src_access != barrier.dst_access ||
        barrier.src_layout != barrier.dst_layout ||
        vkr_image_access_is_write(barrier.src_access)) {
      vector_push_VkrRgImageBarrier(&graph->terminal_image_barriers, barrier);
    }
    state->access = barrier.dst_access;
    state->stages = barrier.dependency.dst_stages;
    state->layout = barrier.dst_layout;
  }

  // Exported images report subresource 0's layout. Layers left in differing
  // layouts cannot be described by a single export layout, so warn rather than
  // pick one silently.
  for (uint32_t i = 0; i < (uint32_t)graph->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&graph->images, i);
    const VkrRgSubresourceState *states =
        &graph->subresource_states[graph->image_state_offsets[i]];
    image->final_layout = states[0].layout;

    uint32_t count = vkr_rg_image_subresource_count(image);
    for (uint32_t s = 1; s < count; ++s) {
      if (states[s].layout != states[0].layout) {
        log_warn("RenderGraph image '%.*s' ends the frame with mixed "
                 "subresource layouts; exporting subresource 0's layout",
                 (int)image->name.length, image->name.str);
        break;
      }
    }
  }

  return true_v;
}

bool8_t vkr_rg_compile_schedule(VkrRenderGraph *graph) {
  if (!graph) {
    log_error("RenderGraph schedule failed: graph is NULL");
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
  vector_clear_VkrRgImageBarrier(&graph->terminal_image_barriers);

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgPass *pass = vector_get_VkrRgPass(&graph->passes, i);
    if (!vkr_rg_validate_pass(graph, pass)) {
      return false_v;
    }
  }

  uint32_t image_count = (uint32_t)graph->images.length;
  uint32_t buffer_count = (uint32_t)graph->buffers.length;
  VkrAllocator *scratch_allocator =
      graph->frame_allocator ? graph->frame_allocator : graph->allocator;

  VkrRgDependencyState *image_states = NULL;
  VkrRgDependencyState *buffer_states = NULL;
  if (image_count > 0) {
    image_states = vkr_allocator_alloc(
        scratch_allocator, sizeof(VkrRgDependencyState) * image_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!image_states) {
      log_error("RenderGraph compile: image_states allocation failed");
      return false_v;
    }
  }
  if (buffer_count > 0) {
    buffer_states = vkr_allocator_alloc(
        scratch_allocator, sizeof(VkrRgDependencyState) * buffer_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!buffer_states) {
      if (image_states) {
        vkr_allocator_free(scratch_allocator, image_states,
                           sizeof(VkrRgDependencyState) * image_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      log_error("RenderGraph compile: buffer_states allocation failed");
      return false_v;
    }
  }

  if (image_states && image_count > 0) {
    for (uint32_t i = 0; i < image_count; ++i) {
      vkr_rg_dependency_state_init(&image_states[i], scratch_allocator);
    }
  }
  if (buffer_states && buffer_count > 0) {
    for (uint32_t i = 0; i < buffer_count; ++i) {
      vkr_rg_dependency_state_init(&buffer_states[i], scratch_allocator);
    }
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
    vkr_allocator_free(scratch_allocator, image_states,
                       sizeof(VkrRgDependencyState) * image_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (buffer_count > 0) {
    vkr_allocator_free(scratch_allocator, buffer_states,
                       sizeof(VkrRgDependencyState) * buffer_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  vkr_rg_cull_passes(graph);

  if (!vkr_rg_topo_sort(graph)) {
    return false_v;
  }

  vkr_rg_compute_lifetimes(graph);
  return vkr_rg_generate_barriers(graph);
}

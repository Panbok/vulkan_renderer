/**
 * @file vkr_rg_debug.c
 * @brief Render graph debug helpers (DOT export and description).
 */
#include "vkr_rg_debug.h"

#include "containers/bitset.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_allocator.h"
#include "vkr_render_graph_internal.h"
#include "vkr_rg_json.h"

vkr_internal const char *vkr_rg_pass_type_name(VkrRgPassType type) {
  switch (type) {
  case VKR_RG_PASS_TYPE_GRAPHICS:
    return "graphics";
  case VKR_RG_PASS_TYPE_COMPUTE:
    return "compute";
  case VKR_RG_PASS_TYPE_TRANSFER:
    return "transfer";
  default:
    break;
  }
  return "unknown";
}

vkr_internal String8 vkr_rg_dot_escape(VkrAllocator *allocator,
                                       const String8 *input) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(input != NULL, "input is NULL");
  assert_log(input->str != NULL, "input->str is NULL");
  assert_log(input->length > 0, "input->length is 0");

  uint64_t extra = 0;
  for (uint64_t i = 0; i < input->length; ++i) {
    uint8_t c = input->str[i];
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
      extra++;
    }
  }

  uint64_t total = input->length + extra;
  uint8_t *buffer = vkr_allocator_alloc(allocator, total + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return (String8){0};
  }

  uint64_t out = 0;
  for (uint64_t i = 0; i < input->length; ++i) {
    uint8_t c = input->str[i];
    switch (c) {
    case '"':
      buffer[out++] = '\\';
      buffer[out++] = '"';
      break;
    case '\\':
      buffer[out++] = '\\';
      buffer[out++] = '\\';
      break;
    case '\n':
      buffer[out++] = '\\';
      buffer[out++] = 'n';
      break;
    case '\r':
      buffer[out++] = '\\';
      buffer[out++] = 'r';
      break;
    case '\t':
      buffer[out++] = '\\';
      buffer[out++] = 't';
      break;
    default:
      buffer[out++] = c;
      break;
    }
  }
  buffer[out] = '\0';
  return string8_create(buffer, out);
}

vkr_internal bool8_t vkr_rg_dot_write(FileHandle *handle, const String8 *text) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(text != NULL, "text is NULL");
  assert_log(text->str != NULL, "text->str is NULL");
  assert_log(text->length > 0, "text->length is 0");

  uint64_t written = 0;
  FileError err = file_write(handle, text->length, text->str, &written);
  return err == FILE_ERROR_NONE && written == text->length;
}

vkr_internal bool8_t vkr_rg_dot_write_fmt(FileHandle *handle,
                                          VkrAllocator *allocator,
                                          const char *fmt, ...) {
  assert_log(handle != NULL, "handle is NULL");
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(fmt != NULL, "fmt is NULL");

  va_list args;
  va_start(args, fmt);
  String8 text = string8_create_formatted_v(allocator, fmt, args);
  va_end(args);

  if (text.length == 0 || text.str == NULL) {
    return false_v;
  }
  return vkr_rg_dot_write(handle, &text);
}

vkr_internal bool8_t vkr_rg_dot_write_pass_node(FileHandle *handle,
                                                VkrAllocator *allocator,
                                                const VkrRgPass *pass,
                                                uint32_t index) {
  if (!pass) {
    return false_v;
  }

  String8 name = pass->desc.name;
  if (!name.str || name.length == 0) {
    name = string8_create_formatted(allocator, "pass_%u", index);
  }
  String8 escaped = vkr_rg_dot_escape(allocator, &name);

  const char *type_name = vkr_rg_pass_type_name(pass->desc.type);
  const char *style = pass->culled ? "dashed,filled" : "rounded,filled";
  const char *fill = pass->culled ? "gray85" : "lightblue";
  if (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) {
    style = "dashed,filled";
    fill = "gray75";
  }

  const char *culled_tag = pass->culled ? "\\n(culled)" : "";
  return vkr_rg_dot_write_fmt(handle, allocator,
                              "  p%u [label=\"%.*s\\n(%s)%s\" shape=box "
                              "style=\"%s\" fillcolor=\"%s\"];\n",
                              index, (int)escaped.length, escaped.str,
                              type_name, culled_tag, style, fill);
}

vkr_internal bool8_t vkr_rg_dot_write_image_node(FileHandle *handle,
                                                 VkrAllocator *allocator,
                                                 const VkrRgImage *image,
                                                 uint32_t index,
                                                 bool8_t is_present) {
  if (!image) {
    return false_v;
  }

  String8 name = image->name;
  if (!name.str || name.length == 0) {
    name = string8_create_formatted(allocator, "image_%u", index);
  }
  String8 escaped = vkr_rg_dot_escape(allocator, &name);

  const char *style = image->imported ? "dashed,filled" : "filled";
  const char *fill = image->imported ? "gray90" : "white";
  const char *present_tag = is_present ? "\\n(present)" : "";
  const char *export_tag = image->exported ? "\\n(export)" : "";
  const char *import_tag = image->imported ? "\\n(import)" : "";

  return vkr_rg_dot_write_fmt(
      handle, allocator,
      "  i%u [label=\"img:%.*s%s%s%s\" shape=ellipse style=\"%s\" "
      "fillcolor=\"%s\"];\n",
      index, (int)escaped.length, escaped.str, import_tag, export_tag,
      present_tag, style, fill);
}

vkr_internal bool8_t vkr_rg_dot_write_buffer_node(FileHandle *handle,
                                                  VkrAllocator *allocator,
                                                  const VkrRgBuffer *buffer,
                                                  uint32_t index) {
  if (!buffer) {
    return false_v;
  }

  String8 name = buffer->name;
  if (!name.str || name.length == 0) {
    name = string8_create_formatted(allocator, "buffer_%u", index);
  }
  String8 escaped = vkr_rg_dot_escape(allocator, &name);

  const char *style = buffer->imported ? "dashed,filled" : "filled";
  const char *fill = buffer->imported ? "gray90" : "white";
  const char *export_tag = buffer->exported ? "\\n(export)" : "";
  const char *import_tag = buffer->imported ? "\\n(import)" : "";

  return vkr_rg_dot_write_fmt(
      handle, allocator,
      "  b%u [label=\"buf:%.*s%s%s\" shape=ellipse style=\"%s\" "
      "fillcolor=\"%s\"];\n",
      index, (int)escaped.length, escaped.str, import_tag, export_tag, style,
      fill);
}

vkr_internal void vkr_rg_dot_write_edge(FileHandle *handle,
                                        VkrAllocator *allocator,
                                        const char *from, uint32_t from_index,
                                        const char *to, uint32_t to_index,
                                        const char *label, const char *color) {
  vkr_rg_dot_write_fmt(handle, allocator,
                       "  %s%u -> %s%u [label=\"%s\" color=\"%s\"];\n", from,
                       from_index, to, to_index, label, color);
}

vkr_internal bool8_t vkr_rg_dot_write_pass_edges(FileHandle *handle,
                                                 VkrAllocator *allocator,
                                                 VkrRenderGraph *graph,
                                                 const VkrRgPass *pass,
                                                 uint32_t pass_index) {
  if (!graph || !pass) {
    return false_v;
  }

  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    VkrRgAttachment *att =
        array_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (!att) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = att->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "i", image_index,
                          "color", "red");
  }

  if (pass->desc.has_depth_attachment) {
    const VkrRgAttachment *att = &pass->desc.depth_attachment;
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (image) {
      uint32_t image_index = att->image.id - 1;
      if (att->read_only) {
        vkr_rg_dot_write_edge(handle, allocator, "i", image_index, "p",
                              pass_index, "depth_ro", "blue");
      } else {
        vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "i",
                              image_index, "depth", "red");
      }
    }
  }

  for (uint64_t i = 0; i < pass->desc.image_reads.length; ++i) {
    VkrRgImageUse *use = array_get_VkrRgImageUse(&pass->desc.image_reads, i);
    if (!use) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = use->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "i", image_index, "p", pass_index,
                          "read", "blue");
  }

  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    VkrRgImageUse *use = array_get_VkrRgImageUse(&pass->desc.image_writes, i);
    if (!use) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = use->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "i", image_index,
                          "write", "red");
  }

  for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
    VkrRgBufferUse *use = array_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
    if (!use) {
      continue;
    }
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      continue;
    }
    uint32_t buffer_index = use->buffer.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "b", buffer_index, "p", pass_index,
                          "read", "blue");
  }

  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    VkrRgBufferUse *use =
        array_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    if (!use) {
      continue;
    }
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      continue;
    }
    uint32_t buffer_index = use->buffer.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "b", buffer_index,
                          "write", "red");
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_dot_write_pass_edges_direct(FileHandle *handle,
                                                        VkrAllocator *allocator,
                                                        const VkrRgPass *pass,
                                                        uint32_t pass_index) {
  if (!pass) {
    return false_v;
  }

  for (uint64_t i = 0; i < pass->out_edges.length; ++i) {
    uint32_t to = *array_get_uint32_t(&pass->out_edges, i);
    vkr_rg_dot_write_fmt(handle, allocator,
                         "  p%u -> p%u [style=dashed color=\"gray50\"];\n",
                         pass_index, to);
  }
  return true_v;
}

vkr_internal FilePathType vkr_rg_dot_path_type(const char *path) {
  if (!path || path[0] == '\0') {
    return FILE_PATH_TYPE_RELATIVE;
  }

  if (path[0] == '/') {
    return FILE_PATH_TYPE_ABSOLUTE;
  }

  /* UNC: \\server\share */
  if (path[0] == '\\' && path[1] == '\\') {
    return FILE_PATH_TYPE_ABSOLUTE;
  }

  /* Drive letter: A: or A:\ */
  if ((path[0] >= 'A' && path[0] <= 'Z') ||
      (path[0] >= 'a' && path[0] <= 'z')) {
    if (path[1] == ':') {
      return FILE_PATH_TYPE_ABSOLUTE;
    }
  }

  return FILE_PATH_TYPE_RELATIVE;
}

bool8_t vkr_rg_export_dot(const VkrRenderGraph *graph, const char *path) {
  VkrRgDotExportDesc desc = {
      .path = path,
      .flags = VKR_RG_DOT_EXPORT_DEFAULT,
  };
  return vkr_rg_export_dot_ex(graph, &desc);
}

bool8_t vkr_rg_export_dot_ex(const VkrRenderGraph *graph,
                             const VkrRgDotExportDesc *desc) {
  if (!graph || !desc || !desc->path) {
    return false_v;
  }

  VkrRenderGraph *rg = (VkrRenderGraph *)graph;
  VkrAllocator *allocator = rg->allocator;
  if (!allocator) {
    return false_v;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(allocator);

  FilePathType path_type = vkr_rg_dot_path_type(desc->path);
  FilePath file_path = file_path_create(desc->path, allocator, path_type);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_TEXT);

  FileHandle handle = {0};
  FileError open_err = file_open(&file_path, mode, &handle);
  if (open_err != FILE_ERROR_NONE) {
    String8 err = file_get_error_string(open_err);
    const char *err_str = (err.str != NULL) ? (const char *)err.str : "(null)";
    log_error("RenderGraph DOT export failed to open '%s': %s", desc->path,
              err_str);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  vkr_rg_dot_write_fmt(&handle, allocator, "digraph RenderGraph {\n");
  vkr_rg_dot_write_fmt(&handle, allocator, "  rankdir=LR;\n");
  vkr_rg_dot_write_fmt(&handle, allocator,
                       "  node [fontname=\"Helvetica\" fontsize=10];\n");
  vkr_rg_dot_write_fmt(&handle, allocator,
                       "  edge [fontname=\"Helvetica\" fontsize=9];\n");

  for (uint32_t i = 0; i < rg->passes.length; ++i) {
    VkrRgPass *pass = array_get_VkrRgPass(&rg->passes, i);
    vkr_rg_dot_write_pass_node(&handle, allocator, pass, i);
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_RESOURCES) {
    for (uint32_t i = 0; i < rg->images.length; ++i) {
      VkrRgImage *image = array_get_VkrRgImage(&rg->images, i);
      bool8_t is_present = vkr_rg_image_handle_valid(rg->present_image) &&
                           (rg->present_image.id - 1) == i;
      vkr_rg_dot_write_image_node(&handle, allocator, image, i, is_present);
    }

    for (uint32_t i = 0; i < rg->buffers.length; ++i) {
      VkrRgBuffer *buffer = array_get_VkrRgBuffer(&rg->buffers, i);
      vkr_rg_dot_write_buffer_node(&handle, allocator, buffer, i);
    }
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_RESOURCES) {
    for (uint32_t i = 0; i < rg->passes.length; ++i) {
      VkrRgPass *pass = array_get_VkrRgPass(&rg->passes, i);
      vkr_rg_dot_write_pass_edges(&handle, allocator, rg, pass, i);
    }
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_PASS_EDGES) {
    for (uint32_t i = 0; i < rg->passes.length; ++i) {
      VkrRgPass *pass = array_get_VkrRgPass(&rg->passes, i);
      vkr_rg_dot_write_pass_edges_direct(&handle, allocator, pass, i);
    }
  }

  vkr_rg_dot_write_fmt(&handle, allocator, "}\n");
  file_close(&handle);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return true_v;
}

// =============================================================================
// Compiled graph description
// =============================================================================

/* Uses of one pass, bounded by VKR_RG_DESCRIBED_USE_MAX; `total` counts
   every use so the caller can mark truncation. */
typedef struct VkrRgDescribeUses {
  VkrRgDescribedUse items[VKR_RG_DESCRIBED_USE_MAX];
  uint32_t count;
  uint32_t total;
} VkrRgDescribeUses;

vkr_internal void vkr_rg_describe_add_use(VkrRgDescribeUses *uses,
                                          String8 resource, const char *role,
                                          bool8_t buffer, bool8_t write) {
  uses->total += 1u;
  if (uses->count >= VKR_RG_DESCRIBED_USE_MAX) {
    return;
  }
  uses->items[uses->count++] = (VkrRgDescribedUse){
      .resource = resource,
      .role = role,
      .buffer = buffer,
      .write = write,
  };
}

vkr_internal void vkr_rg_describe_add_image_use(VkrRenderGraph *graph,
                                                VkrRgDescribeUses *uses,
                                                VkrRgImageHandle handle,
                                                const char *role,
                                                bool8_t write) {
  const VkrRgImage *image = vkr_rg_image_from_handle(graph, handle);
  if (image) {
    vkr_rg_describe_add_use(uses, image->name, role, false_v, write);
  }
}

vkr_internal void vkr_rg_describe_add_buffer_use(VkrRenderGraph *graph,
                                                 VkrRgDescribeUses *uses,
                                                 VkrRgBufferHandle handle,
                                                 const char *role,
                                                 bool8_t write) {
  const VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, handle);
  if (buffer) {
    vkr_rg_describe_add_use(uses, buffer->name, role, true_v, write);
  }
}

/* Attachments first, then image and buffer reads and writes, as the DOT
   export lists a pass's edges. */
vkr_internal void vkr_rg_describe_collect_uses(VkrRenderGraph *graph,
                                               const VkrRgPass *pass,
                                               VkrRgDescribeUses *uses) {
  const VkrRgPassDesc *desc = &pass->desc;
  for (uint64_t i = 0; i < desc->color_attachments.length; ++i) {
    vkr_rg_describe_add_image_use(
        graph, uses, desc->color_attachments.data[i].image, "color", true_v);
  }
  if (desc->has_depth_attachment) {
    const bool8_t read_only = desc->depth_attachment.read_only;
    vkr_rg_describe_add_image_use(graph, uses, desc->depth_attachment.image,
                                  read_only ? "depth_read" : "depth",
                                  !read_only);
  }

  for (uint64_t i = 0; i < desc->image_reads.length; ++i) {
    vkr_rg_describe_add_image_use(graph, uses, desc->image_reads.data[i].image,
                                  "read", false_v);
  }
  for (uint64_t i = 0; i < desc->image_writes.length; ++i) {
    vkr_rg_describe_add_image_use(graph, uses, desc->image_writes.data[i].image,
                                  "write", true_v);
  }
  for (uint64_t i = 0; i < desc->buffer_reads.length; ++i) {
    vkr_rg_describe_add_buffer_use(
        graph, uses, desc->buffer_reads.data[i].buffer, "read", false_v);
  }
  for (uint64_t i = 0; i < desc->buffer_writes.length; ++i) {
    vkr_rg_describe_add_buffer_use(
        graph, uses, desc->buffer_writes.data[i].buffer, "write", true_v);
  }
}

vkr_internal void vkr_rg_describe_pass(VkrRenderGraph *graph,
                                       const VkrRgDescribeVisitor *visitor,
                                       uint32_t pass_index, uint32_t order) {
  const VkrRgPass *pass = array_get_VkrRgPass(&graph->passes, pass_index);
  VkrRgDescribeUses uses;
  uses.count = 0u;
  uses.total = 0u;
  vkr_rg_describe_collect_uses(graph, pass, &uses);

  const VkrRgDescribedPass described = {
      .name = pass->desc.name,
      .index = pass_index,
      .order = order,
      .type = pass->desc.type,
      .type_name = vkr_rg_pass_type_name(pass->desc.type),
      .culled = order == UINT32_MAX,
      .disabled = (pass->desc.flags & VKR_RG_PASS_FLAG_DISABLED) != 0,
      .uses = uses.items,
      .use_count = uses.count,
      .use_total = uses.total,
  };
  visitor->pass(visitor->user, &described);
}

vkr_internal const char *vkr_rg_lifetime_name(VkrRgResourceFlags flags,
                                              bool8_t imported) {
  if (imported || (flags & VKR_RG_RESOURCE_FLAG_EXTERNAL)) {
    return "imported";
  }
  if (flags & VKR_RG_RESOURCE_FLAG_HISTORY) {
    return "history";
  }
  if (flags & VKR_RG_RESOURCE_FLAG_RETAINED) {
    return "retained";
  }
  if (flags & VKR_RG_RESOURCE_FLAG_PER_FRAME_SLOT) {
    return "per_frame_slot";
  }
  if (flags & VKR_RG_RESOURCE_FLAG_PERSISTENT) {
    return "persistent";
  }
  if (flags & VKR_RG_RESOURCE_FLAG_TRANSIENT) {
    return "transient";
  }
  return "graph";
}

vkr_internal const char *vkr_rg_instances_name(VkrRgResourceFlags flags) {
  switch (vkr_rg_resource_instance_domain(flags)) {
  case VKR_RG_RESOURCE_INSTANCE_PER_IMAGE:
    return "per_image";
  case VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT:
    return "per_frame_slot";
  case VKR_RG_RESOURCE_INSTANCE_SINGLE:
    break;
  }
  return "single";
}

vkr_internal VkrRgDescribedResource
vkr_rg_describe_image(const VkrRenderGraph *graph, uint32_t index) {
  const VkrRgImage *image = &graph->images.data[index];
  const VkrRgImageDesc *desc = &image->desc;
  VkrRenderGraphResourceStats one = {0};
  vkr_render_graph_resource_stats_add_image(&one, desc, 1u);
  const bool8_t used = image->first_pass != UINT32_MAX;
  return (VkrRgDescribedResource){
      .name = image->name,
      .index = index,
      .buffer = false_v,
      .declared = image->declared_this_frame,
      .imported = image->imported,
      .exported = image->exported,
      .present = vkr_rg_image_handle_valid(graph->present_image) &&
                 graph->present_image.id - 1u == index,
      .width = desc->width,
      .height = desc->height,
      .depth = desc->depth,
      .layers = desc->layers,
      .mip_levels = desc->mip_levels,
      .samples = (uint32_t)desc->samples,
      .format = desc->format,
      .format_name = vkr_rg_json_format_name(desc->format),
      .bytes = one.live_image_bytes,
      .flags = desc->flags,
      .lifetime = vkr_rg_lifetime_name(desc->flags, image->imported),
      .instances = vkr_rg_instances_name(desc->flags),
      .first_order = used ? image->first_pass : UINT32_MAX,
      .last_order = used ? image->last_pass : UINT32_MAX,
  };
}

vkr_internal VkrRgDescribedResource
vkr_rg_describe_buffer(const VkrRenderGraph *graph, uint32_t index) {
  const VkrRgBuffer *buffer = &graph->buffers.data[index];
  const VkrRgBufferDesc *desc = &buffer->desc;
  const bool8_t used = buffer->first_pass != UINT32_MAX;
  return (VkrRgDescribedResource){
      .name = buffer->name,
      .index = index,
      .buffer = true_v,
      .declared = buffer->declared_this_frame,
      .imported = buffer->imported,
      .exported = buffer->exported,
      .format = VKR_TEXTURE_FORMAT_COUNT,
      .bytes = desc->size,
      .flags = desc->flags,
      .lifetime = vkr_rg_lifetime_name(desc->flags, buffer->imported),
      .instances = vkr_rg_instances_name(desc->flags),
      .first_order = used ? buffer->first_pass : UINT32_MAX,
      .last_order = used ? buffer->last_pass : UINT32_MAX,
  };
}

bool8_t vkr_rg_describe(const VkrRenderGraph *graph,
                        const VkrRgDescribeVisitor *visitor,
                        VkrRgDescription *out_description) {
  if (!graph) {
    return false_v;
  }
  /* The visit only reads; handle resolution takes a mutable graph. */
  VkrRenderGraph *rg = (VkrRenderGraph *)graph;
  const uint32_t pass_count = (uint32_t)rg->passes.length;
  const uint32_t scheduled = (uint32_t)rg->execution_order.length;

  if (visitor && visitor->pass) {
    for (uint32_t order = 0u; order < scheduled; ++order) {
      const uint32_t pass_index = rg->execution_order.data[order];
      if (pass_index < pass_count) {
        vkr_rg_describe_pass(rg, visitor, pass_index, order);
      }
    }
    /* Execution order holds exactly the kept passes, so a compiled graph's
       remaining passes are its culled ones; without a compile every pass is
       unscheduled. */
    for (uint32_t i = 0u; i < pass_count; ++i) {
      const VkrRgPass *pass = array_get_VkrRgPass(&rg->passes, i);
      if (scheduled == 0u || pass->culled) {
        vkr_rg_describe_pass(rg, visitor, i, UINT32_MAX);
      }
    }
  }

  VkrRgDescription description = {
      .frame_index = rg->frame_info.frame_index,
      .viewport_width = rg->frame_info.viewport_width,
      .viewport_height = rg->frame_info.viewport_height,
      .render_scale = rg->frame_info.render_scale,
      .scene_rendering = rg->frame_info.scene_rendering,
      .compiled = scheduled > 0u,
      .pass_count = pass_count,
      .scheduled_count = scheduled,
      .culled_count = pass_count - Min(scheduled, pass_count),
      .image_count = (uint32_t)rg->images.length,
      .buffer_count = (uint32_t)rg->buffers.length,
  };
  for (uint32_t i = 0u; i < description.image_count; ++i) {
    const VkrRgDescribedResource image = vkr_rg_describe_image(rg, i);
    if (image.declared) {
      description.declared_image_count += 1u;
      description.declared_image_bytes += image.imported ? 0u : image.bytes;
    }
    if (visitor && visitor->resource) {
      visitor->resource(visitor->user, &image);
    }
  }
  for (uint32_t i = 0u; i < description.buffer_count; ++i) {
    const VkrRgDescribedResource buffer = vkr_rg_describe_buffer(rg, i);
    if (buffer.declared) {
      description.declared_buffer_count += 1u;
      description.declared_buffer_bytes += buffer.imported ? 0u : buffer.bytes;
    }
    if (visitor && visitor->resource) {
      visitor->resource(visitor->user, &buffer);
    }
  }

  if (out_description) {
    *out_description = description;
  }
  return true_v;
}

// =============================================================================
// Graph snapshot
// =============================================================================

/* Copies `text` into the snapshot's name table; an empty name when it does
   not fit. */
vkr_internal VkrRgSnapshotName vkr_rg_snapshot_name(VkrRgSnapshot *snapshot,
                                                    const uint8_t *text,
                                                    uint64_t length) {
  if (!text || length == 0u) {
    return (VkrRgSnapshotName){0};
  }

  /* A name already stored is shared: every use repeats its resource's name,
     and a large graph's copies would otherwise fill the table before the
     resources' own names. */
  const VkrHashTableKey key = vkr_hash_table_key_string8(
      (String8){.str = (uint8_t *)text, .length = length});
  const uint32_t mask = VKR_RG_SNAPSHOT_NAME_SLOTS - 1u;
  uint32_t slot = (uint32_t)key.hash & mask;
  uint32_t free_slot = UINT32_MAX;
  for (uint32_t probe = 0u; probe < VKR_RG_SNAPSHOT_NAME_SLOTS; ++probe) {
    const VkrRgSnapshotName stored = snapshot->name_slots[slot];
    if (stored.length == 0u) {
      free_slot = slot;
      break;
    }
    if (stored.length == length &&
        MemCompare(snapshot->names + stored.offset, text, length) == 0) {
      return stored;
    }
    slot = (slot + 1u) & mask;
  }

  if (snapshot->name_bytes + length > VKR_RG_SNAPSHOT_NAME_BYTES) {
    snapshot->truncated = true_v;
    return (VkrRgSnapshotName){0};
  }
  const VkrRgSnapshotName name = {.offset = snapshot->name_bytes,
                                  .length = (uint32_t)length};
  MemCopy(snapshot->names + snapshot->name_bytes, text, length);
  snapshot->name_bytes += (uint32_t)length;
  /* A full table still stores the name, only unshared. */
  if (free_slot != UINT32_MAX) {
    snapshot->name_slots[free_slot] = name;
  }
  return name;
}

vkr_internal VkrRgSnapshotName vkr_rg_snapshot_cstr(VkrRgSnapshot *snapshot,
                                                    const char *text) {
  return text ? vkr_rg_snapshot_name(snapshot, (const uint8_t *)text,
                                     string_length(text))
              : (VkrRgSnapshotName){0};
}

vkr_internal VkrRgSnapshotRole vkr_rg_snapshot_role(const char *role) {
  if (!role) {
    return VKR_RG_SNAPSHOT_ROLE_READ;
  }
  if (string_equals(role, "write")) {
    return VKR_RG_SNAPSHOT_ROLE_WRITE;
  }
  if (string_equals(role, "color")) {
    return VKR_RG_SNAPSHOT_ROLE_COLOR;
  }
  if (string_equals(role, "depth")) {
    return VKR_RG_SNAPSHOT_ROLE_DEPTH;
  }
  if (string_equals(role, "depth_read")) {
    return VKR_RG_SNAPSHOT_ROLE_DEPTH_READ;
  }
  return VKR_RG_SNAPSHOT_ROLE_READ;
}

vkr_internal void vkr_rg_snapshot_pass(void *user,
                                       const VkrRgDescribedPass *pass) {
  VkrRgSnapshot *snapshot = user;
  if (snapshot->pass_count == VKR_RG_SNAPSHOT_PASS_MAX) {
    snapshot->truncated = true_v;
    return;
  }
  VkrRgSnapshotPass *out = &snapshot->passes[snapshot->pass_count++];
  *out = (VkrRgSnapshotPass){
      .name = vkr_rg_snapshot_name(snapshot, pass->name.str, pass->name.length),
      .type = pass->type,
      .order = pass->order,
      .first_use = snapshot->use_count,
  };
  out->flags |= pass->culled ? VKR_RG_SNAPSHOT_PASS_CULLED : 0u;
  out->flags |= pass->disabled ? VKR_RG_SNAPSHOT_PASS_DISABLED : 0u;
  out->flags |=
      pass->use_total > pass->use_count ? VKR_RG_SNAPSHOT_PASS_USES_CUT : 0u;
  for (uint32_t i = 0u; i < pass->use_count; ++i) {
    if (snapshot->use_count == VKR_RG_SNAPSHOT_USE_MAX) {
      snapshot->truncated = true_v;
      break;
    }
    const VkrRgDescribedUse *use = &pass->uses[i];
    snapshot->uses[snapshot->use_count++] = (VkrRgSnapshotUse){
        .resource = vkr_rg_snapshot_name(snapshot, use->resource.str,
                                         use->resource.length),
        .role = vkr_rg_snapshot_role(use->role),
        .buffer = use->buffer,
        .write = use->write,
    };
    ++out->use_count;
  }
}

vkr_internal void
vkr_rg_snapshot_resource(void *user, const VkrRgDescribedResource *resource) {
  VkrRgSnapshot *snapshot = user;
  /* A resource of an earlier build stays known; only this one's count. */
  if (!resource->declared) {
    return;
  }
  if (snapshot->resource_count == VKR_RG_SNAPSHOT_RESOURCE_MAX) {
    snapshot->truncated = true_v;
    return;
  }
  uint32_t flags = 0u;
  flags |= resource->buffer ? VKR_RG_SNAPSHOT_RESOURCE_BUFFER : 0u;
  flags |= resource->imported ? VKR_RG_SNAPSHOT_RESOURCE_IMPORTED : 0u;
  flags |= resource->exported ? VKR_RG_SNAPSHOT_RESOURCE_EXPORTED : 0u;
  flags |= resource->present ? VKR_RG_SNAPSHOT_RESOURCE_PRESENT : 0u;
  snapshot->resources[snapshot->resource_count++] = (VkrRgSnapshotResource){
      .name = vkr_rg_snapshot_name(snapshot, resource->name.str,
                                   resource->name.length),
      .format = vkr_rg_snapshot_cstr(snapshot, resource->format_name),
      .lifetime = vkr_rg_snapshot_cstr(snapshot, resource->lifetime),
      .flags = flags,
      .width = resource->width,
      .height = resource->height,
      .depth = resource->depth,
      .layers = resource->layers,
      .mip_levels = resource->mip_levels,
      .samples = resource->samples,
      .bytes = resource->bytes,
      .first_order = resource->first_order,
      .last_order = resource->last_order,
  };
}

bool8_t vkr_rg_snapshot(const VkrRenderGraph *graph, VkrRgSnapshot *out) {
  if (!graph || !out) {
    return false_v;
  }
  out->pass_count = 0u;
  out->use_count = 0u;
  out->resource_count = 0u;
  out->name_bytes = 0u;
  out->truncated = false_v;
  MemZero(out->name_slots, sizeof(out->name_slots));
  const VkrRgDescribeVisitor visitor = {
      .user = out,
      .pass = vkr_rg_snapshot_pass,
      .resource = vkr_rg_snapshot_resource,
  };
  VkrRgDescription description = {0};
  if (!vkr_rg_describe(graph, &visitor, &description)) {
    return false_v;
  }
  out->frame_index = description.frame_index;
  out->viewport_width = description.viewport_width;
  out->viewport_height = description.viewport_height;
  out->render_scale = description.render_scale;
  return true_v;
}

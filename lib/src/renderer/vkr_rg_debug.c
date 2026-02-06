/**
 * @file vkr_rg_debug.c
 * @brief Render graph debug helpers (DOT export).
 */
#include "renderer/vkr_rg_debug.h"

#include "containers/bitset.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_render_graph_internal.h"

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
        vector_get_VkrRgAttachment(&pass->desc.color_attachments, i);
    if (!att) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, att->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = att->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "i",
                                image_index, "color", "red");
  }

  if (pass->desc.has_depth_attachment) {
    VkrRgAttachment *att = &pass->desc.depth_attachment;
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
    VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_reads, i);
    if (!use) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = use->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "i", image_index, "p",
                                pass_index, "read", "blue");
  }

  for (uint64_t i = 0; i < pass->desc.image_writes.length; ++i) {
    VkrRgImageUse *use = vector_get_VkrRgImageUse(&pass->desc.image_writes, i);
    if (!use) {
      continue;
    }
    VkrRgImage *image = vkr_rg_image_from_handle(graph, use->image);
    if (!image) {
      continue;
    }
    uint32_t image_index = use->image.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "i",
                                image_index, "write", "red");
  }

  for (uint64_t i = 0; i < pass->desc.buffer_reads.length; ++i) {
    VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_reads, i);
    if (!use) {
      continue;
    }
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      continue;
    }
    uint32_t buffer_index = use->buffer.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "b", buffer_index, "p",
                                 pass_index, "read", "blue");
  }

  for (uint64_t i = 0; i < pass->desc.buffer_writes.length; ++i) {
    VkrRgBufferUse *use =
        vector_get_VkrRgBufferUse(&pass->desc.buffer_writes, i);
    if (!use) {
      continue;
    }
    VkrRgBuffer *buffer = vkr_rg_buffer_from_handle(graph, use->buffer);
    if (!buffer) {
      continue;
    }
    uint32_t buffer_index = use->buffer.id - 1;
    vkr_rg_dot_write_edge(handle, allocator, "p", pass_index, "b",
                                 buffer_index, "write", "red");
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
    uint32_t to = *vector_get_uint32_t(&pass->out_edges, i);
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
    const char *err_str =
        (err.str != NULL) ? (const char *)err.str : "(null)";
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
    VkrRgPass *pass = vector_get_VkrRgPass(&rg->passes, i);
    vkr_rg_dot_write_pass_node(&handle, allocator, pass, i);
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_RESOURCES) {
    for (uint32_t i = 0; i < rg->images.length; ++i) {
      VkrRgImage *image = vector_get_VkrRgImage(&rg->images, i);
      bool8_t is_present = vkr_rg_image_handle_valid(rg->present_image) &&
                           (rg->present_image.id - 1) == i;
      vkr_rg_dot_write_image_node(&handle, allocator, image, i, is_present);
    }

    for (uint32_t i = 0; i < rg->buffers.length; ++i) {
      VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&rg->buffers, i);
      vkr_rg_dot_write_buffer_node(&handle, allocator, buffer, i);
    }
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_RESOURCES) {
    for (uint32_t i = 0; i < rg->passes.length; ++i) {
      VkrRgPass *pass = vector_get_VkrRgPass(&rg->passes, i);
      vkr_rg_dot_write_pass_edges(&handle, allocator, rg, pass, i);
    }
  }

  if (desc->flags & VKR_RG_DOT_EXPORT_PASS_EDGES) {
    for (uint32_t i = 0; i < rg->passes.length; ++i) {
      VkrRgPass *pass = vector_get_VkrRgPass(&rg->passes, i);
      vkr_rg_dot_write_pass_edges_direct(&handle, allocator, pass, i);
    }
  }

  vkr_rg_dot_write_fmt(&handle, allocator, "}\n");
  file_close(&handle);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return true_v;
}

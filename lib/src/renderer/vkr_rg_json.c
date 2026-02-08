#include "renderer/vkr_rg_json.h"

#include "containers/bitset.h"
#include "core/logger.h"
#include "core/vkr_json.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "renderer/vkr_render_graph_internal.h"

typedef struct VkrRgJsonParseContext {
  VkrAllocator *allocator;
  const char *path;
  VkrRgJsonGraph *graph;
} VkrRgJsonParseContext;

vkr_internal bool8_t vkr_rg_json_error(VkrRgJsonParseContext *ctx,
                                       const char *field, const char *message) {
  if (ctx && ctx->path) {
    log_error("RenderGraph JSON '%s': %s: %s", ctx->path, field, message);
  } else {
    log_error("RenderGraph JSON: %s: %s", field, message);
  }
  return false_v;
}

vkr_internal bool8_t vkr_rg_json_parse_condition(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrRgJsonCondition *out_condition) {
  assert_log(out_condition != NULL, "out_condition is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_condition = (VkrRgJsonCondition){0};

  VkrJsonReader reader = *obj;
  if (!vkr_json_find_field(&reader, "condition")) {
    out_condition->kind = VKR_RG_JSON_CONDITION_NONE;
    return true_v;
  }

  String8 raw = {0};
  if (!vkr_json_parse_string(&reader, &raw)) {
    return vkr_rg_json_error(ctx, field_path, "condition must be a string");
  }

  String8 trimmed = raw;
  string8_trim(&trimmed);
  if (vkr_string8_equals_cstr_i(&trimmed, "editor_enabled")) {
    out_condition->kind = VKR_RG_JSON_CONDITION_EDITOR_ENABLED;
  } else if (vkr_string8_equals_cstr_i(&trimmed, "!editor_enabled")) {
    out_condition->kind = VKR_RG_JSON_CONDITION_EDITOR_DISABLED;
  } else {
    return vkr_rg_json_error(ctx, field_path, "unknown condition expression");
  }

  out_condition->raw = raw;
  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_repeat(VkrRgJsonParseContext *ctx,
                                              VkrJsonReader *obj,
                                              const char *field_path,
                                              VkrRgJsonRepeat *out_repeat) {
  assert_log(out_repeat != NULL, "out_repeat is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_repeat = (VkrRgJsonRepeat){0};

  VkrJsonReader reader = *obj;
  if (!vkr_json_find_field(&reader, "repeat")) {
    return true_v;
  }

  VkrJsonReader repeat_obj = {0};
  if (!vkr_json_enter_object(&reader, &repeat_obj)) {
    return vkr_rg_json_error(ctx, field_path, "repeat must be an object");
  }

  VkrJsonReader count_reader = repeat_obj;
  if (!vkr_json_get_string(&count_reader, "count_source",
                           &out_repeat->count_source)) {
    return vkr_rg_json_error(ctx, field_path,
                             "repeat.count_source is required");
  }

  out_repeat->enabled = true_v;
  return true_v;
}

vkr_internal bool8_t
vkr_rg_json_parse_resource_flags(VkrRgJsonParseContext *ctx, VkrJsonReader *obj,
                                 const char *field_path, uint32_t *out_flags) {
  assert_log(out_flags != NULL, "out_flags is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_flags = 0;

  VkrJsonReader flags_reader = *obj;
  if (!vkr_json_find_array(&flags_reader, "flags")) {
    return true_v;
  }

  while (vkr_json_next_array_element(&flags_reader)) {
    String8 value = {0};
    if (!vkr_json_parse_string(&flags_reader, &value)) {
      return vkr_rg_json_error(ctx, field_path, "flags must be strings");
    }

    if (vkr_string8_equals_cstr_i(&value, "TRANSIENT")) {
      *out_flags |= VKR_RG_JSON_RESOURCE_FLAG_TRANSIENT;
    } else if (vkr_string8_equals_cstr_i(&value, "PERSISTENT")) {
      *out_flags |= VKR_RG_JSON_RESOURCE_FLAG_PERSISTENT;
    } else if (vkr_string8_equals_cstr_i(&value, "EXTERNAL")) {
      *out_flags |= VKR_RG_JSON_RESOURCE_FLAG_EXTERNAL;
    } else if (vkr_string8_equals_cstr_i(&value, "PER_IMAGE")) {
      *out_flags |= VKR_RG_JSON_RESOURCE_FLAG_PER_IMAGE;
    } else if (vkr_string8_equals_cstr_i(&value, "RESIZABLE")) {
      *out_flags |= VKR_RG_JSON_RESOURCE_FLAG_RESIZABLE;
    } else {
      return vkr_rg_json_error(ctx, field_path, "unknown resource flag");
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_pass_flags(VkrRgJsonParseContext *ctx,
                                                  VkrJsonReader *obj,
                                                  const char *field_path,
                                                  VkrRgPassFlags *out_flags) {
  assert_log(out_flags != NULL, "out_flags is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_flags = VKR_RG_PASS_FLAG_NONE;

  VkrJsonReader flags_reader = *obj;
  if (!vkr_json_find_array(&flags_reader, "flags")) {
    return true_v;
  }

  while (vkr_json_next_array_element(&flags_reader)) {
    String8 value = {0};
    if (!vkr_json_parse_string(&flags_reader, &value)) {
      return vkr_rg_json_error(ctx, field_path, "flags must be strings");
    }

    if (vkr_string8_equals_cstr_i(&value, "NO_CULL")) {
      *out_flags |= VKR_RG_PASS_FLAG_NO_CULL;
    } else if (vkr_string8_equals_cstr_i(&value, "DISABLED")) {
      *out_flags |= VKR_RG_PASS_FLAG_DISABLED;
    } else {
      return vkr_rg_json_error(ctx, field_path, "unknown pass flag");
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_extent(VkrRgJsonParseContext *ctx,
                                              VkrJsonReader *obj,
                                              const char *field_path,
                                              VkrRgJsonExtent *out_extent) {
  assert_log(out_extent != NULL, "out_extent is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_extent = (VkrRgJsonExtent){0};

  VkrJsonReader extent_reader = *obj;
  if (!vkr_json_find_field(&extent_reader, "extent")) {
    return true_v;
  }

  VkrJsonReader extent_obj = {0};
  if (!vkr_json_enter_object(&extent_reader, &extent_obj)) {
    return vkr_rg_json_error(ctx, field_path, "extent must be an object");
  }

  VkrJsonReader mode_reader = extent_obj;
  String8 mode = {0};
  if (!vkr_json_get_string(&mode_reader, "mode", &mode)) {
    return vkr_rg_json_error(ctx, field_path, "extent.mode is required");
  }

  if (vkr_string8_equals_cstr_i(&mode, "window")) {
    out_extent->mode = VKR_RG_JSON_EXTENT_WINDOW;
  } else if (vkr_string8_equals_cstr_i(&mode, "viewport")) {
    out_extent->mode = VKR_RG_JSON_EXTENT_VIEWPORT;
  } else if (vkr_string8_equals_cstr_i(&mode, "fixed")) {
    out_extent->mode = VKR_RG_JSON_EXTENT_FIXED;
  } else if (vkr_string8_equals_cstr_i(&mode, "square")) {
    out_extent->mode = VKR_RG_JSON_EXTENT_SQUARE;
  } else {
    return vkr_rg_json_error(ctx, field_path, "unknown extent mode");
  }

  if (out_extent->mode == VKR_RG_JSON_EXTENT_FIXED) {
    VkrJsonReader width_reader = extent_obj;
    VkrJsonReader height_reader = extent_obj;
    int32_t width = 0;
    int32_t height = 0;
    if (!vkr_json_get_int(&width_reader, "width", &width) || width <= 0) {
      return vkr_rg_json_error(ctx, field_path, "extent.width must be > 0");
    }
    if (!vkr_json_get_int(&height_reader, "height", &height) || height <= 0) {
      return vkr_rg_json_error(ctx, field_path, "extent.height must be > 0");
    }
    out_extent->width = (uint32_t)width;
    out_extent->height = (uint32_t)height;
  } else if (out_extent->mode == VKR_RG_JSON_EXTENT_SQUARE) {
    VkrJsonReader size_reader = extent_obj;
    if (!vkr_json_get_string(&size_reader, "size_source",
                             &out_extent->size_source)) {
      return vkr_rg_json_error(ctx, field_path,
                               "extent.size_source is required for square");
    }
  }

  return true_v;
}

typedef struct VkrRgJsonFormatMap {
  const char *name;
  VkrTextureFormat format;
} VkrRgJsonFormatMap;

vkr_global const VkrRgJsonFormatMap k_rg_json_format_map[] = {
    {"R8G8B8A8_UNORM", VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM},
    {"R8G8B8A8_SRGB", VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB},
    {"B8G8R8A8_UNORM", VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM},
    {"B8G8R8A8_SRGB", VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB},
    {"R8G8B8A8_UINT", VKR_TEXTURE_FORMAT_R8G8B8A8_UINT},
    {"R8G8B8A8_SNORM", VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM},
    {"R8G8B8A8_SINT", VKR_TEXTURE_FORMAT_R8G8B8A8_SINT},
    {"R8_UNORM", VKR_TEXTURE_FORMAT_R8_UNORM},
    {"R16_SFLOAT", VKR_TEXTURE_FORMAT_R16_SFLOAT},
    {"R32_SFLOAT", VKR_TEXTURE_FORMAT_R32_SFLOAT},
    {"R32_UINT", VKR_TEXTURE_FORMAT_R32_UINT},
    {"R8G8_UNORM", VKR_TEXTURE_FORMAT_R8G8_UNORM},
    {"D16_UNORM", VKR_TEXTURE_FORMAT_D16_UNORM},
    {"D32_SFLOAT", VKR_TEXTURE_FORMAT_D32_SFLOAT},
    {"D24_UNORM_S8_UINT", VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT},
};

vkr_internal bool8_t vkr_rg_json_parse_format(VkrRgJsonParseContext *ctx,
                                              String8 value,
                                              VkrTextureFormat *out_format,
                                              VkrRgJsonImageFormatSource
                                                  *out_format_source) {
  assert_log(out_format != NULL, "out_format is NULL");
  assert_log(out_format_source != NULL, "out_format_source is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  String8 trimmed = value;
  string8_trim(&trimmed);

  if (vkr_string8_equals_cstr_i(&trimmed, "SWAPCHAIN")) {
    *out_format_source = VKR_RG_JSON_IMAGE_FORMAT_SWAPCHAIN;
    *out_format = VKR_TEXTURE_FORMAT_COUNT;
    return true_v;
  }

  if (vkr_string8_equals_cstr_i(&trimmed, "SWAPCHAIN_DEPTH")) {
    *out_format_source = VKR_RG_JSON_IMAGE_FORMAT_SWAPCHAIN_DEPTH;
    *out_format = VKR_TEXTURE_FORMAT_COUNT;
    return true_v;
  }

  if (vkr_string8_equals_cstr_i(&trimmed, "SHADOW_DEPTH")) {
    *out_format_source = VKR_RG_JSON_IMAGE_FORMAT_SHADOW_DEPTH;
    *out_format = VKR_TEXTURE_FORMAT_COUNT;
    return true_v;
  }

  uint32_t format_count = (uint32_t)(sizeof(k_rg_json_format_map) /
                                     sizeof(k_rg_json_format_map[0]));
  for (uint32_t i = 0; i < format_count; ++i) {
    if (vkr_string8_equals_cstr_i(&trimmed, k_rg_json_format_map[i].name)) {
      *out_format_source = VKR_RG_JSON_IMAGE_FORMAT_EXPLICIT;
      *out_format = k_rg_json_format_map[i].format;
      return true_v;
    }
  }

  return vkr_rg_json_error(ctx, "resource.format", "unknown format token");
}

vkr_internal bool8_t vkr_rg_json_parse_texture_usage(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrTextureUsageFlags *out_flags) {
  assert_log(out_flags != NULL, "out_flags is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_flags = vkr_texture_usage_flags_create();

  VkrJsonReader usage_reader = *obj;
  if (!vkr_json_find_array(&usage_reader, "usage")) {
    return true_v;
  }

  while (vkr_json_next_array_element(&usage_reader)) {
    String8 value = {0};
    if (!vkr_json_parse_string(&usage_reader, &value)) {
      return vkr_rg_json_error(ctx, field_path, "usage must be strings");
    }

    if (vkr_string8_equals_cstr_i(&value, "SAMPLED")) {
      bitset8_set(out_flags, VKR_TEXTURE_USAGE_SAMPLED);
    } else if (vkr_string8_equals_cstr_i(&value, "COLOR_ATTACHMENT")) {
      bitset8_set(out_flags, VKR_TEXTURE_USAGE_COLOR_ATTACHMENT);
    } else if (vkr_string8_equals_cstr_i(&value, "DEPTH_STENCIL_ATTACHMENT")) {
      bitset8_set(out_flags, VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT);
    } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_SRC")) {
      bitset8_set(out_flags, VKR_TEXTURE_USAGE_TRANSFER_SRC);
    } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_DST")) {
      bitset8_set(out_flags, VKR_TEXTURE_USAGE_TRANSFER_DST);
    } else {
      return vkr_rg_json_error(ctx, field_path, "unknown texture usage");
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_buffer_usage(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrBufferUsageFlags *out_flags) {
  assert_log(out_flags != NULL, "out_flags is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_flags = vkr_buffer_usage_flags_create();

  VkrJsonReader usage_reader = *obj;
  if (!vkr_json_find_array(&usage_reader, "usage")) {
    return true_v;
  }

  while (vkr_json_next_array_element(&usage_reader)) {
    String8 value = {0};
    if (!vkr_json_parse_string(&usage_reader, &value)) {
      return vkr_rg_json_error(ctx, field_path, "usage must be strings");
    }

    if (vkr_string8_equals_cstr_i(&value, "VERTEX_BUFFER")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_VERTEX_BUFFER);
    } else if (vkr_string8_equals_cstr_i(&value, "INDEX_BUFFER")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_INDEX_BUFFER);
    } else if (vkr_string8_equals_cstr_i(&value, "GLOBAL_UNIFORM_BUFFER")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER);
    } else if (vkr_string8_equals_cstr_i(&value, "UNIFORM")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_UNIFORM);
    } else if (vkr_string8_equals_cstr_i(&value, "STORAGE")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_STORAGE);
    } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_SRC")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_TRANSFER_SRC);
    } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_DST")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_TRANSFER_DST);
    } else if (vkr_string8_equals_cstr_i(&value, "INDIRECT")) {
      bitset8_set(out_flags, VKR_BUFFER_USAGE_INDIRECT);
    } else {
      return vkr_rg_json_error(ctx, field_path, "unknown buffer usage");
    }
  }

  return true_v;
}

vkr_internal bool8_t
vkr_rg_json_parse_image_access(String8 value, VkrRgJsonImageAccessFlags *out) {
  assert_log(out != NULL, "out is NULL");

  *out = VKR_RG_JSON_IMAGE_ACCESS_NONE;

  if (vkr_string8_equals_cstr_i(&value, "SAMPLED")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_SAMPLED;
  } else if (vkr_string8_equals_cstr_i(&value, "STORAGE_READ")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_STORAGE_READ;
  } else if (vkr_string8_equals_cstr_i(&value, "STORAGE_WRITE")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_STORAGE_WRITE;
  } else if (vkr_string8_equals_cstr_i(&value, "COLOR_ATTACHMENT")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_COLOR_ATTACHMENT;
  } else if (vkr_string8_equals_cstr_i(&value, "DEPTH_ATTACHMENT")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_DEPTH_ATTACHMENT;
  } else if (vkr_string8_equals_cstr_i(&value, "DEPTH_READ_ONLY")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_DEPTH_READ_ONLY;
  } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_SRC")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_SRC;
  } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_DST")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_DST;
  } else if (vkr_string8_equals_cstr_i(&value, "PRESENT")) {
    *out = VKR_RG_JSON_IMAGE_ACCESS_PRESENT;
  } else {
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_buffer_access(
    String8 value, VkrRgJsonBufferAccessFlags *out) {
  assert_log(out != NULL, "out is NULL");

  *out = VKR_RG_JSON_BUFFER_ACCESS_NONE;

  if (vkr_string8_equals_cstr_i(&value, "VERTEX")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_VERTEX;
  } else if (vkr_string8_equals_cstr_i(&value, "INDEX")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_INDEX;
  } else if (vkr_string8_equals_cstr_i(&value, "UNIFORM")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_UNIFORM;
  } else if (vkr_string8_equals_cstr_i(&value, "STORAGE_READ")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_STORAGE_READ;
  } else if (vkr_string8_equals_cstr_i(&value, "STORAGE_WRITE")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_STORAGE_WRITE;
  } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_SRC")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_TRANSFER_SRC;
  } else if (vkr_string8_equals_cstr_i(&value, "TRANSFER_DST")) {
    *out = VKR_RG_JSON_BUFFER_ACCESS_TRANSFER_DST;
  } else {
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_index(VkrRgJsonParseContext *ctx,
                                             VkrJsonReader *obj,
                                             const char *field_path,
                                             const char *field_name,
                                             VkrRgJsonIndex *out_index) {
  assert_log(out_index != NULL, "out_index is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(field_name != NULL, "field_name is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_index = (VkrRgJsonIndex){0};

  VkrJsonReader index_reader = *obj;
  if (!vkr_json_find_field(&index_reader, field_name)) {
    return true_v;
  }

  vkr_json_skip_whitespace(&index_reader);
  if (index_reader.pos < index_reader.length &&
      index_reader.data[index_reader.pos] == '"') {
    String8 token = {0};
    if (!vkr_json_parse_string(&index_reader, &token)) {
      return vkr_rg_json_error(ctx, field_path, "index must be string or int");
    }
    out_index->is_set = true_v;
    out_index->is_token = true_v;
    out_index->token = token;
    return true_v;
  }

  int32_t idx = 0;
  if (!vkr_json_parse_int(&index_reader, &idx)) {
    return vkr_rg_json_error(ctx, field_path, "index must be string or int");
  }
  if (idx < 0) {
    return vkr_rg_json_error(ctx, field_path, "index must be >= 0");
  }
  out_index->is_set = true_v;
  out_index->value = (uint32_t)idx;
  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_image_desc(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrRgJsonImageDesc *out_desc) {
  assert_log(out_desc != NULL, "out_desc is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_desc = (VkrRgJsonImageDesc){0};
  out_desc->usage = vkr_texture_usage_flags_create();
  out_desc->format_source = VKR_RG_JSON_IMAGE_FORMAT_EXPLICIT;

  VkrJsonReader import_reader = *obj;
  if (vkr_json_find_field(&import_reader, "import")) {
    if (!vkr_json_parse_string(&import_reader, &out_desc->import_name)) {
      return vkr_rg_json_error(ctx, field_path, "import must be a string");
    }
    out_desc->is_import = true_v;
  }

  if (!vkr_rg_json_parse_extent(ctx, obj, field_path, &out_desc->extent)) {
    return false_v;
  }

  VkrJsonReader layers_source_reader = *obj;
  if (vkr_json_find_field(&layers_source_reader, "layers_source")) {
    if (!vkr_json_parse_string(&layers_source_reader,
                               &out_desc->layers_source)) {
      return vkr_rg_json_error(ctx, field_path,
                               "layers_source must be a string");
    }
  }

  VkrJsonReader layers_reader = *obj;
  int32_t layers = 0;
  if (vkr_json_get_int(&layers_reader, "layers", &layers)) {
    if (layers <= 0) {
      return vkr_rg_json_error(ctx, field_path, "layers must be >= 1");
    }
    out_desc->layers_is_set = true_v;
    out_desc->layers = (uint32_t)layers;
  }

  if (out_desc->layers_is_set && out_desc->layers_source.length > 0) {
    return vkr_rg_json_error(ctx, field_path,
                             "layers and layers_source are mutually exclusive");
  }

  VkrJsonReader format_reader = *obj;
  if (vkr_json_find_field(&format_reader, "format")) {
    String8 fmt = {0};
    if (!vkr_json_parse_string(&format_reader, &fmt)) {
      return vkr_rg_json_error(ctx, field_path, "format must be a string");
    }
    if (!vkr_rg_json_parse_format(ctx, fmt, &out_desc->format,
                                  &out_desc->format_source)) {
      return false_v;
    }
  } else if (!out_desc->is_import) {
    return vkr_rg_json_error(ctx, field_path, "format is required");
  }

  if (!vkr_rg_json_parse_texture_usage(ctx, obj, field_path,
                                       &out_desc->usage)) {
    return false_v;
  }

  if (!out_desc->is_import &&
      out_desc->extent.mode == VKR_RG_JSON_EXTENT_NONE) {
    return vkr_rg_json_error(ctx, field_path, "extent is required");
  }
  if (!out_desc->is_import && out_desc->usage.set == 0) {
    return vkr_rg_json_error(ctx, field_path, "usage is required");
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_buffer_desc(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrRgJsonBufferDesc *out_desc) {
  assert_log(out_desc != NULL, "out_desc is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_desc = (VkrRgJsonBufferDesc){0};
  out_desc->usage = vkr_buffer_usage_flags_create();

  VkrJsonReader size_reader = *obj;
  int32_t size = 0;
  if (!vkr_json_get_int(&size_reader, "size", &size) || size <= 0) {
    return vkr_rg_json_error(ctx, field_path, "buffer size is required");
  }
  out_desc->size = (uint64_t)size;

  if (!vkr_rg_json_parse_buffer_usage(ctx, obj, field_path, &out_desc->usage)) {
    return false_v;
  }
  if (out_desc->usage.set == 0) {
    return vkr_rg_json_error(ctx, field_path, "usage is required");
  }

  return true_v;
}

vkr_internal bool8_t
vkr_rg_json_parse_resource(VkrRgJsonParseContext *ctx, VkrJsonReader *obj,
                           uint32_t index, VkrRgJsonResource *out_resource) {
  assert_log(out_resource != NULL, "out_resource is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_resource = (VkrRgJsonResource){0};

  char field_path[128];
  snprintf(field_path, sizeof(field_path), "resources[%u]", index);

  VkrJsonReader name_reader = *obj;
  if (!vkr_json_get_string(&name_reader, "name", &out_resource->name)) {
    return vkr_rg_json_error(ctx, field_path, "name is required");
  }

  VkrJsonReader type_reader = *obj;
  String8 type = {0};
  if (!vkr_json_get_string(&type_reader, "type", &type)) {
    return vkr_rg_json_error(ctx, field_path, "type is required");
  }

  if (vkr_string8_equals_cstr_i(&type, "image")) {
    out_resource->type = VKR_RG_JSON_RESOURCE_IMAGE;
  } else if (vkr_string8_equals_cstr_i(&type, "buffer")) {
    out_resource->type = VKR_RG_JSON_RESOURCE_BUFFER;
  } else {
    return vkr_rg_json_error(ctx, field_path, "unknown resource type");
  }

  if (!vkr_rg_json_parse_condition(ctx, obj, field_path,
                                   &out_resource->condition)) {
    return false_v;
  }

  if (!vkr_rg_json_parse_repeat(ctx, obj, field_path, &out_resource->repeat)) {
    return false_v;
  }

  if (!vkr_rg_json_parse_resource_flags(ctx, obj, field_path,
                                        &out_resource->flags)) {
    return false_v;
  }

  if (out_resource->type == VKR_RG_JSON_RESOURCE_IMAGE) {
    if (!vkr_rg_json_parse_image_desc(ctx, obj, field_path,
                                      &out_resource->image)) {
      return false_v;
    }
  } else {
    if (!vkr_rg_json_parse_buffer_desc(ctx, obj, field_path,
                                       &out_resource->buffer)) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_use(VkrRgJsonParseContext *ctx,
                                           VkrJsonReader *obj,
                                           const char *field_path,
                                           VkrRgJsonResourceUse *out_use) {
  assert_log(out_use != NULL, "out_use is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_use = (VkrRgJsonResourceUse){0};

  VkrJsonReader image_reader = *obj;
  VkrJsonReader buffer_reader = *obj;
  bool8_t has_image =
      vkr_json_get_string(&image_reader, "image", &out_use->name);
  bool8_t has_buffer =
      vkr_json_get_string(&buffer_reader, "buffer", &out_use->name);

  if (has_image == has_buffer) {
    return vkr_rg_json_error(ctx, field_path,
                             "use must specify either image or buffer");
  }

  out_use->is_image = has_image;

  VkrJsonReader access_reader = *obj;
  String8 access = {0};
  if (!vkr_json_get_string(&access_reader, "access", &access)) {
    return vkr_rg_json_error(ctx, field_path, "access is required");
  }

  if (out_use->is_image) {
    if (!vkr_rg_json_parse_image_access(access, &out_use->image_access)) {
      return vkr_rg_json_error(ctx, field_path, "unknown image access");
    }
  } else {
    if (!vkr_rg_json_parse_buffer_access(access, &out_use->buffer_access)) {
      return vkr_rg_json_error(ctx, field_path, "unknown buffer access");
    }
  }

  if (!vkr_rg_json_parse_repeat(ctx, obj, field_path, &out_use->repeat)) {
    return false_v;
  }

  VkrJsonReader binding_reader = *obj;
  int32_t binding = 0;
  if (vkr_json_get_int(&binding_reader, "binding", &binding)) {
    if (binding < 0) {
      return vkr_rg_json_error(ctx, field_path, "binding must be >= 0");
    }
    out_use->binding.is_set = true_v;
    out_use->binding.value = (uint32_t)binding;
  }

  if (!vkr_rg_json_parse_index(ctx, obj, field_path, "array_index",
                               &out_use->array_index)) {
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_clear_color(VkrRgJsonParseContext *ctx,
                                                   VkrJsonReader *obj,
                                                   const char *field_path,
                                                   VkrClearValue *out_value) {
  assert_log(out_value != NULL, "out_value is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  VkrJsonReader color_reader = *obj;
  if (!vkr_json_find_array(&color_reader, "color")) {
    return vkr_rg_json_error(ctx, field_path, "clear.color must be an array");
  }

  float32_t components[4] = {0};
  uint32_t count = 0;
  while (vkr_json_next_array_element(&color_reader)) {
    if (count >= 4) {
      return vkr_rg_json_error(ctx, field_path,
                               "clear.color requires 4 components");
    }
    if (!vkr_json_parse_float(&color_reader, &components[count])) {
      return vkr_rg_json_error(ctx, field_path,
                               "clear.color values must be numbers");
    }
    count++;
  }

  if (count != 4) {
    return vkr_rg_json_error(ctx, field_path,
                             "clear.color requires 4 components");
  }

  out_value->color_f32.r = components[0];
  out_value->color_f32.g = components[1];
  out_value->color_f32.b = components[2];
  out_value->color_f32.a = components[3];
  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_clear_depth(VkrRgJsonParseContext *ctx,
                                                   VkrJsonReader *obj,
                                                   const char *field_path,
                                                   VkrClearValue *out_value) {
  assert_log(out_value != NULL, "out_value is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  VkrJsonReader depth_reader = *obj;
  VkrJsonReader stencil_reader = *obj;
  float32_t depth = 0.0f;
  int32_t stencil = 0;

  if (!vkr_json_get_float(&depth_reader, "depth", &depth)) {
    return vkr_rg_json_error(ctx, field_path, "clear.depth is required");
  }

  if (vkr_json_get_int(&stencil_reader, "stencil", &stencil)) {
    if (stencil < 0) {
      return vkr_rg_json_error(ctx, field_path, "clear.stencil must be >= 0");
    }
  }

  out_value->depth_stencil.depth = depth;
  out_value->depth_stencil.stencil = (uint32_t)stencil;
  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_attachment(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrRgJsonAttachment *out_attach, bool8_t *out_read_only) {
  assert_log(out_attach != NULL, "out_attach is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_attach = (VkrRgJsonAttachment){0};
  if (out_read_only) {
    *out_read_only = false_v;
  }

  VkrJsonReader image_reader = *obj;
  if (!vkr_json_get_string(&image_reader, "image", &out_attach->image)) {
    return vkr_rg_json_error(ctx, field_path, "attachment image is required");
  }

  VkrJsonReader load_reader = *obj;
  String8 load = {0};
  if (!vkr_json_get_string(&load_reader, "load", &load)) {
    return vkr_rg_json_error(ctx, field_path, "attachment load is required");
  }

  if (vkr_string8_equals_cstr_i(&load, "LOAD")) {
    out_attach->load_op = VKR_ATTACHMENT_LOAD_OP_LOAD;
  } else if (vkr_string8_equals_cstr_i(&load, "CLEAR")) {
    out_attach->load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR;
  } else if (vkr_string8_equals_cstr_i(&load, "DONT_CARE")) {
    out_attach->load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE;
  } else {
    return vkr_rg_json_error(ctx, field_path, "unknown load op");
  }

  VkrJsonReader store_reader = *obj;
  String8 store = {0};
  if (!vkr_json_get_string(&store_reader, "store", &store)) {
    return vkr_rg_json_error(ctx, field_path, "attachment store is required");
  }

  if (vkr_string8_equals_cstr_i(&store, "STORE")) {
    out_attach->store_op = VKR_ATTACHMENT_STORE_OP_STORE;
  } else if (vkr_string8_equals_cstr_i(&store, "DONT_CARE")) {
    out_attach->store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE;
  } else {
    return vkr_rg_json_error(ctx, field_path, "unknown store op");
  }

  VkrJsonReader clear_reader = *obj;
  if (vkr_json_find_field(&clear_reader, "clear")) {
    VkrJsonReader clear_obj = {0};
    if (!vkr_json_enter_object(&clear_reader, &clear_obj)) {
      return vkr_rg_json_error(ctx, field_path, "clear must be an object");
    }

    VkrJsonReader color_reader = clear_obj;
    if (vkr_json_find_field(&color_reader, "color")) {
      if (!vkr_rg_json_parse_clear_color(ctx, &clear_obj, field_path,
                                         &out_attach->clear_value)) {
        return false_v;
      }
      out_attach->has_clear = true_v;
    } else {
      if (!vkr_rg_json_parse_clear_depth(ctx, &clear_obj, field_path,
                                         &out_attach->clear_value)) {
        return false_v;
      }
      out_attach->has_clear = true_v;
    }
  }

  if (out_read_only) {
    VkrJsonReader read_only_reader = *obj;
    bool8_t read_only = false_v;
    if (vkr_json_get_bool(&read_only_reader, "read_only", &read_only)) {
      *out_read_only = read_only;
    }
  }

  VkrJsonReader slice_reader = *obj;
  if (vkr_json_find_field(&slice_reader, "slice")) {
    VkrJsonReader slice_obj = {0};
    if (!vkr_json_enter_object(&slice_reader, &slice_obj)) {
      return vkr_rg_json_error(ctx, field_path, "slice must be an object");
    }

    if (!vkr_rg_json_parse_index(ctx, &slice_obj, field_path, "mip_level",
                                 &out_attach->slice_mip_level)) {
      return false_v;
    }
    if (!vkr_rg_json_parse_index(ctx, &slice_obj, field_path, "base_layer",
                                 &out_attach->slice_base_layer)) {
      return false_v;
    }
    if (!vkr_rg_json_parse_index(ctx, &slice_obj, field_path, "layer_count",
                                 &out_attach->slice_layer_count)) {
      return false_v;
    }

    out_attach->has_slice = out_attach->slice_mip_level.is_set ||
                            out_attach->slice_base_layer.is_set ||
                            out_attach->slice_layer_count.is_set;
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_attachments(
    VkrRgJsonParseContext *ctx, VkrJsonReader *obj, const char *field_path,
    VkrRgJsonAttachments *out_attach) {
  assert_log(out_attach != NULL, "out_attach is NULL");
  assert_log(field_path != NULL, "field_path is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_attach = (VkrRgJsonAttachments){0};
  out_attach->colors = vector_create_VkrRgJsonAttachment(ctx->allocator);

  VkrJsonReader attachments_reader = *obj;
  if (!vkr_json_find_field(&attachments_reader, "attachments")) {
    return true_v;
  }

  VkrJsonReader attachments_obj = {0};
  if (!vkr_json_enter_object(&attachments_reader, &attachments_obj)) {
    return vkr_rg_json_error(ctx, field_path, "attachments must be an object");
  }

  VkrJsonReader color_reader = attachments_obj;
  if (vkr_json_find_array(&color_reader, "color")) {
    uint32_t color_index = 0;
    while (vkr_json_next_array_element(&color_reader)) {
      VkrJsonReader color_obj = {0};
      if (!vkr_json_enter_object(&color_reader, &color_obj)) {
        return vkr_rg_json_error(ctx, field_path,
                                 "color attachment must be object");
      }

      VkrRgJsonAttachment attachment = {0};
      char color_path[128];
      snprintf(color_path, sizeof(color_path), "%s.color[%u]", field_path,
               color_index);
      if (!vkr_rg_json_parse_attachment(ctx, &color_obj, color_path,
                                        &attachment, NULL)) {
        return false_v;
      }

      vector_push_VkrRgJsonAttachment(&out_attach->colors, attachment);
      color_index++;
    }
  }

  VkrJsonReader depth_reader = attachments_obj;
  if (vkr_json_find_field(&depth_reader, "depth")) {
    VkrJsonReader depth_obj = {0};
    if (!vkr_json_enter_object(&depth_reader, &depth_obj)) {
      return vkr_rg_json_error(ctx, field_path,
                               "depth attachment must be object");
    }

    if (!vkr_rg_json_parse_attachment(ctx, &depth_obj, field_path,
                                      &out_attach->depth,
                                      &out_attach->depth_read_only)) {
      return false_v;
    }
    out_attach->has_depth = true_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_resource_exists(const VkrRgJsonGraph *graph,
                                                 String8 name) {
  assert_log(graph != NULL, "graph is NULL");

  for (uint64_t i = 0; i < graph->resources.length; ++i) {
    VkrRgJsonResource *res = vector_get_VkrRgJsonResource(&graph->resources, i);
    if (string8_equals(&res->name, &name)) {
      return true_v;
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_rg_json_parse_pass(VkrRgJsonParseContext *ctx,
                                            VkrJsonReader *obj, uint32_t index,
                                            VkrRgJsonPass *out_pass) {
  assert_log(out_pass != NULL, "out_pass is NULL");
  assert_log(obj != NULL, "obj is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  bool8_t ok = true_v;
  *out_pass = (VkrRgJsonPass){0};
  out_pass->reads = vector_create_VkrRgJsonResourceUse(ctx->allocator);
  out_pass->writes = vector_create_VkrRgJsonResourceUse(ctx->allocator);

  char field_path[128];
  snprintf(field_path, sizeof(field_path), "passes[%u]", index);

  VkrJsonReader name_reader = *obj;
  if (!vkr_json_get_string(&name_reader, "name", &out_pass->name)) {
    ok = vkr_rg_json_error(ctx, field_path, "name is required");
    goto cleanup;
  }

  VkrJsonReader type_reader = *obj;
  String8 type = {0};
  if (!vkr_json_get_string(&type_reader, "type", &type)) {
    ok = vkr_rg_json_error(ctx, field_path, "type is required");
    goto cleanup;
  }

  if (vkr_string8_equals_cstr_i(&type, "graphics")) {
    out_pass->type = VKR_RG_JSON_PASS_GRAPHICS;
  } else if (vkr_string8_equals_cstr_i(&type, "compute")) {
    out_pass->type = VKR_RG_JSON_PASS_COMPUTE;
  } else if (vkr_string8_equals_cstr_i(&type, "transfer")) {
    out_pass->type = VKR_RG_JSON_PASS_TRANSFER;
  } else {
    ok = vkr_rg_json_error(ctx, field_path, "unknown pass type");
    goto cleanup;
  }

  if (!vkr_rg_json_parse_pass_flags(ctx, obj, field_path, &out_pass->flags)) {
    ok = false_v;
    goto cleanup;
  }

  if (!vkr_rg_json_parse_condition(ctx, obj, field_path,
                                   &out_pass->condition)) {
    ok = false_v;
    goto cleanup;
  }

  if (!vkr_rg_json_parse_repeat(ctx, obj, field_path, &out_pass->repeat)) {
    ok = false_v;
    goto cleanup;
  }

  if (out_pass->type == VKR_RG_JSON_PASS_GRAPHICS) {
    VkrJsonReader domain_reader = *obj;
    String8 domain = {0};
    if (!vkr_json_get_string(&domain_reader, "domain", &domain)) {
      ok = vkr_rg_json_error(ctx, field_path, "domain is required");
      goto cleanup;
    }

    out_pass->has_domain = true_v;
    if (vkr_string8_equals_cstr_i(&domain, "WORLD")) {
      out_pass->domain = VKR_PIPELINE_DOMAIN_WORLD;
    } else if (vkr_string8_equals_cstr_i(&domain, "UI")) {
      out_pass->domain = VKR_PIPELINE_DOMAIN_UI;
    } else if (vkr_string8_equals_cstr_i(&domain, "SHADOW")) {
      out_pass->domain = VKR_PIPELINE_DOMAIN_SHADOW;
    } else if (vkr_string8_equals_cstr_i(&domain, "POST")) {
      out_pass->domain = VKR_PIPELINE_DOMAIN_POST;
    } else if (vkr_string8_equals_cstr_i(&domain, "SKYBOX")) {
      out_pass->domain = VKR_PIPELINE_DOMAIN_SKYBOX;
    } else {
      ok = vkr_rg_json_error(ctx, field_path, "unknown pipeline domain");
      goto cleanup;
    }
  }

  VkrJsonReader reads_reader = *obj;
  if (vkr_json_find_array(&reads_reader, "reads")) {
    uint32_t read_index = 0;
    while (vkr_json_next_array_element(&reads_reader)) {
      VkrJsonReader use_obj = {0};
      if (!vkr_json_enter_object(&reads_reader, &use_obj)) {
        ok = vkr_rg_json_error(ctx, field_path, "read entry must be object");
        goto cleanup;
      }

      VkrRgJsonResourceUse use = {0};
      char use_path[128];
      snprintf(use_path, sizeof(use_path), "%s.reads[%u]", field_path,
               read_index);
      if (!vkr_rg_json_parse_use(ctx, &use_obj, use_path, &use)) {
        ok = false_v;
        goto cleanup;
      }

      if (!vkr_rg_json_resource_exists(ctx->graph, use.name)) {
        ok = vkr_rg_json_error(ctx, use_path, "resource not declared");
        goto cleanup;
      }

      vector_push_VkrRgJsonResourceUse(&out_pass->reads, use);
      read_index++;
    }
  }

  VkrJsonReader writes_reader = *obj;
  if (vkr_json_find_array(&writes_reader, "writes")) {
    uint32_t write_index = 0;
    while (vkr_json_next_array_element(&writes_reader)) {
      VkrJsonReader use_obj = {0};
      if (!vkr_json_enter_object(&writes_reader, &use_obj)) {
        ok = vkr_rg_json_error(ctx, field_path, "write entry must be object");
        goto cleanup;
      }

      VkrRgJsonResourceUse use = {0};
      char use_path[128];
      snprintf(use_path, sizeof(use_path), "%s.writes[%u]", field_path,
               write_index);
      if (!vkr_rg_json_parse_use(ctx, &use_obj, use_path, &use)) {
        ok = false_v;
        goto cleanup;
      }

      if (!vkr_rg_json_resource_exists(ctx->graph, use.name)) {
        ok = vkr_rg_json_error(ctx, use_path, "resource not declared");
        goto cleanup;
      }

      vector_push_VkrRgJsonResourceUse(&out_pass->writes, use);
      write_index++;
    }
  }

  if (!vkr_rg_json_parse_attachments(ctx, obj, field_path,
                                     &out_pass->attachments)) {
    ok = false_v;
    goto cleanup;
  }

  if (out_pass->type == VKR_RG_JSON_PASS_GRAPHICS) {
    bool8_t has_any_attachment =
        out_pass->attachments.has_depth || out_pass->attachments.colors.length;
    if (!has_any_attachment) {
      ok = vkr_rg_json_error(ctx, field_path,
                             "graphics pass requires attachments");
      goto cleanup;
    }
  }

  VkrJsonReader execute_reader = *obj;
  if (!vkr_json_get_string(&execute_reader, "execute", &out_pass->execute)) {
    ok = vkr_rg_json_error(ctx, field_path, "execute is required");
    goto cleanup;
  }

cleanup:
  if (!ok) {
    vector_destroy_VkrRgJsonResourceUse(&out_pass->reads);
    vector_destroy_VkrRgJsonResourceUse(&out_pass->writes);
    vector_destroy_VkrRgJsonAttachment(&out_pass->attachments.colors);
    *out_pass = (VkrRgJsonPass){0};
  }

  return ok;
}

vkr_internal bool8_t vkr_rg_json_parse_outputs(VkrRgJsonParseContext *ctx,
                                               VkrJsonReader *root,
                                               VkrRgJsonOutputs *out_outputs) {
  assert_log(out_outputs != NULL, "out_outputs is NULL");
  assert_log(root != NULL, "root is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_outputs = (VkrRgJsonOutputs){0};
  out_outputs->export_images = vector_create_String8(ctx->allocator);
  out_outputs->export_buffers = vector_create_String8(ctx->allocator);

  VkrJsonReader outputs_reader = *root;
  if (!vkr_json_find_field(&outputs_reader, "outputs")) {
    return true_v;
  }

  VkrJsonReader outputs_obj = {0};
  if (!vkr_json_enter_object(&outputs_reader, &outputs_obj)) {
    return vkr_rg_json_error(ctx, "outputs", "outputs must be object");
  }

  VkrJsonReader present_reader = outputs_obj;
  vkr_json_get_string(&present_reader, "present", &out_outputs->present);

  VkrJsonReader export_images_reader = outputs_obj;
  if (vkr_json_find_array(&export_images_reader, "export_images")) {
    while (vkr_json_next_array_element(&export_images_reader)) {
      String8 value = {0};
      if (!vkr_json_parse_string(&export_images_reader, &value)) {
        return vkr_rg_json_error(ctx, "outputs.export_images",
                                 "export_images must be strings");
      }
      vector_push_String8(&out_outputs->export_images, value);
    }
  }

  VkrJsonReader export_buffers_reader = outputs_obj;
  if (vkr_json_find_array(&export_buffers_reader, "export_buffers")) {
    while (vkr_json_next_array_element(&export_buffers_reader)) {
      String8 value = {0};
      if (!vkr_json_parse_string(&export_buffers_reader, &value)) {
        return vkr_rg_json_error(ctx, "outputs.export_buffers",
                                 "export_buffers must be strings");
      }
      vector_push_String8(&out_outputs->export_buffers, value);
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_rg_json_parse_graph(VkrRgJsonParseContext *ctx,
                                             String8 json,
                                             VkrRgJsonGraph *out_graph) {
  assert_log(out_graph != NULL, "out_graph is NULL");
  assert_log(ctx != NULL, "ctx is NULL");

  *out_graph = (VkrRgJsonGraph){0};
  out_graph->allocator = ctx->allocator;
  out_graph->source = json;
  out_graph->resources = vector_create_VkrRgJsonResource(ctx->allocator);
  out_graph->passes = vector_create_VkrRgJsonPass(ctx->allocator);

  VkrJsonReader root = vkr_json_reader_from_string(json);

  VkrJsonReader version_reader = root;
  int32_t version = 0;
  if (!vkr_json_get_int(&version_reader, "version", &version)) {
    return vkr_rg_json_error(ctx, "version", "version is required");
  }
  if (version != 1) {
    return vkr_rg_json_error(ctx, "version",
                             "unsupported render graph version");
  }
  out_graph->version = (uint32_t)version;

  VkrJsonReader name_reader = root;
  if (!vkr_json_get_string(&name_reader, "name", &out_graph->name)) {
    return vkr_rg_json_error(ctx, "name", "name is required");
  }

  VkrJsonReader resources_reader = root;
  if (!vkr_json_find_array(&resources_reader, "resources")) {
    return vkr_rg_json_error(ctx, "resources", "resources array is required");
  }

  uint32_t resource_index = 0;
  while (vkr_json_next_array_element(&resources_reader)) {
    VkrJsonReader resource_obj = {0};
    if (!vkr_json_enter_object(&resources_reader, &resource_obj)) {
      return vkr_rg_json_error(ctx, "resources",
                               "resource entry must be object");
    }

    VkrRgJsonResource resource = {0};
    if (!vkr_rg_json_parse_resource(ctx, &resource_obj, resource_index,
                                    &resource)) {
      return false_v;
    }

    for (uint64_t i = 0; i < out_graph->resources.length; ++i) {
      VkrRgJsonResource *existing =
          vector_get_VkrRgJsonResource(&out_graph->resources, i);
      if (string8_equals(&existing->name, &resource.name)) {
        return vkr_rg_json_error(ctx, "resources", "duplicate resource name");
      }
    }

    vector_push_VkrRgJsonResource(&out_graph->resources, resource);
    resource_index++;
  }

  VkrJsonReader passes_reader = root;
  if (!vkr_json_find_array(&passes_reader, "passes")) {
    return vkr_rg_json_error(ctx, "passes", "passes array is required");
  }

  uint32_t pass_index = 0;
  while (vkr_json_next_array_element(&passes_reader)) {
    VkrJsonReader pass_obj = {0};
    if (!vkr_json_enter_object(&passes_reader, &pass_obj)) {
      return vkr_rg_json_error(ctx, "passes", "pass entry must be object");
    }

    VkrRgJsonPass pass = {0};
    if (!vkr_rg_json_parse_pass(ctx, &pass_obj, pass_index, &pass)) {
      return false_v;
    }

    for (uint64_t i = 0; i < out_graph->passes.length; ++i) {
      VkrRgJsonPass *existing = vector_get_VkrRgJsonPass(&out_graph->passes, i);
      if (string8_equals(&existing->name, &pass.name)) {
        return vkr_rg_json_error(ctx, "passes", "duplicate pass name");
      }
    }

    vector_push_VkrRgJsonPass(&out_graph->passes, pass);
    pass_index++;
  }

  if (!vkr_rg_json_parse_outputs(ctx, &root, &out_graph->outputs)) {
    return false_v;
  }

  if (out_graph->outputs.present.length > 0 &&
      !vkr_rg_json_resource_exists(out_graph, out_graph->outputs.present)) {
    return vkr_rg_json_error(ctx, "outputs.present",
                             "present resource not declared");
  }

  for (uint64_t i = 0; i < out_graph->outputs.export_images.length; ++i) {
    String8 *name = vector_get_String8(&out_graph->outputs.export_images, i);
    if (!vkr_rg_json_resource_exists(out_graph, *name)) {
      return vkr_rg_json_error(ctx, "outputs.export_images",
                               "export image not declared");
    }
  }

  for (uint64_t i = 0; i < out_graph->outputs.export_buffers.length; ++i) {
    String8 *name = vector_get_String8(&out_graph->outputs.export_buffers, i);
    if (!vkr_rg_json_resource_exists(out_graph, *name)) {
      return vkr_rg_json_error(ctx, "outputs.export_buffers",
                               "export buffer not declared");
    }
  }

  return true_v;
}

bool8_t vkr_rg_json_load_file(VkrAllocator *allocator, const char *path,
                              VkrRgJsonGraph *out_graph) {
  if (!allocator || !path || !out_graph) {
    log_error("RenderGraph JSON load failed: invalid args");
    return false_v;
  }

  *out_graph = (VkrRgJsonGraph){0};

  FilePath file_path = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_RELATIVE,
  };

  FileHandle handle = {0};
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileError open_err = file_open(&file_path, mode, &handle);
  if (open_err != FILE_ERROR_NONE) {
    log_error("RenderGraph JSON: failed to open '%s': %s", path,
              file_get_error_string(open_err).str);
    return false_v;
  }

  uint8_t *buffer = NULL;
  uint64_t size = 0;
  FileError read_err = file_read_all(&handle, allocator, &buffer, &size);
  file_close(&handle);
  if (read_err != FILE_ERROR_NONE) {
    log_error("RenderGraph JSON: failed to read '%s': %s", path,
              file_get_error_string(read_err).str);
    return false_v;
  }

  uint8_t *json_buf =
      vkr_allocator_alloc(allocator, size + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!json_buf) {
    log_error("RenderGraph JSON: out of memory reading '%s'", path);
    vkr_allocator_free(allocator, buffer, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  MemCopy(json_buf, buffer, size);
  json_buf[size] = '\0';
  vkr_allocator_free(allocator, buffer, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);

  String8 json = {.str = json_buf, .length = size};
  VkrRgJsonParseContext ctx = {
      .allocator = allocator,
      .path = path,
      .graph = out_graph,
  };

  if (!vkr_rg_json_parse_graph(&ctx, json, out_graph)) {
    vkr_rg_json_destroy(out_graph);
    return false_v;
  }

  return true_v;
}

void vkr_rg_json_destroy(VkrRgJsonGraph *graph) {
  if (!graph) {
    return;
  }

  for (uint64_t i = 0; i < graph->passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&graph->passes, i);
    vector_destroy_VkrRgJsonResourceUse(&pass->reads);
    vector_destroy_VkrRgJsonResourceUse(&pass->writes);
    vector_destroy_VkrRgJsonAttachment(&pass->attachments.colors);
  }

  vector_destroy_VkrRgJsonPass(&graph->passes);
  vector_destroy_VkrRgJsonResource(&graph->resources);
  vector_destroy_String8(&graph->outputs.export_images);
  vector_destroy_String8(&graph->outputs.export_buffers);

  if (graph->allocator && graph->source.str) {
    vkr_allocator_free(graph->allocator, graph->source.str,
                       graph->source.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  *graph = (VkrRgJsonGraph){0};
}

vkr_internal bool8_t vkr_rg_json_condition_enabled(
    const VkrRgJsonCondition *condition, const VkrRenderGraphFrameInfo *frame) {
  if (!condition || condition->kind == VKR_RG_JSON_CONDITION_NONE) {
    return true_v;
  }
  if (!frame) {
    return false_v;
  }

  switch (condition->kind) {
  case VKR_RG_JSON_CONDITION_EDITOR_ENABLED:
    return frame->editor_enabled;
  case VKR_RG_JSON_CONDITION_EDITOR_DISABLED:
    return !frame->editor_enabled;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_rg_json_repeat_count(
    const VkrRgJsonRepeat *repeat, const VkrRenderGraphFrameInfo *frame,
    uint32_t *out_count) {
  if (!out_count) {
    return false_v;
  }

  *out_count = 1;
  if (!repeat || !repeat->enabled) {
    return true_v;
  }
  if (!frame) {
    return false_v;
  }

  if (vkr_string8_equals_cstr_i(&repeat->count_source,
                                "shadow_cascade_count")) {
    *out_count = frame->shadow_cascade_count;
    return true_v;
  }

  log_error("RenderGraph JSON: unknown repeat source '%.*s'",
            (int)repeat->count_source.length, repeat->count_source.str);
  return false_v;
}

vkr_internal bool8_t vkr_rg_json_resolve_extent(
    const VkrRgJsonExtent *extent, const VkrRenderGraphFrameInfo *frame,
    uint32_t *out_width, uint32_t *out_height) {
  if (!out_width || !out_height) {
    return false_v;
  }

  *out_width = 0;
  *out_height = 0;

  if (!extent || extent->mode == VKR_RG_JSON_EXTENT_NONE) {
    return true_v;
  }
  if (!frame) {
    return false_v;
  }

  switch (extent->mode) {
  case VKR_RG_JSON_EXTENT_WINDOW:
    *out_width = frame->window_width;
    *out_height = frame->window_height;
    return true_v;
  case VKR_RG_JSON_EXTENT_VIEWPORT:
    *out_width = frame->viewport_width;
    *out_height = frame->viewport_height;
    return true_v;
  case VKR_RG_JSON_EXTENT_FIXED:
    *out_width = extent->width;
    *out_height = extent->height;
    return true_v;
  case VKR_RG_JSON_EXTENT_SQUARE:
    if (vkr_string8_equals_cstr_i(&extent->size_source, "shadow_map_size")) {
      *out_width = frame->shadow_map_size;
      *out_height = frame->shadow_map_size;
      return true_v;
    }
    log_error("RenderGraph JSON: unknown square size source '%.*s'",
              (int)extent->size_source.length, extent->size_source.str);
    return false_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_rg_json_resolve_layers(
    const VkrRgJsonImageDesc *desc, const VkrRenderGraphFrameInfo *frame,
    uint32_t *out_layers) {
  if (!out_layers) {
    return false_v;
  }

  *out_layers = 1;
  if (!desc) {
    return true_v;
  }

  if (desc->layers_source.length > 0) {
    if (!frame) {
      return false_v;
    }
    if (vkr_string8_equals_cstr_i(&desc->layers_source,
                                  "shadow_cascade_count")) {
      *out_layers = frame->shadow_cascade_count;
      return true_v;
    }
    log_error("RenderGraph JSON: unknown layers source '%.*s'",
              (int)desc->layers_source.length, desc->layers_source.str);
    return false_v;
  }

  if (desc->layers_is_set) {
    *out_layers = desc->layers;
  }

  if (*out_layers == 0) {
    *out_layers = 1;
  }

  return true_v;
}

vkr_internal VkrRgResourceFlags vkr_rg_json_resource_flags(uint32_t flags) {
  VkrRgResourceFlags out = VKR_RG_RESOURCE_FLAG_NONE;
  if (flags & VKR_RG_JSON_RESOURCE_FLAG_TRANSIENT) {
    out |= VKR_RG_RESOURCE_FLAG_TRANSIENT;
  }
  if (flags & VKR_RG_JSON_RESOURCE_FLAG_PERSISTENT) {
    out |= VKR_RG_RESOURCE_FLAG_PERSISTENT;
  }
  if (flags & VKR_RG_JSON_RESOURCE_FLAG_EXTERNAL) {
    out |= VKR_RG_RESOURCE_FLAG_EXTERNAL;
  }
  if (flags & VKR_RG_JSON_RESOURCE_FLAG_PER_IMAGE) {
    out |= VKR_RG_RESOURCE_FLAG_PER_IMAGE;
  }
  if (flags & VKR_RG_JSON_RESOURCE_FLAG_RESIZABLE) {
    out |= VKR_RG_RESOURCE_FLAG_RESIZABLE;
  }
  return out;
}

vkr_internal bool8_t vkr_rg_expand_name(VkrAllocator *allocator, String8 name,
                                        uint32_t index, String8 *out_name,
                                        bool8_t *out_owned) {
  if (!out_name || !out_owned) {
    return false_v;
  }

  *out_owned = false_v;
  *out_name = name;

  const char *token = "${i}";
  const uint32_t token_len = 4;
  if (!name.str || name.length < token_len) {
    return true_v;
  }

  uint32_t token_count = 0;
  for (uint32_t i = 0; i + token_len <= name.length; ++i) {
    if (MemCompare(name.str + i, token, token_len) == 0) {
      token_count++;
      i += token_len - 1;
    }
  }

  if (token_count == 0) {
    return true_v;
  }

  char index_buf[32] = {0};
  int32_t index_len = snprintf(index_buf, sizeof(index_buf), "%u", index);
  if (index_len <= 0) {
    return false_v;
  }

  uint32_t new_len =
      name.length - token_count * token_len + token_count * (uint32_t)index_len;
  uint8_t *buffer = vkr_allocator_alloc(allocator, new_len + 1,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    return false_v;
  }

  uint32_t write = 0;
  for (uint32_t i = 0; i < name.length; ++i) {
    if (i + token_len <= name.length &&
        MemCompare(name.str + i, token, token_len) == 0) {
      MemCopy(buffer + write, index_buf, (uint32_t)index_len);
      write += (uint32_t)index_len;
      i += token_len - 1;
    } else {
      buffer[write++] = name.str[i];
    }
  }

  buffer[new_len] = '\0';
  *out_name = (String8){.str = buffer, .length = new_len};
  *out_owned = true_v;
  return true_v;
}

vkr_internal void vkr_rg_release_name(VkrAllocator *allocator, String8 name,
                                      bool8_t owned) {
  if (owned && allocator && name.str) {
    vkr_allocator_free(allocator, name.str, name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
}

// TODO: use hash map instead of linear search
vkr_internal VkrRgImageHandle vkr_rg_build_find_image(VkrRenderGraph *rg,
                                                      String8 name) {
  if (!rg) {
    return VKR_RG_IMAGE_HANDLE_INVALID;
  }

  for (uint64_t i = 0; i < rg->images.length; ++i) {
    VkrRgImage *image = vector_get_VkrRgImage(&rg->images, i);
    if (string8_equals(&image->name, &name)) {
      return (VkrRgImageHandle){(uint32_t)i + 1, image->generation};
    }
  }

  return VKR_RG_IMAGE_HANDLE_INVALID;
}

// TODO: use hash map instead of linear search
vkr_internal VkrRgBufferHandle vkr_rg_build_find_buffer(VkrRenderGraph *rg,
                                                        String8 name) {
  if (!rg) {
    return VKR_RG_BUFFER_HANDLE_INVALID;
  }

  for (uint64_t i = 0; i < rg->buffers.length; ++i) {
    VkrRgBuffer *buffer = vector_get_VkrRgBuffer(&rg->buffers, i);
    if (string8_equals(&buffer->name, &name)) {
      return (VkrRgBufferHandle){(uint32_t)i + 1, buffer->generation};
    }
  }

  return VKR_RG_BUFFER_HANDLE_INVALID;
}

vkr_internal uint32_t vkr_rg_resolve_index(const VkrRgJsonIndex *index,
                                           uint32_t fallback) {
  if (!index || !index->is_set) {
    return 0;
  }
  if (!index->is_token) {
    return index->value;
  }

  if (vkr_string8_equals_cstr_i(&index->token, "${i}") ||
      vkr_string8_equals_cstr_i(&index->token, "i")) {
    return fallback;
  }

  log_error("RenderGraph JSON: unknown index token '%.*s'",
            (int)index->token.length, index->token.str);
  return 0;
}

vkr_internal bool8_t vkr_rg_json_apply_slice(const VkrRgJsonAttachment *att,
                                             uint32_t fallback,
                                             VkrRgAttachmentDesc *out_desc) {
  if (!att || !out_desc || !att->has_slice) {
    return true_v;
  }

  if (att->slice_mip_level.is_set) {
    out_desc->slice.mip_level =
        vkr_rg_resolve_index(&att->slice_mip_level, fallback);
  }
  if (att->slice_base_layer.is_set) {
    out_desc->slice.base_layer =
        vkr_rg_resolve_index(&att->slice_base_layer, fallback);
  }
  if (att->slice_layer_count.is_set) {
    uint32_t count = vkr_rg_resolve_index(&att->slice_layer_count, fallback);
    if (count == 0) {
      return false_v;
    }
    out_desc->slice.layer_count = count;
  }

  return true_v;
}

bool8_t vkr_rg_build_from_json(VkrRenderGraph *rg,
                               const VkrRgJsonGraph *json_graph,
                               const VkrRenderGraphFrameInfo *frame,
                               const VkrRgExecutorRegistry *executors) {
  if (!rg || !json_graph || !frame || !executors) {
    log_error("RenderGraph build failed: invalid args");
    return false_v;
  }

  for (uint64_t i = 0; i < json_graph->resources.length; ++i) {
    VkrRgJsonResource *resource =
        vector_get_VkrRgJsonResource(&json_graph->resources, i);

    if (!vkr_rg_json_condition_enabled(&resource->condition, frame)) {
      continue;
    }

    uint32_t repeat_count = 1;
    if (!vkr_rg_json_repeat_count(&resource->repeat, frame, &repeat_count)) {
      return false_v;
    }

    for (uint32_t r = 0; r < repeat_count; ++r) {
      String8 resolved_name = {0};
      bool8_t owned_name = false_v;
      if (!vkr_rg_expand_name(rg->allocator, resource->name, r, &resolved_name,
                              &owned_name)) {
        log_error("RenderGraph build failed: name expansion failed");
        return false_v;
      }

      if (resource->type == VKR_RG_JSON_RESOURCE_IMAGE) {
        if (resource->image.is_import) {
          VkrRgImageAccessFlags access = VKR_RG_IMAGE_ACCESS_NONE;
          VkrTextureLayout layout = VKR_TEXTURE_LAYOUT_UNDEFINED;
          VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
          desc.flags = vkr_rg_json_resource_flags(resource->flags);
          if (resource->image.layers_is_set ||
              resource->image.layers_source.length > 0) {
            desc.flags |= VKR_RG_RESOURCE_FLAG_FORCE_ARRAY;
          }
          desc.width = frame->window_width;
          desc.height = frame->window_height;
          desc.usage = resource->image.usage;
          uint32_t layers = 1;
          if (!vkr_rg_json_resolve_layers(&resource->image, frame, &layers)) {
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            return false_v;
          }
          desc.layers = layers;
          if (vkr_string8_equals_cstr_i(&resource->image.import_name,
                                        "swapchain")) {
            access = VKR_RG_IMAGE_ACCESS_PRESENT;
            layout = VKR_TEXTURE_LAYOUT_UNDEFINED;
            desc.format = frame->swapchain_format;
            desc.layers = 1;
            if (desc.usage.set == 0) {
              desc.usage = vkr_texture_usage_flags_from_bits(
                  VKR_TEXTURE_USAGE_COLOR_ATTACHMENT);
            }
          } else if (vkr_string8_equals_cstr_i(&resource->image.import_name,
                                               "swapchain_depth")) {
            access = VKR_RG_IMAGE_ACCESS_DEPTH_ATTACHMENT;
            layout = VKR_TEXTURE_LAYOUT_UNDEFINED;
            desc.format = frame->swapchain_depth_format;
            desc.layers = 1;
            if (desc.usage.set == 0) {
              desc.usage = vkr_texture_usage_flags_from_bits(
                  VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT);
            }
          } else {
            log_error("RenderGraph JSON: unknown import '%.*s'",
                      (int)resource->image.import_name.length,
                      resource->image.import_name.str);
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            return false_v;
          }

          vkr_rg_import_image(rg, resolved_name, NULL, access, layout, &desc);
        } else {
          uint32_t width = 0;
          uint32_t height = 0;
          if (!vkr_rg_json_resolve_extent(&resource->image.extent, frame,
                                          &width, &height)) {
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            return false_v;
          }

          VkrRgImageDesc desc = VKR_RG_IMAGE_DESC_DEFAULT;
          desc.width = width;
          desc.height = height;
          desc.usage = resource->image.usage;
          desc.flags = vkr_rg_json_resource_flags(resource->flags);
          if (resource->image.layers_is_set ||
              resource->image.layers_source.length > 0) {
            desc.flags |= VKR_RG_RESOURCE_FLAG_FORCE_ARRAY;
          }
          uint32_t layers = 1;
          if (!vkr_rg_json_resolve_layers(&resource->image, frame, &layers)) {
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            return false_v;
          }
          desc.layers = layers;
          switch (resource->image.format_source) {
          case VKR_RG_JSON_IMAGE_FORMAT_SWAPCHAIN:
            desc.format = frame->swapchain_format;
            break;
          case VKR_RG_JSON_IMAGE_FORMAT_SWAPCHAIN_DEPTH:
            desc.format = frame->swapchain_depth_format;
            break;
          case VKR_RG_JSON_IMAGE_FORMAT_SHADOW_DEPTH:
            desc.format = frame->shadow_depth_format;
            break;
          case VKR_RG_JSON_IMAGE_FORMAT_EXPLICIT:
          default:
            desc.format = resource->image.format;
            break;
          }

          vkr_rg_create_image(rg, resolved_name, &desc);
        }
      } else {
        VkrRgBufferDesc desc = {0};
        desc.size = resource->buffer.size;
        desc.usage = resource->buffer.usage;
        desc.flags = vkr_rg_json_resource_flags(resource->flags);
        vkr_rg_create_buffer(rg, resolved_name, &desc);
      }

      vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
    }
  }

  for (uint64_t i = 0; i < json_graph->passes.length; ++i) {
    VkrRgJsonPass *pass = vector_get_VkrRgJsonPass(&json_graph->passes, i);

    if (!vkr_rg_json_condition_enabled(&pass->condition, frame)) {
      continue;
    }

    uint32_t repeat_count = 1;
    if (!vkr_rg_json_repeat_count(&pass->repeat, frame, &repeat_count)) {
      return false_v;
    }

    for (uint32_t r = 0; r < repeat_count; ++r) {
      String8 resolved_name = {0};
      bool8_t owned_name = false_v;
      if (!vkr_rg_expand_name(rg->allocator, pass->name, r, &resolved_name,
                              &owned_name)) {
        log_error("RenderGraph build failed: pass name expansion failed");
        return false_v;
      }

      VkrRgPassBuilder pb =
          vkr_rg_add_pass(rg, (VkrRgPassType)pass->type, resolved_name);
      vkr_rg_release_name(rg->allocator, resolved_name, owned_name);

      if (pass->flags != VKR_RG_PASS_FLAG_NONE) {
        vkr_rg_pass_set_flags(&pb, pass->flags);
      }

      if (pass->has_domain) {
        vkr_rg_pass_set_domain(&pb, pass->domain);
      }

      void *executor_user_data = NULL;
      VkrRgPassExecuteFn execute = vkr_rg_executor_registry_find(
          executors, pass->execute, &executor_user_data);
      if (!execute) {
        log_error("RenderGraph JSON: missing executor '%.*s'",
                  (int)pass->execute.length, pass->execute.str);
        return false_v;
      }

      VkrRgPass *graph_pass = vector_get_VkrRgPass(&rg->passes, pb.pass_index);
      if (graph_pass && pass->execute.length > 0) {
        graph_pass->desc.execute_name =
            string8_duplicate(rg->allocator, &pass->execute);
        if (!graph_pass->desc.execute_name.str) {
          log_error("RenderGraph JSON: execute name allocation failed");
          return false_v;
        }
      }

      if (repeat_count > 1 && executor_user_data == NULL) {
        executor_user_data = (void *)(uintptr_t)r;
      }

      vkr_rg_pass_set_execute(&pb, execute, executor_user_data);

      for (uint64_t c = 0; c < pass->attachments.colors.length; ++c) {
        VkrRgJsonAttachment *att =
            vector_get_VkrRgJsonAttachment(&pass->attachments.colors, c);
        String8 resolved_image = {0};
        bool8_t owned_image = false_v;
        if (!vkr_rg_expand_name(rg->allocator, att->image, r, &resolved_image,
                                &owned_image)) {
          return false_v;
        }
        VkrRgImageHandle handle = vkr_rg_build_find_image(rg, resolved_image);
        vkr_rg_release_name(rg->allocator, resolved_image, owned_image);
        if (!vkr_rg_image_handle_valid(handle)) {
          log_error("RenderGraph JSON: missing image '%.*s'",
                    (int)att->image.length, att->image.str);
          return false_v;
        }

        VkrRgAttachmentDesc desc = {
            .slice = VKR_RG_IMAGE_SLICE_DEFAULT,
            .load_op = att->load_op,
            .store_op = att->store_op,
        };
        if (att->has_clear) {
          desc.clear_value = att->clear_value;
        }
        if (!vkr_rg_json_apply_slice(att, r, &desc)) {
          log_error(
              "RenderGraph JSON: attachment slice layer_count must be >= 1");
          return false_v;
        }

        vkr_rg_pass_add_color_attachment(&pb, handle, &desc);
      }

      if (pass->attachments.has_depth) {
        VkrRgJsonAttachment *att = &pass->attachments.depth;
        String8 resolved_image = {0};
        bool8_t owned_image = false_v;
        if (!vkr_rg_expand_name(rg->allocator, att->image, r, &resolved_image,
                                &owned_image)) {
          return false_v;
        }
        VkrRgImageHandle handle = vkr_rg_build_find_image(rg, resolved_image);
        vkr_rg_release_name(rg->allocator, resolved_image, owned_image);
        if (!vkr_rg_image_handle_valid(handle)) {
          log_error("RenderGraph JSON: missing image '%.*s'",
                    (int)att->image.length, att->image.str);
          return false_v;
        }

        VkrRgAttachmentDesc desc = {
            .slice = VKR_RG_IMAGE_SLICE_DEFAULT,
            .load_op = att->load_op,
            .store_op = att->store_op,
        };
        if (att->has_clear) {
          desc.clear_value = att->clear_value;
        }
        if (!vkr_rg_json_apply_slice(att, r, &desc)) {
          log_error(
              "RenderGraph JSON: attachment slice layer_count must be >= 1");
          return false_v;
        }

        vkr_rg_pass_set_depth_attachment(&pb, handle, &desc,
                                         pass->attachments.depth_read_only);
      }

      for (uint64_t u = 0; u < pass->reads.length; ++u) {
        VkrRgJsonResourceUse *use =
            vector_get_VkrRgJsonResourceUse(&pass->reads, u);

        uint32_t use_repeat = 1;
        if (!vkr_rg_json_repeat_count(&use->repeat, frame, &use_repeat)) {
          return false_v;
        }

        for (uint32_t ur = 0; ur < use_repeat; ++ur) {
          String8 resolved_name = {0};
          bool8_t owned_name = false_v;
          if (!vkr_rg_expand_name(rg->allocator, use->name, ur, &resolved_name,
                                  &owned_name)) {
            return false_v;
          }

          if (use->is_image) {
            VkrRgImageHandle handle =
                vkr_rg_build_find_image(rg, resolved_name);
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            if (!vkr_rg_image_handle_valid(handle)) {
              log_error("RenderGraph JSON: missing image '%.*s'",
                        (int)use->name.length, use->name.str);
              return false_v;
            }

            uint32_t binding = use->binding.is_set ? use->binding.value : 0;
            uint32_t array_index = vkr_rg_resolve_index(
                &use->array_index, use_repeat > 1 ? ur : r);
            vkr_rg_pass_read_image(&pb, handle,
                                   (VkrRgImageAccessFlags)use->image_access,
                                   binding, array_index);
          } else {
            VkrRgBufferHandle handle =
                vkr_rg_build_find_buffer(rg, resolved_name);
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            if (!vkr_rg_buffer_handle_valid(handle)) {
              log_error("RenderGraph JSON: missing buffer '%.*s'",
                        (int)use->name.length, use->name.str);
              return false_v;
            }

            uint32_t binding = use->binding.is_set ? use->binding.value : 0;
            uint32_t array_index = vkr_rg_resolve_index(
                &use->array_index, use_repeat > 1 ? ur : r);
            vkr_rg_pass_read_buffer(&pb, handle,
                                    (VkrRgBufferAccessFlags)use->buffer_access,
                                    binding, array_index);
          }
        }
      }

      for (uint64_t u = 0; u < pass->writes.length; ++u) {
        VkrRgJsonResourceUse *use =
            vector_get_VkrRgJsonResourceUse(&pass->writes, u);

        uint32_t use_repeat = 1;
        if (!vkr_rg_json_repeat_count(&use->repeat, frame, &use_repeat)) {
          return false_v;
        }

        for (uint32_t ur = 0; ur < use_repeat; ++ur) {
          String8 resolved_name = {0};
          bool8_t owned_name = false_v;
          if (!vkr_rg_expand_name(rg->allocator, use->name, ur, &resolved_name,
                                  &owned_name)) {
            return false_v;
          }

          if (use->is_image) {
            VkrRgImageHandle handle =
                vkr_rg_build_find_image(rg, resolved_name);
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            if (!vkr_rg_image_handle_valid(handle)) {
              log_error("RenderGraph JSON: missing image '%.*s'",
                        (int)use->name.length, use->name.str);
              return false_v;
            }

            uint32_t binding = use->binding.is_set ? use->binding.value : 0;
            uint32_t array_index = vkr_rg_resolve_index(
                &use->array_index, use_repeat > 1 ? ur : r);
            vkr_rg_pass_write_image(&pb, handle,
                                    (VkrRgImageAccessFlags)use->image_access,
                                    binding, array_index);
          } else {
            VkrRgBufferHandle handle =
                vkr_rg_build_find_buffer(rg, resolved_name);
            vkr_rg_release_name(rg->allocator, resolved_name, owned_name);
            if (!vkr_rg_buffer_handle_valid(handle)) {
              log_error("RenderGraph JSON: missing buffer '%.*s'",
                        (int)use->name.length, use->name.str);
              return false_v;
            }

            uint32_t binding = use->binding.is_set ? use->binding.value : 0;
            uint32_t array_index = vkr_rg_resolve_index(
                &use->array_index, use_repeat > 1 ? ur : r);
            vkr_rg_pass_write_buffer(&pb, handle,
                                     (VkrRgBufferAccessFlags)use->buffer_access,
                                     binding, array_index);
          }
        }
      }
    }
  }

  if (json_graph->outputs.present.length > 0) {
    VkrRgImageHandle handle =
        vkr_rg_build_find_image(rg, json_graph->outputs.present);
    if (!vkr_rg_image_handle_valid(handle)) {
      log_error("RenderGraph JSON: missing present image '%.*s'",
                (int)json_graph->outputs.present.length,
                json_graph->outputs.present.str);
      return false_v;
    }
    vkr_rg_set_present_image(rg, handle);
  }

  for (uint64_t i = 0; i < json_graph->outputs.export_images.length; ++i) {
    String8 name = json_graph->outputs.export_images.data[i];
    VkrRgImageHandle handle = vkr_rg_build_find_image(rg, name);
    if (!vkr_rg_image_handle_valid(handle)) {
      log_error("RenderGraph JSON: missing export image '%.*s'",
                (int)name.length, name.str);
      return false_v;
    }
    vkr_rg_export_image(rg, handle);
  }

  for (uint64_t i = 0; i < json_graph->outputs.export_buffers.length; ++i) {
    String8 name = json_graph->outputs.export_buffers.data[i];
    VkrRgBufferHandle handle = vkr_rg_build_find_buffer(rg, name);
    if (!vkr_rg_buffer_handle_valid(handle)) {
      log_error("RenderGraph JSON: missing export buffer '%.*s'",
                (int)name.length, name.str);
      return false_v;
    }
    vkr_rg_export_buffer(rg, handle);
  }

  return true_v;
}

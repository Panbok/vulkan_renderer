#include "renderer/resources/loaders/shader_loader.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_shader_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "renderer/vkr_buffer.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_SHADER_CONFIG_MAX_LINE_LENGTH 4096
#define VKR_SHADER_CONFIG_MAX_KEY_LENGTH 128
#define VKR_SHADER_CONFIG_MAX_VALUE_LENGTH 512
#define VKR_SHADER_CONFIG_MAX_TOKEN_LENGTH 64
#define VKR_SHADER_UBO_ALIGNMENT 256
#define VKR_SHADER_PUSH_CONSTANT_ALIGNMENT 4
#define VKR_SHADER_UNIFORM_REGISTER_SIZE 16
#define VKR_SHADER_CONFIG_MAX_STAGES 8

#define VKR_SHADER_ATTRIBUTE_COUNT_MAX 32
#define VKR_SHADER_UNIFORM_COUNT_MAX 64

// Temporary allocator sizes for parsing operations
#define VKR_SHADER_PARSER_SCRATCH_SIZE MB(1)
#define VKR_SHADER_PARSER_LINE_SCRATCH_SIZE KB(8)

// =============================================================================
// Parse Result and Error Types
// =============================================================================

typedef enum VkrShaderConfigErrorType {
  VKR_SHADER_CONFIG_ERROR_NONE = 0,
  VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND,
  VKR_SHADER_CONFIG_ERROR_FILE_READ_FAILED,
  VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
  VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD,
  VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
  VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
  VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
  VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
  VKR_SHADER_CONFIG_ERROR_DUPLICATE_KEY,
  VKR_SHADER_CONFIG_ERROR_UNKNOWN
} VkrShaderConfigErrorType;

typedef struct VkrShaderConfigParseResult {
  bool8_t is_valid;
  VkrShaderConfigErrorType error_type;
  String8 error_message;  // Allocator-allocated detailed error message
  uint64_t line_number;   // 0 if not line-specific
  uint64_t column_number; // 0 if not column-specific
} VkrShaderConfigParseResult;

// =============================================================================
// Parser Context
// =============================================================================

typedef struct VkrShaderConfigParser {
  VkrAllocator *allocator; // Main allocator for persistent data
  VkrAllocator
      *scratch_allocator; // Scratch allocator for temporary allocations
  String8 current_line;
  uint64_t line_number;
  uint64_t column_number;
  String8 file_path;
} VkrShaderConfigParser;

// =============================================================================
// Internal Helper Functions
// =============================================================================

vkr_internal INLINE uint64_t vkr_align_up_u64(uint64_t value,
                                              uint64_t alignment) {
  // NOTE: alignment must be a power of 2
  return (value + alignment - 1) & ~(alignment - 1);
}

vkr_internal INLINE uint32_t
vkr_attribute_type_size(VkrShaderAttributeType type) {
  switch (type) {
  case SHADER_ATTRIBUTE_TYPE_VEC2:
    return sizeof(Vec2);
  case SHADER_ATTRIBUTE_TYPE_VEC3:
    return sizeof(Vec3);
  case SHADER_ATTRIBUTE_TYPE_VEC4:
    return sizeof(Vec4);
  case SHADER_ATTRIBUTE_TYPE_MAT4:
    return sizeof(Mat4);
  case SHADER_ATTRIBUTE_TYPE_INT32:
    return sizeof(int32_t);
  case SHADER_ATTRIBUTE_TYPE_UINT32:
    return sizeof(uint32_t);
  default:
    return 0;
  }
}

typedef struct VkrVertexAttributeExpectation {
  const char *name;
  VkrShaderAttributeType type;
  uint32_t offset;
} VkrVertexAttributeExpectation;

vkr_internal VkrVertexType
vkr_shader_config_detect_vertex_type(const VkrShaderConfig *cfg) {
  if (cfg->renderpass_name.length > 0 &&
      vkr_string8_equals_cstr_i(&cfg->renderpass_name,
                                "Renderpass.Builtin.UI")) {
    return VKR_VERTEX_TYPE_2D;
  }

  for (uint32_t i = 0; i < cfg->attribute_count; ++i) {
    const VkrShaderAttributeDesc *ad =
        array_get_VkrShaderAttributeDesc(&cfg->attributes, i);
    if (vkr_string8_equals_cstr_i(&ad->name, "in_position") &&
        (ad->type == SHADER_ATTRIBUTE_TYPE_VEC3 ||
         ad->type == SHADER_ATTRIBUTE_TYPE_VEC4)) {
      return VKR_VERTEX_TYPE_3D;
    }
    if (ad->name.str && vkr_string8_equals_cstr_i(&ad->name, "in_normal")) {
      return VKR_VERTEX_TYPE_3D;
    }
  }

  return VKR_VERTEX_TYPE_2D;
}

vkr_internal void vkr_compute_attribute_layout(VkrShaderConfig *cfg) {
  cfg->vertex_type = vkr_shader_config_detect_vertex_type(cfg);

  if (cfg->vertex_type == VKR_VERTEX_TYPE_3D) {
    static const VkrVertexAttributeExpectation expectations[] = {
        {"in_position", SHADER_ATTRIBUTE_TYPE_VEC3,
         (uint32_t)offsetof(VkrVertex3d, position)},
        {"in_normal", SHADER_ATTRIBUTE_TYPE_VEC3,
         (uint32_t)offsetof(VkrVertex3d, normal)},
        {"in_texcoord", SHADER_ATTRIBUTE_TYPE_VEC2,
         (uint32_t)offsetof(VkrVertex3d, texcoord)},
        {"in_color", SHADER_ATTRIBUTE_TYPE_VEC4,
         (uint32_t)offsetof(VkrVertex3d, colour)},
        {"in_tangent", SHADER_ATTRIBUTE_TYPE_VEC4,
         (uint32_t)offsetof(VkrVertex3d, tangent)},
    };
    cfg->attribute_stride = sizeof(VkrVertex3d);
    for (uint32_t i = 0; i < ArrayCount(expectations); ++i) {
      const VkrVertexAttributeExpectation *exp = &expectations[i];
      bool8_t found = false_v;
      for (uint32_t j = 0; j < cfg->attribute_count; ++j) {
        VkrShaderAttributeDesc *ad =
            array_get_VkrShaderAttributeDesc(&cfg->attributes, j);
        if (vkr_string8_equals_cstr(&ad->name, exp->name)) {
          ad->location = i;
          ad->offset = exp->offset;
          ad->size = vkr_attribute_type_size(exp->type);
          found = true_v;
          break;
        }
      }
      if (!found) {
        log_warn("Shader '%s' missing expected vertex attribute '%s'",
                 string8_cstr(&cfg->name), exp->name);
      }
    }
  } else {
    bool8_t has_color = false_v;
    for (uint32_t j = 0; j < cfg->attribute_count; ++j) {
      VkrShaderAttributeDesc *ad =
          array_get_VkrShaderAttributeDesc(&cfg->attributes, j);
      if (vkr_string8_equals_cstr(&ad->name, "in_color")) {
        has_color = true_v;
        break;
      }
    }

    if (has_color) {
      static const VkrVertexAttributeExpectation expectations[] = {
          {"in_position", SHADER_ATTRIBUTE_TYPE_VEC2,
           (uint32_t)offsetof(VkrTextVertex, position)},
          {"in_texcoord", SHADER_ATTRIBUTE_TYPE_VEC2,
           (uint32_t)offsetof(VkrTextVertex, texcoord)},
          {"in_color", SHADER_ATTRIBUTE_TYPE_VEC4,
           (uint32_t)offsetof(VkrTextVertex, color)},
      };
      cfg->attribute_stride = sizeof(VkrTextVertex);
      for (uint32_t i = 0; i < ArrayCount(expectations); ++i) {
        const VkrVertexAttributeExpectation *exp = &expectations[i];
        bool8_t found = false_v;
        for (uint32_t j = 0; j < cfg->attribute_count; ++j) {
          VkrShaderAttributeDesc *ad =
              array_get_VkrShaderAttributeDesc(&cfg->attributes, j);
          if (vkr_string8_equals_cstr(&ad->name, exp->name)) {
            ad->location = i;
            ad->offset = exp->offset;
            ad->size = vkr_attribute_type_size(exp->type);
            found = true_v;
            break;
          }
        }
        if (!found) {
          log_warn(
              "Shader '%s' missing vertex attribute '%s'; defaulting to zero",
              string8_cstr(&cfg->name), exp->name);
        }
      }
    } else {
      static const VkrVertexAttributeExpectation expectations[] = {
          {"in_position", SHADER_ATTRIBUTE_TYPE_VEC2,
           (uint32_t)offsetof(VkrVertex2d, position)},
          {"in_texcoord", SHADER_ATTRIBUTE_TYPE_VEC2,
           (uint32_t)offsetof(VkrVertex2d, texcoord)},
      };
      cfg->attribute_stride = sizeof(VkrVertex2d);
      for (uint32_t i = 0; i < ArrayCount(expectations); ++i) {
        const VkrVertexAttributeExpectation *exp = &expectations[i];
        bool8_t found = false_v;
        for (uint32_t j = 0; j < cfg->attribute_count; ++j) {
          VkrShaderAttributeDesc *ad =
              array_get_VkrShaderAttributeDesc(&cfg->attributes, j);
          if (vkr_string8_equals_cstr(&ad->name, exp->name)) {
            ad->location = i;
            ad->offset = exp->offset;
            ad->size = vkr_attribute_type_size(exp->type);
            found = true_v;
            break;
          }
        }
        if (!found) {
          log_warn(
              "Shader '%s' missing vertex attribute '%s'; defaulting to zero",
              string8_cstr(&cfg->name), exp->name);
        }
      }
    }
  }
}

vkr_internal INLINE uint64_t vkr_std140_alignment(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
  case SHADER_UNIFORM_TYPE_INT32:
  case SHADER_UNIFORM_TYPE_UINT32:
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
    return sizeof(float32_t);
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return sizeof(float32_t) * 4;
  case SHADER_UNIFORM_TYPE_SAMPLER:
    return 0;
  case SHADER_UNIFORM_TYPE_UNDEFINED:
    return sizeof(float32_t);
  }
  return sizeof(float32_t);
}

vkr_internal INLINE uint64_t vkr_uniform_type_size(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
    return sizeof(float32_t);
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
    return sizeof(float32_t) * 2;
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
    return sizeof(float32_t) * 3;
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
    return sizeof(float32_t) * 4;
  case SHADER_UNIFORM_TYPE_INT32:
    return sizeof(int32_t);
  case SHADER_UNIFORM_TYPE_UINT32:
    return sizeof(uint32_t);
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return sizeof(float32_t) * 16;
  case SHADER_UNIFORM_TYPE_SAMPLER:
  case SHADER_UNIFORM_TYPE_UNDEFINED:
    return 0;
  }
  return 0;
}

vkr_internal INLINE uint64_t vkr_apply_uniform_register_packing(uint64_t offset,
                                                                uint64_t size) {
  const uint64_t reg = VKR_SHADER_UNIFORM_REGISTER_SIZE;

  if (size == 0)
    return offset;

  if (size > reg)
    return vkr_align_up_u64(offset, reg);

  uint64_t row_offset = offset % reg;
  if (row_offset + size > reg)
    return vkr_align_up_u64(offset, reg);

  return offset;
}

vkr_internal String8 vkr_create_formatted_error(VkrAllocator *allocator,
                                                const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = string8_create_formatted_v(allocator, fmt, args);
  va_end(args);
  return result;
}

// =============================================================================
// Type Parsing Functions
// =============================================================================

vkr_internal VkrShaderAttributeType
vkr_parse_attribute_type(const String8 *type_str) {
  if (vkr_string8_equals_cstr_i(type_str, "vec2"))
    return SHADER_ATTRIBUTE_TYPE_VEC2;
  if (vkr_string8_equals_cstr_i(type_str, "vec3"))
    return SHADER_ATTRIBUTE_TYPE_VEC3;
  if (vkr_string8_equals_cstr_i(type_str, "vec4"))
    return SHADER_ATTRIBUTE_TYPE_VEC4;
  if (vkr_string8_equals_cstr_i(type_str, "mat4"))
    return SHADER_ATTRIBUTE_TYPE_MAT4;
  if (vkr_string8_equals_cstr_i(type_str, "int32"))
    return SHADER_ATTRIBUTE_TYPE_INT32;
  if (vkr_string8_equals_cstr_i(type_str, "uint32"))
    return SHADER_ATTRIBUTE_TYPE_UINT32;
  return SHADER_ATTRIBUTE_TYPE_UNDEFINED;
}

vkr_internal VkrShaderUniformType
vkr_parse_uniform_type(const String8 *type_str) {
  if (vkr_string8_equals_cstr_i(type_str, "float"))
    return SHADER_UNIFORM_TYPE_FLOAT32;
  if (vkr_string8_equals_cstr_i(type_str, "vec2"))
    return SHADER_UNIFORM_TYPE_FLOAT32_2;
  if (vkr_string8_equals_cstr_i(type_str, "vec3"))
    return SHADER_UNIFORM_TYPE_FLOAT32_3;
  if (vkr_string8_equals_cstr_i(type_str, "vec4"))
    return SHADER_UNIFORM_TYPE_FLOAT32_4;
  if (vkr_string8_equals_cstr_i(type_str, "int32"))
    return SHADER_UNIFORM_TYPE_INT32;
  if (vkr_string8_equals_cstr_i(type_str, "uint32"))
    return SHADER_UNIFORM_TYPE_UINT32;
  if (vkr_string8_equals_cstr_i(type_str, "mat4"))
    return SHADER_UNIFORM_TYPE_MATRIX_4;
  if (vkr_string8_equals_cstr_i(type_str, "samp"))
    return SHADER_UNIFORM_TYPE_SAMPLER;
  return SHADER_UNIFORM_TYPE_UNDEFINED;
}

vkr_internal VkrShaderStage vkr_parse_shader_stage(const String8 *stage_str) {
  if (vkr_string8_equals_cstr_i(stage_str, "vertex"))
    return VKR_SHADER_STAGE_VERTEX;
  if (vkr_string8_equals_cstr_i(stage_str, "fragment"))
    return VKR_SHADER_STAGE_FRAGMENT;
  return VKR_SHADER_STAGE_COUNT; // Invalid
}

vkr_internal VkrCullMode vkr_parse_cull_mode(const String8 *cull_str) {
  if (vkr_string8_equals_cstr_i(cull_str, "none"))
    return VKR_CULL_MODE_NONE;
  if (vkr_string8_equals_cstr_i(cull_str, "front"))
    return VKR_CULL_MODE_FRONT;
  if (vkr_string8_equals_cstr_i(cull_str, "back"))
    return VKR_CULL_MODE_BACK;
  if (vkr_string8_equals_cstr_i(cull_str, "front_and_back"))
    return VKR_CULL_MODE_FRONT_AND_BACK;
  return VKR_CULL_MODE_BACK; // Default
}

// =============================================================================
// String Processing Functions (Using Temporary Allocators)
// =============================================================================

vkr_internal String8 vkr_trim_string8_scratch(VkrAllocator *allocator,
                                              const String8 *str) {
  if (!str || !str->str || str->length == 0) {
    return (String8){NULL, 0};
  }

  uint64_t start = 0;
  uint64_t end = str->length;

  // Trim leading whitespace
  while (start < end && (str->str[start] == ' ' || str->str[start] == '\t' ||
                         str->str[start] == '\r' || str->str[start] == '\n')) {
    start++;
  }

  // Trim trailing whitespace
  while (end > start &&
         (str->str[end - 1] == ' ' || str->str[end - 1] == '\t' ||
          str->str[end - 1] == '\r' || str->str[end - 1] == '\n')) {
    end--;
  }

  if (start >= end) {
    return (String8){NULL, 0};
  }

  // If no trimming needed, return original
  if (start == 0 && end == str->length) {
    return *str;
  }

  // Create trimmed copy in scratch arena
  uint64_t trimmed_length = end - start;
  uint8_t *trimmed_data = (uint8_t *)vkr_allocator_alloc(
      allocator, trimmed_length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!trimmed_data) {
    return (String8){NULL, 0};
  }

  MemCopy(trimmed_data, str->str + start, trimmed_length);
  trimmed_data[trimmed_length] = '\0';

  return (String8){trimmed_data, trimmed_length};
}

vkr_internal String8 vkr_strip_comments_scratch(VkrAllocator *allocator,
                                                const String8 *str) {
  if (!str || !str->str || str->length == 0) {
    return (String8){NULL, 0};
  }

  // Find first comment marker
  uint64_t comment_pos = str->length;
  for (uint64_t i = 0; i < str->length; i++) {
    if (str->str[i] == '#' || str->str[i] == ';') {
      comment_pos = i;
      break;
    }
  }

  // If no comments, return original
  if (comment_pos == str->length) {
    return *str;
  }

  // If comment at start, return empty
  if (comment_pos == 0) {
    return (String8){NULL, 0};
  }

  // Create stripped copy in scratch arena
  uint8_t *stripped_data = (uint8_t *)vkr_allocator_alloc(
      allocator, comment_pos + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stripped_data) {
    return (String8){NULL, 0};
  }

  MemCopy(stripped_data, str->str, comment_pos);
  stripped_data[comment_pos] = '\0';

  return (String8){stripped_data, comment_pos};
}

vkr_internal bool8_t vkr_split_key_value_scratch(VkrAllocator *scratch,
                                                 const String8 *line,
                                                 String8 *out_key,
                                                 String8 *out_value) {
  if (!line || !line->str || line->length == 0) {
    return false_v;
  }

  // Find the '=' separator
  uint64_t eq_pos = 0;
  bool8_t found_eq = false_v;
  for (uint64_t i = 0; i < line->length; i++) {
    if (line->str[i] == '=') {
      eq_pos = i;
      found_eq = true_v;
      break;
    }
  }

  if (!found_eq) {
    return false_v;
  }

  // Extract and trim key
  String8 raw_key = string8_substring(line, 0, eq_pos);
  String8 trimmed_key = vkr_trim_string8_scratch(scratch, &raw_key);

  if (trimmed_key.length == 0 ||
      trimmed_key.length >= VKR_SHADER_CONFIG_MAX_KEY_LENGTH) {
    return false_v;
  }

  // Extract and trim value
  String8 raw_value = string8_substring(line, eq_pos + 1, line->length);
  String8 stripped_value = vkr_strip_comments_scratch(scratch, &raw_value);
  String8 trimmed_value = vkr_trim_string8_scratch(scratch, &stripped_value);

  if (trimmed_value.length >= VKR_SHADER_CONFIG_MAX_VALUE_LENGTH) {
    return false_v;
  }

  *out_key = trimmed_key;
  *out_value = trimmed_value;
  return true_v;
}

vkr_internal bool8_t vkr_split_csv_values_scratch(VkrAllocator *scratch,
                                                  const String8 *csv_str,
                                                  String8 *out_values,
                                                  uint32_t max_values,
                                                  uint32_t *out_count) {
  if (!csv_str || !csv_str->str || csv_str->length == 0) {
    *out_count = 0;
    return true_v;
  }

  uint32_t count = 0;
  uint64_t start = 0;

  for (uint64_t i = 0; i <= csv_str->length && count < max_values; i++) {
    if (i == csv_str->length || csv_str->str[i] == ',') {
      String8 token = string8_substring(csv_str, start, i);
      String8 trimmed = vkr_trim_string8_scratch(scratch, &token);

      if (trimmed.length > 0) {
        out_values[count] = trimmed;
        count++;
      }

      start = i + 1;
    }
  }

  *out_count = count;
  return true_v;
}

// =============================================================================
// Layout Computation Functions
// =============================================================================

vkr_internal void vkr_compute_uniform_layout(VkrShaderConfig *cfg) {
  uint64_t global_offset = 0, instance_offset = 0, local_size = 0;
  uint32_t global_tex = 0, instance_tex = 0;

  for (uint64_t i = 0; i < cfg->uniform_count; i++) {
    VkrShaderUniformDesc *ud =
        array_get_VkrShaderUniformDesc(&cfg->uniforms, i);

    if (ud->type == SHADER_UNIFORM_TYPE_SAMPLER) {
      if (ud->scope == VKR_SHADER_SCOPE_GLOBAL) {
        ud->location = global_tex++;
      } else if (ud->scope == VKR_SHADER_SCOPE_INSTANCE) {
        ud->location = instance_tex++;
      }
      ud->offset = 0;
      ud->size = 0;
      continue;
    }

    uint64_t align = vkr_std140_alignment(ud->type);
    uint64_t size = vkr_uniform_type_size(ud->type);
    ud->size = (uint32_t)size;

    if (ud->scope == VKR_SHADER_SCOPE_GLOBAL) {
      uint64_t aligned = vkr_align_up_u64(global_offset, align);
      aligned = vkr_apply_uniform_register_packing(aligned, size);
      ud->offset = (uint32_t)aligned;
      ud->location = 0;
      global_offset = aligned + size;
    } else if (ud->scope == VKR_SHADER_SCOPE_INSTANCE) {
      uint64_t aligned = vkr_align_up_u64(instance_offset, align);
      aligned = vkr_apply_uniform_register_packing(aligned, size);
      ud->offset = (uint32_t)aligned;
      ud->location = 0;
      instance_offset = aligned + size;
    } else if (ud->scope == VKR_SHADER_SCOPE_LOCAL) {
      uint64_t aligned =
          vkr_align_up_u64(local_size, VKR_SHADER_PUSH_CONSTANT_ALIGNMENT);
      ud->offset = (uint32_t)aligned;
      ud->location = 0;
      local_size = aligned + size;
    }
  }

  cfg->global_ubo_size =
      vkr_align_up_u64(global_offset, VKR_SHADER_UNIFORM_REGISTER_SIZE);
  cfg->instance_ubo_size =
      vkr_align_up_u64(instance_offset, VKR_SHADER_UNIFORM_REGISTER_SIZE);
  cfg->push_constant_size = local_size;

  cfg->global_ubo_stride =
      vkr_align_up_u64(cfg->global_ubo_size, VKR_SHADER_UBO_ALIGNMENT);
  cfg->instance_ubo_stride =
      vkr_align_up_u64(cfg->instance_ubo_size, VKR_SHADER_UBO_ALIGNMENT);
  cfg->push_constant_stride = vkr_align_up_u64(
      cfg->push_constant_size, VKR_SHADER_PUSH_CONSTANT_ALIGNMENT);

  cfg->global_texture_count = global_tex;
  cfg->instance_texture_count = instance_tex;
}

// =============================================================================
// Parser Functions
// =============================================================================

vkr_internal VkrShaderConfigParseResult vkr_create_parse_error(
    VkrAllocator *arena_alloc, VkrShaderConfigErrorType error_type,
    uint64_t line_number, uint64_t column_number, const char *fmt, ...) {

  VkrShaderConfigParseResult result = {0};
  result.is_valid = false_v;
  result.error_type = error_type;
  result.line_number = line_number;
  result.column_number = column_number;

  va_list args;
  va_start(args, fmt);
  result.error_message = string8_create_formatted_v(arena_alloc, fmt, args);
  va_end(args);

  return result;
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_attribute_line(VkrShaderConfigParser *parser, const String8 *value,
                         VkrShaderConfig *config) {

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(parser->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
        parser->line_number, 0, "Failed to allocate scratch scope");
  }

  String8 tokens[3];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(parser->scratch_allocator, value, tokens, 3,
                                    &token_count) ||
      token_count != 2) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
        parser->line_number, 0, "Attribute line must have format: type,name");
  }

  VkrShaderAttributeType type = vkr_parse_attribute_type(&tokens[0]);
  if (type == SHADER_ATTRIBUTE_TYPE_UNDEFINED) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Unknown attribute type: %.*s",
        tokens[0].length, tokens[0].str);
  }

  if (config->attribute_count >= config->attributes.length) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
        parser->line_number, 0, "Too many attributes defined");
  }

  VkrShaderAttributeDesc attr = {0};
  attr.type = type;
  // Store the name in the main arena for persistence
  attr.name = string8_duplicate(parser->allocator, &tokens[1]);

  array_set_VkrShaderAttributeDesc(&config->attributes, config->attribute_count,
                                   attr);
  vkr_hash_table_insert_uint32_t(&config->attribute_name_to_index,
                                 (const char *)attr.name.str,
                                 config->attribute_count);
  config->attribute_count++;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_uniform_line(VkrShaderConfigParser *parser, const String8 *value,
                       VkrShaderConfig *config) {

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(parser->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
        parser->line_number, 0, "Failed to allocate scratch scope");
  }

  String8 tokens[4];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(parser->scratch_allocator, value, tokens, 4,
                                    &token_count) ||
      token_count != 3) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
        parser->line_number, 0,
        "Uniform line must have format: type,scope,name");
  }

  VkrShaderUniformType type = vkr_parse_uniform_type(&tokens[0]);
  if (type == SHADER_UNIFORM_TYPE_UNDEFINED) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Unknown uniform type: %.*s", tokens[0].length,
        tokens[0].str);
  }

  uint32_t scope_val;
  if (!string8_to_u32(&tokens[1], &scope_val) ||
      scope_val > VKR_SHADER_SCOPE_LOCAL) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Invalid uniform scope: %.*s", tokens[1].length,
        tokens[1].str);
  }

  if (config->uniform_count >= config->uniforms.length) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
        parser->line_number, 0, "Too many uniforms defined");
  }

  VkrShaderUniformDesc uniform = {0};
  uniform.type = type;
  uniform.scope = (VkrShaderScope)scope_val;
  // Store the name in the main arena for persistence
  uniform.name = string8_duplicate(parser->allocator, &tokens[2]);

  array_set_VkrShaderUniformDesc(&config->uniforms, config->uniform_count,
                                 uniform);
  vkr_hash_table_insert_uint32_t(&config->uniform_name_to_index,
                                 (const char *)uniform.name.str,
                                 config->uniform_count);
  config->uniform_count++;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_stages_line(VkrShaderConfigParser *parser, const String8 *value,
                      VkrShaderConfig *config) {

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(parser->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
        parser->line_number, 0, "Failed to allocate scratch scope");
  }

  String8 tokens[VKR_SHADER_CONFIG_MAX_STAGES];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(parser->scratch_allocator, value, tokens,
                                    VKR_SHADER_CONFIG_MAX_STAGES,
                                    &token_count)) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
        parser->line_number, 0, "Failed to parse stages list");
  }

  for (uint32_t i = 0;
       i < token_count && config->stage_count < config->stages.length; i++) {
    VkrShaderStage stage = vkr_parse_shader_stage(&tokens[i]);
    if (stage == VKR_SHADER_STAGE_COUNT) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      return vkr_create_parse_error(
          parser->allocator, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
          parser->line_number, 0, "Unknown shader stage: %.*s",
          tokens[i].length, tokens[i].str);
    }

    VkrShaderStageFile stage_file = {0};
    stage_file.stage = stage;
    stage_file.entry_point = (stage == VKR_SHADER_STAGE_VERTEX)
                                 ? string8_lit("vertexMain")
                                 : string8_lit("fragmentMain");

    array_set_VkrShaderStageFile(&config->stages, config->stage_count,
                                 stage_file);
    config->stage_count++;
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_stage_files_line(VkrShaderConfigParser *parser, const String8 *value,
                           VkrShaderConfig *config) {

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(parser->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
        parser->line_number, 0, "Failed to allocate scratch scope");
  }

  String8 tokens[VKR_SHADER_CONFIG_MAX_STAGES];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(parser->scratch_allocator, value, tokens,
                                    VKR_SHADER_CONFIG_MAX_STAGES,
                                    &token_count)) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->allocator, VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
        parser->line_number, 0, "Failed to parse stage files list");
  }

  if (token_count == 1) {
    // Single file for all stages - store in main arena
    String8 filename = string8_duplicate(parser->allocator, &tokens[0]);
    for (uint32_t i = 0; i < config->stage_count; i++) {
      VkrShaderStageFile *stage_file =
          array_get_VkrShaderStageFile(&config->stages, i);
      stage_file->filename = filename;
    }
  } else {
    // Individual files for each stage - store each in main arena
    uint32_t files_to_assign =
        (token_count < config->stage_count) ? token_count : config->stage_count;
    for (uint32_t i = 0; i < files_to_assign; i++) {
      VkrShaderStageFile *stage_file =
          array_get_VkrShaderStageFile(&config->stages, i);
      stage_file->filename = string8_duplicate(parser->allocator, &tokens[i]);
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_initialize_config(VkrAllocator *config_alloc, VkrShaderConfig *config) {

  config->attributes = array_create_VkrShaderAttributeDesc(
      config_alloc, VKR_SHADER_ATTRIBUTE_COUNT_MAX);
  config->uniforms = array_create_VkrShaderUniformDesc(
      config_alloc, VKR_SHADER_UNIFORM_COUNT_MAX);
  config->uniform_name_to_index = vkr_hash_table_create_uint32_t(
      config_alloc, VKR_SHADER_UNIFORM_COUNT_MAX);
  config->attribute_name_to_index = vkr_hash_table_create_uint32_t(
      config_alloc, VKR_SHADER_ATTRIBUTE_COUNT_MAX);
  config->stages =
      array_create_VkrShaderStageFile(config_alloc, VKR_SHADER_STAGE_COUNT);

  config->attribute_count = 0;
  config->uniform_count = 0;
  config->stage_count = 0;
  config->use_instance = 0;
  config->use_local = 0;
  config->cull_mode = VKR_CULL_MODE_BACK;
  config->name = (String8){0};
  config->renderpass_name = (String8){0};

  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult vkr_shader_loader_parse(
    String8 path, VkrAllocator *allocator, VkrAllocator *scratch_alloc,
    VkrShaderConfig *out_config) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(scratch_alloc != NULL, "Scratch alloc is NULL");
  assert_log(out_config != NULL, "Out config is NULL");

  if (!path.str || path.length == 0) {
    return vkr_create_parse_error(allocator,
                                  VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT, 0, 0,
                                  "Invalid file path");
  }

  if (!allocator || !out_config) {
    return vkr_create_parse_error(allocator,
                                  VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT, 0, 0,
                                  "Invalid parameters");
  }

  if (!scratch_alloc) {
    return vkr_create_parse_error(allocator,
                                  VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION, 0,
                                  0, "Failed to create scratch arena");
  }

  // Initialize configuration
  VkrShaderConfigParseResult init_result =
      vkr_initialize_config(allocator, out_config);
  if (!init_result.is_valid) {
    return init_result;
  }

  // Create parser context
  VkrShaderConfigParser parser = {0};
  parser.allocator = allocator;
  parser.scratch_allocator = scratch_alloc;
  parser.line_number = 0;
  parser.column_number = 0;
  parser.file_path = path;

  // Open file
  FilePath fp = file_path_create((const char *)path.str, parser.allocator,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError fe = file_open(&fp, mode, &handle);

  if (fe != FILE_ERROR_NONE) {
    return vkr_create_parse_error(
        parser.allocator, VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND, 0, 0,
        "Failed to open shader config file: %s", path.str);
  }

  // Required fields tracking
  bool8_t has_name = false_v;
  bool8_t has_renderpass = false_v;
  bool8_t has_stages = false_v;

  // Parse file line by line
  while (true) {
    // Create a line-level scope for temporary line processing
    VkrAllocatorScope line_scope = vkr_allocator_begin_scope(scratch_alloc);
    if (!vkr_allocator_scope_is_valid(&line_scope)) {
      file_close(&handle);
      return vkr_create_parse_error(
          parser.allocator, VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION,
          parser.line_number, 0, "Failed to allocate line scope");
    }

    String8 raw_line = {0};
    fe = file_read_line(&handle, scratch_alloc, scratch_alloc,
                        VKR_SHADER_CONFIG_MAX_LINE_LENGTH, &raw_line);

    if (fe == FILE_ERROR_EOF) {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      break;
    }

    if (fe != FILE_ERROR_NONE) {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      file_close(&handle);
      return vkr_create_parse_error(
          parser.allocator, VKR_SHADER_CONFIG_ERROR_FILE_READ_FAILED,
          parser.line_number, 0, "Failed to read line from file");
    }

    parser.line_number++;
    parser.current_line = raw_line;

    // Skip empty lines and comments
    String8 trimmed_line = vkr_trim_string8_scratch(scratch_alloc, &raw_line);
    if (trimmed_line.length == 0 || trimmed_line.str[0] == '#' ||
        trimmed_line.str[0] == ';') {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      continue;
    }

    // Parse key=value using line scratch
    String8 key, value;
    if (!vkr_split_key_value_scratch(scratch_alloc, &trimmed_line, &key,
                                     &value)) {
      log_warn("Malformed key=value line %u: %.*s", parser.line_number,
               trimmed_line.length, trimmed_line.str);
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      continue; // Skip malformed lines
    }

    // Process different key types using our safe String8 comparison functions
    VkrShaderConfigParseResult line_result = {.is_valid = true_v};

    if (vkr_string8_equals_cstr_i(&key, "name")) {
      if (value.length > VKR_SHADER_NAME_MAX_LENGTH) {
        line_result = vkr_create_parse_error(
            parser.allocator, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
            parser.line_number, 0, "Shader name too long");
      } else {
        out_config->name = string8_duplicate(parser.allocator, &value);
        has_name = true_v;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "renderpass")) {
      out_config->renderpass_name = string8_duplicate(parser.allocator, &value);
      has_renderpass = true_v;
    } else if (vkr_string8_equals_cstr_i(&key, "stages")) {
      line_result = vkr_parse_stages_line(&parser, &value, out_config);
      if (line_result.is_valid) {
        has_stages = true_v;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "stagefiles")) {
      line_result = vkr_parse_stage_files_line(&parser, &value, out_config);
    } else if (vkr_string8_equals_cstr_i(&key, "attribute")) {
      line_result = vkr_parse_attribute_line(&parser, &value, out_config);
    } else if (vkr_string8_equals_cstr_i(&key, "uniform")) {
      line_result = vkr_parse_uniform_line(&parser, &value, out_config);
    } else if (vkr_string8_equals_cstr_i(&key, "use_instance")) {
      uint32_t use_instance;
      if (string8_to_u32(&value, &use_instance)) {
        out_config->use_instance = (uint8_t)use_instance;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "use_local")) {
      uint32_t use_local;
      if (string8_to_u32(&value, &use_local)) {
        out_config->use_local = (uint8_t)use_local;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "cull_mode")) {
      out_config->cull_mode = vkr_parse_cull_mode(&value);
    } else if (vkr_string8_equals_cstr_i(&key, "vertex_layout")) {
      log_warn("vertex_layout key is deprecated and will be ignored");
    } else if (vkr_string8_equals_cstr_i(&key, "version")) {
      log_debug("Version: %.*s", value.length, value.str);
    } else {
      log_warn("Unknown key: %.*s", key.length, key.str);
    }

    vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

    if (!line_result.is_valid) {
      file_close(&handle);
      return line_result;
    }
  }

  file_close(&handle);

  // Validate required fields
  if (!has_name || !has_stages) {
    return vkr_create_parse_error(
        parser.allocator, VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD, 0, 0,
        "Missing required field(s): name and stages are both required");
  }

  // Compute layouts
  vkr_compute_attribute_layout(out_config);

  if (!has_renderpass) {
    if (out_config->vertex_type == VKR_VERTEX_TYPE_2D) {
      out_config->renderpass_name = string8_lit("Renderpass.Builtin.UI");
    } else {
      out_config->renderpass_name = string8_lit("Renderpass.Builtin.World");
    }
  }

  vkr_compute_uniform_layout(out_config);

  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

// =============================================================================
// Error string conversion
// =============================================================================

vkr_internal const char *
vkr_shader_config_error_string(VkrShaderConfigErrorType error_type) {
  switch (error_type) {
  case VKR_SHADER_CONFIG_ERROR_NONE:
    return "No error";
  case VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND:
    return "Configuration file not found";
  case VKR_SHADER_CONFIG_ERROR_FILE_READ_FAILED:
    return "Failed to read configuration file";
  case VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT:
    return "Invalid configuration file format";
  case VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD:
    return "Missing required configuration field";
  case VKR_SHADER_CONFIG_ERROR_INVALID_VALUE:
    return "Invalid configuration value";
  case VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW:
    return "Configuration data too large";
  case VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION:
    return "Memory allocation failed";
  case VKR_SHADER_CONFIG_ERROR_PARSE_FAILED:
    return "Configuration parsing failed";
  case VKR_SHADER_CONFIG_ERROR_DUPLICATE_KEY:
    return "Duplicate configuration key";
  case VKR_SHADER_CONFIG_ERROR_UNKNOWN:
  default:
    return "Unknown configuration error";
  }
}

vkr_internal VkrShaderConfigParseResult
vkr_shader_config_validate(const VkrShaderConfig *config) {
  if (!config) {
    return (VkrShaderConfigParseResult){
        .is_valid = false_v,
        .error_type = VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
        .error_message = string8_lit("Configuration is null")};
  }

  if (config->name.length == 0) {
    return (VkrShaderConfigParseResult){
        .is_valid = false_v,
        .error_type = VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD,
        .error_message = string8_lit("Shader name is required")};
  }

  if (config->stage_count == 0) {
    return (VkrShaderConfigParseResult){
        .is_valid = false_v,
        .error_type = VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD,
        .error_message = string8_lit("At least one shader stage is required")};
  }

  if (config->vertex_type == VKR_VERTEX_TYPE_UNKNOWN) {
    return (VkrShaderConfigParseResult){
        .is_valid = false_v,
        .error_type = VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        .error_message =
            string8_lit("Failed to determine vertex layout for shader")};
  }

  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

// =============================================================================
// Resource Loader Integration
// =============================================================================

vkr_internal bool8_t vkr_shader_loader_can_load(VkrResourceLoader *self,
                                                String8 name) {
  (void)self;
  assert_log(name.str != NULL, "Name is NULL");

  const uint8_t *s = name.str;
  for (uint64_t ch = name.length; ch > 0; ch--) {
    if (s[ch - 1] == '.') {
      String8 ext = string8_substring(&name, ch, name.length);
      String8 shadercfg = string8_lit("shadercfg");
      return string8_equalsi(&ext, &shadercfg);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_shader_loader_load(VkrResourceLoader *self,
                                            String8 name,
                                            VkrAllocator *temp_alloc,
                                            VkrResourceHandleInfo *out_handle,
                                            VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrShaderSystem *shader_system = (VkrShaderSystem *)self->resource_system;
  assert_log(shader_system != NULL, "Shader system is NULL");

  VkrShaderConfig *cfg =
      vkr_allocator_alloc(&shader_system->allocator, sizeof(VkrShaderConfig),
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!cfg) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(cfg, sizeof(*cfg));

  VkrShaderConfigParseResult parse_result =
      vkr_shader_loader_parse(name, &shader_system->allocator, temp_alloc, cfg);
  if (!parse_result.is_valid) {
    const char *err_str =
        vkr_shader_config_error_string(parse_result.error_type);
    log_error("Shader loader: failed to parse '%s': %s (line %u)",
              string8_cstr(&name), err_str, parse_result.line_number);
    *out_error =
        (parse_result.error_type == VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND)
            ? VKR_RENDERER_ERROR_FILE_NOT_FOUND
            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  VkrShaderConfigParseResult valid = vkr_shader_config_validate(cfg);
  if (!valid.is_valid) {
    const char *err_str = vkr_shader_config_error_string(valid.error_type);
    log_error("Shader loader: validation failed for '%s': %s", "(cfg)",
              err_str);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_CUSTOM;
  out_handle->as.custom = (void *)cfg;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal void vkr_shader_loader_unload(VkrResourceLoader *self,
                                           const VkrResourceHandleInfo *handle,
                                           String8 name) {
  (void)self;
  (void)handle;
  (void)name;
  // Config memory is owned by the shader system arena; no-op here for now.
}

VkrResourceLoader vkr_shader_loader_create(void) {
  VkrResourceLoader loader = (VkrResourceLoader){0};
  loader.type = VKR_RESOURCE_TYPE_CUSTOM;
  loader.custom_type = string8_lit("shadercfg");
  loader.can_load = vkr_shader_loader_can_load;
  loader.load = vkr_shader_loader_load;
  loader.unload = vkr_shader_loader_unload;
  return loader;
}

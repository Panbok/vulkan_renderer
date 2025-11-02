#include "renderer/resources/loaders/shader_loader.h"
#include "renderer/systems/vkr_shader_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_SHADER_CONFIG_MAX_LINE_LENGTH 4096
#define VKR_SHADER_CONFIG_MAX_KEY_LENGTH 128
#define VKR_SHADER_CONFIG_MAX_VALUE_LENGTH 512
#define VKR_SHADER_CONFIG_MAX_TOKEN_LENGTH 64
#define VKR_SHADER_UBO_ALIGNMENT 256
#define VKR_SHADER_PUSH_CONSTANT_ALIGNMENT 4
#define VKR_SHADER_CONFIG_MAX_STAGES 8

#define VKR_SHADER_ATTRIBUTE_COUNT_MAX 32
#define VKR_SHADER_UNIFORM_COUNT_MAX 64

// Scratch arena sizes for parsing operations
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
  String8 error_message;  // Arena-allocated detailed error message
  uint64_t line_number;   // 0 if not line-specific
  uint64_t column_number; // 0 if not column-specific
} VkrShaderConfigParseResult;

// =============================================================================
// Parser Context
// =============================================================================

typedef struct VkrShaderConfigParser {
  Arena *arena;         // Main arena for persistent data
  Arena *scratch_arena; // Scratch arena for temporary allocations
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

vkr_internal INLINE uint64_t vkr_std140_alignment(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
    return sizeof(float32_t);
  case SHADER_UNIFORM_TYPE_INT32:
    return sizeof(int32_t);
  case SHADER_UNIFORM_TYPE_UINT32:
    return sizeof(uint32_t);
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
    return sizeof(Vec2);
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
    return sizeof(Vec3);
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
    return sizeof(Vec4);
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return sizeof(Mat4);
  case SHADER_UNIFORM_TYPE_SAMPLER:
    return 0;
  case SHADER_UNIFORM_TYPE_UNDEFINED:
    return 4;
  }
  return 4;
}

vkr_internal INLINE uint64_t vkr_uniform_type_size(VkrShaderUniformType type) {
  switch (type) {
  case SHADER_UNIFORM_TYPE_FLOAT32:
    return sizeof(float32_t);
  case SHADER_UNIFORM_TYPE_FLOAT32_2:
    return sizeof(Vec2);
  case SHADER_UNIFORM_TYPE_FLOAT32_3:
    return sizeof(Vec3);
  case SHADER_UNIFORM_TYPE_FLOAT32_4:
    return sizeof(Vec4);
  case SHADER_UNIFORM_TYPE_INT32:
    return sizeof(int32_t);
  case SHADER_UNIFORM_TYPE_UINT32:
    return sizeof(uint32_t);
  case SHADER_UNIFORM_TYPE_MATRIX_4:
    return sizeof(Mat4);
  case SHADER_UNIFORM_TYPE_SAMPLER:
  case SHADER_UNIFORM_TYPE_UNDEFINED:
    return 0;
  }
  return 0;
}

vkr_internal String8 vkr_create_formatted_error(Arena *arena, const char *fmt,
                                                ...) {
  va_list args;
  va_start(args, fmt);
  String8 result = string8_create_formatted_v(arena, fmt, args);
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

vkr_internal VkrGeometryVertexLayoutType
vkr_parse_vertex_layout_type(const String8 *layout_str) {
  if (vkr_string8_equals_cstr_i(layout_str,
                                "GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD"))
    return GEOMETRY_VERTEX_LAYOUT_POSITION_TEXCOORD;
  if (vkr_string8_equals_cstr_i(layout_str,
                                "GEOMETRY_VERTEX_LAYOUT_POSITION_COLOR"))
    return GEOMETRY_VERTEX_LAYOUT_POSITION_COLOR;
  if (vkr_string8_equals_cstr_i(layout_str,
                                "GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_COLOR"))
    return GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_COLOR;
  if (vkr_string8_equals_cstr_i(
          layout_str, "GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD"))
    return GEOMETRY_VERTEX_LAYOUT_POSITION_NORMAL_TEXCOORD;
  if (vkr_string8_equals_cstr_i(layout_str,
                                "GEOMETRY_VERTEX_LAYOUT_POSITION2_TEXCOORD"))
    return GEOMETRY_VERTEX_LAYOUT_POSITION2_TEXCOORD;
  if (vkr_string8_equals_cstr_i(layout_str, "GEOMETRY_VERTEX_LAYOUT_FULL"))
    return GEOMETRY_VERTEX_LAYOUT_FULL;
  return GEOMETRY_VERTEX_LAYOUT_COUNT;
}

// =============================================================================
// String Processing Functions (Using Scratch Arenas)
// =============================================================================

vkr_internal String8 vkr_trim_string8_scratch(Arena *scratch,
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
  uint8_t *trimmed_data = (uint8_t *)arena_alloc(scratch, trimmed_length + 1,
                                                 ARENA_MEMORY_TAG_STRING);
  if (!trimmed_data) {
    return (String8){NULL, 0};
  }

  MemCopy(trimmed_data, str->str + start, trimmed_length);
  trimmed_data[trimmed_length] = '\0';

  return (String8){trimmed_data, trimmed_length};
}

vkr_internal String8 vkr_strip_comments_scratch(Arena *scratch,
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
  uint8_t *stripped_data =
      (uint8_t *)arena_alloc(scratch, comment_pos + 1, ARENA_MEMORY_TAG_STRING);
  if (!stripped_data) {
    return (String8){NULL, 0};
  }

  MemCopy(stripped_data, str->str, comment_pos);
  stripped_data[comment_pos] = '\0';

  return (String8){stripped_data, comment_pos};
}

vkr_internal bool8_t vkr_split_key_value_scratch(Arena *scratch,
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

vkr_internal bool8_t vkr_split_csv_values_scratch(Arena *scratch,
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

vkr_internal void vkr_compute_attribute_layout(VkrShaderConfig *cfg) {
  uint64_t offset = 0;
  for (uint64_t i = 0; i < cfg->attribute_count; i++) {
    VkrShaderAttributeDesc *ad =
        array_get_VkrShaderAttributeDesc(&cfg->attributes, i);
    ad->location = (uint32_t)i;
    ad->offset = (uint32_t)offset;
    ad->size = vkr_attribute_type_size(ad->type);
    offset += ad->size;
  }
  cfg->attribute_stride = vkr_align_up_u64(offset, 16);
}

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
      ud->offset = (uint32_t)vkr_align_up_u64(global_offset, align);
      ud->location = 0;
      global_offset = ud->offset + size;
    } else if (ud->scope == VKR_SHADER_SCOPE_INSTANCE) {
      ud->offset = (uint32_t)vkr_align_up_u64(instance_offset, align);
      ud->location = 0;
      instance_offset = ud->offset + size;
    } else if (ud->scope == VKR_SHADER_SCOPE_LOCAL) {
      uint64_t aligned =
          vkr_align_up_u64(local_size, VKR_SHADER_PUSH_CONSTANT_ALIGNMENT);
      ud->offset = (uint32_t)aligned;
      ud->location = 0;
      local_size = aligned + size;
    }
  }

  cfg->global_ubo_size = global_offset;
  cfg->instance_ubo_size = instance_offset;
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
    Arena *arena, VkrShaderConfigErrorType error_type, uint64_t line_number,
    uint64_t column_number, const char *fmt, ...) {

  VkrShaderConfigParseResult result = {0};
  result.is_valid = false_v;
  result.error_type = error_type;
  result.line_number = line_number;
  result.column_number = column_number;

  va_list args;
  va_start(args, fmt);
  result.error_message = string8_create_formatted_v(arena, fmt, args);
  va_end(args);

  return result;
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_attribute_line(VkrShaderConfigParser *parser, const String8 *value,
                         VkrShaderConfig *config) {

  Scratch scratch = scratch_create(parser->scratch_arena);

  String8 tokens[3];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(scratch.arena, value, tokens, 3,
                                    &token_count) ||
      token_count != 2) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
        parser->line_number, 0, "Attribute line must have format: type,name");
  }

  VkrShaderAttributeType type = vkr_parse_attribute_type(&tokens[0]);
  if (type == SHADER_ATTRIBUTE_TYPE_UNDEFINED) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Unknown attribute type: %.*s",
        tokens[0].length, tokens[0].str);
  }

  if (config->attribute_count >= config->attributes.length) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
        parser->line_number, 0, "Too many attributes defined");
  }

  VkrShaderAttributeDesc attr = {0};
  attr.type = type;
  // Store the name in the main arena for persistence
  attr.name = string8_duplicate(parser->arena, &tokens[1]);

  array_set_VkrShaderAttributeDesc(&config->attributes, config->attribute_count,
                                   attr);
  vkr_hash_table_insert_uint32_t(&config->attribute_name_to_index,
                                 (const char *)attr.name.str,
                                 config->attribute_count);
  config->attribute_count++;

  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_uniform_line(VkrShaderConfigParser *parser, const String8 *value,
                       VkrShaderConfig *config) {

  Scratch scratch = scratch_create(parser->scratch_arena);

  String8 tokens[4];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(scratch.arena, value, tokens, 4,
                                    &token_count) ||
      token_count != 3) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
        parser->line_number, 0,
        "Uniform line must have format: type,scope,name");
  }

  VkrShaderUniformType type = vkr_parse_uniform_type(&tokens[0]);
  if (type == SHADER_UNIFORM_TYPE_UNDEFINED) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Unknown uniform type: %.*s", tokens[0].length,
        tokens[0].str);
  }

  uint32_t scope_val;
  if (!string8_to_u32(&tokens[1], &scope_val) ||
      scope_val > VKR_SHADER_SCOPE_LOCAL) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
        parser->line_number, 0, "Invalid uniform scope: %.*s", tokens[1].length,
        tokens[1].str);
  }

  if (config->uniform_count >= config->uniforms.length) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_BUFFER_OVERFLOW,
        parser->line_number, 0, "Too many uniforms defined");
  }

  VkrShaderUniformDesc uniform = {0};
  uniform.type = type;
  uniform.scope = (VkrShaderScope)scope_val;
  // Store the name in the main arena for persistence
  uniform.name = string8_duplicate(parser->arena, &tokens[2]);

  array_set_VkrShaderUniformDesc(&config->uniforms, config->uniform_count,
                                 uniform);
  vkr_hash_table_insert_uint32_t(&config->uniform_name_to_index,
                                 (const char *)uniform.name.str,
                                 config->uniform_count);
  config->uniform_count++;

  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_stages_line(VkrShaderConfigParser *parser, const String8 *value,
                      VkrShaderConfig *config) {

  Scratch scratch = scratch_create(parser->scratch_arena);

  String8 tokens[VKR_SHADER_CONFIG_MAX_STAGES];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(scratch.arena, value, tokens,
                                    VKR_SHADER_CONFIG_MAX_STAGES,
                                    &token_count)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
        parser->line_number, 0, "Failed to parse stages list");
  }

  for (uint32_t i = 0;
       i < token_count && config->stage_count < config->stages.length; i++) {
    VkrShaderStage stage = vkr_parse_shader_stage(&tokens[i]);
    if (stage == VKR_SHADER_STAGE_COUNT) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
      return vkr_create_parse_error(
          parser->arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE,
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

  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_parse_stage_files_line(VkrShaderConfigParser *parser, const String8 *value,
                           VkrShaderConfig *config) {

  Scratch scratch = scratch_create(parser->scratch_arena);

  String8 tokens[VKR_SHADER_CONFIG_MAX_STAGES];
  uint32_t token_count;

  if (!vkr_split_csv_values_scratch(scratch.arena, value, tokens,
                                    VKR_SHADER_CONFIG_MAX_STAGES,
                                    &token_count)) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
    return vkr_create_parse_error(
        parser->arena, VKR_SHADER_CONFIG_ERROR_PARSE_FAILED,
        parser->line_number, 0, "Failed to parse stage files list");
  }

  if (token_count == 1) {
    // Single file for all stages - store in main arena
    String8 filename = string8_duplicate(parser->arena, &tokens[0]);
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
      stage_file->filename = string8_duplicate(parser->arena, &tokens[i]);
    }
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_initialize_config(Arena *arena, VkrShaderConfig *config) {
  config->attributes = array_create_VkrShaderAttributeDesc(
      arena, VKR_SHADER_ATTRIBUTE_COUNT_MAX);
  config->uniforms =
      array_create_VkrShaderUniformDesc(arena, VKR_SHADER_UNIFORM_COUNT_MAX);
  config->uniform_name_to_index =
      vkr_hash_table_create_uint32_t(arena, VKR_SHADER_UNIFORM_COUNT_MAX);
  config->attribute_name_to_index =
      vkr_hash_table_create_uint32_t(arena, VKR_SHADER_ATTRIBUTE_COUNT_MAX);
  config->stages =
      array_create_VkrShaderStageFile(arena, VKR_SHADER_STAGE_COUNT);

  config->attribute_count = 0;
  config->uniform_count = 0;
  config->stage_count = 0;
  config->vertex_layout = GEOMETRY_VERTEX_LAYOUT_COUNT; // invalid sentinel
  config->use_instance = 0;
  config->use_local = 0;
  config->name = (String8){0};
  config->renderpass_name = (String8){0};

  return (VkrShaderConfigParseResult){.is_valid = true_v};
}

vkr_internal VkrShaderConfigParseResult
vkr_shader_loader_parse(String8 path, Arena *arena, Arena *scratch_arena,
                        VkrShaderConfig *out_config) {
  if (!path.str || path.length == 0) {
    return vkr_create_parse_error(arena, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
                                  0, 0, "Invalid file path");
  }

  if (!arena || !out_config) {
    return vkr_create_parse_error(arena, VKR_SHADER_CONFIG_ERROR_INVALID_FORMAT,
                                  0, 0, "Invalid parameters");
  }

  if (!scratch_arena) {
    return vkr_create_parse_error(arena,
                                  VKR_SHADER_CONFIG_ERROR_MEMORY_ALLOCATION, 0,
                                  0, "Failed to create scratch arena");
  }

  // Initialize configuration
  VkrShaderConfigParseResult init_result =
      vkr_initialize_config(arena, out_config);
  if (!init_result.is_valid) {
    return init_result;
  }

  // Create parser context
  VkrShaderConfigParser parser = {0};
  parser.arena = arena;
  parser.scratch_arena = scratch_arena;
  parser.line_number = 0;
  parser.column_number = 0;
  parser.file_path = path;

  // Open file
  FilePath fp =
      file_path_create((const char *)path.str, arena, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError fe = file_open(&fp, mode, &handle);

  if (fe != FILE_ERROR_NONE) {
    return vkr_create_parse_error(arena, VKR_SHADER_CONFIG_ERROR_FILE_NOT_FOUND,
                                  0, 0, "Failed to open shader config file: %s",
                                  path.str);
  }

  // Required fields tracking
  bool8_t has_name = false_v;
  bool8_t has_renderpass = false_v;
  bool8_t has_stages = false_v;

  // Parse file line by line
  while (true) {
    // Create a line-level scratch for temporary line processing
    Scratch line_scratch = scratch_create(scratch_arena);

    String8 raw_line = {0};
    fe = file_read_line(&handle, line_scratch.arena, line_scratch.arena,
                        VKR_SHADER_CONFIG_MAX_LINE_LENGTH, &raw_line);

    if (fe == FILE_ERROR_EOF) {
      scratch_destroy(line_scratch, ARENA_MEMORY_TAG_STRING);
      break;
    }

    if (fe != FILE_ERROR_NONE) {
      scratch_destroy(line_scratch, ARENA_MEMORY_TAG_STRING);
      file_close(&handle);
      return vkr_create_parse_error(
          arena, VKR_SHADER_CONFIG_ERROR_FILE_READ_FAILED, parser.line_number,
          0, "Failed to read line from file");
    }

    parser.line_number++;
    parser.current_line = raw_line;

    // Skip empty lines and comments
    String8 trimmed_line =
        vkr_trim_string8_scratch(line_scratch.arena, &raw_line);
    if (trimmed_line.length == 0 || trimmed_line.str[0] == '#' ||
        trimmed_line.str[0] == ';') {
      scratch_destroy(line_scratch, ARENA_MEMORY_TAG_STRING);
      continue;
    }

    // Parse key=value using line scratch
    String8 key, value;
    if (!vkr_split_key_value_scratch(line_scratch.arena, &trimmed_line, &key,
                                     &value)) {
      scratch_destroy(line_scratch, ARENA_MEMORY_TAG_STRING);
      continue; // Skip malformed lines
    }

    // Process different key types using our safe String8 comparison functions
    VkrShaderConfigParseResult line_result = {.is_valid = true_v};

    if (vkr_string8_equals_cstr_i(&key, "name")) {
      if (value.length > VKR_SHADER_NAME_MAX_LENGTH) {
        line_result = vkr_create_parse_error(
            arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE, parser.line_number, 0,
            "Shader name too long");
      } else {
        out_config->name = string8_duplicate(arena, &value);
        has_name = true_v;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "renderpass")) {
      out_config->renderpass_name = string8_duplicate(arena, &value);
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
    } else if (vkr_string8_equals_cstr_i(&key, "vertex_layout")) {
      VkrGeometryVertexLayoutType layout = vkr_parse_vertex_layout_type(&value);
      if (layout == GEOMETRY_VERTEX_LAYOUT_COUNT) {
        line_result = vkr_create_parse_error(
            arena, VKR_SHADER_CONFIG_ERROR_INVALID_VALUE, parser.line_number, 0,
            "Unknown vertex_layout: %.*s", value.length, value.str);
      } else {
        out_config->vertex_layout = layout;
      }
    } else if (vkr_string8_equals_cstr_i(&key, "version")) {
      log_debug("Version: %.*s", value.length, value.str);
    } else {
      log_warn("Unknown key: %.*s", key.length, key.str);
    }

    scratch_destroy(line_scratch, ARENA_MEMORY_TAG_STRING);

    if (!line_result.is_valid) {
      file_close(&handle);
      return line_result;
    }
  }

  file_close(&handle);

  // Validate required fields
  if (!has_name || !has_renderpass || !has_stages) {
    return vkr_create_parse_error(
        arena, VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD, 0, 0,
        "Missing required fields: name, renderpass, or stages");
  }

  // Compute layouts
  vkr_compute_attribute_layout(out_config);
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

  if (config->vertex_layout == GEOMETRY_VERTEX_LAYOUT_COUNT) {
    return (VkrShaderConfigParseResult){
        .is_valid = false_v,
        .error_type = VKR_SHADER_CONFIG_ERROR_MISSING_REQUIRED_FIELD,
        .error_message = string8_lit(
            "vertex_layout is required and must be a valid enum value")};
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
                                            String8 name, Arena *temp_arena,
                                            VkrResourceHandleInfo *out_handle,
                                            VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrShaderSystem *shader_system = (VkrShaderSystem *)self->resource_system;
  assert_log(shader_system != NULL, "Shader system is NULL");
  assert_log(shader_system->arena != NULL, "Shader system arena is NULL");

  VkrShaderConfig *cfg = arena_alloc(
      shader_system->arena, sizeof(VkrShaderConfig), ARENA_MEMORY_TAG_RENDERER);
  if (!cfg) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(cfg, sizeof(*cfg));

  VkrShaderConfigParseResult parse_result =
      vkr_shader_loader_parse(name, shader_system->arena, temp_arena, cfg);
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
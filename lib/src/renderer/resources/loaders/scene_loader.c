/**
 * @file scene_loader.c
 * @brief Scene JSON loader implementation.
 */

#include "renderer/resources/loaders/scene_loader.h"

#include "core/logger.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_mesh_manager.h"

typedef struct SceneText3DImport {
  String8 text;
  float32_t font_size;
  Vec4 color;
  String8 font_name;
  uint32_t texture_width;
  uint32_t texture_height;
  float32_t uv_inset_px;
} SceneText3DImport;

typedef struct SceneShapeImport {
  SceneShapeType type;
  Vec3 dimensions;
  Vec4 color;
  String8 material_name; // Material name for acquire (matches .mt name= field)
  String8 material_path; // Material file path for loading
} SceneShapeImport;

typedef struct ScenePointLightImport {
  Vec3 color;
  float32_t intensity;
  float32_t constant;
  float32_t linear;
  float32_t quadratic;
  bool8_t enabled;
} ScenePointLightImport;

typedef struct SceneDirectionalLightImport {
  Vec3 color;
  float32_t intensity;
  Vec3 direction_local;
  bool8_t enabled;
} SceneDirectionalLightImport;

typedef struct SceneEntityImport {
  String8 name;
  int32_t parent_index;
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  bool8_t has_mesh;
  String8 mesh_path;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  bool8_t has_text3d;
  SceneText3DImport text3d;
  bool8_t has_shape;
  SceneShapeImport shape;
  bool8_t has_point_light;
  ScenePointLightImport point_light;
  bool8_t has_directional_light;
  SceneDirectionalLightImport directional_light;
} SceneEntityImport;

#define SCENE_ASYNC_ENTITY_CHUNK 64u
#define SCENE_ASYNC_RELATION_CHUNK 128u
#define SCENE_ASYNC_COMPONENT_CHUNK 16u
#define SCENE_ASYNC_MESH_CHUNK 8u

typedef enum SceneAsyncFinalizeStage {
  SCENE_ASYNC_STAGE_CREATE_ENTITIES = 0,
  SCENE_ASYNC_STAGE_SET_PARENTS,
  SCENE_ASYNC_STAGE_SET_COMPONENTS,
  SCENE_ASYNC_STAGE_ATTACH_MESHES,
  SCENE_ASYNC_STAGE_WAIT_DEPENDENCIES,
  SCENE_ASYNC_STAGE_COMPLETE
} SceneAsyncFinalizeStage;

typedef struct SceneMeshAsyncState {
  bool8_t requested;
  bool8_t attached;
  bool8_t completed;
  VkrResourceHandleInfo request_info;
} SceneMeshAsyncState;

typedef struct SceneShapeMaterialAsyncState {
  bool8_t requested;
  VkrResourceHandleInfo request_info;
} SceneShapeMaterialAsyncState;

typedef struct VkrSceneLoaderAsyncPayload {
  struct s_RendererFrontend *rf;
  char *json_storage;
  uint64_t json_length;
  SceneEntityImport *imports;
  uint32_t imports_capacity;
  uint32_t entity_count;
  VkrEntityId *entity_ids;
  SceneMeshAsyncState *mesh_states;
  SceneShapeMaterialAsyncState *shape_material_states;
  SceneAsyncFinalizeStage stage;
  uint32_t stage_cursor;
  VkrSceneLoadResult load_result;
  VkrSceneHandle scene_handle;
  bool8_t ownership_transferred;
} VkrSceneLoaderAsyncPayload;

vkr_internal VkrRendererError scene_error_to_renderer_error(VkrSceneError err) {
  switch (err) {
  case VKR_SCENE_ERROR_NONE:
    return VKR_RENDERER_ERROR_NONE;
  case VKR_SCENE_ERROR_ALLOC_FAILED:
  case VKR_SCENE_ERROR_WORLD_INIT_FAILED:
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  case VKR_SCENE_ERROR_INVALID_ENTITY:
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  case VKR_SCENE_ERROR_FILE_NOT_FOUND:
    return VKR_RENDERER_ERROR_FILE_NOT_FOUND;
  case VKR_SCENE_ERROR_MESH_LOAD_FAILED:
    return VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
  default:
    return VKR_RENDERER_ERROR_UNKNOWN;
  }
}

vkr_internal bool8_t scene_loader_alloc_copy_string(VkrAllocator *allocator,
                                                    VkrMutex mutex,
                                                    String8 source,
                                                    char **out_storage,
                                                    String8 *out_copy);
vkr_internal bool8_t scene_loader_parse_json_imports(
    VkrAllocator *allocator, VkrMutex mutex, String8 json,
    SceneEntityImport **out_imports, uint32_t *out_entity_count,
    uint32_t *out_import_capacity, VkrSceneError *out_error);
vkr_internal void scene_loader_destroy_async_payload_contents(
    VkrSceneLoaderAsyncPayload *payload);
vkr_internal bool8_t scene_loader_ensure_scene_handle(
    VkrSceneLoaderAsyncPayload *payload, VkrRendererError *out_error);
vkr_internal bool8_t scene_loader_apply_component_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error);
vkr_internal bool8_t scene_loader_attach_mesh_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error);
vkr_internal bool8_t vkr_scene_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error);
vkr_internal bool8_t vkr_scene_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error);
vkr_internal bool8_t vkr_scene_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost);
vkr_internal void
vkr_scene_loader_release_async_payload(VkrResourceLoader *self, void *payload);

vkr_internal bool8_t scene_string8_ends_with_cstr_i(String8 s,
                                                    const char *suffix) {
  if (!suffix)
    return false_v;
  uint64_t suffix_len = string_length(suffix);
  if (s.length < suffix_len)
    return false_v;
  String8 tail = string8_substring(&s, s.length - suffix_len, s.length);
  String8 suf = string8_create_from_cstr((const uint8_t *)suffix, suffix_len);
  return string8_equalsi(&tail, &suf);
}

vkr_internal bool8_t scene_json_parse_null(VkrJsonReader *reader) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos + 4 <= reader->length &&
      MemCompare(reader->data + reader->pos, "null", 4) == 0) {
    reader->pos += 4;
    return true_v;
  }
  return false_v;
}

vkr_internal bool8_t scene_json_parse_float_array(VkrJsonReader *reader,
                                                  float32_t *out_values,
                                                  uint32_t value_count) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != '[') {
    return false_v;
  }

  reader->pos++;
  for (uint32_t i = 0; i < value_count; i++) {
    vkr_json_skip_whitespace(reader);
    if (!vkr_json_parse_float(reader, &out_values[i])) {
      return false_v;
    }

    vkr_json_skip_whitespace(reader);
    if (i + 1 < value_count) {
      if (reader->pos >= reader->length || reader->data[reader->pos] != ',') {
        return false_v;
      }
      reader->pos++;
    }
  }

  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != ']') {
    return false_v;
  }

  reader->pos++;
  return true_v;
}

vkr_internal bool8_t scene_json_parse_vec3(VkrJsonReader *reader,
                                           Vec3 *out_value) {
  float32_t values[3] = {0};
  if (!scene_json_parse_float_array(reader, values, 3)) {
    return false_v;
  }
  *out_value = vec3_new(values[0], values[1], values[2]);
  return true_v;
}

vkr_internal bool8_t scene_json_parse_quat(VkrJsonReader *reader,
                                           VkrQuat *out_value) {
  float32_t values[4] = {0};
  if (!scene_json_parse_float_array(reader, values, 4)) {
    return false_v;
  }
  *out_value =
      vkr_quat_normalize(vec4_new(values[0], values[1], values[2], values[3]));
  return true_v;
}

vkr_internal bool8_t scene_json_parse_vec4(VkrJsonReader *reader,
                                           Vec4 *out_value) {
  float32_t values[4] = {0};
  if (!scene_json_parse_float_array(reader, values, 4)) {
    return false_v;
  }
  *out_value = vec4_new(values[0], values[1], values[2], values[3]);
  return true_v;
}

vkr_internal bool8_t scene_json_read_string_field(const VkrJsonReader *object,
                                                  const char *field,
                                                  String8 *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return vkr_json_parse_string(&reader, out_value);
}

vkr_internal bool8_t scene_json_read_float_field(const VkrJsonReader *object,
                                                 const char *field,
                                                 float32_t *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return vkr_json_parse_float(&reader, out_value);
}

vkr_internal bool8_t scene_json_read_int_field(const VkrJsonReader *object,
                                               const char *field,
                                               int32_t *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return vkr_json_parse_int(&reader, out_value);
}

vkr_internal bool8_t scene_json_read_bool_field(const VkrJsonReader *object,
                                                const char *field,
                                                bool8_t *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return vkr_json_parse_bool(&reader, out_value);
}

vkr_internal bool8_t scene_json_read_vec3_field(const VkrJsonReader *object,
                                                const char *field,
                                                Vec3 *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return scene_json_parse_vec3(&reader, out_value);
}

vkr_internal bool8_t scene_json_read_vec4_field(const VkrJsonReader *object,
                                                const char *field,
                                                Vec4 *out_value) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return false_v;
  }
  return scene_json_parse_vec4(&reader, out_value);
}

vkr_internal SceneShapeType scene_json_parse_shape_type(String8 value,
                                                        bool8_t *valid) {
  if (vkr_string8_equals_cstr_i(&value, "cube")) {
    if (valid)
      *valid = true_v;
    return SCENE_SHAPE_TYPE_CUBE;
  }

  if (valid)
    *valid = false_v;
  return SCENE_SHAPE_TYPE_CUBE;
}

vkr_internal bool8_t scene_json_parse_parent_index(VkrJsonReader *reader,
                                                   int32_t *out_index) {
  if (scene_json_parse_null(reader)) {
    *out_index = -1;
    return true_v;
  }
  return vkr_json_parse_int(reader, out_index);
}

vkr_internal VkrPipelineDomain
scene_json_parse_pipeline_domain(String8 value, bool8_t *valid) {
  if (vkr_string8_equals_cstr_i(&value, "world")) {
    if (valid)
      *valid = true_v;
    return VKR_PIPELINE_DOMAIN_WORLD;
  }
  if (vkr_string8_equals_cstr_i(&value, "ui")) {
    if (valid)
      *valid = true_v;
    return VKR_PIPELINE_DOMAIN_UI;
  }
  if (vkr_string8_equals_cstr_i(&value, "shadow")) {
    if (valid)
      *valid = true_v;
    return VKR_PIPELINE_DOMAIN_SHADOW;
  }
  if (vkr_string8_equals_cstr_i(&value, "post")) {
    if (valid)
      *valid = true_v;
    return VKR_PIPELINE_DOMAIN_POST;
  }

  if (valid)
    *valid = false_v;
  return VKR_PIPELINE_DOMAIN_WORLD;
}

vkr_internal SceneText3DImport scene_text3d_import_defaults(void) {
  return (SceneText3DImport){
      .font_size = 32.0f,
      .color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f),
      .texture_width = 512,
      .texture_height = 128,
      .uv_inset_px = 0.5f,
  };
}

vkr_internal SceneShapeImport scene_shape_import_defaults(void) {
  return (SceneShapeImport){
      .type = SCENE_SHAPE_TYPE_CUBE,
      .dimensions = vec3_new(1.0f, 1.0f, 1.0f),
      .color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f),
  };
}

vkr_internal ScenePointLightImport scene_point_light_import_defaults(void) {
  return (ScenePointLightImport){
      .color = vec3_new(1.0f, 1.0f, 1.0f),
      .intensity = 1.0f,
      .constant = 1.0f,
      .linear = 0.35f,
      .quadratic = 0.44f,
      .enabled = true_v,
  };
}

vkr_internal SceneDirectionalLightImport
scene_directional_light_import_defaults(void) {
  return (SceneDirectionalLightImport){
      .color = vec3_new(1.0f, 1.0f, 1.0f),
      .intensity = 1.0f,
      .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
      .enabled = true_v,
  };
}

vkr_internal bool8_t scene_json_count_entities(const VkrJsonReader *root,
                                               uint32_t *out_count) {
  VkrJsonReader reader = *root;
  if (!vkr_json_find_array(&reader, "entities")) {
    return false_v;
  }

  uint32_t count = 0;
  while (vkr_json_next_array_element(&reader)) {
    VkrJsonReader obj_reader = {0};
    if (!vkr_json_enter_object(&reader, &obj_reader)) {
      return false_v;
    }
    count++;
  }

  *out_count = count;
  return true_v;
}

vkr_internal void scene_json_parse_name(const VkrJsonReader *entity_reader,
                                        SceneEntityImport *out_entity) {
  String8 name = {0};
  if (scene_json_read_string_field(entity_reader, "name", &name)) {
    out_entity->name = name;
  }
}

vkr_internal void scene_json_parse_parent(const VkrJsonReader *entity_reader,
                                          uint32_t entity_index,
                                          SceneEntityImport *out_entity) {
  VkrJsonReader parent_reader = *entity_reader;
  if (!vkr_json_find_field(&parent_reader, "parent")) {
    return;
  }

  int32_t parent_index = -1;
  if (scene_json_parse_parent_index(&parent_reader, &parent_index)) {
    out_entity->parent_index = parent_index;
  } else {
    log_warn("Scene loader: entity %u has invalid parent value", entity_index);
  }
}

vkr_internal void scene_json_parse_transform(const VkrJsonReader *entity_reader,
                                             uint32_t entity_index,
                                             SceneEntityImport *out_entity) {
  VkrJsonReader transform_reader = *entity_reader;
  if (!vkr_json_find_field(&transform_reader, "transform")) {
    return;
  }

  VkrJsonReader transform_obj = {0};
  if (!vkr_json_enter_object(&transform_reader, &transform_obj)) {
    log_warn("Scene loader: entity %u transform is not an object",
             entity_index);
    return;
  }

  VkrJsonReader pos_reader = transform_obj;
  if (vkr_json_find_field(&pos_reader, "pos")) {
    Vec3 position;
    if (scene_json_parse_vec3(&pos_reader, &position)) {
      out_entity->position = position;
    } else {
      log_warn("Scene loader: entity %u has invalid position array",
               entity_index);
    }
  }

  VkrJsonReader rot_reader = transform_obj;
  if (vkr_json_find_field(&rot_reader, "rot")) {
    VkrQuat rotation;
    if (scene_json_parse_quat(&rot_reader, &rotation)) {
      out_entity->rotation = rotation;
    } else {
      log_warn("Scene loader: entity %u has invalid rotation array",
               entity_index);
    }
  }

  VkrJsonReader scale_reader = transform_obj;
  if (vkr_json_find_field(&scale_reader, "scale")) {
    Vec3 scale;
    if (scene_json_parse_vec3(&scale_reader, &scale)) {
      out_entity->scale = scale;
    } else {
      log_warn("Scene loader: entity %u has invalid scale array", entity_index);
    }
  }
}

vkr_internal void scene_json_parse_mesh(const VkrJsonReader *entity_reader,
                                        uint32_t entity_index,
                                        SceneEntityImport *out_entity) {
  VkrJsonReader mesh_reader = *entity_reader;
  if (!vkr_json_find_field(&mesh_reader, "mesh")) {
    return;
  }

  if (scene_json_parse_null(&mesh_reader)) {
    return;
  }

  VkrJsonReader mesh_obj = {0};
  if (!vkr_json_enter_object(&mesh_reader, &mesh_obj)) {
    log_warn("Scene loader: entity %u mesh is not an object", entity_index);
    return;
  }

  String8 mesh_path = {0};
  if (scene_json_read_string_field(&mesh_obj, "path", &mesh_path)) {
    if (mesh_path.length > 0) {
      out_entity->mesh_path = mesh_path;
      out_entity->has_mesh = true_v;
    } else {
      log_warn("Scene loader: entity %u mesh path is empty", entity_index);
    }
  }

  String8 domain_str = {0};
  if (scene_json_read_string_field(&mesh_obj, "pipeline_domain", &domain_str)) {
    bool8_t valid = false_v;
    out_entity->pipeline_domain =
        scene_json_parse_pipeline_domain(domain_str, &valid);
    if (!valid) {
      log_warn("Scene loader: entity %u has unknown pipeline domain",
               entity_index);
    }
  }

  String8 shader_override = {0};
  if (scene_json_read_string_field(&mesh_obj, "shader_override",
                                   &shader_override) &&
      shader_override.length > 0) {
    out_entity->shader_override = shader_override;
  }
}

vkr_internal void scene_json_parse_text3d(const VkrJsonReader *entity_reader,
                                          uint32_t entity_index,
                                          SceneEntityImport *out_entity) {
  VkrJsonReader text3d_reader = *entity_reader;
  if (!vkr_json_find_field(&text3d_reader, "text3d")) {
    return;
  }

  if (scene_json_parse_null(&text3d_reader)) {
    return;
  }

  VkrJsonReader text3d_obj = {0};
  if (!vkr_json_enter_object(&text3d_reader, &text3d_obj)) {
    log_warn("Scene loader: entity %u text3d is not an object", entity_index);
    return;
  }

  out_entity->has_text3d = true_v;
  out_entity->text3d = scene_text3d_import_defaults();

  String8 text = {0};
  if (scene_json_read_string_field(&text3d_obj, "text", &text)) {
    out_entity->text3d.text = text;
  }

  float32_t font_size = 0.0f;
  if (scene_json_read_float_field(&text3d_obj, "font_size", &font_size)) {
    out_entity->text3d.font_size = font_size;
  }

  Vec4 color;
  if (scene_json_read_vec4_field(&text3d_obj, "color", &color)) {
    out_entity->text3d.color = color;
  }

  String8 font_name = {0};
  if (scene_json_read_string_field(&text3d_obj, "font", &font_name)) {
    out_entity->text3d.font_name = font_name;
  }

  int32_t tex_w = 0;
  if (scene_json_read_int_field(&text3d_obj, "texture_width", &tex_w) &&
      tex_w > 0) {
    out_entity->text3d.texture_width = (uint32_t)tex_w;
  }

  int32_t tex_h = 0;
  if (scene_json_read_int_field(&text3d_obj, "texture_height", &tex_h) &&
      tex_h > 0) {
    out_entity->text3d.texture_height = (uint32_t)tex_h;
  }

  float32_t inset = 0.0f;
  if (scene_json_read_float_field(&text3d_obj, "inset", &inset)) {
    out_entity->text3d.uv_inset_px = inset;
  }
}

vkr_internal void scene_json_parse_shape(const VkrJsonReader *entity_reader,
                                         uint32_t entity_index,
                                         SceneEntityImport *out_entity) {
  VkrJsonReader shape_reader = *entity_reader;
  if (!vkr_json_find_field(&shape_reader, "shape")) {
    return;
  }

  if (scene_json_parse_null(&shape_reader)) {
    return;
  }

  VkrJsonReader shape_obj = {0};
  if (!vkr_json_enter_object(&shape_reader, &shape_obj)) {
    log_warn("Scene loader: entity %u shape is not an object", entity_index);
    return;
  }

  out_entity->has_shape = true_v;
  out_entity->shape = scene_shape_import_defaults();

  String8 type_str = {0};
  if (scene_json_read_string_field(&shape_obj, "type", &type_str)) {
    bool8_t valid = false_v;
    out_entity->shape.type = scene_json_parse_shape_type(type_str, &valid);
    if (!valid) {
      log_warn("Scene loader: entity %u has unknown shape type", entity_index);
    }
  }

  Vec3 dims;
  if (scene_json_read_vec3_field(&shape_obj, "dimensions", &dims)) {
    out_entity->shape.dimensions = dims;
  }

  Vec4 color;
  if (scene_json_read_vec4_field(&shape_obj, "color", &color)) {
    out_entity->shape.color = color;
  }

  VkrJsonReader material_reader = shape_obj;
  if (vkr_json_find_field(&material_reader, "material")) {
    if (!scene_json_parse_null(&material_reader)) {
      VkrJsonReader material_obj = {0};
      if (vkr_json_enter_object(&material_reader, &material_obj)) {
        String8 mat_name = {0};
        if (scene_json_read_string_field(&material_obj, "name", &mat_name)) {
          out_entity->shape.material_name = mat_name;
        }

        String8 mat_path = {0};
        if (scene_json_read_string_field(&material_obj, "path", &mat_path)) {
          out_entity->shape.material_path = mat_path;
        }
      }
    }
  }
}

vkr_internal void
scene_json_parse_point_light(const VkrJsonReader *entity_reader,
                             uint32_t entity_index,
                             SceneEntityImport *out_entity) {
  VkrJsonReader point_light_reader = *entity_reader;
  if (!vkr_json_find_field(&point_light_reader, "point_light")) {
    return;
  }

  if (scene_json_parse_null(&point_light_reader)) {
    return;
  }

  VkrJsonReader point_light_obj = {0};
  if (!vkr_json_enter_object(&point_light_reader, &point_light_obj)) {
    log_warn("Scene loader: entity %u point_light is not an object",
             entity_index);
    return;
  }

  out_entity->has_point_light = true_v;
  out_entity->point_light = scene_point_light_import_defaults();

  bool8_t enabled = true_v;
  if (scene_json_read_bool_field(&point_light_obj, "enabled", &enabled)) {
    out_entity->point_light.enabled = enabled;
  }

  Vec3 color;
  if (scene_json_read_vec3_field(&point_light_obj, "color", &color)) {
    out_entity->point_light.color = color;
  }

  float32_t intensity = 0.0f;
  if (scene_json_read_float_field(&point_light_obj, "intensity", &intensity)) {
    out_entity->point_light.intensity = intensity;
  }

  VkrJsonReader attenuation_reader = point_light_obj;
  if (vkr_json_find_field(&attenuation_reader, "attenuation")) {
    if (!scene_json_parse_null(&attenuation_reader)) {
      VkrJsonReader attenuation_obj = {0};
      if (vkr_json_enter_object(&attenuation_reader, &attenuation_obj)) {
        float32_t constant = 0.0f;
        if (scene_json_read_float_field(&attenuation_obj, "constant",
                                        &constant)) {
          out_entity->point_light.constant = constant;
        }

        float32_t linear = 0.0f;
        if (scene_json_read_float_field(&attenuation_obj, "linear", &linear)) {
          out_entity->point_light.linear = linear;
        }

        float32_t quadratic = 0.0f;
        if (scene_json_read_float_field(&attenuation_obj, "quadratic",
                                        &quadratic)) {
          out_entity->point_light.quadratic = quadratic;
        }
      }
    }
  }
}

vkr_internal void
scene_json_parse_directional_light(const VkrJsonReader *entity_reader,
                                   uint32_t entity_index,
                                   SceneEntityImport *out_entity) {
  VkrJsonReader dir_light_reader = *entity_reader;
  if (!vkr_json_find_field(&dir_light_reader, "directional_light")) {
    return;
  }

  if (scene_json_parse_null(&dir_light_reader)) {
    return;
  }

  VkrJsonReader dir_light_obj = {0};
  if (!vkr_json_enter_object(&dir_light_reader, &dir_light_obj)) {
    log_warn("Scene loader: entity %u directional_light is not an object",
             entity_index);
    return;
  }

  out_entity->has_directional_light = true_v;
  out_entity->directional_light = scene_directional_light_import_defaults();

  bool8_t enabled = true_v;
  if (scene_json_read_bool_field(&dir_light_obj, "enabled", &enabled)) {
    out_entity->directional_light.enabled = enabled;
  }

  Vec3 color;
  if (scene_json_read_vec3_field(&dir_light_obj, "color", &color)) {
    out_entity->directional_light.color = color;
  }

  float32_t intensity = 0.0f;
  if (scene_json_read_float_field(&dir_light_obj, "intensity", &intensity)) {
    out_entity->directional_light.intensity = intensity;
  }

  Vec3 direction;
  if (scene_json_read_vec3_field(&dir_light_obj, "direction_local",
                                 &direction)) {
    out_entity->directional_light.direction_local = direction;
  }
}

vkr_internal void scene_json_parse_entity(const VkrJsonReader *entity_reader,
                                          uint32_t entity_index,
                                          SceneEntityImport *out_entity) {
  *out_entity = (SceneEntityImport){
      .parent_index = -1,
      .position = vec3_new(0.0f, 0.0f, 0.0f),
      .rotation = vkr_quat_identity(),
      .scale = vec3_new(1.0f, 1.0f, 1.0f),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  };

  scene_json_parse_name(entity_reader, out_entity);
  scene_json_parse_parent(entity_reader, entity_index, out_entity);
  scene_json_parse_transform(entity_reader, entity_index, out_entity);
  scene_json_parse_mesh(entity_reader, entity_index, out_entity);
  scene_json_parse_text3d(entity_reader, entity_index, out_entity);
  scene_json_parse_shape(entity_reader, entity_index, out_entity);
  scene_json_parse_point_light(entity_reader, entity_index, out_entity);
  scene_json_parse_directional_light(entity_reader, entity_index, out_entity);
}

bool8_t vkr_scene_load_from_file(VkrScene *scene, struct s_RendererFrontend *rf,
                                 String8 path, VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error) {
  if (out_result) {
    *out_result = (VkrSceneLoadResult){0};
  }
  if (!scene || !rf || !temp_alloc || !path.str) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  FilePath file_path = file_path_create((const char *)path.str, temp_alloc,
                                        FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError fe = file_open(&file_path, mode, &handle);
  if (fe != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_FILE_NOT_FOUND;
    log_error("Scene loader: failed to open '%s': %s", (const char *)path.str,
              file_get_error_string(fe).str);
    return false_v;
  }

  String8 json = {0};
  fe = file_read_string(&handle, temp_alloc, &json);
  file_close(&handle);
  if (fe != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_FILE_READ_FAILED;
    log_error("Scene loader: failed to read '%s': %s", (const char *)path.str,
              file_get_error_string(fe).str);
    return false_v;
  }

  return vkr_scene_load_from_json(scene, rf, json, temp_alloc, out_result,
                                  out_error);
}

bool8_t vkr_scene_load_from_json(VkrScene *scene, struct s_RendererFrontend *rf,
                                 String8 json, VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error) {
  if (out_result) {
    *out_result = (VkrSceneLoadResult){0};
  }
  if (!scene || !scene->world || !rf || !temp_alloc || !json.str) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  // Store renderer frontend reference for layer message sending
  scene->rf = rf;

  VkrJsonReader root = vkr_json_reader_from_string(json);
  int32_t version = 1;
  VkrJsonReader version_reader = root;
  if (vkr_json_get_int(&version_reader, "version", &version) &&
      (version < 1 || version > 2)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_UNSUPPORTED_VERSION;
    log_error("Scene loader: unsupported scene version %d", version);
    return false_v;
  }

  uint32_t entity_count = 0;
  if (!scene_json_count_entities(&root, &entity_count)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    log_error("Scene loader: missing or invalid entities array");
    return false_v;
  }

  if (entity_count == 0) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_NONE;
    return true_v;
  }

  SceneEntityImport *imports =
      vkr_allocator_alloc(temp_alloc, entity_count * sizeof(SceneEntityImport),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrEntityId *entity_ids =
      vkr_allocator_alloc(temp_alloc, entity_count * sizeof(VkrEntityId),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!imports || !entity_ids) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  VkrJsonReader entities_reader = root;
  if (!vkr_json_find_array(&entities_reader, "entities")) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }

  uint32_t parsed = 0;
  while (vkr_json_next_array_element(&entities_reader)) {
    if (parsed >= entity_count) {
      break;
    }

    VkrJsonReader entity_obj = {0};
    if (!vkr_json_enter_object(&entities_reader, &entity_obj)) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      log_error("Scene loader: entity %u is not an object", parsed);
      return false_v;
    }

    scene_json_parse_entity(&entity_obj, parsed, &imports[parsed]);
    parsed++;
  }

  if (parsed != entity_count) {
    entity_count = parsed;
  }

  for (uint32_t i = 0; i < entity_count; i++) {
    VkrSceneError create_err = VKR_SCENE_ERROR_NONE;
    VkrEntityId entity = vkr_scene_create_entity(scene, &create_err);
    if (entity.u64 == VKR_ENTITY_ID_INVALID.u64) {
      if (out_error)
        *out_error = create_err;
      log_error("Scene loader: failed to create entity %u", i);
      return false_v;
    }

    entity_ids[i] = entity;

    if (imports[i].name.length > 0 &&
        !vkr_scene_set_name(scene, entity, imports[i].name)) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
      log_error("Scene loader: failed to set name for entity %u", i);
      return false_v;
    }

    if (!vkr_scene_set_transform(scene, entity, imports[i].position,
                                 imports[i].rotation, imports[i].scale)) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
      log_error("Scene loader: failed to set transform for entity %u", i);
      return false_v;
    }
  }

  for (uint32_t i = 0; i < entity_count; i++) {
    int32_t parent_index = imports[i].parent_index;
    if (parent_index < 0) {
      continue;
    }
    if ((uint32_t)parent_index >= entity_count) {
      log_warn("Scene loader: entity %u parent index %d is out of range", i,
               parent_index);
      continue;
    }
    vkr_scene_set_parent(scene, entity_ids[i], entity_ids[parent_index]);
  }

  uint32_t mesh_desc_count = 0;
  for (uint32_t i = 0; i < entity_count; i++) {
    if (imports[i].has_mesh) {
      mesh_desc_count++;
    }
  }

  uint32_t loaded_meshes = 0;
  if (mesh_desc_count > 0) {
    VkrMeshLoadDesc *mesh_descs = vkr_allocator_alloc(
        temp_alloc, mesh_desc_count * sizeof(VkrMeshLoadDesc),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *mesh_entity_indices =
        vkr_allocator_alloc(temp_alloc, mesh_desc_count * sizeof(uint32_t),
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrMeshInstanceHandle *instance_handles = vkr_allocator_alloc(
        temp_alloc, mesh_desc_count * sizeof(VkrMeshInstanceHandle),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrRendererError *mesh_errors = vkr_allocator_alloc(
        temp_alloc, mesh_desc_count * sizeof(VkrRendererError),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    if (!mesh_descs || !mesh_entity_indices || !instance_handles ||
        !mesh_errors) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
      log_error("Scene loader: failed to allocate mesh load buffers");
      return false_v;
    }

    uint32_t desc_index = 0;
    for (uint32_t i = 0; i < entity_count; i++) {
      if (!imports[i].has_mesh) {
        continue;
      }

      mesh_descs[desc_index] = (VkrMeshLoadDesc){
          .mesh_path = imports[i].mesh_path,
          .transform = vkr_transform_from_position_scale_rotation(
              imports[i].position, imports[i].scale, imports[i].rotation),
          .pipeline_domain = imports[i].pipeline_domain,
          .shader_override = imports[i].shader_override,
      };
      mesh_entity_indices[desc_index] = i;
      desc_index++;
    }

    vkr_mesh_manager_create_instances_batch(&rf->mesh_manager, mesh_descs,
                                            mesh_desc_count, instance_handles,
                                            mesh_errors);

    for (uint32_t i = 0; i < mesh_desc_count; i++) {
      VkrMeshInstanceHandle instance = instance_handles[i];
      VkrRendererError mesh_err = mesh_errors[i];
      if (instance.id == 0 || mesh_err != VKR_RENDERER_ERROR_NONE) {
        String8 err_str = vkr_renderer_get_error_string(mesh_err);
        log_error("Scene loader: failed to load mesh '%.*s': %.*s",
                  (int)mesh_descs[i].mesh_path.length,
                  mesh_descs[i].mesh_path.str, (int)err_str.length,
                  err_str.str);
        continue;
      }

      uint32_t entity_index = mesh_entity_indices[i];
      VkrEntityId entity = entity_ids[entity_index];

      if (!vkr_scene_set_mesh_renderer(scene, entity, instance)) {
        if (out_error)
          *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
        log_error("Scene loader: failed to add mesh renderer to entity %u",
                  entity_index);
        return false_v;
      }

      if (!vkr_scene_track_instance(scene, instance, out_error)) {
        log_error("Scene loader: failed to track instance %u", instance.id);
        return false_v;
      }

      loaded_meshes++;
    }
  }

  // Load text3d components
  uint32_t loaded_text3d = 0;
  for (uint32_t i = 0; i < entity_count; i++) {
    if (!imports[i].has_text3d)
      continue;

    VkrEntityId entity = entity_ids[i];
    SceneText3DImport *text_import = &imports[i].text3d;

    // Try to acquire font by name if specified
    VkrFontHandle font = VKR_FONT_HANDLE_INVALID;
    if (text_import->font_name.length > 0) {
      // Create null-terminated copy for font system lookup
      String8 font_name_copy =
          string8_duplicate(temp_alloc, &text_import->font_name);
      VkrRendererError font_err = VKR_RENDERER_ERROR_NONE;
      font = vkr_font_system_acquire(&rf->font_system, font_name_copy, true_v,
                                     &font_err);
      if (font.id == 0) {
        log_warn("Scene loader: entity %u text3d font '%.*s' not found, using "
                 "default",
                 i, (int)text_import->font_name.length,
                 text_import->font_name.str);
        font = VKR_FONT_HANDLE_INVALID;
      }
    }

    // Create null-terminated copy of text content
    String8 text_copy = string8_duplicate(temp_alloc, &text_import->text);

    VkrSceneText3DConfig text_config = VKR_SCENE_TEXT3D_CONFIG_DEFAULT;
    text_config.text = text_copy;
    text_config.font = font;
    text_config.font_size = text_import->font_size;
    text_config.color = text_import->color;
    text_config.texture_width = text_import->texture_width;
    text_config.texture_height = text_import->texture_height;
    text_config.uv_inset_px = text_import->uv_inset_px;

    VkrSceneError text_err = VKR_SCENE_ERROR_NONE;
    if (!vkr_scene_set_text3d(scene, entity, &text_config, &text_err)) {
      log_error("Scene loader: failed to set text3d for entity %u (err=%d)", i,
                (int)text_err);
      continue;
    }

    loaded_text3d++;
  }

  // Load shape components
  uint32_t loaded_shapes = 0;
  for (uint32_t i = 0; i < entity_count; i++) {
    if (!imports[i].has_shape)
      continue;

    VkrEntityId entity = entity_ids[i];
    SceneShapeImport *shape_import = &imports[i].shape;

    VkrSceneShapeConfig shape_config = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
    shape_config.type = shape_import->type;
    shape_config.dimensions = shape_import->dimensions;
    shape_config.color = shape_import->color;
    shape_config.material_name = shape_import->material_name;
    shape_config.material_path = shape_import->material_path;

    VkrSceneError shape_err = VKR_SCENE_ERROR_NONE;
    if (!vkr_scene_set_shape(scene, rf, entity, &shape_config, &shape_err)) {
      log_error("Scene loader: failed to set shape for entity %u (err=%d)", i,
                (int)shape_err);
      continue;
    }

    loaded_shapes++;
  }

  // Load point light components
  uint32_t loaded_point_lights = 0;
  for (uint32_t i = 0; i < entity_count; i++) {
    if (!imports[i].has_point_light)
      continue;

    VkrEntityId entity = entity_ids[i];
    ScenePointLightImport *light_import = &imports[i].point_light;
    ScenePointLight light = {
        .color = light_import->color,
        .intensity = light_import->intensity,
        .constant = light_import->constant,
        .linear = light_import->linear,
        .quadratic = light_import->quadratic,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_point_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set point light for entity %u", i);
      continue;
    }

    loaded_point_lights++;
  }

  // Load directional light components
  uint32_t loaded_directional_lights = 0;
  bool8_t enabled_directional_seen = false_v;
  for (uint32_t i = 0; i < entity_count; i++) {
    if (!imports[i].has_directional_light)
      continue;

    VkrEntityId entity = entity_ids[i];
    SceneDirectionalLightImport *light_import = &imports[i].directional_light;
    SceneDirectionalLight light = {
        .color = light_import->color,
        .intensity = light_import->intensity,
        .direction_local = light_import->direction_local,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_directional_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set directional light for entity %u",
                i);
      continue;
    }

    if (light.enabled) {
      if (enabled_directional_seen) {
        log_warn("Scene loader: multiple enabled directional lights present");
      } else {
        enabled_directional_seen = true_v;
      }
    }

    loaded_directional_lights++;
  }

  if (out_result) {
    out_result->entity_count = entity_count;
    out_result->mesh_count = loaded_meshes;
    out_result->text3d_count = loaded_text3d;
    out_result->shape_count = loaded_shapes;
    out_result->directional_light_count = loaded_directional_lights;
    out_result->point_light_count = loaded_point_lights;
  }

  if (out_error)
    *out_error = VKR_SCENE_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t scene_loader_alloc_copy_string(VkrAllocator *allocator,
                                                    VkrMutex mutex,
                                                    String8 source,
                                                    char **out_storage,
                                                    String8 *out_copy) {
  if (!out_storage || !out_copy || !source.str || source.length == 0) {
    return false_v;
  }

  char *copy = (char *)vkr_allocator_alloc_ts(
      allocator, source.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING, mutex);
  if (!copy) {
    return false_v;
  }

  MemCopy(copy, source.str, (size_t)source.length);
  copy[source.length] = '\0';
  *out_storage = copy;
  *out_copy = string8_create((uint8_t *)copy, source.length);
  return true_v;
}

vkr_internal bool8_t scene_loader_parse_json_imports(
    VkrAllocator *allocator, VkrMutex mutex, String8 json,
    SceneEntityImport **out_imports, uint32_t *out_entity_count,
    uint32_t *out_import_capacity, VkrSceneError *out_error) {
  if (out_imports) {
    *out_imports = NULL;
  }
  if (out_entity_count) {
    *out_entity_count = 0;
  }
  if (out_import_capacity) {
    *out_import_capacity = 0;
  }
  if (!json.str || json.length == 0) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    return false_v;
  }

  VkrJsonReader root = vkr_json_reader_from_string(json);
  int32_t version = 1;
  VkrJsonReader version_reader = root;
  if (vkr_json_get_int(&version_reader, "version", &version) &&
      (version < 1 || version > 2)) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_UNSUPPORTED_VERSION;
    }
    log_error("Scene loader: unsupported scene version %d", version);
    return false_v;
  }

  uint32_t entity_count = 0;
  if (!scene_json_count_entities(&root, &entity_count)) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    log_error("Scene loader: missing or invalid entities array");
    return false_v;
  }

  if (entity_count == 0) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_NONE;
    }
    return true_v;
  }

  uint64_t import_bytes = sizeof(SceneEntityImport) * entity_count;
  SceneEntityImport *imports = (SceneEntityImport *)vkr_allocator_alloc_ts(
      allocator, import_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
  if (!imports) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    }
    return false_v;
  }
  MemZero(imports, sizeof(SceneEntityImport) * entity_count);

  VkrJsonReader entities_reader = root;
  if (!vkr_json_find_array(&entities_reader, "entities")) {
    vkr_allocator_free_ts(allocator, imports, import_bytes,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    return false_v;
  }

  uint32_t parsed = 0;
  while (vkr_json_next_array_element(&entities_reader)) {
    if (parsed >= entity_count) {
      break;
    }

    VkrJsonReader entity_obj = {0};
    if (!vkr_json_enter_object(&entities_reader, &entity_obj)) {
      vkr_allocator_free_ts(allocator, imports, import_bytes,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
      if (out_error) {
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      }
      log_error("Scene loader: entity %u is not an object", parsed);
      return false_v;
    }

    scene_json_parse_entity(&entity_obj, parsed, &imports[parsed]);
    parsed++;
  }

  entity_count = parsed;
  if (out_imports) {
    *out_imports = imports;
  } else {
    vkr_allocator_free_ts(allocator, imports, import_bytes,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
  }
  if (out_entity_count) {
    *out_entity_count = entity_count;
  }
  if (out_import_capacity) {
    *out_import_capacity = (uint32_t)(import_bytes / sizeof(SceneEntityImport));
  }
  if (out_error) {
    *out_error = VKR_SCENE_ERROR_NONE;
  }
  return true_v;
}

vkr_internal void scene_loader_init_request_info(VkrResourceHandleInfo *info,
                                                 VkrResourceType type) {
  if (!info) {
    return;
  }

  MemZero(info, sizeof(*info));
  info->type = type;
  info->loader_id = VKR_INVALID_ID;
  info->load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
  info->last_error = VKR_RENDERER_ERROR_NONE;
  info->request_id = 0;
}

vkr_internal bool8_t scene_loader_ensure_scene_handle(
    VkrSceneLoaderAsyncPayload *payload, VkrRendererError *out_error) {
  if (!payload || !payload->rf || !out_error) {
    return false_v;
  }

  if (payload->scene_handle) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  VkrSceneHandle handle = vkr_scene_handle_create(&payload->rf->allocator, 0,
                                                  64, 256, &scene_error);
  if (!handle) {
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  if (!scene) {
    vkr_scene_handle_destroy(handle, payload->rf);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  scene->rf = payload->rf;
  payload->scene_handle = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal void
scene_loader_sync_partial(VkrSceneLoaderAsyncPayload *payload) {
  if (!payload || !payload->scene_handle || !payload->rf) {
    return;
  }

  vkr_scene_handle_update_and_sync(payload->scene_handle, payload->rf, 0.0);
}

vkr_internal bool8_t scene_loader_apply_component_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error) {
  if (!payload || !payload->rf || !payload->scene_handle || !out_error) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }

  if (entity_index >= payload->entity_count) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(payload->scene_handle);
  if (!scene) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  SceneEntityImport *entity_import = &payload->imports[entity_index];
  VkrEntityId entity = payload->entity_ids[entity_index];

  VkrAllocatorScope scope =
      vkr_allocator_begin_scope(&payload->rf->scratch_allocator);
  bool8_t scope_valid = vkr_allocator_scope_is_valid(&scope);
  if (!scope_valid) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (entity_import->has_shape && payload->shape_material_states &&
      entity_import->shape.material_path.str &&
      entity_import->shape.material_path.length > 0) {
    SceneShapeMaterialAsyncState *shape_material_state =
        &payload->shape_material_states[entity_index];

    if (shape_material_state->requested) {
      VkrRendererError dep_state_error = VKR_RENDERER_ERROR_NONE;
      VkrResourceLoadState dep_state = vkr_resource_system_get_state(
          &shape_material_state->request_info, &dep_state_error);
      if (dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
          dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
          dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
        return false_v;
      }
    }
  }

  if (entity_import->has_text3d) {
    SceneText3DImport *text_import = &entity_import->text3d;

    VkrFontHandle font = VKR_FONT_HANDLE_INVALID;
    if (text_import->font_name.length > 0) {
      String8 font_name_copy = string8_duplicate(
          &payload->rf->scratch_allocator, &text_import->font_name);
      VkrRendererError font_err = VKR_RENDERER_ERROR_NONE;
      font = vkr_font_system_acquire(&payload->rf->font_system, font_name_copy,
                                     true_v, &font_err);
      if (font.id == 0) {
        log_warn("Scene loader: entity %u text3d font '%.*s' not found, using "
                 "default",
                 entity_index, (int)text_import->font_name.length,
                 text_import->font_name.str);
        font = VKR_FONT_HANDLE_INVALID;
      }
    }

    String8 text_copy =
        string8_duplicate(&payload->rf->scratch_allocator, &text_import->text);
    VkrSceneText3DConfig text_config = VKR_SCENE_TEXT3D_CONFIG_DEFAULT;
    text_config.text = text_copy;
    text_config.font = font;
    text_config.font_size = text_import->font_size;
    text_config.color = text_import->color;
    text_config.texture_width = text_import->texture_width;
    text_config.texture_height = text_import->texture_height;
    text_config.uv_inset_px = text_import->uv_inset_px;

    VkrSceneError text_error = VKR_SCENE_ERROR_NONE;
    if (!vkr_scene_set_text3d(scene, entity, &text_config, &text_error)) {
      log_error("Scene loader: failed to set text3d for entity %u (err=%d)",
                entity_index, (int)text_error);
    } else {
      payload->load_result.text3d_count++;
    }
  }

  if (entity_import->has_shape) {
    VkrSceneShapeConfig shape_config = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
    shape_config.type = entity_import->shape.type;
    shape_config.dimensions = entity_import->shape.dimensions;
    shape_config.color = entity_import->shape.color;
    shape_config.material_name = entity_import->shape.material_name;
    // Async scene finalize never forces a blocking material load fallback.
    shape_config.material_path = (String8){0};

    VkrSceneError shape_error = VKR_SCENE_ERROR_NONE;
    if (!vkr_scene_set_shape(scene, payload->rf, entity, &shape_config,
                             &shape_error)) {
      log_error("Scene loader: failed to set shape for entity %u (err=%d)",
                entity_index, (int)shape_error);
    } else {
      payload->load_result.shape_count++;
    }

    if (payload->shape_material_states) {
      SceneShapeMaterialAsyncState *shape_material_state =
          &payload->shape_material_states[entity_index];
      if (shape_material_state->request_info.request_id != 0 &&
          entity_import->shape.material_path.str &&
          entity_import->shape.material_path.length > 0) {
        vkr_resource_system_unload(&shape_material_state->request_info,
                                   entity_import->shape.material_path);
        scene_loader_init_request_info(&shape_material_state->request_info,
                                       VKR_RESOURCE_TYPE_MATERIAL);
      }
    }
  }

  if (entity_import->has_point_light) {
    ScenePointLightImport *light_import = &entity_import->point_light;
    ScenePointLight light = {
        .color = light_import->color,
        .intensity = light_import->intensity,
        .constant = light_import->constant,
        .linear = light_import->linear,
        .quadratic = light_import->quadratic,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_point_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set point light for entity %u",
                entity_index);
    } else {
      payload->load_result.point_light_count++;
    }
  }

  if (entity_import->has_directional_light) {
    SceneDirectionalLightImport *light_import =
        &entity_import->directional_light;
    SceneDirectionalLight light = {
        .color = light_import->color,
        .intensity = light_import->intensity,
        .direction_local = light_import->direction_local,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_directional_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set directional light for entity %u",
                entity_index);
    } else {
      payload->load_result.directional_light_count++;
    }
  }

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t scene_loader_attach_mesh_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error) {
  if (!payload || !payload->rf || !payload->scene_handle || !out_error) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }

  if (entity_index >= payload->entity_count) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  SceneEntityImport *entity_import = &payload->imports[entity_index];
  if (!entity_import->has_mesh) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  SceneMeshAsyncState *mesh_state = &payload->mesh_states[entity_index];
  if (mesh_state->completed) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  if (!mesh_state->requested) {
    mesh_state->completed = true_v;
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  if (mesh_state->attached) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrRendererError load_state_error = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState load_state = vkr_resource_system_get_state(
      &mesh_state->request_info, &load_state_error);
  if (load_state == VKR_RESOURCE_LOAD_STATE_FAILED ||
      load_state == VKR_RESOURCE_LOAD_STATE_CANCELED ||
      load_state == VKR_RESOURCE_LOAD_STATE_INVALID) {
    if (mesh_state->request_info.request_id != 0) {
      vkr_resource_system_unload(&mesh_state->request_info,
                                 entity_import->mesh_path);
      scene_loader_init_request_info(&mesh_state->request_info,
                                     VKR_RESOURCE_TYPE_MESH);
    }
    mesh_state->completed = true_v;
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrMeshLoadDesc mesh_desc = {
      .mesh_path = entity_import->mesh_path,
      .transform = vkr_transform_from_position_scale_rotation(
          entity_import->position, entity_import->scale,
          entity_import->rotation),
      .pipeline_domain = entity_import->pipeline_domain,
      .shader_override = entity_import->shader_override,
  };

  VkrRendererError mesh_error = VKR_RENDERER_ERROR_NONE;
  VkrMeshInstanceHandle instance =
      vkr_mesh_manager_create_instance_from_resource(
          &payload->rf->mesh_manager, &mesh_desc, &mesh_state->request_info, 0,
          true_v, &mesh_error);
  if (instance.id == 0 || mesh_error != VKR_RENDERER_ERROR_NONE) {
    if (mesh_error == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
      /*
       * Async mesh/material/texture dependencies are still converging. Keep the
       * entity in the attach stage and retry next pump tick.
       */
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }

    String8 error_string = vkr_renderer_get_error_string(mesh_error);
    log_error("Scene loader: failed to create mesh instance for '%.*s': %.*s",
              (int)entity_import->mesh_path.length,
              entity_import->mesh_path.str, (int)error_string.length,
              error_string.str);
    if (mesh_state->request_info.request_id != 0) {
      vkr_resource_system_unload(&mesh_state->request_info,
                                 entity_import->mesh_path);
      scene_loader_init_request_info(&mesh_state->request_info,
                                     VKR_RESOURCE_TYPE_MESH);
    }
    mesh_state->completed = true_v;
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(payload->scene_handle);
  VkrEntityId entity = payload->entity_ids[entity_index];

  if (!vkr_scene_set_mesh_renderer(scene, entity, instance)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!vkr_scene_track_instance(scene, instance, &scene_error)) {
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }

  payload->load_result.mesh_count++;
  mesh_state->attached = true_v;

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t scene_loader_wait_mesh_dependencies(
    VkrSceneLoaderAsyncPayload *payload, VkrRendererError *out_error) {
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  bool8_t has_pending = false_v;
  for (uint32_t i = 0; i < payload->entity_count; ++i) {
    SceneMeshAsyncState *mesh_state = &payload->mesh_states[i];
    if (!mesh_state->requested || mesh_state->completed ||
        mesh_state->request_info.request_id == 0) {
      continue;
    }

    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState dep_state = vkr_resource_system_get_state(
        &mesh_state->request_info, &dependency_error);
    if (dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      has_pending = true_v;
      continue;
    }

    if (mesh_state->request_info.request_id != 0 &&
        payload->imports[i].mesh_path.str &&
        payload->imports[i].mesh_path.length > 0) {
      vkr_resource_system_unload(&mesh_state->request_info,
                                 payload->imports[i].mesh_path);
    }
    scene_loader_init_request_info(&mesh_state->request_info,
                                   VKR_RESOURCE_TYPE_MESH);
    mesh_state->completed = true_v;
  }

  if (has_pending) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_scene_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_payload != NULL, "Out payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_payload = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;

  struct s_RendererFrontend *rf =
      (struct s_RendererFrontend *)self->resource_system;
  if (!rf) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  FilePath file_path = file_path_create((const char *)name.str, temp_alloc,
                                        FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError file_error = file_open(&file_path, mode, &handle);
  if (file_error != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("Scene loader: failed to open '%s': %s", (const char *)name.str,
              file_get_error_string(file_error).str);
    return false_v;
  }

  String8 json = {0};
  file_error = file_read_string(&handle, temp_alloc, &json);
  file_close(&handle);
  if (file_error != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_UNKNOWN;
    log_error("Scene loader: failed to read '%s': %s", (const char *)name.str,
              file_get_error_string(file_error).str);
    return false_v;
  }

  VkrSceneLoaderAsyncPayload *payload =
      (VkrSceneLoaderAsyncPayload *)vkr_allocator_alloc_ts(
          &rf->scene_async_allocator, sizeof(*payload),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, rf->scene_async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));
  payload->rf = rf;
  payload->stage = SCENE_ASYNC_STAGE_CREATE_ENTITIES;
  payload->stage_cursor = 0;
  payload->ownership_transferred = false_v;

  String8 json_copy = {0};
  if (!scene_loader_alloc_copy_string(&rf->scene_async_allocator,
                                      rf->scene_async_mutex, json,
                                      &payload->json_storage, &json_copy)) {
    vkr_allocator_free_ts(&rf->scene_async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          rf->scene_async_mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  payload->json_length = json_copy.length;

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!scene_loader_parse_json_imports(
          &rf->scene_async_allocator, rf->scene_async_mutex, json_copy,
          &payload->imports, &payload->entity_count, &payload->imports_capacity,
          &scene_error)) {
    scene_loader_destroy_async_payload_contents(payload);
    vkr_allocator_free_ts(&rf->scene_async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                          rf->scene_async_mutex);
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }

  payload->load_result.entity_count = payload->entity_count;

  if (payload->entity_count > 0) {
    uint64_t entity_id_bytes = sizeof(VkrEntityId) * payload->entity_count;
    uint64_t mesh_state_bytes =
        sizeof(SceneMeshAsyncState) * payload->entity_count;
    uint64_t shape_state_bytes =
        sizeof(SceneShapeMaterialAsyncState) * payload->entity_count;
    payload->entity_ids = (VkrEntityId *)vkr_allocator_alloc_ts(
        &rf->scene_async_allocator, entity_id_bytes,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, rf->scene_async_mutex);
    payload->mesh_states = (SceneMeshAsyncState *)vkr_allocator_alloc_ts(
        &rf->scene_async_allocator, mesh_state_bytes,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, rf->scene_async_mutex);
    payload->shape_material_states =
        (SceneShapeMaterialAsyncState *)vkr_allocator_alloc_ts(
            &rf->scene_async_allocator, shape_state_bytes,
            VKR_ALLOCATOR_MEMORY_TAG_ARRAY, rf->scene_async_mutex);
    if (!payload->entity_ids || !payload->mesh_states ||
        !payload->shape_material_states) {
      scene_loader_destroy_async_payload_contents(payload);
      vkr_allocator_free_ts(&rf->scene_async_allocator, payload,
                            sizeof(*payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                            rf->scene_async_mutex);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    MemZero(payload->entity_ids, sizeof(VkrEntityId) * payload->entity_count);
    MemZero(payload->mesh_states,
            sizeof(SceneMeshAsyncState) * payload->entity_count);
    MemZero(payload->shape_material_states,
            sizeof(SceneShapeMaterialAsyncState) * payload->entity_count);

    for (uint32_t i = 0; i < payload->entity_count; ++i) {
      scene_loader_init_request_info(&payload->mesh_states[i].request_info,
                                     VKR_RESOURCE_TYPE_MESH);
      scene_loader_init_request_info(
          &payload->shape_material_states[i].request_info,
          VKR_RESOURCE_TYPE_MATERIAL);

      if (payload->imports[i].has_mesh && payload->imports[i].mesh_path.str &&
          payload->imports[i].mesh_path.length > 0) {
        VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
        (void)vkr_resource_system_load(
            VKR_RESOURCE_TYPE_MESH, payload->imports[i].mesh_path, temp_alloc,
            &payload->mesh_states[i].request_info, &dependency_error);
        payload->mesh_states[i].requested = true_v;
      }

      if (payload->imports[i].has_shape &&
          payload->imports[i].shape.material_path.str &&
          payload->imports[i].shape.material_path.length > 0) {
        VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
        (void)vkr_resource_system_load(
            VKR_RESOURCE_TYPE_MATERIAL, payload->imports[i].shape.material_path,
            temp_alloc, &payload->shape_material_states[i].request_info,
            &dependency_error);
        payload->shape_material_states[i].requested = true_v;
      }
    }
  }

  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_scene_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  (void)name;
  assert_log(self != NULL, "Self is NULL");
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrSceneLoaderAsyncPayload *async_payload =
      (VkrSceneLoaderAsyncPayload *)payload;

  if (!scene_loader_ensure_scene_handle(async_payload, out_error)) {
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(async_payload->scene_handle);
  if (!scene) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  if (async_payload->entity_count == 0) {
    async_payload->stage = SCENE_ASYNC_STAGE_COMPLETE;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_CREATE_ENTITIES) {
    uint32_t end = async_payload->stage_cursor + SCENE_ASYNC_ENTITY_CHUNK;
    if (end > async_payload->entity_count) {
      end = async_payload->entity_count;
    }
    for (uint32_t i = async_payload->stage_cursor; i < end; ++i) {
      VkrSceneError create_error = VKR_SCENE_ERROR_NONE;
      VkrEntityId entity = vkr_scene_create_entity(scene, &create_error);
      if (entity.u64 == VKR_ENTITY_ID_INVALID.u64) {
        *out_error = scene_error_to_renderer_error(create_error);
        return false_v;
      }

      async_payload->entity_ids[i] = entity;

      if (async_payload->imports[i].name.length > 0 &&
          !vkr_scene_set_name(scene, entity, async_payload->imports[i].name)) {
        *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }

      if (!vkr_scene_set_transform(scene, entity,
                                   async_payload->imports[i].position,
                                   async_payload->imports[i].rotation,
                                   async_payload->imports[i].scale)) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
    }

    async_payload->stage_cursor = end;
    scene_loader_sync_partial(async_payload);
    if (async_payload->stage_cursor < async_payload->entity_count) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
    async_payload->stage = SCENE_ASYNC_STAGE_SET_PARENTS;
    async_payload->stage_cursor = 0;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_SET_PARENTS) {
    uint32_t end = async_payload->stage_cursor + SCENE_ASYNC_RELATION_CHUNK;
    if (end > async_payload->entity_count) {
      end = async_payload->entity_count;
    }
    for (uint32_t i = async_payload->stage_cursor; i < end; ++i) {
      int32_t parent_index = async_payload->imports[i].parent_index;
      if (parent_index < 0) {
        continue;
      }
      if ((uint32_t)parent_index >= async_payload->entity_count) {
        log_warn("Scene loader: entity %u parent index %d is out of range", i,
                 parent_index);
        continue;
      }
      vkr_scene_set_parent(scene, async_payload->entity_ids[i],
                           async_payload->entity_ids[parent_index]);
    }

    async_payload->stage_cursor = end;
    scene_loader_sync_partial(async_payload);
    if (async_payload->stage_cursor < async_payload->entity_count) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
    async_payload->stage = SCENE_ASYNC_STAGE_SET_COMPONENTS;
    async_payload->stage_cursor = 0;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_SET_COMPONENTS) {
    uint32_t processed = 0;
    while (async_payload->stage_cursor < async_payload->entity_count &&
           processed < SCENE_ASYNC_COMPONENT_CHUNK) {
      if (!scene_loader_apply_component_for_entity(
              async_payload, async_payload->stage_cursor, out_error)) {
        if (*out_error == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
          scene_loader_sync_partial(async_payload);
        }
        return false_v;
      }
      async_payload->stage_cursor++;
      processed++;
    }

    scene_loader_sync_partial(async_payload);
    if (async_payload->stage_cursor < async_payload->entity_count) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
    async_payload->stage = SCENE_ASYNC_STAGE_ATTACH_MESHES;
    async_payload->stage_cursor = 0;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_ATTACH_MESHES) {
    uint32_t processed = 0;
    while (async_payload->stage_cursor < async_payload->entity_count &&
           processed < SCENE_ASYNC_MESH_CHUNK) {
      if (!scene_loader_attach_mesh_for_entity(
              async_payload, async_payload->stage_cursor, out_error)) {
        if (*out_error == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
          scene_loader_sync_partial(async_payload);
        }
        return false_v;
      }
      async_payload->stage_cursor++;
      processed++;
    }

    scene_loader_sync_partial(async_payload);
    if (async_payload->stage_cursor < async_payload->entity_count) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
    async_payload->stage = SCENE_ASYNC_STAGE_WAIT_DEPENDENCIES;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_WAIT_DEPENDENCIES) {
    if (!scene_loader_wait_mesh_dependencies(async_payload, out_error)) {
      if (*out_error == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
        scene_loader_sync_partial(async_payload);
      }
      return false_v;
    }

    scene_loader_sync_partial(async_payload);
    async_payload->stage = SCENE_ASYNC_STAGE_COMPLETE;
  }

  out_handle->type = VKR_RESOURCE_TYPE_SCENE;
  out_handle->loader_id = self->id;
  out_handle->as.scene = async_payload->scene_handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  async_payload->ownership_transferred = true_v;

  log_info(
      "Scene loaded async: %u entities, %u meshes, %u text3d, %u shapes, %u "
      "point lights, %u directional lights",
      async_payload->load_result.entity_count,
      async_payload->load_result.mesh_count,
      async_payload->load_result.text3d_count,
      async_payload->load_result.shape_count,
      async_payload->load_result.point_light_count,
      async_payload->load_result.directional_light_count);

  return true_v;
}

vkr_internal bool8_t vkr_scene_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  (void)payload;
  assert_log(out_cost != NULL, "Out cost is NULL");

  // Scene finalize applies staged ECS data and dependency attachment.
  // Heavy texture/mesh uploads are accounted on their own resource requests.
  MemZero(out_cost, sizeof(*out_cost));
  return true_v;
}

vkr_internal void scene_loader_destroy_async_payload_contents(
    VkrSceneLoaderAsyncPayload *payload) {
  if (!payload) {
    return;
  }

  if (payload->imports) {
    if (payload->mesh_states) {
      for (uint32_t i = 0; i < payload->entity_count; ++i) {
        SceneMeshAsyncState *mesh_state = &payload->mesh_states[i];
        if (mesh_state->request_info.request_id != 0 &&
            payload->imports[i].mesh_path.str &&
            payload->imports[i].mesh_path.length > 0) {
          vkr_resource_system_unload(&mesh_state->request_info,
                                     payload->imports[i].mesh_path);
        }
      }
    }

    if (payload->shape_material_states) {
      for (uint32_t i = 0; i < payload->entity_count; ++i) {
        SceneShapeMaterialAsyncState *shape_state =
            &payload->shape_material_states[i];
        if (shape_state->request_info.request_id != 0 &&
            payload->imports[i].shape.material_path.str &&
            payload->imports[i].shape.material_path.length > 0) {
          vkr_resource_system_unload(&shape_state->request_info,
                                     payload->imports[i].shape.material_path);
        }
      }
    }
  }

  if (!payload->ownership_transferred && payload->scene_handle) {
    vkr_scene_handle_destroy(payload->scene_handle, payload->rf);
    payload->scene_handle = VKR_SCENE_HANDLE_INVALID;
  }

  if (payload->shape_material_states) {
    vkr_allocator_free_ts(
        &payload->rf->scene_async_allocator, payload->shape_material_states,
        sizeof(SceneShapeMaterialAsyncState) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->rf->scene_async_mutex);
    payload->shape_material_states = NULL;
  }
  if (payload->mesh_states) {
    vkr_allocator_free_ts(
        &payload->rf->scene_async_allocator, payload->mesh_states,
        sizeof(SceneMeshAsyncState) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->rf->scene_async_mutex);
    payload->mesh_states = NULL;
  }
  if (payload->entity_ids) {
    vkr_allocator_free_ts(
        &payload->rf->scene_async_allocator, payload->entity_ids,
        sizeof(VkrEntityId) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->rf->scene_async_mutex);
    payload->entity_ids = NULL;
  }
  if (payload->imports) {
    vkr_allocator_free_ts(&payload->rf->scene_async_allocator, payload->imports,
                          sizeof(SceneEntityImport) * payload->imports_capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY,
                          payload->rf->scene_async_mutex);
    payload->imports = NULL;
    payload->imports_capacity = 0;
  }
  if (payload->json_storage) {
    vkr_allocator_free_ts(&payload->rf->scene_async_allocator,
                          payload->json_storage, payload->json_length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING,
                          payload->rf->scene_async_mutex);
    payload->json_storage = NULL;
    payload->json_length = 0;
  }
}

vkr_internal void
vkr_scene_loader_release_async_payload(VkrResourceLoader *self, void *payload) {
  assert_log(self != NULL, "Self is NULL");
  if (!payload) {
    return;
  }

  VkrSceneLoaderAsyncPayload *async_payload =
      (VkrSceneLoaderAsyncPayload *)payload;
  scene_loader_destroy_async_payload_contents(async_payload);
  vkr_allocator_free_ts(&async_payload->rf->scene_async_allocator,
                        async_payload, sizeof(*async_payload),
                        VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        async_payload->rf->scene_async_mutex);
}

// =============================================================================
// Resource System Integration
// =============================================================================

vkr_internal bool8_t vkr_scene_loader_can_load(VkrResourceLoader *self,
                                               String8 name) {
  (void)self;
  if (!name.str || name.length == 0)
    return false_v;
  return scene_string8_ends_with_cstr_i(name, ".scene.json");
}

vkr_internal bool8_t vkr_scene_loader_load(VkrResourceLoader *self,
                                           String8 name,
                                           VkrAllocator *temp_alloc,
                                           VkrResourceHandleInfo *out_handle,
                                           VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  struct s_RendererFrontend *rf =
      (struct s_RendererFrontend *)self->resource_system;
  if (!rf) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSceneError scene_err = VKR_SCENE_ERROR_NONE;
  VkrSceneHandle handle =
      vkr_scene_handle_create(&rf->allocator, 0, 64, 256, &scene_err);
  if (!handle) {
    *out_error = scene_error_to_renderer_error(scene_err);
    return false_v;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(temp_alloc);
  bool8_t scoped = vkr_allocator_scope_is_valid(&scope);

  VkrSceneLoadResult load_result = {0};
  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  bool8_t loaded = vkr_scene_load_from_file(scene, rf, name, temp_alloc,
                                            &load_result, &scene_err);

  if (scoped) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (!loaded) {
    log_error("Scene loader: failed to load '%s' (error=%d)",
              string8_cstr(&name), (int)scene_err);
    vkr_scene_handle_destroy(handle, rf);
    *out_error = scene_error_to_renderer_error(scene_err);
    return false_v;
  }

  // Sync through the incremental path; async load uses staged partial syncs.
  vkr_scene_handle_update(handle, 0.0);
  vkr_scene_handle_sync(handle, rf);

  log_info(
      "Scene loaded: %u entities, %u meshes, %u text3d, %u shapes, %u point "
      "lights, %u directional lights",
      load_result.entity_count, load_result.mesh_count,
      load_result.text3d_count, load_result.shape_count,
      load_result.point_light_count, load_result.directional_light_count);

  out_handle->type = VKR_RESOURCE_TYPE_SCENE;
  out_handle->loader_id = self->id;
  out_handle->as.scene = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

vkr_internal void vkr_scene_loader_unload(VkrResourceLoader *self,
                                          const VkrResourceHandleInfo *handle,
                                          String8 name) {
  (void)name;
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");

  if (handle->type != VKR_RESOURCE_TYPE_SCENE || !handle->as.scene) {
    return;
  }

  struct s_RendererFrontend *rf =
      (struct s_RendererFrontend *)self->resource_system;
  vkr_scene_handle_destroy(handle->as.scene, rf);
}

VkrResourceLoader vkr_scene_loader_create(void) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_SCENE;
  loader.can_load = vkr_scene_loader_can_load;
  loader.load = vkr_scene_loader_load;
  loader.prepare_async = vkr_scene_loader_prepare_async;
  loader.finalize_async = vkr_scene_loader_finalize_async;
  loader.estimate_async_finalize_cost =
      vkr_scene_loader_estimate_async_finalize_cost;
  loader.release_async_payload = vkr_scene_loader_release_async_payload;
  loader.unload = vkr_scene_loader_unload;
  return loader;
}

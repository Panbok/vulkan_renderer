/**
 * @file scene_loader.c
 * @brief Scene JSON loader implementation.
 */

#include "renderer/resources/loaders/scene_loader.h"

#include <cgltf.h>

#include "core/logger.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_world_resources.h"

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
  float32_t range;
  Vec3 direction_local;
  float32_t inner_cone_angle;
  float32_t outer_cone_angle;
  VkrPointLightKind kind;
  bool8_t enabled;
} ScenePointLightImport;

typedef struct SceneDirectionalLightImport {
  Vec3 color;
  float32_t intensity;
  Vec3 direction_local;
  bool8_t enabled;
} SceneDirectionalLightImport;

typedef struct SceneEnvironmentImport {
  bool8_t has_block;
  bool8_t valid;
  bool8_t enabled;
  VkrSceneEnvironmentSourceKind source_kind;
  String8 cubemap_path;
  String8 cubemap_base_path;
  String8 cubemap_extension;
  String8 equirect_path;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
} SceneEnvironmentImport;

typedef struct SceneReflectionProbeImport {
  bool8_t enabled;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  bool8_t has_cubemap;
  String8 cubemap_base_path;
  String8 cubemap_extension;
} SceneReflectionProbeImport;

typedef struct SceneEntityImport {
  String8 name;
  int32_t parent_index;
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  bool8_t has_mesh;
  String8 mesh_path;
  String8 gltf_light_source;
  String8 shader_override;
  VkrPipelineDomain pipeline_domain;
  /** False marks the caster DYNAMIC; scene meshes are static by default. */
  bool8_t shadow_caster_static;
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
#define SCENE_GLTF_PATH_MAX 1024u
#define SCENE_GLTF_PUNCTUAL_LIGHT_MAX 256u

typedef enum SceneAsyncFinalizeStage {
  SCENE_ASYNC_STAGE_CREATE_ENTITIES = 0,
  SCENE_ASYNC_STAGE_SET_PARENTS,
  SCENE_ASYNC_STAGE_SET_COMPONENTS,
  SCENE_ASYNC_STAGE_APPLY_GLTF_LIGHTS,
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
  VkrSceneGltfPunctualLightImport
      gltf_punctual_lights[SCENE_GLTF_PUNCTUAL_LIGHT_MAX];
  uint32_t gltf_punctual_light_count;
  VkrEntityId *entity_ids;
  SceneMeshAsyncState *mesh_states;
  SceneShapeMaterialAsyncState *shape_material_states;
  SceneEnvironmentImport environment_import;
  VkrTexturePreparedLoad environment_prepared;
  bool8_t environment_prepared_ready;
  bool8_t environment_applied;
  SceneReflectionProbeImport
      reflection_probe_imports[VKR_SCENE_REFLECTION_PROBE_MAX];
  uint32_t reflection_probe_import_count;
  bool8_t reflection_probes_applied;
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
vkr_internal SceneEnvironmentImport scene_environment_import_defaults(void);
vkr_internal SceneEnvironmentImport
scene_loader_parse_environment_import(String8 json);
vkr_internal SceneReflectionProbeImport
scene_reflection_probe_import_defaults(void);
vkr_internal uint32_t scene_loader_parse_reflection_probe_imports(
    String8 json,
    SceneReflectionProbeImport out_imports[VKR_SCENE_REFLECTION_PROBE_MAX]);
vkr_internal void
scene_loader_reset_scene_environment(VkrScene *scene,
                                     struct s_RendererFrontend *rf);
vkr_internal void scene_loader_apply_environment_import(
    VkrScene *scene, struct s_RendererFrontend *rf,
    const SceneEnvironmentImport *environment_import,
    const VkrTexturePreparedLoad *prepared_environment);
vkr_internal void
scene_loader_reset_scene_reflection_probes(VkrScene *scene,
                                           struct s_RendererFrontend *rf);
vkr_internal void scene_loader_apply_reflection_probe_imports(
    VkrScene *scene, struct s_RendererFrontend *rf,
    const SceneReflectionProbeImport *imports, uint32_t import_count);
vkr_internal bool8_t scene_loader_parse_json_imports(
    VkrAllocator *allocator, VkrMutex mutex, String8 json,
    SceneEntityImport **out_imports, uint32_t *out_entity_count,
    uint32_t *out_import_capacity, VkrSceneError *out_error);
vkr_internal void scene_loader_destroy_async_payload_contents(
    VkrSceneLoaderAsyncPayload *payload);
vkr_internal void
scene_loader_destroy_async_payload(VkrSceneLoaderAsyncPayload *payload);
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

vkr_internal bool8_t scene_loader_apply_gltf_punctual_light(
    VkrSceneLoaderAsyncPayload *payload, uint32_t light_index,
    VkrRendererError *out_error) {
  if (!payload || !payload->scene_handle || !out_error ||
      light_index >= payload->gltf_punctual_light_count) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return false_v;
  }
  VkrScene *scene = vkr_scene_handle_get_scene(payload->scene_handle);
  if (!scene) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  const VkrSceneGltfPunctualLightImport *import =
      &payload->gltf_punctual_lights[light_index];
  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  const VkrEntityId entity = vkr_scene_create_entity(scene, &scene_error);
  if (entity.u64 == VKR_ENTITY_ID_INVALID.u64) {
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }
  String8 name = string8_create_from_cstr((const uint8_t *)import->name,
                                          string_length(import->name));
  if (!vkr_scene_set_name(scene, entity, name) ||
      !vkr_scene_set_transform(scene, entity, import->position,
                               vkr_quat_identity(), vec3_one())) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  if (import->type == VKR_SCENE_GLTF_LIGHT_DIRECTIONAL) {
    const SceneDirectionalLight light = {
        .color = import->color,
        .intensity = import->intensity,
        .direction_local = import->direction,
        .enabled = true_v,
    };
    if (!vkr_scene_set_directional_light(scene, entity, &light)) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return false_v;
    }
    payload->load_result.directional_light_count++;
  } else if (import->type == VKR_SCENE_GLTF_LIGHT_POINT ||
             import->type == VKR_SCENE_GLTF_LIGHT_SPOT) {
    const ScenePointLight light = {
        .color = import->color,
        .intensity = import->intensity,
        .constant = 1.0f,
        .linear = 0.0f,
        .quadratic = 0.0f,
        .range = import->range,
        .direction_local = import->direction,
        .inner_cone_angle = import->inner_cone_angle,
        .outer_cone_angle = import->outer_cone_angle,
        .kind = import->type == VKR_SCENE_GLTF_LIGHT_SPOT
                    ? VKR_POINT_LIGHT_KIND_GLTF_SPOT
                    : VKR_POINT_LIGHT_KIND_GLTF_POINT,
        .enabled = true_v,
    };
    if (!vkr_scene_set_point_light(scene, entity, &light)) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return false_v;
    }
    payload->load_result.point_light_count++;
  }
  payload->load_result.entity_count++;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
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
      .range = 0.0f,
      .direction_local = {0.0f, 0.0f, -1.0f},
      .inner_cone_angle = 0.0f,
      .outer_cone_angle = 0.78539816339f,
      .kind = VKR_POINT_LIGHT_KIND_POLYNOMIAL,
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

vkr_internal Mat4 scene_loader_entity_import_world_matrix(
    const SceneEntityImport *imports, uint32_t entity_count,
    uint32_t entity_index) {
  uint32_t chain[VKR_TRANSFORM_MAX_DEPTH];
  uint32_t chain_count = 0;
  int32_t current = (int32_t)entity_index;
  while (current >= 0 && (uint32_t)current < entity_count &&
         chain_count < VKR_TRANSFORM_MAX_DEPTH) {
    chain[chain_count++] = (uint32_t)current;
    current = imports[current].parent_index;
  }

  Mat4 world = mat4_identity();
  while (chain_count > 0) {
    const SceneEntityImport *import = &imports[chain[--chain_count]];
    Mat4 local = mat4_translate(import->position);
    local = mat4_mul(local, vkr_quat_to_mat4(import->rotation));
    local = mat4_mul(local, mat4_scale(import->scale));
    world = mat4_mul(world, local);
  }
  return world;
}

bool8_t vkr_scene_loader_read_gltf_punctual_lights(
    String8 path, Mat4 scene_world, uint32_t scene_entity_index,
    VkrSceneGltfPunctualLightImport *out_lights, uint32_t capacity,
    uint32_t *out_count) {
  if (!path.str || path.length == 0u || path.length >= SCENE_GLTF_PATH_MAX ||
      !out_lights || capacity == 0u || !out_count) {
    return false_v;
  }
  *out_count = 0u;

  char path_cstr[SCENE_GLTF_PATH_MAX] = {0};
  MemCopy(path_cstr, path.str, path.length);
  cgltf_options options = {0};
  cgltf_data *data = NULL;
  if (cgltf_parse_file(&options, path_cstr, &data) != cgltf_result_success ||
      !data) {
    return false_v;
  }

  for (uint32_t node_index = 0;
       node_index < data->nodes_count && *out_count < capacity; ++node_index) {
    const cgltf_node *node = &data->nodes[node_index];
    if (!node->light) {
      continue;
    }

    VkrSceneGltfPunctualLightType type;
    switch (node->light->type) {
    case cgltf_light_type_directional:
      type = VKR_SCENE_GLTF_LIGHT_DIRECTIONAL;
      break;
    case cgltf_light_type_point:
      type = VKR_SCENE_GLTF_LIGHT_POINT;
      break;
    case cgltf_light_type_spot:
      type = VKR_SCENE_GLTF_LIGHT_SPOT;
      break;
    default:
      continue;
    }

    cgltf_float gltf_values[16];
    cgltf_node_transform_world(node, gltf_values);
    const Mat4 gltf_world = mat4_new(
        gltf_values[0], gltf_values[1], gltf_values[2], gltf_values[3],
        gltf_values[4], gltf_values[5], gltf_values[6], gltf_values[7],
        gltf_values[8], gltf_values[9], gltf_values[10], gltf_values[11],
        gltf_values[12], gltf_values[13], gltf_values[14], gltf_values[15]);
    const Mat4 combined = mat4_mul(scene_world, gltf_world);
    const Vec4 direction4 =
        mat4_mul_vec4(combined, vec4_new(0.0f, 0.0f, -1.0f, 0.0f));
    VkrSceneGltfPunctualLightImport *out = &out_lights[(*out_count)++];
    *out = (VkrSceneGltfPunctualLightImport){
        .position = mat4_position(combined),
        .direction =
            vec3_normalize(vec3_new(direction4.x, direction4.y, direction4.z)),
        .color = vec3_new(node->light->color[0], node->light->color[1],
                          node->light->color[2]),
        .intensity = (float32_t)node->light->intensity,
        .range = (float32_t)node->light->range,
        .inner_cone_angle = (float32_t)node->light->spot_inner_cone_angle,
        .outer_cone_angle = (float32_t)node->light->spot_outer_cone_angle,
        .type = type,
    };
    snprintf(out->name, sizeof(out->name), "gltf.%u.%s", scene_entity_index,
             node->name          ? node->name
             : node->light->name ? node->light->name
                                 : "light");
  }
  cgltf_free(data);
  return true_v;
}

vkr_internal bool8_t
scene_loader_collect_gltf_punctual_lights(VkrSceneLoaderAsyncPayload *payload) {
  if (!payload || (payload->entity_count > 0u && !payload->imports)) {
    return false_v;
  }

  for (uint32_t entity_index = 0; entity_index < payload->entity_count;
       ++entity_index) {
    const SceneEntityImport *entity = &payload->imports[entity_index];
    if (!entity->has_mesh) {
      continue;
    }
    const bool8_t explicit_light_source =
        entity->gltf_light_source.str && entity->gltf_light_source.length > 0u;
    const String8 light_source =
        explicit_light_source ? entity->gltf_light_source : entity->mesh_path;
    if (!light_source.str || light_source.length < 4u) {
      continue;
    }
    if (!scene_string8_ends_with_cstr_i(light_source, ".gltf") &&
        !scene_string8_ends_with_cstr_i(light_source, ".glb")) {
      continue;
    }

    const Mat4 scene_world = scene_loader_entity_import_world_matrix(
        payload->imports, payload->entity_count, entity_index);
    const uint32_t remaining =
        SCENE_GLTF_PUNCTUAL_LIGHT_MAX - payload->gltf_punctual_light_count;
    if (remaining == 0u) {
      log_warn("Scene loader: glTF punctual-light import capacity reached (%u)",
               SCENE_GLTF_PUNCTUAL_LIGHT_MAX);
      break;
    }
    uint32_t imported_count = 0u;
    if (!vkr_scene_loader_read_gltf_punctual_lights(
            light_source, scene_world, entity_index,
            payload->gltf_punctual_lights + payload->gltf_punctual_light_count,
            remaining, &imported_count)) {
      if (explicit_light_source) {
        log_error(
            "Scene loader: unable to inspect required glTF punctual lights "
            "in '%.*s'",
            (int32_t)light_source.length, light_source.str);
        return false_v;
      }
      log_warn("Scene loader: unable to inspect glTF punctual lights in '%.*s'",
               (int32_t)light_source.length, light_source.str);
      continue;
    }
    payload->gltf_punctual_light_count += imported_count;
    if (imported_count == remaining && remaining > 0u) {
      log_warn("Scene loader: glTF punctual-light import capacity reached (%u)",
               SCENE_GLTF_PUNCTUAL_LIGHT_MAX);
    }
  }
  return true_v;
}

vkr_internal SceneEnvironmentImport scene_environment_import_defaults(void) {
  return (SceneEnvironmentImport){
      .has_block = false_v,
      .valid = true_v,
      .enabled = false_v,
      .source_kind = VKR_SCENE_ENV_SOURCE_NONE,
      .cubemap_path = {0},
      .cubemap_base_path = {0},
      .cubemap_extension = {0},
      .equirect_path = {0},
      .intensity = 1.0f,
      .diffuse_intensity = 1.0f,
      .specular_intensity = 1.0f,
  };
}

vkr_internal SceneEnvironmentImport
scene_loader_parse_environment_import(String8 json) {
  SceneEnvironmentImport result = scene_environment_import_defaults();
  if (!json.str || json.length == 0) {
    return result;
  }

  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader environment_reader = root;
  if (!vkr_json_find_field(&environment_reader, "environment")) {
    return result;
  }

  result.has_block = true_v;
  if (scene_json_parse_null(&environment_reader)) {
    return result;
  }

  VkrJsonReader environment_object = {0};
  if (!vkr_json_enter_object(&environment_reader, &environment_object)) {
    result.valid = false_v;
    return result;
  }

  result.enabled = true_v;
  (void)scene_json_read_bool_field(&environment_object, "enabled",
                                   &result.enabled);
  (void)scene_json_read_float_field(&environment_object, "intensity",
                                    &result.intensity);
  (void)scene_json_read_float_field(&environment_object, "diffuse_intensity",
                                    &result.diffuse_intensity);
  (void)scene_json_read_float_field(&environment_object, "specular_intensity",
                                    &result.specular_intensity);

  if (!result.enabled) {
    return result;
  }

  VkrJsonReader cubemap_reader = environment_object;
  const bool8_t has_cubemap_field =
      vkr_json_find_field(&cubemap_reader, "cubemap");
  const bool8_t has_equirect_field = scene_json_read_string_field(
      &environment_object, "equirect", &result.equirect_path);
  const bool8_t has_equirect =
      has_equirect_field && result.equirect_path.length > 0u;

  if (has_cubemap_field && has_equirect) {
    log_warn("Scene loader: environment fields 'cubemap' and 'equirect' are "
             "mutually exclusive; using fallback IBL");
    result.valid = false_v;
    return result;
  }

  if (has_equirect) {
    result.source_kind = VKR_SCENE_ENV_SOURCE_EQUIRECT;
    return result;
  }

  if (!has_cubemap_field || scene_json_parse_null(&cubemap_reader)) {
    result.valid = false_v;
    return result;
  }

  VkrJsonReader cubemap_object = {0};
  if (!vkr_json_enter_object(&cubemap_reader, &cubemap_object)) {
    result.valid = false_v;
    return result;
  }
  const bool8_t has_path = scene_json_read_string_field(&cubemap_object, "path",
                                                        &result.cubemap_path);
  const bool8_t has_base_path = scene_json_read_string_field(
      &cubemap_object, "base_path", &result.cubemap_base_path);
  const bool8_t has_extension = scene_json_read_string_field(
      &cubemap_object, "extension", &result.cubemap_extension);
  const bool8_t direct = has_path && result.cubemap_path.length > 0u;
  const bool8_t faces = has_base_path && has_extension &&
                        result.cubemap_base_path.length > 0u &&
                        result.cubemap_extension.length > 0u;
  if (direct == faces) {
    result.valid = false_v;
  } else {
    result.source_kind = VKR_SCENE_ENV_SOURCE_CUBEMAP;
  }

  return result;
}

vkr_internal SceneReflectionProbeImport
scene_reflection_probe_import_defaults(void) {
  return (SceneReflectionProbeImport){
      .enabled = true_v,
      .center = {0},
      .extents = {1.0f, 1.0f, 1.0f},
      .blend_distance = 1.0f,
      .intensity = 1.0f,
      .diffuse_intensity = 1.0f,
      .specular_intensity = 1.0f,
      .has_cubemap = false_v,
      .cubemap_base_path = {0},
      .cubemap_extension = {0},
  };
}

vkr_internal uint32_t scene_loader_parse_reflection_probe_imports(
    String8 json,
    SceneReflectionProbeImport out_imports[VKR_SCENE_REFLECTION_PROBE_MAX]) {
  if (!json.str || json.length == 0 || !out_imports) {
    return 0;
  }

  for (uint32_t i = 0; i < VKR_SCENE_REFLECTION_PROBE_MAX; ++i) {
    out_imports[i] = scene_reflection_probe_import_defaults();
  }

  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader probes_reader = root;
  if (!vkr_json_find_field(&probes_reader, "reflection_probes")) {
    return 0;
  }

  VkrJsonReader null_check = probes_reader;
  if (scene_json_parse_null(&null_check)) {
    return 0;
  }

  vkr_json_skip_whitespace(&probes_reader);
  if (probes_reader.pos >= probes_reader.length ||
      probes_reader.data[probes_reader.pos] != '[') {
    log_warn("Scene loader: reflection_probes must be an array");
    return 0;
  }
  probes_reader.pos++;

  uint32_t import_count = 0;
  uint32_t input_index = 0;
  bool8_t overflow_warned = false_v;
  while (vkr_json_next_array_element(&probes_reader)) {
    if (import_count >= VKR_SCENE_REFLECTION_PROBE_MAX) {
      if (!overflow_warned) {
        log_warn(
            "Scene loader: reflection_probes exceeds limit (%u), extra entries "
            "are ignored",
            VKR_SCENE_REFLECTION_PROBE_MAX);
        overflow_warned = true_v;
      }
      break;
    }

    VkrJsonReader probe_object = {0};
    if (!vkr_json_enter_object(&probes_reader, &probe_object)) {
      log_warn("Scene loader: reflection probe %u is not an object",
               input_index);
      input_index++;
      continue;
    }

    SceneReflectionProbeImport import =
        scene_reflection_probe_import_defaults();
    (void)scene_json_read_bool_field(&probe_object, "enabled", &import.enabled);
    (void)scene_json_read_float_field(&probe_object, "blend_distance",
                                      &import.blend_distance);
    import.blend_distance = Max(import.blend_distance, 0.0f);
    (void)scene_json_read_float_field(&probe_object, "intensity",
                                      &import.intensity);
    (void)scene_json_read_float_field(&probe_object, "diffuse_intensity",
                                      &import.diffuse_intensity);
    (void)scene_json_read_float_field(&probe_object, "specular_intensity",
                                      &import.specular_intensity);

    bool8_t has_center =
        scene_json_read_vec3_field(&probe_object, "center", &import.center);
    bool8_t has_extents =
        scene_json_read_vec3_field(&probe_object, "extents", &import.extents);

    if (import.enabled && (!has_center || !has_extents)) {
      log_warn("Scene loader: reflection probe %u missing center/extents",
               input_index);
      input_index++;
      continue;
    }

    if ((has_extents || import.enabled) &&
        (import.extents.x <= 0.0f || import.extents.y <= 0.0f ||
         import.extents.z <= 0.0f)) {
      log_warn("Scene loader: reflection probe %u has non-positive extents",
               input_index);
      input_index++;
      continue;
    }

    VkrJsonReader cubemap_reader = probe_object;
    if (vkr_json_find_field(&cubemap_reader, "cubemap")) {
      if (!scene_json_parse_null(&cubemap_reader)) {
        VkrJsonReader cubemap_object = {0};
        if (!vkr_json_enter_object(&cubemap_reader, &cubemap_object)) {
          log_warn("Scene loader: reflection probe %u cubemap is invalid",
                   input_index);
          input_index++;
          continue;
        }

        bool8_t has_base_path = scene_json_read_string_field(
            &cubemap_object, "base_path", &import.cubemap_base_path);
        bool8_t has_extension = scene_json_read_string_field(
            &cubemap_object, "extension", &import.cubemap_extension);
        if (!has_base_path || !has_extension ||
            import.cubemap_base_path.length == 0 ||
            import.cubemap_extension.length == 0) {
          log_warn("Scene loader: reflection probe %u cubemap path is invalid",
                   input_index);
          input_index++;
          continue;
        }

        import.has_cubemap = true_v;
      }
    }

    out_imports[import_count++] = import;
    input_index++;
  }

  return import_count;
}

vkr_internal void scene_loader_reset_scene_environment(VkrScene *scene,
                                                       RendererFrontend *rf) {
  if (!scene) {
    return;
  }

  if (rf) {
    vkr_world_resources_release_scene_environment_targets(rf, scene);
    if (scene->environment.prefilter_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &rf->texture_system, scene->environment.prefilter_cubemap);
    }
    if (scene->environment.irradiance_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &rf->texture_system, scene->environment.irradiance_cubemap);
    }
    if (scene->environment.source_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(&rf->texture_system,
                                           scene->environment.source_cubemap);
    }
    if (scene->environment.delivery_equirect.id != 0) {
      vkr_texture_system_release_by_handle(
          &rf->texture_system, scene->environment.delivery_equirect);
    }
  }

  scene->environment =
      (VkrSceneEnvironment){.enabled = false_v,
                            .source_kind = VKR_SCENE_ENV_SOURCE_NONE,
                            .intensity = 1.0f,
                            .diffuse_intensity = 1.0f,
                            .specular_intensity = 1.0f,
                            .delivery_equirect = VKR_TEXTURE_HANDLE_INVALID,
                            .source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                            .irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                            .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                            .bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE};
}

vkr_internal void scene_loader_apply_environment_import(
    VkrScene *scene, struct s_RendererFrontend *rf,
    const SceneEnvironmentImport *environment_import,
    const VkrTexturePreparedLoad *prepared_environment) {
  if (!scene) {
    return;
  }

  scene_loader_reset_scene_environment(scene, rf);
  if (!environment_import || !environment_import->has_block) {
    return;
  }

  scene->environment.intensity = environment_import->intensity;
  scene->environment.diffuse_intensity = environment_import->diffuse_intensity;
  scene->environment.specular_intensity =
      environment_import->specular_intensity;
  scene->environment.enabled = environment_import->enabled;
  scene->environment.source_kind = environment_import->source_kind;

  if (!environment_import->valid) {
    log_warn("Scene loader: invalid environment block, using fallback IBL");
    scene->environment.enabled = false_v;
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE;
    return;
  }

  if (!environment_import->enabled) {
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE;
    return;
  }

  if (!rf) {
    log_warn("Scene loader: renderer missing while applying environment block");
    scene->environment.enabled = false_v;
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE;
    return;
  }

  if (environment_import->source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP) {
    VkrTextureHandle source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    VkrRendererError cubemap_error = VKR_RENDERER_ERROR_NONE;
    if (environment_import->cubemap_path.length > 0u) {
      VkrTexturePreparedLoad local_prepared = {0};
      const VkrTexturePreparedLoad *upload = prepared_environment;
      if (!upload && !vkr_texture_system_prepare_load_from_file(
                         &rf->texture_system, environment_import->cubemap_path,
                         VKR_TEXTURE_RGBA_CHANNELS, &rf->scratch_allocator,
                         &local_prepared, &cubemap_error)) {
        goto failed;
      }
      upload = upload ? upload : &local_prepared;
      const bool8_t valid =
          upload->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
      const bool8_t finalized =
          valid && vkr_texture_system_finalize_prepared_load(
                       &rf->texture_system, environment_import->cubemap_path,
                       upload, &source_cubemap, &cubemap_error);
      vkr_texture_system_release_prepared_load(&local_prepared);
      if (!finalized) {
        goto failed;
      }
      vkr_texture_system_add_ref_by_handle(&rf->texture_system, source_cubemap);
    } else if (!vkr_texture_system_load_cube_map(
                   &rf->texture_system, environment_import->cubemap_base_path,
                   environment_import->cubemap_extension, &source_cubemap,
                   &cubemap_error)) {
      String8 err_str = vkr_renderer_get_error_string(cubemap_error);
      log_warn("Scene loader: environment cubemap load failed for '%.*s': %.*s",
               (int)environment_import->cubemap_base_path.length,
               environment_import->cubemap_base_path.str, (int)err_str.length,
               err_str.str);
      goto failed;
    }
    scene->environment.source_cubemap = source_cubemap;
  } else if (environment_import->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    VkrTexturePreparedLoad local_prepared = {0};
    const VkrTexturePreparedLoad *upload = prepared_environment;
    VkrRendererError texture_error = VKR_RENDERER_ERROR_NONE;
    if (!upload) {
      if (!vkr_texture_system_prepare_load_from_file(
              &rf->texture_system, environment_import->equirect_path,
              VKR_TEXTURE_RGBA_CHANNELS, &rf->scratch_allocator,
              &local_prepared, &texture_error)) {
        goto failed;
      }
      upload = &local_prepared;
    }

    VkrTextureHandle delivery = VKR_TEXTURE_HANDLE_INVALID;
    if (upload->description.type != VKR_TEXTURE_TYPE_2D ||
        upload->description.format != VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT) {
      vkr_texture_system_release_prepared_load(&local_prepared);
      goto failed;
    }
    const bool8_t finalized = vkr_texture_system_finalize_prepared_load(
        &rf->texture_system, environment_import->equirect_path, upload,
        &delivery, &texture_error);
    vkr_texture_system_release_prepared_load(&local_prepared);
    if (!finalized) {
      goto failed;
    }
    vkr_texture_system_add_ref_by_handle(&rf->texture_system, delivery);
    scene->environment.delivery_equirect = delivery;
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_SOURCE_LOADING;
  } else {
    goto failed;
  }

  if (!vkr_world_resources_prepare_scene_environment(rf, &rf->world_resources,
                                                     scene)) {
    goto failed;
  }
  return;

failed:
  scene_loader_reset_scene_environment(scene, rf);
  scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

vkr_internal void
scene_loader_reset_scene_reflection_probes(VkrScene *scene,
                                           struct s_RendererFrontend *rf) {
  if (!scene) {
    return;
  }

  if (rf) {
    vkr_world_resources_release_scene_reflection_probe_targets(rf, scene);
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (probe->prefilter_cubemap.id != 0) {
        vkr_texture_system_release_by_handle(&rf->texture_system,
                                             probe->prefilter_cubemap);
      }
      if (probe->irradiance_cubemap.id != 0) {
        vkr_texture_system_release_by_handle(&rf->texture_system,
                                             probe->irradiance_cubemap);
      }
      if (probe->source_cubemap.id != 0) {
        vkr_texture_system_release_by_handle(&rf->texture_system,
                                             probe->source_cubemap);
      }
    }
  }

  scene->reflection_probe_count = 0;
  for (uint32_t i = 0; i < VKR_SCENE_REFLECTION_PROBE_MAX; ++i) {
    scene->reflection_probes[i] = (VkrSceneReflectionProbe){
        .enabled = false_v,
        .center = {0},
        .extents = {0},
        .blend_distance = 1.0f,
        .intensity = 1.0f,
        .diffuse_intensity = 1.0f,
        .specular_intensity = 1.0f,
        .source_mip_count = 1u,
        .uses_scene_environment_source = false_v,
        .source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE,
    };
  }
}

vkr_internal void scene_loader_apply_reflection_probe_imports(
    VkrScene *scene, struct s_RendererFrontend *rf,
    const SceneReflectionProbeImport *imports, uint32_t import_count) {
  if (!scene) {
    return;
  }

  scene_loader_reset_scene_reflection_probes(scene, rf);
  if (!imports || import_count == 0) {
    return;
  }

  VkrTextureHandle environment_source = VKR_TEXTURE_HANDLE_INVALID;
  bool8_t environment_source_valid = false_v;
  if (rf && scene->environment.source_cubemap.id != 0) {
    VkrTexture *environment_texture = vkr_texture_system_get_by_handle(
        &rf->texture_system, scene->environment.source_cubemap);
    if (environment_texture && environment_texture->handle &&
        environment_texture->description.type == VKR_TEXTURE_TYPE_CUBE_MAP) {
      environment_source = scene->environment.source_cubemap;
      environment_source_valid = true_v;
    }
  }

  for (uint32_t i = 0; i < import_count; ++i) {
    if (scene->reflection_probe_count >= VKR_SCENE_REFLECTION_PROBE_MAX) {
      break;
    }

    const SceneReflectionProbeImport *import = &imports[i];
    VkrSceneReflectionProbe probe = {
        .enabled = import->enabled,
        .center = import->center,
        .extents = import->extents,
        .blend_distance = import->blend_distance,
        .intensity = import->intensity,
        .diffuse_intensity = import->diffuse_intensity,
        .specular_intensity = import->specular_intensity,
        .source_mip_count = 1u,
        .uses_scene_environment_source = false_v,
        .source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE,
    };

    if (probe.enabled) {
      if (!rf) {
        log_warn(
            "Scene loader: renderer missing while applying reflection probe %u",
            i);
        probe.enabled = false_v;
      } else {
        VkrTextureHandle source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
        bool8_t source_valid = false_v;
        if (import->has_cubemap) {
          VkrRendererError cubemap_error = VKR_RENDERER_ERROR_NONE;
          if (vkr_texture_system_load_cube_map(
                  &rf->texture_system, import->cubemap_base_path,
                  import->cubemap_extension, &source_cubemap, &cubemap_error)) {
            source_valid = true_v;
          } else {
            String8 err_str = vkr_renderer_get_error_string(cubemap_error);
            log_warn(
                "Scene loader: reflection probe %u cubemap load failed for "
                "'%.*s': %.*s",
                i, (int)import->cubemap_base_path.length,
                import->cubemap_base_path.str, (int)err_str.length,
                err_str.str);
          }
        } else if (environment_source_valid) {
          source_cubemap = environment_source;
          vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                               source_cubemap);
          probe.source_mip_count = scene->environment.source_mip_count;
          probe.uses_scene_environment_source = true_v;
          source_valid = true_v;
        } else {
          log_warn(
              "Scene loader: reflection probe %u has no cubemap and no valid "
              "environment source",
              i);
        }

        if (source_valid) {
          probe.source_cubemap = source_cubemap;
          probe.bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING;
        } else {
          probe.enabled = false_v;
          probe.bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE;
        }
      }
    }

    scene->reflection_probes[scene->reflection_probe_count++] = probe;
  }
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

  String8 gltf_light_source = {0};
  if (scene_json_read_string_field(&mesh_obj, "gltf_light_source",
                                   &gltf_light_source) &&
      gltf_light_source.length > 0) {
    out_entity->gltf_light_source = gltf_light_source;
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

  /* Scene-authored meshes are architecture and do not move, so STATIC is the
     useful default; without it the static span is empty and cascade reuse can
     never fire. An authored mover opts out with "dynamic_caster": true. The
     contract this asserts is enforceable because every mesh-manager mutation
     path bumps a generation. */
  out_entity->shadow_caster_static = true_v;
  bool8_t dynamic_caster = false_v;
  if (scene_json_read_bool_field(&mesh_obj, "dynamic_caster",
                                 &dynamic_caster) &&
      dynamic_caster) {
    out_entity->shadow_caster_static = false_v;
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

  SceneEnvironmentImport environment_import =
      scene_loader_parse_environment_import(json);
  scene_loader_apply_environment_import(scene, rf, &environment_import, NULL);

  SceneReflectionProbeImport
      reflection_probe_imports[VKR_SCENE_REFLECTION_PROBE_MAX] = {0};
  uint32_t reflection_probe_import_count =
      scene_loader_parse_reflection_probe_imports(json,
                                                  reflection_probe_imports);
  scene_loader_apply_reflection_probe_imports(
      scene, rf, reflection_probe_imports, reflection_probe_import_count);
  (void)vkr_world_resources_prepare_scene_reflection_probes(
      rf, &rf->world_resources, scene);

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

      (void)vkr_mesh_manager_instance_set_shadow_mobility(
          &rf->mesh_manager, instance,
          imports[entity_index].shadow_caster_static
              ? VKR_SHADOW_CASTER_MOBILITY_STATIC
              : VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);

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
        .range = light_import->range,
        .direction_local = light_import->direction_local,
        .inner_cone_angle = light_import->inner_cone_angle,
        .outer_cone_angle = light_import->outer_cone_angle,
        .kind = light_import->kind,
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
        .range = light_import->range,
        .direction_local = light_import->direction_local,
        .inner_cone_angle = light_import->inner_cone_angle,
        .outer_cone_angle = light_import->outer_cone_angle,
        .kind = light_import->kind,
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
    const VkrRendererError failure =
        load_state_error != VKR_RENDERER_ERROR_NONE
            ? load_state_error
            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    const String8 error_string = vkr_renderer_get_error_string(failure);
    log_error("Scene loader: required mesh dependency '%.*s' failed: %.*s",
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
    *out_error = failure;
    return false_v;
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

    const VkrRendererError failure =
        mesh_error != VKR_RENDERER_ERROR_NONE
            ? mesh_error
            : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    String8 error_string = vkr_renderer_get_error_string(failure);
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
    *out_error = failure;
    return false_v;
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

  /* The asynchronous attach path creates instances too, so it needs the same
     classification as the batch path; without it every async-loaded scene
     mesh stays DYNAMIC and the static span is empty. */
  (void)vkr_mesh_manager_instance_set_shadow_mobility(
      &payload->rf->mesh_manager, instance,
      entity_import->shadow_caster_static ? VKR_SHADOW_CASTER_MOBILITY_STATIC
                                          : VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);

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
  payload->environment_import = scene_environment_import_defaults();
  payload->reflection_probe_import_count = 0;
  payload->reflection_probes_applied = false_v;
  payload->stage = SCENE_ASYNC_STAGE_CREATE_ENTITIES;
  payload->stage_cursor = 0;
  payload->ownership_transferred = false_v;

  String8 json_copy = {0};
  if (!scene_loader_alloc_copy_string(&rf->scene_async_allocator,
                                      rf->scene_async_mutex, json,
                                      &payload->json_storage, &json_copy)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  payload->json_length = json_copy.length;

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!scene_loader_parse_json_imports(
          &rf->scene_async_allocator, rf->scene_async_mutex, json_copy,
          &payload->imports, &payload->entity_count, &payload->imports_capacity,
          &scene_error)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }
  if (!scene_loader_collect_gltf_punctual_lights(payload)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  payload->environment_import =
      scene_loader_parse_environment_import(json_copy);
  const bool8_t direct_cubemap =
      payload->environment_import.source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP &&
      payload->environment_import.cubemap_path.length > 0u;
  const bool8_t equirect =
      payload->environment_import.source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT;
  if (payload->environment_import.valid &&
      payload->environment_import.enabled && (equirect || direct_cubemap)) {
    const String8 environment_path =
        direct_cubemap ? payload->environment_import.cubemap_path
                       : payload->environment_import.equirect_path;
    VkrRendererError environment_error = VKR_RENDERER_ERROR_NONE;
    if (vkr_texture_system_prepare_load_from_file(
            &rf->texture_system, environment_path, VKR_TEXTURE_RGBA_CHANNELS,
            temp_alloc, &payload->environment_prepared, &environment_error) &&
        (!direct_cubemap || payload->environment_prepared.description.type ==
                                VKR_TEXTURE_TYPE_CUBE_MAP)) {
      payload->environment_prepared_ready = true_v;
    } else {
      vkr_texture_system_release_prepared_load(&payload->environment_prepared);
      payload->environment_import.valid = false_v;
      log_warn("Scene loader: failed to prepare environment '%.*s'",
               (int)environment_path.length, environment_path.str);
    }
  }
  payload->reflection_probe_import_count =
      scene_loader_parse_reflection_probe_imports(
          json_copy, payload->reflection_probe_imports);

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
      scene_loader_destroy_async_payload(payload);
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

  if (!async_payload->environment_applied) {
    scene_loader_apply_environment_import(
        scene, async_payload->rf, &async_payload->environment_import,
        async_payload->environment_prepared_ready
            ? &async_payload->environment_prepared
            : NULL);
    if (async_payload->environment_prepared_ready) {
      vkr_texture_system_release_prepared_load(
          &async_payload->environment_prepared);
      async_payload->environment_prepared_ready = false_v;
    }
    async_payload->environment_applied = true_v;
  }
  if (!async_payload->reflection_probes_applied) {
    scene_loader_apply_reflection_probe_imports(
        scene, async_payload->rf, async_payload->reflection_probe_imports,
        async_payload->reflection_probe_import_count);
    (void)vkr_world_resources_prepare_scene_reflection_probes(
        async_payload->rf, &async_payload->rf->world_resources, scene);
    async_payload->reflection_probes_applied = true_v;
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
    async_payload->stage = SCENE_ASYNC_STAGE_APPLY_GLTF_LIGHTS;
    async_payload->stage_cursor = 0;
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (async_payload->stage == SCENE_ASYNC_STAGE_APPLY_GLTF_LIGHTS) {
    uint32_t processed = 0;
    while (async_payload->stage_cursor <
               async_payload->gltf_punctual_light_count &&
           processed < SCENE_ASYNC_COMPONENT_CHUNK) {
      if (!scene_loader_apply_gltf_punctual_light(
              async_payload, async_payload->stage_cursor, out_error)) {
        return false_v;
      }
      async_payload->stage_cursor++;
      processed++;
    }
    scene_loader_sync_partial(async_payload);
    if (async_payload->stage_cursor <
        async_payload->gltf_punctual_light_count) {
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

  if (payload->environment_prepared_ready) {
    vkr_texture_system_release_prepared_load(&payload->environment_prepared);
    payload->environment_prepared_ready = false_v;
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
scene_loader_destroy_async_payload(VkrSceneLoaderAsyncPayload *payload) {
  if (!payload) {
    return;
  }

  struct s_RendererFrontend *rf = payload->rf;
  scene_loader_destroy_async_payload_contents(payload);
  vkr_allocator_free_ts(&rf->scene_async_allocator, payload, sizeof(*payload),
                        VKR_ALLOCATOR_MEMORY_TAG_STRUCT, rf->scene_async_mutex);
}

vkr_internal void
vkr_scene_loader_release_async_payload(VkrResourceLoader *self, void *payload) {
  assert_log(self != NULL, "Self is NULL");
  if (!payload) {
    return;
  }

  VkrSceneLoaderAsyncPayload *async_payload =
      (VkrSceneLoaderAsyncPayload *)payload;
  scene_loader_destroy_async_payload(async_payload);
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

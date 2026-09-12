#include "filesystem/vkr_asset_path.h"
/**
 * @file scene_loader.c
 * @brief Scene JSON loader implementation.
 */

#include "renderer/resources/loaders/scene_loader.h"

#include "assets/vkr_diffuse_volume.h"
#include "vkr_subsurface.h"
#include "core/logger.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "math/vkr_transform.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_world_resources.h"

#include <stdlib.h>
#include <stdio.h>

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
  bool8_t casts_shadow;
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

typedef struct SceneRectangleLightImport {
  Vec3 color;
  float32_t radiance;
  Vec2 size;
  bool8_t enabled;
} SceneRectangleLightImport;

typedef struct SceneDirectionalLightImport {
  Vec3 color;
  float32_t intensity;
  Vec3 direction_local;
  float32_t sun_angular_diameter_degrees;
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
  float32_t sh_deringing;
} SceneEnvironmentImport;

typedef struct SceneAtmosphereImport {
  bool8_t has_block;
  bool8_t valid;
  VkrAtmosphereSettings settings;
  float32_t sh_deringing;
} SceneAtmosphereImport;

typedef struct SceneFogImport {
  bool8_t has_block;
  bool8_t valid;
  VkrFogSettings settings;
} SceneFogImport;

typedef struct SceneFroxelFogImport {
  bool8_t valid;
  VkrFroxelFogSettings settings;
} SceneFroxelFogImport;

typedef struct SceneDiffuseVolumeImport {
  bool8_t has_block;
  bool8_t valid;
  String8 path;
} SceneDiffuseVolumeImport;

typedef struct SceneSubsurfaceImport {
  bool8_t has_block;
  bool8_t valid;
  bool8_t enabled;
  VkrSubsurfaceProfile profiles[VKR_SUBSURFACE_PROFILE_COUNT];
  uint32_t profile_count;
} SceneSubsurfaceImport;

typedef struct SceneReflectionProbeImport {
  bool8_t enabled;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  float32_t sh_deringing;
  bool8_t has_cubemap;
  String8 cubemap_path;
  String8 cubemap_base_path;
  String8 cubemap_extension;
} SceneReflectionProbeImport;

typedef struct SceneEntityImport {
  String8 name;
  int32_t parent_index;
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  Mat4 matrix;
  bool8_t has_matrix;
  bool8_t has_mesh;
  String8 mesh_path;
  String8 gltf_light_source;
  String8 gltf_light_range_overrides;
  bool8_t has_gltf_light_range_overrides;
  bool8_t gltf_light_range_overrides_invalid;
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
  bool8_t has_rectangle_light;
  SceneRectangleLightImport rectangle_light;
  bool8_t has_directional_light;
  SceneDirectionalLightImport directional_light;
} SceneEntityImport;

typedef struct SceneGltfLightRangeOverride {
  String8 light;
  float32_t range;
  uint32_t definition_match_count;
  uint32_t node_match_count;
} SceneGltfLightRangeOverride;

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
  SCENE_ASYNC_STAGE_ATTACH_MESHES,
  SCENE_ASYNC_STAGE_WAIT_DEPENDENCIES,
  SCENE_ASYNC_STAGE_COMPLETE
} SceneAsyncFinalizeStage;

typedef struct SceneMeshAsyncState {
  VkrEntityId *source_nodes;
  uint32_t source_node_count;
  uint32_t source_cursor;
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
  struct VkrRenderAssets *assets;
  char *json_storage;
  uint64_t json_length;
  uint8_t *path_storage;
  uint64_t path_storage_size;
  uint64_t scene_source_fingerprint;
  SceneEntityImport *imports;
  uint32_t imports_capacity;
  uint32_t entity_count;
  VkrEntityId *entity_ids;
  SceneMeshAsyncState *mesh_states;
  SceneShapeMaterialAsyncState *shape_material_states;
  SceneEnvironmentImport environment_import;
  SceneAtmosphereImport atmosphere_import;
  SceneFogImport fog_import;
  SceneFroxelFogImport froxel_fog_import;
  VkrTexturePreparedLoad environment_prepared;
  bool8_t environment_prepared_ready;
  bool8_t environment_applied;
  bool8_t atmosphere_applied;
  bool8_t fog_applied;
  SceneDiffuseVolumeImport diffuse_volume_import;
  VkrTexturePreparedLoad diffuse_volume_prepared;
  VkrDiffuseVolumeBinding diffuse_volume_binding;
  bool8_t diffuse_volume_prepared_ready;
  bool8_t diffuse_volume_applied;
  SceneSubsurfaceImport subsurface_import;
  bool8_t subsurface_applied;
  SceneReflectionProbeImport
      reflection_probe_imports[VKR_SCENE_REFLECTION_PROBE_MAX];
  uint32_t reflection_probe_import_count;
  VkrTexturePreparedLoad
      reflection_probe_prepared[VKR_SCENE_REFLECTION_PROBE_MAX];
  bool8_t reflection_probe_prepared_ready[VKR_SCENE_REFLECTION_PROBE_MAX];
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
vkr_internal SceneAtmosphereImport scene_atmosphere_import_defaults(void);
vkr_internal SceneAtmosphereImport
scene_loader_parse_atmosphere_import(String8 json);
vkr_internal SceneFogImport scene_fog_import_defaults(void);
vkr_internal SceneFogImport scene_loader_parse_fog_import(String8 json);
vkr_internal SceneDiffuseVolumeImport
scene_diffuse_volume_import_defaults(void);
vkr_internal SceneDiffuseVolumeImport
scene_loader_parse_diffuse_volume_import(String8 json);
vkr_internal bool8_t scene_loader_prepare_diffuse_volume(
    String8 path, VkrAllocator *temp_alloc,
    VkrDiffuseVolumeBinding *out_binding, VkrTexturePreparedLoad *out_prepared);
vkr_internal void scene_loader_apply_diffuse_volume_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneDiffuseVolumeImport *import,
    const VkrDiffuseVolumeBinding *binding,
    const VkrTexturePreparedLoad *prepared);
vkr_internal SceneSubsurfaceImport scene_subsurface_import_defaults(void);
vkr_internal SceneSubsurfaceImport
scene_loader_parse_subsurface_import(String8 json);
vkr_internal bool8_t scene_loader_apply_subsurface_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneSubsurfaceImport *import, VkrRendererError *out_error);
vkr_internal SceneReflectionProbeImport
scene_reflection_probe_import_defaults(void);
vkr_internal uint32_t scene_loader_parse_reflection_probe_imports(
    String8 json,
    SceneReflectionProbeImport out_imports[VKR_SCENE_REFLECTION_PROBE_MAX]);
vkr_internal void
scene_loader_reset_scene_environment(VkrScene *scene,
                                     struct VkrRenderAssets *assets);
vkr_internal void scene_loader_apply_environment_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneEnvironmentImport *environment_import,
    const VkrTexturePreparedLoad *prepared_environment);
vkr_internal bool8_t scene_loader_apply_atmosphere_import(
    VkrScene *scene, const SceneAtmosphereImport *atmosphere_import);
vkr_internal void
scene_loader_reset_scene_reflection_probes(VkrScene *scene,
                                           struct VkrRenderAssets *assets);
vkr_internal void scene_loader_apply_reflection_probe_imports(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneReflectionProbeImport *imports, uint32_t import_count,
    const VkrTexturePreparedLoad *prepared_cubemaps,
    const bool8_t *prepared_cubemaps_ready);
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

vkr_internal bool8_t scene_string8_ends_with_cstr_i(String8 value,
                                                    const char *suffix) {
  if (!suffix) {
    return false_v;
  }
  const uint64_t suffix_length = string_length(suffix);
  if (value.length < suffix_length) {
    return false_v;
  }
  const String8 tail =
      string8_substring(&value, value.length - suffix_length, value.length);
  const String8 suffix_string =
      string8_create_from_cstr((const uint8_t *)suffix, suffix_length);
  return string8_equalsi(&tail, &suffix_string);
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

vkr_internal bool8_t scene_json_parse_vec2(VkrJsonReader *reader,
                                           Vec2 *out_value) {
  float32_t values[2] = {0};
  if (!scene_json_parse_float_array(reader, values, 2))
    return false_v;
  *out_value = vec2_new(values[0], values[1]);
  return true_v;
}

vkr_internal bool8_t scene_json_capture_composite(VkrJsonReader *reader,
                                                  String8 *out_value) {
  if (!reader || !out_value) {
    return false_v;
  }
  vkr_json_skip_whitespace(reader);
  const uint64_t start = reader->pos;
  if (start >= reader->length ||
      (reader->data[start] != '{' && reader->data[start] != '[')) {
    return false_v;
  }

  uint8_t delimiters[64] = {0};
  uint32_t delimiter_count = 0u;
  bool8_t quoted = false_v;
  bool8_t escaped = false_v;
  while (reader->pos < reader->length) {
    const uint8_t c = reader->data[reader->pos++];
    if (quoted) {
      if (escaped) {
        escaped = false_v;
      } else if (c == '\\') {
        escaped = true_v;
      } else if (c == '"') {
        quoted = false_v;
      }
      continue;
    }
    if (c == '"') {
      quoted = true_v;
      continue;
    }
    if (c == '{' || c == '[') {
      if (delimiter_count == ArrayCount(delimiters)) {
        return false_v;
      }
      delimiters[delimiter_count++] = c == '{' ? '}' : ']';
    } else if (c == '}' || c == ']') {
      if (delimiter_count == 0u || delimiters[delimiter_count - 1u] != c) {
        return false_v;
      }
      delimiter_count--;
    }
    if (delimiter_count == 0u) {
      *out_value = (String8){.str = (uint8_t *)(reader->data + start),
                             .length = reader->pos - start};
      return true_v;
    }
  }
  return false_v;
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

vkr_internal SceneRectangleLightImport
scene_rectangle_light_import_defaults(void) {
  return (SceneRectangleLightImport){
      .color = vec3_new(1.0f, 1.0f, 1.0f),
      .radiance = 1.0f,
      .size = vec2_new(1.0f, 1.0f),
      .enabled = true_v,
  };
}

vkr_internal SceneDirectionalLightImport
scene_directional_light_import_defaults(void) {
  return (SceneDirectionalLightImport){
      .color = vec3_new(1.0f, 1.0f, 1.0f),
      .intensity = 1.0f,
      .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
      .sun_angular_diameter_degrees =
          VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES,
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
    world = mat4_mul(world, import->has_matrix ? import->matrix : local);
  }
  return world;
}

vkr_internal bool8_t scene_json_string_equals_cstr(String8 value,
                                                   const char *text) {
  const uint64_t length = text ? string_length(text) : 0u;
  return value.str && value.length == length &&
         MemCompare(value.str, text, length) == 0;
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
      .sh_deringing = 0.0f,
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
  /* Validated and normalized here so lighting never interprets an authored
     value (ADR-038). A bad value is a load-time error naming its path. */
  if (scene_json_read_float_field(&environment_object, "sh_deringing",
                                  &result.sh_deringing) &&
      !(isfinite(result.sh_deringing) && result.sh_deringing >= 0.0f)) {
    log_error("Scene loader: $.environment.sh_deringing must be finite and "
              "greater than or equal to zero");
    result.valid = false_v;
    return result;
  }

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
  VkrJsonReader base_path_reader = cubemap_object;
  VkrJsonReader extension_reader = cubemap_object;
  const bool8_t has_base_path_field =
      vkr_json_find_field(&base_path_reader, "base_path");
  const bool8_t has_extension_field =
      vkr_json_find_field(&extension_reader, "extension");
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
  if (direct == faces ||
      (direct && (has_base_path_field || has_extension_field))) {
    result.valid = false_v;
  } else {
    result.source_kind = VKR_SCENE_ENV_SOURCE_CUBEMAP;
  }

  return result;
}

vkr_internal SceneAtmosphereImport scene_atmosphere_import_defaults(void) {
  VkrAtmosphereSettings settings = vkr_atmosphere_settings_defaults();
  settings.enabled = false_v;
  return (SceneAtmosphereImport){
      .has_block = false_v,
      .valid = true_v,
      .settings = settings,
      .sh_deringing = 0.0f,
  };
}

/* Atmosphere JSON is stricter than a disabled renderer packet: every supplied
   field is parsed and validated even when enabled=false. */
vkr_internal SceneAtmosphereImport
scene_loader_parse_atmosphere_import(String8 json) {
  SceneAtmosphereImport result = scene_atmosphere_import_defaults();
  if (!json.str || !json.length)
    return result;

  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader atmosphere_reader = root;
  if (!vkr_json_find_field(&atmosphere_reader, "atmosphere"))
    return result;
  result.has_block = true_v;
  if (scene_json_parse_null(&atmosphere_reader))
    return result;

  VkrJsonReader atmosphere_object = {0};
  if (!vkr_json_enter_object(&atmosphere_reader, &atmosphere_object))
    goto invalid;

  result.settings.enabled = true_v;
  VkrJsonReader field = atmosphere_object;
  if (vkr_json_find_field(&field, "enabled") &&
      !vkr_json_parse_bool(&field, &result.settings.enabled))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "sun_direction") &&
      !scene_json_parse_vec3(&field, &result.settings.sun_direction))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "solar_irradiance") &&
      !scene_json_parse_vec3(&field, &result.settings.solar_irradiance))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "ground_albedo") &&
      !scene_json_parse_vec3(&field, &result.settings.ground_albedo))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "observer_altitude_m") &&
      !vkr_json_parse_float(&field, &result.settings.observer_altitude_m))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "sun_angular_diameter_degrees") &&
      !vkr_json_parse_float(&field,
                            &result.settings.sun_angular_diameter_degrees))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "rayleigh_density_scale") &&
      !vkr_json_parse_float(&field, &result.settings.rayleigh_density_scale))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "mie_density_scale") &&
      !vkr_json_parse_float(&field, &result.settings.mie_density_scale))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "ozone_density_scale") &&
      !vkr_json_parse_float(&field, &result.settings.ozone_density_scale))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "mie_anisotropy") &&
      !vkr_json_parse_float(&field, &result.settings.mie_anisotropy))
    goto invalid;
  field = atmosphere_object;
  if (vkr_json_find_field(&field, "sh_deringing") &&
      !vkr_json_parse_float(&field, &result.sh_deringing))
    goto invalid;

  VkrAtmosphereSettings validation = result.settings;
  validation.enabled = true_v;
  if (vkr_atmosphere_settings_valid(&validation) &&
      isfinite(result.sh_deringing) && result.sh_deringing >= 0.0f)
    return result;

invalid:
  result.valid = false_v;
  log_error("Scene loader: $.atmosphere must contain a finite nonzero sun "
            "direction; finite nonnegative solar_irradiance; density scales in [0,100]; "
            "ground_albedo in [0,1]; altitude in [0,100000]; angular "
            "diameter in [1e-16,5]; mie_anisotropy in [-.95,.95]; and finite "
            "nonnegative sh_deringing");
  return result;
}

vkr_internal SceneFogImport scene_fog_import_defaults(void) {
  return (SceneFogImport){
      .has_block = false_v,
      .valid = true_v,
      .settings = vkr_fog_settings_defaults(),
  };
}

/* Fog has no resource preparation, but it remains a distinct import so both
   sync and async loads reject malformed authored values before scene mutation.
   The renderer accepts a zeroed disabled packet for old callers; scene JSON is
   stricter and validates every supplied numeric field even when disabled. */
vkr_internal SceneFogImport scene_loader_parse_fog_import(String8 json) {
  SceneFogImport result = scene_fog_import_defaults();
  if (!json.str || json.length == 0u)
    return result;

  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader fog_reader = root;
  if (!vkr_json_find_field(&fog_reader, "fog"))
    return result;

  result.has_block = true_v;
  if (scene_json_parse_null(&fog_reader))
    return result;

  VkrJsonReader fog_object = {0};
  if (!vkr_json_enter_object(&fog_reader, &fog_object))
    goto invalid;

  VkrJsonReader field = fog_object;
  if (vkr_json_find_field(&field, "enabled") &&
      !vkr_json_parse_bool(&field, &result.settings.enabled))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "color") &&
      !scene_json_parse_vec3(&field, &result.settings.color))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "density") &&
      !vkr_json_parse_float(&field, &result.settings.density))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "base_height") &&
      !vkr_json_parse_float(&field, &result.settings.base_height))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "height_falloff") &&
      !vkr_json_parse_float(&field, &result.settings.height_falloff))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "max_distance") &&
      !vkr_json_parse_float(&field, &result.settings.max_distance))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "sky_distance") &&
      !vkr_json_parse_float(&field, &result.settings.sky_distance))
    goto invalid;

  /* Force full authored-value validation: disabled frame packets may be zero
     for backwards compatibility, while an authored fog object cannot hide a
     NaN or invalid distance behind enabled=false. */
  VkrFogSettings validation = result.settings;
  validation.enabled = true_v;
  if (vkr_fog_settings_valid(&validation))
    return result;

invalid:
  result.valid = false_v;
  log_error("Scene loader: $.fog must contain finite nonnegative "
            "color/density/height_falloff, finite base_height, positive "
            "max_distance/sky_distance, and sky_distance <= max_distance");
  return result;
}

vkr_internal SceneFroxelFogImport scene_loader_parse_froxel_fog_import(String8 json) {
  SceneFroxelFogImport result = {.valid = true_v,
      .settings = vkr_froxel_fog_settings_defaults()};
  if (!json.str || json.length == 0u)
    return result;

  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader fog_reader = root;
  if (!vkr_json_find_field(&fog_reader, "volumetric_fog"))
    return result;

  if (scene_json_parse_null(&fog_reader))
    return result;

  VkrJsonReader fog_object = {0};
  if (!vkr_json_enter_object(&fog_reader, &fog_object))
    goto invalid;

  VkrJsonReader field = fog_object;
  if (vkr_json_find_field(&field, "enabled") &&
      !vkr_json_parse_bool(&field, &result.settings.enabled))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "color") &&
      !scene_json_parse_vec3(&field, &result.settings.color))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "density") &&
      !vkr_json_parse_float(&field, &result.settings.density))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "base_height") &&
      !vkr_json_parse_float(&field, &result.settings.base_height))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "height_falloff") &&
      !vkr_json_parse_float(&field, &result.settings.height_falloff))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "max_distance") &&
      !vkr_json_parse_float(&field, &result.settings.max_distance))
    goto invalid;
  field = fog_object;
  if (vkr_json_find_field(&field, "density_boxes")) {
    vkr_json_skip_whitespace(&field);
    if (field.pos >= field.length || field.data[field.pos++] != '[')
      goto invalid;
    while (vkr_json_next_array_element(&field)) {
      if (result.settings.box_count == VKR_FROXEL_FOG_BOX_COUNT_MAX)
        goto invalid;
      VkrJsonReader box_object = {0};
      if (!vkr_json_enter_object(&field, &box_object))
        goto invalid;
      VkrFroxelDensityBox *box =
          &result.settings.boxes[result.settings.box_count++];
      VkrJsonReader member = box_object;
      if (!vkr_json_find_field(&member, "minimum") ||
          !scene_json_parse_vec3(&member, &box->minimum))
        goto invalid;
      member = box_object;
      if (!vkr_json_find_field(&member, "maximum") ||
          !scene_json_parse_vec3(&member, &box->maximum))
        goto invalid;
      member = box_object;
      if (!vkr_json_find_field(&member, "density_multiplier") ||
          !vkr_json_parse_float(&member, &box->density_multiplier))
        goto invalid;
    }
  }

  /* Force full authored-value validation: disabled frame packets may be zero
     for backwards compatibility, while an authored fog object cannot hide a
     NaN or invalid distance behind enabled=false. */
  VkrFroxelFogSettings validation = result.settings;
  validation.enabled = true_v;
  if (vkr_froxel_fog_settings_valid(&validation))
    return result;

invalid:
  result.valid = false_v;
  log_error("Scene loader: $.volumetric_fog requires color in [0,1], "
            "finite nonnegative density/height_falloff, finite base_height, "
            "positive max_distance, and at most 16 ordered finite density boxes");
  return result;
}

vkr_internal SceneDiffuseVolumeImport
scene_diffuse_volume_import_defaults(void) {
  return (SceneDiffuseVolumeImport){
      .has_block = false_v,
      .valid = true_v,
      .path = {0},
  };
}

vkr_internal SceneDiffuseVolumeImport
scene_loader_parse_diffuse_volume_import(String8 json) {
  SceneDiffuseVolumeImport result = scene_diffuse_volume_import_defaults();
  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader volume_reader = root;
  if (!vkr_json_find_field(&volume_reader, "diffuse_volume"))
    return result;

  result.has_block = true_v;
  if (scene_json_parse_null(&volume_reader))
    return result;

  VkrJsonReader volume_object = {0};
  if (!vkr_json_enter_object(&volume_reader, &volume_object) ||
      !scene_json_read_string_field(&volume_object, "path", &result.path) ||
      result.path.length == 0u ||
      !scene_string8_ends_with_cstr_i(result.path, ".vkdv")) {
    result.valid = false_v;
    log_error("Scene loader: $.diffuse_volume.path must name a .vkdv asset");
  }
  return result;
}

vkr_internal SceneSubsurfaceImport scene_subsurface_import_defaults(void) {
  return (SceneSubsurfaceImport){
      .has_block = false_v,
      .valid = true_v,
      .enabled = false_v,
      .profile_count = 0u,
  };
}

vkr_internal SceneSubsurfaceImport
scene_loader_parse_subsurface_import(String8 json) {
  SceneSubsurfaceImport result = scene_subsurface_import_defaults();
  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader subsurface_reader = root;
  if (!vkr_json_find_field(&subsurface_reader, "subsurface"))
    return result;

  result.has_block = true_v;
  if (scene_json_parse_null(&subsurface_reader))
    return result;

  VkrJsonReader subsurface_object = {0};
  if (!vkr_json_enter_object(&subsurface_reader, &subsurface_object))
    goto invalid;

  VkrJsonReader enabled_reader = subsurface_object;
  if (vkr_json_find_field(&enabled_reader, "enabled") &&
      !vkr_json_parse_bool(&enabled_reader, &result.enabled))
    goto invalid;
  if (!result.enabled)
    return result;

  VkrJsonReader profiles_reader = subsurface_object;
  if (!vkr_json_find_field(&profiles_reader, "profiles"))
    goto invalid;
  vkr_json_skip_whitespace(&profiles_reader);
  if (profiles_reader.pos >= profiles_reader.length ||
      profiles_reader.data[profiles_reader.pos++] != '[')
    goto invalid;
  while (vkr_json_next_array_element(&profiles_reader)) {
    if (result.profile_count == VKR_SUBSURFACE_PROFILE_COUNT ||
        !scene_json_parse_vec3(
            &profiles_reader,
            &result.profiles[result.profile_count].diffusion_distance) ||
        !vkr_subsurface_profile_valid(result.profiles[result.profile_count]))
      goto invalid;
    ++result.profile_count;
  }
  vkr_json_skip_whitespace(&profiles_reader);
  if (profiles_reader.pos >= profiles_reader.length ||
      profiles_reader.data[profiles_reader.pos] != ']')
    goto invalid;
  if (result.profile_count == 0u)
    goto invalid;
  return result;

invalid:
  result.valid = false_v;
  log_error("Scene loader: active $.subsurface requires one to eight positive "
            "finite RGB diffusion-distance profiles");
  return result;
}

vkr_internal uint32_t scene_loader_diffuse_volume_cell_index(
    const VkrDiffuseVolume *volume, uint32_t x, uint32_t y, uint32_t z) {
  const uint32_t width = volume->dimensions[0] - 1u;
  const uint32_t height = volume->dimensions[1] - 1u;
  return x + width * (y + height * z);
}

vkr_internal bool8_t
scene_loader_prepare_diffuse_volume(String8 path, VkrAllocator *temp_alloc,
                                    VkrDiffuseVolumeBinding *out_binding,
                                    VkrTexturePreparedLoad *out_prepared) {
  if (!path.str || path.length == 0u || !temp_alloc || !out_binding ||
      !out_prepared)
    return false_v;
  *out_binding = (VkrDiffuseVolumeBinding){
      .texture = VKR_TEXTURE_HANDLE_INVALID,
  };
  MemZero(out_prepared, sizeof(*out_prepared));

  String8 terminated_path = string8_duplicate(temp_alloc, &path);
  if (!terminated_path.str) {
    log_error("Scene loader: diffuse volume path allocation failed for '%.*s'",
              (int)path.length, path.str);
    return false_v;
  }
  FilePath file_path = vkr_asset_path_file(temp_alloc, terminated_path);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError file_error = file_open(&file_path, mode, &handle);
  if (file_error != FILE_ERROR_NONE) {
    log_error("Scene loader: diffuse volume open failed for '%.*s': %.*s",
              (int)path.length, path.str,
              (int)file_get_error_string(file_error).length,
              file_get_error_string(file_error).str);
    return false_v;
  }

  uint8_t *bytes = NULL;
  uint64_t byte_count = 0u;
  file_error = file_read_all(&handle, temp_alloc, &bytes, &byte_count);
  file_close(&handle);
  if (file_error != FILE_ERROR_NONE || !bytes || byte_count == 0u) {
    log_error("Scene loader: diffuse volume read failed for '%.*s': %.*s",
              (int)path.length, path.str,
              (int)file_get_error_string(file_error).length,
              file_get_error_string(file_error).str);
    return false_v;
  }

  Arena *decode_arena = arena_create(KB(64), KB(4));
  if (!decode_arena) {
    log_error("Scene loader: diffuse volume decode arena allocation failed for "
              "'%.*s'",
              (int)path.length, path.str);
    return false_v;
  }
  VkrDiffuseVolume volume = {0};
  const bool8_t decoded =
      vkr_diffuse_volume_decode(bytes, byte_count, decode_arena, &volume);
  if (!decoded) {
    arena_destroy(decode_arena);
    log_error("Scene loader: diffuse volume validation failed for '%.*s'",
              (int)path.length, path.str);
    return false_v;
  }
  const Vec3 inverse_spacing = {1.0f / volume.spacing.x,
                                1.0f / volume.spacing.y,
                                1.0f / volume.spacing.z};
  if (!isfinite(inverse_spacing.x) || !isfinite(inverse_spacing.y) ||
      !isfinite(inverse_spacing.z)) {
    arena_destroy(decode_arena);
    log_error("Scene loader: diffuse volume spacing is too small for '%.*s'",
              (int)path.length, path.str);
    return false_v;
  }

  const uint64_t texel_count = (uint64_t)volume.probe_count * 8u;
  const uint64_t pixel_bytes = texel_count * 4u * sizeof(float32_t);
  float32_t *pixels = (float32_t *)malloc((size_t)pixel_bytes);
  VkrTextureUploadRegion *region =
      (VkrTextureUploadRegion *)malloc(sizeof(*region));
  if (!pixels || !region) {
    free(region);
    free(pixels);
    arena_destroy(decode_arena);
    log_error(
        "Scene loader: diffuse volume upload allocation failed for '%.*s'",
        (int)path.length, path.str);
    return false_v;
  }

  for (uint32_t probe_index = 0u; probe_index < volume.probe_count;
       ++probe_index) {
    const VkrDiffuseVolumeProbe *probe = &volume.probes[probe_index];
    float32_t *row = pixels + (uint64_t)probe_index * 8u * 4u;
    for (uint32_t vector = 0u; vector < VKR_SH_PACKED_VECTOR_COUNT; ++vector)
      for (uint32_t component = 0u; component < 4u; ++component)
        row[vector * 4u + component] = probe->sh.v[vector][component];

    const uint32_t x = probe_index % volume.dimensions[0];
    const uint32_t yz = probe_index / volume.dimensions[0];
    const uint32_t y = yz % volume.dimensions[1];
    const uint32_t z = yz / volume.dimensions[1];
    const uint32_t cell_region =
        x + 1u < volume.dimensions[0] && y + 1u < volume.dimensions[1] &&
                z + 1u < volume.dimensions[2]
            ? volume.cell_region_ids[scene_loader_diffuse_volume_cell_index(
                  &volume, x, y, z)]
            : 0u;
    row[7u * 4u + 0u] = (float32_t)probe->region_id;
    row[7u * 4u + 1u] = (float32_t)cell_region;
    row[7u * 4u + 2u] = 0.0f;
    row[7u * 4u + 3u] = 0.0f;
  }

  *region = (VkrTextureUploadRegion){
      .mip_level = 0u,
      .array_layer = 0u,
      .width = 8u,
      .height = volume.probe_count,
      .depth = 1u,
      .byte_offset = 0u,
      .byte_size = pixel_bytes,
  };
  *out_prepared = (VkrTexturePreparedLoad){
      .description =
          {
              .width = 8u,
              .height = volume.probe_count,
              .channels = 4u,
              .mip_levels = 1u,
              .array_layers = 1u,
              .type = VKR_TEXTURE_TYPE_2D,
              .format = VKR_TEXTURE_FORMAT_R32G32B32A32_SFLOAT,
              .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
              .sample_count = VKR_SAMPLE_COUNT_1,
              .properties = vkr_texture_property_flags_create(),
              .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .min_filter = VKR_FILTER_NEAREST,
              .mag_filter = VKR_FILTER_NEAREST,
              .mip_filter = VKR_MIP_FILTER_NONE,
              .anisotropy_enable = false_v,
          },
      .upload_data = (uint8_t *)pixels,
      .upload_data_size = pixel_bytes,
      .upload_regions = region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
      .upload_is_compressed = false_v,
  };
  *out_binding = (VkrDiffuseVolumeBinding){
      .texture = VKR_TEXTURE_HANDLE_INVALID,
      .origin = volume.origin,
      .inverse_spacing = inverse_spacing,
      .dimensions = {volume.dimensions[0], volume.dimensions[1],
                     volume.dimensions[2]},
  };
  arena_destroy(decode_arena);
  return true_v;
}

vkr_internal void scene_loader_apply_diffuse_volume_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneDiffuseVolumeImport *import,
    const VkrDiffuseVolumeBinding *binding,
    const VkrTexturePreparedLoad *prepared) {
  if (!scene)
    return;
  vkr_scene_reset_diffuse_volume(scene, assets);
  if (!import || !import->has_block || !import->valid)
    return;
  if (!assets || !binding || !prepared) {
    log_error("Scene loader: diffuse volume '%.*s' is unavailable",
              (int)import->path.length, import->path.str);
    return;
  }

  VkrTextureHandle texture = VKR_TEXTURE_HANDLE_INVALID;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_finalize_prepared_load(
          &assets->texture_system, import->path, prepared, &texture, &error)) {
    String8 error_text = vkr_renderer_get_error_string(error);
    log_error("Scene loader: diffuse volume upload failed for '%.*s': %.*s",
              (int)import->path.length, import->path.str,
              (int)error_text.length, error_text.str);
    return;
  }
  vkr_texture_system_add_ref_by_handle(&assets->texture_system, texture);

  scene->diffuse_volume = *binding;
  scene->diffuse_volume.texture = texture;
}

vkr_internal bool8_t scene_loader_subsurface_texture_key(
    const SceneSubsurfaceImport *import, char storage[256u],
    String8 *out_key) {
  if (!import || !storage || !out_key)
    return false_v;

  int32_t written = snprintf(storage, 256u, "__scene_subsurface_%u",
                             import->profile_count);
  if (written < 0 || (uint32_t)written >= 256u)
    return false_v;
  uint32_t length = (uint32_t)written;
  for (uint32_t profile = 0u; profile < import->profile_count; ++profile) {
    const Vec3 distance = import->profiles[profile].diffusion_distance;
    const float32_t components[3] = {distance.x, distance.y, distance.z};
    for (uint32_t component = 0u; component < ArrayCount(components);
         ++component) {
      uint32_t bits = 0u;
      MemCopy(&bits, &components[component], sizeof(bits));
      written = snprintf(storage + length, 256u - length, "_%08x", bits);
      if (written < 0 || (uint32_t)written >= 256u - length)
        return false_v;
      length += (uint32_t)written;
    }
  }
  *out_key = (String8){
      .str = (uint8_t *)storage,
      .length = length,
  };
  return true_v;
}

vkr_internal bool8_t scene_loader_apply_subsurface_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneSubsurfaceImport *import, VkrRendererError *out_error) {
  if (!scene || !assets || !import || !out_error) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  *out_error = VKR_RENDERER_ERROR_NONE;
  if (!import->valid) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (scene->subsurface.texture.id != 0u) {
    const bool8_t released = vkr_texture_system_release_by_handle(
        &assets->texture_system, scene->subsurface.texture);
    /* The release consumes the scene reference even when native destruction
       fails; the texture system retains that zero-reference entry. */
    scene->subsurface = (VkrSubsurfaceBinding){
        .texture = VKR_TEXTURE_HANDLE_INVALID,
    };
    if (!released) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return false_v;
    }
  }
  if (!import->enabled || import->profile_count == 0u)
    return true_v;

  VkrSubsurfaceTable table = {0};
  if (!vkr_subsurface_table_build(import->profiles, import->profile_count,
                                  &table)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  char key_storage[256u] = {0};
  String8 texture_key = {0};
  if (!scene_loader_subsurface_texture_key(import, key_storage, &texture_key)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrTextureUploadRegion region = {
      .mip_level = 0u,
      .array_layer = 0u,
      .width = VKR_SUBSURFACE_TABLE_WIDTH,
      .height = VKR_SUBSURFACE_TABLE_HEIGHT,
      .depth = 1u,
      .byte_offset = 0u,
      .byte_size = VKR_SUBSURFACE_TABLE_BYTE_COUNT,
  };
  const VkrTexturePreparedLoad prepared = {
      .description =
          {
              .id = VKR_INVALID_ID,
              .generation = VKR_INVALID_ID,
              .width = VKR_SUBSURFACE_TABLE_WIDTH,
              .height = VKR_SUBSURFACE_TABLE_HEIGHT,
              .channels = 4u,
              .mip_levels = 1u,
              .array_layers = 1u,
              .type = VKR_TEXTURE_TYPE_2D,
              .format = VKR_TEXTURE_FORMAT_R32G32B32A32_SFLOAT,
              .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
              .sample_count = VKR_SAMPLE_COUNT_1,
              .properties = vkr_texture_property_flags_create(),
              .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
              .min_filter = VKR_FILTER_NEAREST,
              .mag_filter = VKR_FILTER_NEAREST,
              .mip_filter = VKR_MIP_FILTER_NONE,
              .anisotropy_enable = false_v,
          },
      .upload_data = (uint8_t *)&table,
      .upload_data_size = VKR_SUBSURFACE_TABLE_BYTE_COUNT,
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
      .upload_is_compressed = false_v,
  };
  VkrTextureHandle texture = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_texture_system_finalize_prepared_load(
          &assets->texture_system, texture_key, &prepared, &texture,
          out_error)) {
    return false_v;
  }

  /* The texture system copies this stack-backed table before returning. */
  vkr_texture_system_add_ref_by_handle(&assets->texture_system, texture);
  scene->subsurface = (VkrSubsurfaceBinding){
      .texture = texture,
      .profile_count = import->profile_count,
  };
  return true_v;
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
      .sh_deringing = 0.0f,
      .has_cubemap = false_v,
      .cubemap_path = {0},
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
    if (scene_json_read_float_field(&probe_object, "sh_deringing",
                                    &import.sh_deringing) &&
        !(isfinite(import.sh_deringing) && import.sh_deringing >= 0.0f)) {
      log_error("Scene loader: $.reflection_probes[%u].sh_deringing must be "
                "finite and greater than or equal to zero",
                input_index);
      return false_v;
    }

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

        VkrJsonReader base_path_reader = cubemap_object;
        VkrJsonReader extension_reader = cubemap_object;
        const bool8_t has_base_path_field =
            vkr_json_find_field(&base_path_reader, "base_path");
        const bool8_t has_extension_field =
            vkr_json_find_field(&extension_reader, "extension");
        const bool8_t has_path = scene_json_read_string_field(
            &cubemap_object, "path", &import.cubemap_path);
        const bool8_t has_base_path = scene_json_read_string_field(
            &cubemap_object, "base_path", &import.cubemap_base_path);
        const bool8_t has_extension = scene_json_read_string_field(
            &cubemap_object, "extension", &import.cubemap_extension);
        const bool8_t direct = has_path && import.cubemap_path.length > 0u;
        const bool8_t faces = has_base_path && has_extension &&
                              import.cubemap_base_path.length > 0u &&
                              import.cubemap_extension.length > 0u;
        if (direct == faces ||
            (direct && (has_base_path_field || has_extension_field))) {
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

vkr_internal void
scene_loader_reset_scene_environment(VkrScene *scene, VkrRenderAssets *assets) {
  if (!scene) {
    return;
  }

  if (assets) {
    if (scene->atmosphere.candidate_prefilter_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system,
          scene->atmosphere.candidate_prefilter_cubemap);
    }
    if (scene->atmosphere.candidate_source_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system, scene->atmosphere.candidate_source_cubemap);
    }
    if (scene->atmosphere.retired_environment.prefilter_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system,
          scene->atmosphere.retired_environment.prefilter_cubemap);
    }
    if (scene->atmosphere.retired_environment.source_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system,
          scene->atmosphere.retired_environment.source_cubemap);
    }
    if (scene->atmosphere.retired_environment.delivery_equirect.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system,
          scene->atmosphere.retired_environment.delivery_equirect);
    }
    if (scene->environment.prefilter_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system, scene->environment.prefilter_cubemap);
    }
    if (scene->environment.source_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(&assets->texture_system,
                                           scene->environment.source_cubemap);
    }
    if (scene->environment.delivery_equirect.id != 0) {
      vkr_texture_system_release_by_handle(
          &assets->texture_system, scene->environment.delivery_equirect);
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
                            .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                            .bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE};
  scene->atmosphere = (VkrSceneAtmosphere){
      .requested_settings = vkr_atmosphere_settings_defaults(),
      .candidate_settings = vkr_atmosphere_settings_defaults(),
      .active_settings = vkr_atmosphere_settings_defaults(),
      .retired_environment = {.source_kind = VKR_SCENE_ENV_SOURCE_NONE,
                              .delivery_equirect = VKR_TEXTURE_HANDLE_INVALID,
                              .source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                              .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
                              .bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE},
      .candidate_source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
      .candidate_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
  };
  scene->atmosphere.requested_settings.enabled = false_v;
  scene->atmosphere.candidate_settings.enabled = false_v;
  scene->atmosphere.active_settings.enabled = false_v;
}

vkr_internal void scene_loader_apply_environment_import(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneEnvironmentImport *environment_import,
    const VkrTexturePreparedLoad *prepared_environment) {
  if (!scene) {
    return;
  }

  scene_loader_reset_scene_environment(scene, assets);
  if (!environment_import || !environment_import->has_block) {
    return;
  }

  scene->environment.intensity = environment_import->intensity;
  scene->environment.diffuse_intensity = environment_import->diffuse_intensity;
  scene->environment.specular_intensity =
      environment_import->specular_intensity;
  scene->environment.sh_deringing = environment_import->sh_deringing;
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

  if (!assets) {
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
      if (!upload &&
          !vkr_texture_system_prepare_load_from_file(
              &assets->texture_system, environment_import->cubemap_path,
              VKR_TEXTURE_RGBA_CHANNELS, &assets->scratch_allocator,
              &local_prepared, &cubemap_error)) {
        goto failed;
      }
      upload = upload ? upload : &local_prepared;
      const bool8_t valid =
          upload->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
      const bool8_t finalized =
          valid &&
          vkr_texture_system_finalize_prepared_load(
              &assets->texture_system, environment_import->cubemap_path, upload,
              &source_cubemap, &cubemap_error);
      vkr_texture_system_release_prepared_load(&local_prepared);
      if (!finalized) {
        goto failed;
      }
      vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                           source_cubemap);
    } else if (!vkr_texture_system_load_cube_map(
                   &assets->texture_system,
                   environment_import->cubemap_base_path,
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
              &assets->texture_system, environment_import->equirect_path,
              VKR_TEXTURE_RGBA_CHANNELS, &assets->scratch_allocator,
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
        &assets->texture_system, environment_import->equirect_path, upload,
        &delivery, &texture_error);
    vkr_texture_system_release_prepared_load(&local_prepared);
    if (!finalized) {
      goto failed;
    }
    vkr_texture_system_add_ref_by_handle(&assets->texture_system, delivery);
    scene->environment.delivery_equirect = delivery;
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_SOURCE_LOADING;
  } else {
    goto failed;
  }

  if (!vkr_world_resources_prepare_scene_environment(
          assets, &assets->world_resources, scene)) {
    goto failed;
  }
  if (environment_import->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    log_info("Prepared scene HDR skybox environment: %.*s",
             (int)environment_import->equirect_path.length,
             environment_import->equirect_path.str);
  }
  return;

failed:
  scene_loader_reset_scene_environment(scene, assets);
  scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

vkr_internal bool8_t scene_loader_apply_atmosphere_import(
    VkrScene *scene, const SceneAtmosphereImport *atmosphere_import) {
  if (!scene || !atmosphere_import || !atmosphere_import->has_block)
    return true_v;
  if (!atmosphere_import->valid ||
      !vkr_scene_request_atmosphere(scene, &atmosphere_import->settings,
                                    atmosphere_import->sh_deringing)) {
    log_error("Scene loader: rejected $.atmosphere before scene mutation");
    return false_v;
  }
  return true_v;
}

vkr_internal void
scene_loader_reset_scene_reflection_probes(VkrScene *scene,
                                           struct VkrRenderAssets *assets) {
  if (!scene) {
    return;
  }

  if (assets) {
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (probe->prefilter_cubemap.id != 0) {
        vkr_texture_system_release_by_handle(&assets->texture_system,
                                             probe->prefilter_cubemap);
      }
      if (probe->source_cubemap.id != 0) {
        vkr_texture_system_release_by_handle(&assets->texture_system,
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
        .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE,
    };
  }
}

vkr_internal void scene_loader_apply_reflection_probe_imports(
    VkrScene *scene, struct VkrRenderAssets *assets,
    const SceneReflectionProbeImport *imports, uint32_t import_count,
    const VkrTexturePreparedLoad *prepared_cubemaps,
    const bool8_t *prepared_cubemaps_ready) {
  if (!scene) {
    return;
  }

  scene_loader_reset_scene_reflection_probes(scene, assets);
  if (!imports || import_count == 0) {
    return;
  }

  VkrTextureHandle environment_source = VKR_TEXTURE_HANDLE_INVALID;
  bool8_t environment_source_valid = false_v;
  if (assets && scene->environment.source_cubemap.id != 0) {
    VkrTexture *environment_texture = vkr_texture_system_get_by_handle(
        &assets->texture_system, scene->environment.source_cubemap);
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
        .sh_deringing = import->sh_deringing,
        .source_mip_count = 1u,
        .uses_scene_environment_source = false_v,
        .source_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID,
        .bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE,
    };

    if (probe.enabled) {
      if (!assets) {
        log_warn(
            "Scene loader: renderer missing while applying reflection probe %u",
            i);
        probe.enabled = false_v;
      } else {
        VkrTextureHandle source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
        bool8_t source_valid = false_v;
        if (import->has_cubemap) {
          VkrRendererError cubemap_error = VKR_RENDERER_ERROR_NONE;
          if (import->cubemap_path.length > 0u) {
            VkrTexturePreparedLoad local_prepared = {0};
            const VkrTexturePreparedLoad *upload =
                prepared_cubemaps && prepared_cubemaps_ready &&
                        prepared_cubemaps_ready[i]
                    ? &prepared_cubemaps[i]
                    : &local_prepared;
            const bool8_t prepared =
                upload != &local_prepared ||
                vkr_texture_system_prepare_load_from_file(
                    &assets->texture_system, import->cubemap_path,
                    VKR_TEXTURE_RGBA_CHANNELS, &assets->scratch_allocator,
                    &local_prepared, &cubemap_error);
            if (prepared &&
                upload->description.type == VKR_TEXTURE_TYPE_CUBE_MAP &&
                vkr_texture_system_finalize_prepared_load(
                    &assets->texture_system, import->cubemap_path, upload,
                    &source_cubemap, &cubemap_error)) {
              vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                                   source_cubemap);
              source_valid = true_v;
            } else {
              if (prepared &&
                  upload->description.type != VKR_TEXTURE_TYPE_CUBE_MAP) {
                cubemap_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
              }
              String8 err_str = vkr_renderer_get_error_string(cubemap_error);
              log_warn("Scene loader: reflection probe %u cubemap load failed "
                       "for '%.*s': %.*s",
                       i, (int)import->cubemap_path.length,
                       import->cubemap_path.str, (int)err_str.length,
                       err_str.str);
            }
            vkr_texture_system_release_prepared_load(&local_prepared);
          } else if (vkr_texture_system_load_cube_map(
                         &assets->texture_system, import->cubemap_base_path,
                         import->cubemap_extension, &source_cubemap,
                         &cubemap_error)) {
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
          vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                               source_cubemap);
          probe.source_mip_count = scene->environment.source_mip_count;
          probe.uses_scene_environment_source = true_v;
          /* An aliasing probe shares the environment's published coefficients
             and inherits its window; it must not carry a conflicting one
             (ADR-038). */
          if (probe.sh_deringing != 0.0f) {
            log_warn("Scene loader: $.reflection_probes[%u].sh_deringing is "
                     "ignored because the probe aliases the scene environment "
                     "source and inherits its window",
                     i);
            probe.sh_deringing = scene->environment.sh_deringing;
          }
          source_valid = true_v;
        } else {
          log_warn(
              "Scene loader: reflection probe %u has no cubemap and no valid "
              "environment source",
              i);
        }

        if (source_valid && !probe.uses_scene_environment_source) {
          VkrTexture *source_texture = vkr_texture_system_get_by_handle(
              &assets->texture_system, source_cubemap);
          if (!source_texture || !source_texture->handle ||
              source_texture->description.type != VKR_TEXTURE_TYPE_CUBE_MAP ||
              source_texture->description.mip_levels == 0u) {
            vkr_texture_system_release_by_handle(&assets->texture_system,
                                                 source_cubemap);
            source_valid = false_v;
          } else {
            probe.source_mip_count = source_texture->description.mip_levels;
          }
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

vkr_internal bool8_t scene_json_parse_transform(
    const VkrJsonReader *entity_reader, uint32_t entity_index,
    SceneEntityImport *out_entity) {
  VkrJsonReader transform_reader = *entity_reader;
  if (!vkr_json_find_field(&transform_reader, "transform")) {
    return true_v;
  }

  VkrJsonReader transform_obj = {0};
  if (!vkr_json_enter_object(&transform_reader, &transform_obj)) {
    log_warn("Scene loader: entity %u transform is not an object",
             entity_index);
    return true_v;
  }

  VkrJsonReader matrix_reader = transform_obj;
  if (vkr_json_find_field(&matrix_reader, "matrix")) {
    const char *trs_fields[] = {"pos", "rot", "scale"};
    for (uint32_t i = 0; i < ArrayCount(trs_fields); ++i) {
      VkrJsonReader field = transform_obj;
      if (vkr_json_find_field(&field, trs_fields[i])) {
        return false_v;
      }
    }
    matrix_reader = transform_obj;
    if (!vkr_json_find_array(&matrix_reader, "matrix")) {
      return false_v;
    }
    for (uint32_t i = 0; i < 16u; ++i) {
      if (!vkr_json_next_array_element(&matrix_reader) ||
          !vkr_json_parse_float(&matrix_reader,
                                &out_entity->matrix.elements[i]) ||
          !isfinite(out_entity->matrix.elements[i])) {
        return false_v;
      }
    }
    if (vkr_json_next_array_element(&matrix_reader)) {
      return false_v;
    }
    out_entity->has_matrix = true_v;
    return true_v;
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
  return true_v;
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

  VkrJsonReader range_overrides_reader = mesh_obj;
  if (vkr_json_find_field(&range_overrides_reader,
                          "gltf_light_range_overrides")) {
    out_entity->has_gltf_light_range_overrides = true_v;
    if (!scene_json_capture_composite(
            &range_overrides_reader, &out_entity->gltf_light_range_overrides)) {
      out_entity->gltf_light_range_overrides_invalid = true_v;
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

  scene_json_read_bool_field(&point_light_obj, "casts_shadow",
                             &out_entity->point_light.casts_shadow);
  scene_json_read_float_field(&point_light_obj, "range",
                              &out_entity->point_light.range);
  scene_json_read_vec3_field(&point_light_obj, "direction_local",
                             &out_entity->point_light.direction_local);
  scene_json_read_float_field(&point_light_obj, "inner_cone_angle",
                              &out_entity->point_light.inner_cone_angle);
  scene_json_read_float_field(&point_light_obj, "outer_cone_angle",
                              &out_entity->point_light.outer_cone_angle);
  float32_t kind = (float32_t)out_entity->point_light.kind;
  if (scene_json_read_float_field(&point_light_obj, "kind", &kind) &&
      (kind == 0.0f || kind == 1.0f || kind == 2.0f))
    out_entity->point_light.kind = (VkrPointLightKind)kind;
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

vkr_internal bool8_t scene_json_parse_rectangle_light(
    const VkrJsonReader *entity_reader, uint32_t entity_index,
    SceneEntityImport *out_entity) {
  VkrJsonReader rectangle_reader = *entity_reader;
  if (!vkr_json_find_field(&rectangle_reader, "rectangle_light"))
    return true_v;
  if (scene_json_parse_null(&rectangle_reader))
    return true_v;

  VkrJsonReader object = {0};
  if (!vkr_json_enter_object(&rectangle_reader, &object)) {
    log_error("Scene loader: entity %u rectangle_light is not an object",
              entity_index);
    return false_v;
  }
  SceneRectangleLightImport light = scene_rectangle_light_import_defaults();
  VkrJsonReader field = object;
  if (vkr_json_find_field(&field, "enabled") &&
      !vkr_json_parse_bool(&field, &light.enabled))
    goto invalid;
  field = object;
  if (vkr_json_find_field(&field, "color") &&
      !scene_json_parse_vec3(&field, &light.color))
    goto invalid;
  field = object;
  if (vkr_json_find_field(&field, "radiance") &&
      !vkr_json_parse_float(&field, &light.radiance))
    goto invalid;
  field = object;
  if (vkr_json_find_field(&field, "size") &&
      !scene_json_parse_vec2(&field, &light.size))
    goto invalid;
  if (light.enabled > 1u || !isfinite(light.color.x) ||
      !isfinite(light.color.y) || !isfinite(light.color.z) ||
      light.color.x < 0.0f || light.color.y < 0.0f || light.color.z < 0.0f ||
      !isfinite(light.radiance) || light.radiance < 0.0f ||
      !isfinite(light.size.x) || !isfinite(light.size.y) ||
      light.size.x <= 0.0f || light.size.y <= 0.0f)
    goto invalid;
  out_entity->has_rectangle_light = true_v;
  out_entity->rectangle_light = light;
  return true_v;

invalid:
  log_error("Scene loader: entity %u rectangle_light must have finite "
            "nonnegative color/radiance and positive size",
            entity_index);
  return false_v;
}

vkr_internal bool8_t scene_json_parse_directional_light(
    const VkrJsonReader *entity_reader, uint32_t entity_index,
    SceneEntityImport *out_entity) {
  VkrJsonReader dir_light_reader = *entity_reader;
  if (!vkr_json_find_field(&dir_light_reader, "directional_light")) {
    return true_v;
  }

  if (scene_json_parse_null(&dir_light_reader)) {
    return true_v;
  }

  VkrJsonReader dir_light_obj = {0};
  if (!vkr_json_enter_object(&dir_light_reader, &dir_light_obj)) {
    log_warn("Scene loader: entity %u directional_light is not an object",
             entity_index);
    return true_v;
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

  VkrJsonReader sun_reader = dir_light_obj;
  if (vkr_json_find_field(&sun_reader, "sun_angular_diameter_degrees")) {
    float32_t sun_angular_diameter_degrees = 0.0f;
    if (!vkr_json_parse_float(&sun_reader, &sun_angular_diameter_degrees) ||
        !isfinite(sun_angular_diameter_degrees) ||
        sun_angular_diameter_degrees < 0.0f ||
        sun_angular_diameter_degrees >= 180.0f) {
      log_error("Scene loader: entity %u directional_light "
                "sun_angular_diameter_degrees must be finite and in [0, 180)",
                entity_index);
      return false_v;
    }
    out_entity->directional_light.sun_angular_diameter_degrees =
        sun_angular_diameter_degrees;
  }
  return true_v;
}

vkr_internal bool8_t scene_json_parse_entity(const VkrJsonReader *entity_reader,
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
  if (!scene_json_parse_transform(entity_reader, entity_index, out_entity)) {
    return false_v;
  }
  scene_json_parse_mesh(entity_reader, entity_index, out_entity);
  scene_json_parse_text3d(entity_reader, entity_index, out_entity);
  scene_json_parse_shape(entity_reader, entity_index, out_entity);
  scene_json_parse_point_light(entity_reader, entity_index, out_entity);
  if (!scene_json_parse_rectangle_light(entity_reader, entity_index,
                                        out_entity))
    return false_v;
  return scene_json_parse_directional_light(entity_reader, entity_index,
                                            out_entity);
}

vkr_internal bool8_t scene_loader_source_fingerprint(String8 json,
                                                     uint64_t *out_hash) {
  String8 source = json;
  VkrJsonReader reader = vkr_json_reader_from_string(json);
  if (vkr_json_find_field(&reader, "source_identity")) {
    if (!vkr_json_parse_string(&reader, &source) || source.length != 36u) {
      return false_v;
    }
    for (uint32_t i = 0; i < 36u; ++i) {
      const bool8_t separator = i == 8u || i == 13u || i == 18u || i == 23u;
      const uint8_t c = source.str[i];
      if ((separator && c != '-') ||
          (!separator && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))) {
        return false_v;
      }
    }
  }
  uint64_t hash = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < source.length; ++i) {
    hash = (hash ^ source.str[i]) * UINT64_C(1099511628211);
  }
  *out_hash = hash;
  return true_v;
}

bool8_t vkr_scene_instantiate_source_nodes(VkrScene *scene,
                                           const VkrMeshSource *source,
                                           VkrEntityId wrapper,
                                           uint32_t scene_entity_index,
                                           VkrEntityId *out_nodes,
                                           VkrSceneError *out_error) {
  if (!scene || !source || !out_nodes ||
      !vkr_scene_entity_alive(scene, wrapper))
    return false_v;
  const SceneSourceIdentity *wrapper_source = vkr_entity_get_component(
      scene->world, wrapper, scene->comp_source_identity);
  uint64_t fingerprint =
      wrapper_source ? wrapper_source->source_fingerprint : 0u;
  if (fingerprint) {
    for (uint32_t i = 0; i < 8u; ++i)
      fingerprint = (fingerprint ^ ((source->fingerprint >> (i * 8u)) & 255u)) *
                    UINT64_C(1099511628211);
  } else {
    fingerprint = source->fingerprint;
  }
  SceneSourceIdentity identity = {
      .scene_entity_index = scene_entity_index,
      .gltf_node_index = UINT32_MAX,
      .gltf_mesh_index = UINT32_MAX,
      .gltf_camera_index = UINT32_MAX,
      .gltf_skin_index = UINT32_MAX,
      .gltf_light_index = UINT32_MAX,
      .source_fingerprint = fingerprint,
  };
  const SceneSourceIdentity wrapper_identity = identity;
  for (uint32_t i = 0; i < source->nodes.length; ++i)
    out_nodes[i] = VKR_ENTITY_ID_INVALID;
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    if (!node->in_scene)
      continue;
    VkrEntityId entity = vkr_scene_create_entity(scene, out_error);
    if (entity.u64 == VKR_ENTITY_ID_INVALID.u64)
      goto cleanup;
    out_nodes[i] = entity;
    identity.gltf_node_index = i;
    identity.gltf_mesh_index = node->mesh;
    identity.gltf_camera_index = node->camera;
    identity.gltf_skin_index = node->skin;
    identity.gltf_light_index = node->light;
    if (!vkr_scene_set_name(scene, entity, node->name) ||
        !vkr_scene_set_local_matrix(scene, entity, node->local) ||
        !vkr_scene_set_source_identity(scene, entity, &identity))
      goto cleanup;
    vkr_scene_set_visibility(scene, entity, true_v, true_v);
    if (!vkr_entity_has_component(scene->world, entity, scene->comp_visibility))
      goto cleanup;
    if (node->punctual.kind == 1u) {
      SceneDirectionalLight light = {
          .color = node->punctual.color,
          .intensity = node->punctual.intensity,
          .direction_local = {0, 0, -1},
          .sun_angular_diameter_degrees =
              VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES,
          .enabled = true_v};
      if (!vkr_scene_set_directional_light(scene, entity, &light))
        goto cleanup;
    } else if (node->punctual.kind == 2u || node->punctual.kind == 3u) {
      const bool8_t casts_shadow =
          isfinite(node->punctual.range) && node->punctual.range > 0.0f &&
          (node->punctual.kind == 2u ||
           (isfinite(node->punctual.outer_cone) &&
            node->punctual.outer_cone > 0.0f &&
            node->punctual.outer_cone < 1.57079632679f));
      ScenePointLight light = {.casts_shadow = casts_shadow,
                               .color = node->punctual.color,
                               .intensity = node->punctual.intensity,
                               .constant = 1.0f,
                               .range = node->punctual.range,
                               .direction_local = {0, 0, -1},
                               .inner_cone_angle = node->punctual.inner_cone,
                               .outer_cone_angle = node->punctual.outer_cone,
                               .kind = node->punctual.kind == 3u
                                           ? VKR_POINT_LIGHT_KIND_GLTF_SPOT
                                           : VKR_POINT_LIGHT_KIND_GLTF_POINT,
                               .enabled = true_v};
      if (!vkr_scene_set_point_light(scene, entity, &light))
        goto cleanup;
    }
  }
  if (!vkr_scene_set_source_identity(scene, wrapper, &wrapper_identity))
    goto cleanup;
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    if (node->in_scene)
      vkr_scene_set_parent(
          scene, out_nodes[i],
          node->parent == UINT32_MAX ? wrapper : out_nodes[node->parent]);
  }
  return true_v;

cleanup:
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    if (out_nodes[i].u64 == VKR_ENTITY_ID_INVALID.u64)
      continue;
    SceneName *name = vkr_entity_get_component_mut(scene->world, out_nodes[i],
                                                   scene->comp_name);
    if (name && name->name.str)
      vkr_allocator_free(scene->alloc, name->name.str, name->name.length + 1u,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    vkr_scene_destroy_entity(scene, out_nodes[i]);
    out_nodes[i] = VKR_ENTITY_ID_INVALID;
  }
  if (out_error && *out_error == VKR_SCENE_ERROR_NONE)
    *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
  return false_v;
}

vkr_internal bool8_t scene_loader_attach_source_mesh(
    VkrScene *scene, VkrRenderAssets *assets, VkrEntityId entity,
    const VkrMeshLoadDesc *desc, const VkrResourceHandleInfo *resource,
    bool8_t shadow_static, VkrRendererError *out_error) {
  VkrMeshInstanceHandle instance =
      vkr_mesh_manager_create_instance_from_resource(
          &assets->mesh_manager, desc, resource, 0u, true_v, out_error);
  if (!instance.id)
    return false_v;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  if (!vkr_scene_track_instance(scene, instance, &error)) {
    (void)vkr_mesh_manager_destroy_instance(&assets->mesh_manager, instance);
    *out_error = scene_error_to_renderer_error(error);
    return false_v;
  }
  if (!vkr_scene_set_mesh_renderer(scene, entity, instance)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }
  (void)vkr_mesh_manager_instance_set_shadow_mobility(
      &assets->mesh_manager, instance,
      shadow_static ? VKR_SHADOW_CASTER_MOBILITY_STATIC
                    : VKR_SHADOW_CASTER_MOBILITY_DYNAMIC);
  return true_v;
}

static bool8_t scene_resolve_path(VkrAllocator *allocator, String8 owner,
                                  String8 *path) {
  if (!path->length) {
    return true_v;
  }
  String8 resolved = vkr_asset_path_resolve(allocator, owner, *path);
  if (!resolved.str) {
    log_error("Scene dependency path is invalid or too long: '%.*s'",
              (int)path->length, path->str);
    return false_v;
  }
  *path = resolved;
  return true_v;
}

static bool8_t
scene_resolve_environment_paths(VkrAllocator *allocator, String8 owner,
                                SceneEnvironmentImport *environment) {
  return scene_resolve_path(allocator, owner, &environment->cubemap_path) &&
         scene_resolve_path(allocator, owner,
                            &environment->cubemap_base_path) &&
         scene_resolve_path(allocator, owner, &environment->equirect_path);
}

static bool8_t scene_resolve_probe_paths(VkrAllocator *allocator, String8 owner,
                                         SceneReflectionProbeImport *probes,
                                         uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (!scene_resolve_path(allocator, owner, &probes[i].cubemap_path) ||
        !scene_resolve_path(allocator, owner, &probes[i].cubemap_base_path)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t scene_resolve_entity_paths(VkrAllocator *allocator,
                                          String8 owner,
                                          SceneEntityImport *entity) {
  return scene_resolve_path(allocator, owner, &entity->mesh_path) &&
         scene_resolve_path(allocator, owner, &entity->shape.material_path) &&
         scene_resolve_path(allocator, owner, &entity->text3d.font_name);
}

static bool8_t
scene_load_json_owned(VkrScene *scene, struct VkrRenderAssets *assets,
                      String8 json, String8 owner, VkrAllocator *temp_alloc,
                      VkrSceneLoadResult *out_result, VkrSceneError *out_error);

bool8_t vkr_scene_load_from_file(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 path,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error) {
  if (out_result) {
    *out_result = (VkrSceneLoadResult){0};
  }
  if (!scene || !assets || !temp_alloc || !path.str) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

  FilePath file_path = vkr_asset_path_file(temp_alloc, path);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError fe = file_open(&file_path, mode, &handle);
  if (fe != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_FILE_NOT_FOUND;
    log_error("Scene loader: failed to open '%.*s': %s", (int)path.length,
              path.str, file_get_error_string(fe).str);
    return false_v;
  }

  String8 json = {0};
  fe = file_read_string(&handle, temp_alloc, &json);
  file_close(&handle);
  if (fe != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_FILE_READ_FAILED;
    log_error("Scene loader: failed to read '%.*s': %s", (int)path.length,
              path.str, file_get_error_string(fe).str);
    return false_v;
  }

  return scene_load_json_owned(scene, assets, json, path, temp_alloc,
                               out_result, out_error);
}

bool8_t vkr_scene_load_from_json(VkrScene *scene,
                                 struct VkrRenderAssets *assets, String8 json,
                                 VkrAllocator *temp_alloc,
                                 VkrSceneLoadResult *out_result,
                                 VkrSceneError *out_error) {
  return scene_load_json_owned(scene, assets, json, (String8){0}, temp_alloc,
                               out_result, out_error);
}

static bool8_t scene_load_json_owned(VkrScene *scene,
                                     struct VkrRenderAssets *assets,
                                     String8 json, String8 owner,
                                     VkrAllocator *temp_alloc,
                                     VkrSceneLoadResult *out_result,
                                     VkrSceneError *out_error) {
  if (out_result) {
    *out_result = (VkrSceneLoadResult){0};
  }
  if (!scene || !scene->world || !assets || !temp_alloc || !json.str) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
    return false_v;
  }

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

  uint64_t scene_source_fingerprint = 0;
  if (!scene_loader_source_fingerprint(json, &scene_source_fingerprint)) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    return false_v;
  }
  const SceneFogImport fog_import = scene_loader_parse_fog_import(json);
  if (!fog_import.valid) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }
  const SceneFroxelFogImport froxel_fog_import =
      scene_loader_parse_froxel_fog_import(json);
  if (!froxel_fog_import.valid) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }
  const SceneSubsurfaceImport subsurface_import =
      scene_loader_parse_subsurface_import(json);
  if (!subsurface_import.valid) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }
  scene->fog = fog_import.settings;
  scene->froxel_fog = froxel_fog_import.settings;

  // Store renderer frontend reference for layer message sending only after
  // root-level authored settings have passed their input boundary.
  scene->assets = assets;

  const SceneAtmosphereImport atmosphere_import =
      scene_loader_parse_atmosphere_import(json);
  if (!atmosphere_import.valid) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }
  if (atmosphere_import.settings.enabled) {
    /* A procedural atmosphere is the global source; do not load an HDR source
       that this scene generation will immediately replace. */
    scene_loader_reset_scene_environment(scene, assets);
  } else {
    SceneEnvironmentImport environment_import =
        scene_loader_parse_environment_import(json);
    if (!scene_resolve_environment_paths(temp_alloc, owner,
                                         &environment_import)) {
      if (out_error) {
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      }
      return false_v;
    }
    scene_loader_apply_environment_import(scene, assets, &environment_import,
                                          NULL);
  }
  if (!scene_loader_apply_atmosphere_import(scene, &atmosphere_import)) {
    if (out_error)
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    return false_v;
  }

  SceneDiffuseVolumeImport diffuse_volume_import =
      scene_loader_parse_diffuse_volume_import(json);
  if (!scene_resolve_path(temp_alloc, owner, &diffuse_volume_import.path)) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    return false_v;
  }
  VkrDiffuseVolumeBinding diffuse_volume_binding = {
      .texture = VKR_TEXTURE_HANDLE_INVALID,
  };
  VkrTexturePreparedLoad diffuse_volume_prepared = {0};
  if (diffuse_volume_import.has_block && diffuse_volume_import.valid &&
      !scene_loader_prepare_diffuse_volume(diffuse_volume_import.path,
                                           temp_alloc, &diffuse_volume_binding,
                                           &diffuse_volume_prepared)) {
    diffuse_volume_import.valid = false_v;
  }
  scene_loader_apply_diffuse_volume_import(
      scene, assets, &diffuse_volume_import, &diffuse_volume_binding,
      diffuse_volume_prepared.upload_data ? &diffuse_volume_prepared : NULL);
  vkr_texture_system_release_prepared_load(&diffuse_volume_prepared);

  VkrRendererError subsurface_error = VKR_RENDERER_ERROR_NONE;
  if (!scene_loader_apply_subsurface_import(scene, assets, &subsurface_import,
                                            &subsurface_error)) {
    String8 error_text = vkr_renderer_get_error_string(subsurface_error);
    log_error("Scene loader: subsurface upload failed: %.*s",
              (int)error_text.length, error_text.str);
    if (out_error)
      *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
    return false_v;
  }

  SceneReflectionProbeImport
      reflection_probe_imports[VKR_SCENE_REFLECTION_PROBE_MAX] = {0};
  uint32_t reflection_probe_import_count =
      scene_loader_parse_reflection_probe_imports(json,
                                                  reflection_probe_imports);
  if (!scene_resolve_probe_paths(temp_alloc, owner, reflection_probe_imports,
                                 reflection_probe_import_count)) {
    if (out_error) {
      *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
    }
    return false_v;
  }
  scene_loader_apply_reflection_probe_imports(
      scene, assets, reflection_probe_imports, reflection_probe_import_count,
      NULL, NULL);
  (void)vkr_world_resources_prepare_scene_reflection_probes(
      assets, &assets->world_resources, scene);

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
  uint32_t rectangle_light_count = 0;
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

    if (!scene_json_parse_entity(&entity_obj, parsed, &imports[parsed])) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      return false_v;
    }
    if (!scene_resolve_entity_paths(temp_alloc, owner, &imports[parsed])) {
      if (out_error) {
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      }
      return false_v;
    }
    if (imports[parsed].has_rectangle_light &&
        ++rectangle_light_count > VKR_MAX_SCENE_RECTANGLE_LIGHTS) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      log_error("Scene loader: scene exceeds %u rectangle lights",
                VKR_MAX_SCENE_RECTANGLE_LIGHTS);
      return false_v;
    }
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
    vkr_scene_set_visibility(scene, entity, true_v, true_v);
    if (!vkr_entity_has_component(scene->world, entity, scene->comp_visibility))
      return false_v;

    if (!vkr_scene_set_name(scene, entity, imports[i].name)) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_ALLOC_FAILED;
      log_error("Scene loader: failed to set name for entity %u", i);
      return false_v;
    }

    const bool8_t transformed =
        imports[i].has_matrix
            ? vkr_scene_set_local_matrix(scene, entity, imports[i].matrix)
            : vkr_scene_set_transform(scene, entity, imports[i].position,
                                      imports[i].rotation, imports[i].scale);
    if (!transformed) {
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

  uint32_t loaded_meshes = 0;
  uint32_t imported_nodes = 0;
  for (uint32_t i = 0; i < entity_count; ++i) {
    SceneSourceIdentity identity = {.scene_entity_index = i,
                                    .gltf_node_index = UINT32_MAX,
                                    .gltf_mesh_index = UINT32_MAX,
                                    .gltf_camera_index = UINT32_MAX,
                                    .gltf_skin_index = UINT32_MAX,
                                    .gltf_light_index = UINT32_MAX,
                                    .source_fingerprint =
                                        scene_source_fingerprint};
    if (!vkr_scene_set_source_identity(scene, entity_ids[i], &identity))
      return false_v;
    if (!imports[i].has_mesh)
      continue;
    VkrResourceHandleInfo resource = {0}, resolved = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, imports[i].mesh_path,
                                  temp_alloc, &resource, &error))
      return false_v;
    resolved = resource;
    bool8_t success = resource.as.mesh || vkr_resource_system_try_get_resolved(
                                              &resource, &resolved);
    VkrMeshLoadDesc desc = {.mesh_path = imports[i].mesh_path,
                            .pipeline_domain = imports[i].pipeline_domain,
                            .shader_override = imports[i].shader_override,
                            .transform = vkr_transform_identity()};
    if (success && resolved.as.mesh->source.nodes.length) {
      const VkrMeshSource *source = &resolved.as.mesh->source;
      VkrEntityId *nodes = vkr_allocator_alloc(
          temp_alloc, source->nodes.length * sizeof(VkrEntityId),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      success = nodes && vkr_scene_instantiate_source_nodes(
                             scene, source, entity_ids[i], i, nodes, out_error);
      for (uint32_t n = 0; success && n < source->nodes.length; ++n) {
        const VkrMeshSourceNode *node = &source->nodes.data[n];
        if (!node->in_scene)
          continue;
        imported_nodes++;
        if (node->mesh_variant == UINT32_MAX ||
            !source->meshes.data[node->mesh_variant].range_count)
          continue;
        desc.source_mesh_index_plus_one = node->mesh_variant + 1u;
        success = scene_loader_attach_source_mesh(
            scene, assets, nodes[n], &desc, &resolved,
            imports[i].shadow_caster_static, &error);
        loaded_meshes += success;
      }
    } else if (success) {
      success = scene_loader_attach_source_mesh(
          scene, assets, entity_ids[i], &desc, &resolved,
          imports[i].shadow_caster_static, &error);
      loaded_meshes += success;
    }
    vkr_resource_system_unload(&resource, imports[i].mesh_path);
    if (!success) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_MESH_LOAD_FAILED;
      return false_v;
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
      font = vkr_font_system_acquire(&assets->font_system, font_name_copy,
                                     true_v, &font_err);
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
    if (!vkr_scene_set_shape(scene, assets, entity, &shape_config,
                             &shape_err)) {
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
        .casts_shadow = light_import->casts_shadow,
        .kind = light_import->kind,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_point_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set point light for entity %u", i);
      continue;
    }

    loaded_point_lights++;
  }

  // Load rectangle light components
  for (uint32_t i = 0; i < entity_count; i++) {
    if (!imports[i].has_rectangle_light)
      continue;
    const SceneRectangleLightImport *light_import = &imports[i].rectangle_light;
    const SceneRectangleLight light = {
        .color = light_import->color,
        .radiance = light_import->radiance,
        .size = light_import->size,
        .enabled = light_import->enabled,
    };
    if (!vkr_scene_set_rectangle_light(scene, entity_ids[i], &light)) {
      if (out_error)
        *out_error = VKR_SCENE_ERROR_COMPONENT_ADD_FAILED;
      log_error("Scene loader: failed to set rectangle light for entity %u", i);
      return false_v;
    }
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
        .sun_angular_diameter_degrees =
            light_import->sun_angular_diameter_degrees,
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
    out_result->entity_count = entity_count + imported_nodes;
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
  uint32_t rectangle_light_count = 0;
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

    if (!scene_json_parse_entity(&entity_obj, parsed, &imports[parsed])) {
      vkr_allocator_free_ts(allocator, imports, import_bytes,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
      if (out_error)
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      return false_v;
    }
    if (imports[parsed].has_rectangle_light &&
        ++rectangle_light_count > VKR_MAX_SCENE_RECTANGLE_LIGHTS) {
      vkr_allocator_free_ts(allocator, imports, import_bytes,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY, mutex);
      if (out_error)
        *out_error = VKR_SCENE_ERROR_PARSE_FAILED;
      log_error("Scene loader: scene exceeds %u rectangle lights",
                VKR_MAX_SCENE_RECTANGLE_LIGHTS);
      return false_v;
    }
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
  if (!payload || !payload->assets || !out_error) {
    return false_v;
  }

  if (payload->scene_handle) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  VkrSceneHandle handle = vkr_scene_handle_create(&payload->assets->allocator,
                                                  0, 64, 256, &scene_error);
  if (!handle) {
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }

  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  if (!scene) {
    vkr_scene_handle_destroy(handle, payload->assets);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  scene->assets = payload->assets;
  payload->scene_handle = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal void
scene_loader_sync_partial(VkrSceneLoaderAsyncPayload *payload) {
  if (!payload || !payload->scene_handle || !payload->assets) {
    return;
  }

  vkr_scene_handle_update_and_sync(payload->scene_handle, payload->assets, 0.0);
}

vkr_internal bool8_t scene_loader_apply_component_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error) {
  if (!payload || !payload->assets || !payload->scene_handle || !out_error) {
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
      vkr_allocator_begin_scope(&payload->assets->scratch_allocator);
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
          &payload->assets->scratch_allocator, &text_import->font_name);
      VkrRendererError font_err = VKR_RENDERER_ERROR_NONE;
      font = vkr_font_system_acquire(&payload->assets->font_system,
                                     font_name_copy, true_v, &font_err);
      if (font.id == 0) {
        log_warn("Scene loader: entity %u text3d font '%.*s' not found, using "
                 "default",
                 entity_index, (int)text_import->font_name.length,
                 text_import->font_name.str);
        font = VKR_FONT_HANDLE_INVALID;
      }
    }

    String8 text_copy = string8_duplicate(&payload->assets->scratch_allocator,
                                          &text_import->text);
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
    if (!vkr_scene_set_shape(scene, payload->assets, entity, &shape_config,
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
        .casts_shadow = light_import->casts_shadow,
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
        .sun_angular_diameter_degrees =
            light_import->sun_angular_diameter_degrees,
        .enabled = light_import->enabled,
    };

    if (!vkr_scene_set_directional_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set directional light for entity %u",
                entity_index);
    } else {
      payload->load_result.directional_light_count++;
    }
  }

  if (entity_import->has_rectangle_light) {
    const SceneRectangleLightImport *light_import =
        &entity_import->rectangle_light;
    const SceneRectangleLight light = {
        .color = light_import->color,
        .radiance = light_import->radiance,
        .size = light_import->size,
        .enabled = light_import->enabled,
    };
    if (!vkr_scene_set_rectangle_light(scene, entity, &light)) {
      log_error("Scene loader: failed to set rectangle light for entity %u",
                entity_index);
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
      return false_v;
    }
  }

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t scene_loader_attach_mesh_for_entity(
    VkrSceneLoaderAsyncPayload *payload, uint32_t entity_index,
    VkrRendererError *out_error) {
  if (!payload || !payload->assets || !payload->scene_handle || !out_error) {
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

  VkrResourceHandleInfo resolved = mesh_state->request_info;
  if (!resolved.as.mesh && !vkr_resource_system_try_get_resolved(
                               &mesh_state->request_info, &resolved)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }
  const VkrMeshSource *source = &resolved.as.mesh->source;
  if (source->nodes.length) {
    VkrScene *scene = vkr_scene_handle_get_scene(payload->scene_handle);
    if (!mesh_state->source_nodes) {
      mesh_state->source_node_count = (uint32_t)source->nodes.length;
      mesh_state->source_nodes = vkr_allocator_alloc_ts(
          &payload->assets->scene_async_allocator,
          source->nodes.length * sizeof(VkrEntityId),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->assets->scene_async_mutex);
      VkrSceneError error = VKR_SCENE_ERROR_NONE;
      if (!mesh_state->source_nodes ||
          !vkr_scene_instantiate_source_nodes(
              scene, source, payload->entity_ids[entity_index], entity_index,
              mesh_state->source_nodes, &error)) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }
      for (uint32_t n = 0; n < source->nodes.length; ++n) {
        const VkrMeshSourceNode *node = &source->nodes.data[n];
        payload->load_result.entity_count += node->in_scene;
        payload->load_result.directional_light_count +=
            node->in_scene && node->punctual.kind == 1u;
        payload->load_result.point_light_count +=
            node->in_scene && node->punctual.kind >= 2u;
      }
    }
    uint32_t end = Min(mesh_state->source_cursor + SCENE_ASYNC_ENTITY_CHUNK,
                       mesh_state->source_node_count);
    for (; mesh_state->source_cursor < end; ++mesh_state->source_cursor) {
      uint32_t n = mesh_state->source_cursor;
      const VkrMeshSourceNode *node = &source->nodes.data[n];
      if (!node->in_scene || node->mesh_variant == UINT32_MAX ||
          !source->meshes.data[node->mesh_variant].range_count)
        continue;
      VkrMeshLoadDesc desc = {.mesh_path = entity_import->mesh_path,
                              .source_mesh_index_plus_one =
                                  node->mesh_variant + 1u,
                              .pipeline_domain = entity_import->pipeline_domain,
                              .shader_override = entity_import->shader_override,
                              .transform = vkr_transform_identity()};
      if (!scene_loader_attach_source_mesh(
              scene, payload->assets, mesh_state->source_nodes[n], &desc,
              &resolved, entity_import->shadow_caster_static, out_error))
        return false_v;
      payload->load_result.mesh_count++;
    }
    mesh_state->attached = end == mesh_state->source_node_count;
    *out_error = mesh_state->attached ? VKR_RENDERER_ERROR_NONE
                                      : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return mesh_state->attached;
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
          &payload->assets->mesh_manager, &mesh_desc, &mesh_state->request_info,
          0, true_v, &mesh_error);
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
      &payload->assets->mesh_manager, instance,
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

static bool8_t scene_resolve_async_paths(VkrSceneLoaderAsyncPayload *payload,
                                         String8 owner, VkrAllocator *scratch) {
  uint64_t count = (uint64_t)payload->entity_count * 3 + 4 +
                   (uint64_t)payload->reflection_probe_import_count * 2;
  String8 **paths = vkr_allocator_alloc(scratch, count * sizeof(*paths),
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!paths) {
    return false_v;
  }
  uint64_t cursor = 0;
  paths[cursor++] = &payload->environment_import.cubemap_path;
  paths[cursor++] = &payload->environment_import.cubemap_base_path;
  paths[cursor++] = &payload->environment_import.equirect_path;
  paths[cursor++] = &payload->diffuse_volume_import.path;
  for (uint32_t i = 0; i < payload->entity_count; ++i) {
    paths[cursor++] = &payload->imports[i].mesh_path;
    paths[cursor++] = &payload->imports[i].shape.material_path;
    paths[cursor++] = &payload->imports[i].text3d.font_name;
  }
  for (uint32_t i = 0; i < payload->reflection_probe_import_count; ++i) {
    paths[cursor++] = &payload->reflection_probe_imports[i].cubemap_path;
    paths[cursor++] = &payload->reflection_probe_imports[i].cubemap_base_path;
  }
  uint64_t bytes = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (!scene_resolve_path(scratch, owner, paths[i])) {
      return false_v;
    }
    bytes += paths[i]->length ? paths[i]->length + 1 : 0;
  }
  if (!bytes) {
    return true_v;
  }
  payload->path_storage = vkr_allocator_alloc_ts(
      &payload->assets->scene_async_allocator, bytes,
      VKR_ALLOCATOR_MEMORY_TAG_STRING, payload->assets->scene_async_mutex);
  if (!payload->path_storage) {
    return false_v;
  }
  payload->path_storage_size = bytes;
  uint8_t *destination = payload->path_storage;
  for (uint64_t i = 0; i < count; ++i) {
    if (paths[i]->length) {
      MemCopy(destination, paths[i]->str, paths[i]->length + 1);
      paths[i]->str = destination;
      destination += paths[i]->length + 1;
    }
  }
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

  struct VkrRenderAssets *assets =
      (struct VkrRenderAssets *)self->resource_system;
  if (!assets) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  FilePath file_path = vkr_asset_path_file(temp_alloc, name);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError file_error = file_open(&file_path, mode, &handle);
  if (file_error != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("Scene loader: failed to open '%.*s': %s", (int)name.length,
              name.str, file_get_error_string(file_error).str);
    return false_v;
  }

  String8 json = {0};
  file_error = file_read_string(&handle, temp_alloc, &json);
  file_close(&handle);
  if (file_error != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_UNKNOWN;
    log_error("Scene loader: failed to read '%.*s': %s", (int)name.length,
              name.str, file_get_error_string(file_error).str);
    return false_v;
  }

  VkrSceneLoaderAsyncPayload *payload =
      (VkrSceneLoaderAsyncPayload *)vkr_allocator_alloc_ts(
          &assets->scene_async_allocator, sizeof(*payload),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, assets->scene_async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));
  payload->assets = assets;
  payload->environment_import = scene_environment_import_defaults();
  payload->atmosphere_import = scene_atmosphere_import_defaults();
  payload->fog_import = scene_fog_import_defaults();
  payload->diffuse_volume_import = scene_diffuse_volume_import_defaults();
  payload->subsurface_import = scene_subsurface_import_defaults();
  payload->reflection_probe_import_count = 0;
  payload->reflection_probes_applied = false_v;
  payload->stage = SCENE_ASYNC_STAGE_CREATE_ENTITIES;
  payload->stage_cursor = 0;
  payload->ownership_transferred = false_v;

  String8 json_copy = {0};
  if (!scene_loader_alloc_copy_string(&assets->scene_async_allocator,
                                      assets->scene_async_mutex, json,
                                      &payload->json_storage, &json_copy)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  payload->json_length = json_copy.length;
  if (!scene_loader_source_fingerprint(json_copy, &payload->scene_source_fingerprint)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  payload->fog_import = scene_loader_parse_fog_import(json_copy);
  payload->froxel_fog_import = scene_loader_parse_froxel_fog_import(json_copy);
  if (!payload->fog_import.valid || !payload->froxel_fog_import.valid) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  payload->subsurface_import = scene_loader_parse_subsurface_import(json_copy);
  if (!payload->subsurface_import.valid) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSceneError scene_error = VKR_SCENE_ERROR_NONE;
  if (!scene_loader_parse_json_imports(
          &assets->scene_async_allocator, assets->scene_async_mutex, json_copy,
          &payload->imports, &payload->entity_count, &payload->imports_capacity,
          &scene_error)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = scene_error_to_renderer_error(scene_error);
    return false_v;
  }
  for (uint32_t index = 0; index < payload->entity_count; ++index) {
    const SceneEntityImport *entity = &payload->imports[index];
    if (entity->gltf_light_source.length ||
        entity->has_gltf_light_range_overrides) {
      log_error("Scene loader: entity %u contains source glTF light authoring; "
                "bake resolved lights before runtime loading",
                index);
      scene_loader_destroy_async_payload(payload);
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return false_v;
    }
  }
  payload->atmosphere_import = scene_loader_parse_atmosphere_import(json_copy);
  if (!payload->atmosphere_import.valid) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
  payload->environment_import =
      payload->atmosphere_import.settings.enabled
          ? scene_environment_import_defaults()
          : scene_loader_parse_environment_import(json_copy);
  payload->diffuse_volume_import =
      scene_loader_parse_diffuse_volume_import(json_copy);
  payload->reflection_probe_import_count =
      scene_loader_parse_reflection_probe_imports(
          json_copy, payload->reflection_probe_imports);
  if (!scene_resolve_async_paths(payload, name, temp_alloc)) {
    scene_loader_destroy_async_payload(payload);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }
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
            &assets->texture_system, environment_path,
            VKR_TEXTURE_RGBA_CHANNELS, temp_alloc,
            &payload->environment_prepared, &environment_error) &&
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
  if (payload->diffuse_volume_import.has_block &&
      payload->diffuse_volume_import.valid) {
    payload->diffuse_volume_prepared_ready =
        scene_loader_prepare_diffuse_volume(payload->diffuse_volume_import.path,
                                            temp_alloc,
                                            &payload->diffuse_volume_binding,
                                            &payload->diffuse_volume_prepared);
    if (!payload->diffuse_volume_prepared_ready)
      payload->diffuse_volume_import.valid = false_v;
  }
  for (uint32_t i = 0u; i < payload->reflection_probe_import_count; ++i) {
    SceneReflectionProbeImport *probe = &payload->reflection_probe_imports[i];
    if (!probe->enabled || !probe->has_cubemap ||
        probe->cubemap_path.length == 0u) {
      continue;
    }
    VkrRendererError cubemap_error = VKR_RENDERER_ERROR_NONE;
    if (vkr_texture_system_prepare_load_from_file(
            &assets->texture_system, probe->cubemap_path,
            VKR_TEXTURE_RGBA_CHANNELS, temp_alloc,
            &payload->reflection_probe_prepared[i], &cubemap_error)) {
      if (payload->reflection_probe_prepared[i].description.type ==
          VKR_TEXTURE_TYPE_CUBE_MAP) {
        payload->reflection_probe_prepared_ready[i] = true_v;
        continue;
      }
      cubemap_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    vkr_texture_system_release_prepared_load(
        &payload->reflection_probe_prepared[i]);
    String8 err_str = vkr_renderer_get_error_string(cubemap_error);
    log_warn("Scene loader: reflection probe %u cubemap preparation failed "
             "for '%.*s': %.*s",
             i, (int)probe->cubemap_path.length, probe->cubemap_path.str,
             (int)err_str.length, err_str.str);
    probe->enabled = false_v;
  }

  payload->load_result.entity_count = payload->entity_count;

  if (payload->entity_count > 0) {
    uint64_t entity_id_bytes = sizeof(VkrEntityId) * payload->entity_count;
    uint64_t mesh_state_bytes =
        sizeof(SceneMeshAsyncState) * payload->entity_count;
    uint64_t shape_state_bytes =
        sizeof(SceneShapeMaterialAsyncState) * payload->entity_count;
    payload->entity_ids = (VkrEntityId *)vkr_allocator_alloc_ts(
        &assets->scene_async_allocator, entity_id_bytes,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, assets->scene_async_mutex);
    payload->mesh_states = (SceneMeshAsyncState *)vkr_allocator_alloc_ts(
        &assets->scene_async_allocator, mesh_state_bytes,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, assets->scene_async_mutex);
    payload->shape_material_states =
        (SceneShapeMaterialAsyncState *)vkr_allocator_alloc_ts(
            &assets->scene_async_allocator, shape_state_bytes,
            VKR_ALLOCATOR_MEMORY_TAG_ARRAY, assets->scene_async_mutex);
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

  if (!async_payload->fog_applied) {
    scene->fog = async_payload->fog_import.settings;
    scene->froxel_fog = async_payload->froxel_fog_import.settings;
    async_payload->fog_applied = true_v;
  }

  if (!async_payload->environment_applied) {
    scene_loader_apply_environment_import(
        scene, async_payload->assets, &async_payload->environment_import,
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
  if (!async_payload->atmosphere_applied) {
    if (!scene_loader_apply_atmosphere_import(
            scene, &async_payload->atmosphere_import)) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      return false_v;
    }
    async_payload->atmosphere_applied = true_v;
  }
  if (!async_payload->diffuse_volume_applied) {
    scene_loader_apply_diffuse_volume_import(
        scene, async_payload->assets, &async_payload->diffuse_volume_import,
        &async_payload->diffuse_volume_binding,
        async_payload->diffuse_volume_prepared_ready
            ? &async_payload->diffuse_volume_prepared
            : NULL);
    if (async_payload->diffuse_volume_prepared_ready) {
      vkr_texture_system_release_prepared_load(
          &async_payload->diffuse_volume_prepared);
      async_payload->diffuse_volume_prepared_ready = false_v;
    }
    async_payload->diffuse_volume_applied = true_v;
  }
  if (!async_payload->subsurface_applied) {
    if (!scene_loader_apply_subsurface_import(
            scene, async_payload->assets, &async_payload->subsurface_import,
            out_error)) {
      return false_v;
    }
    async_payload->subsurface_applied = true_v;
  }
  if (!async_payload->reflection_probes_applied) {
    scene_loader_apply_reflection_probe_imports(
        scene, async_payload->assets, async_payload->reflection_probe_imports,
        async_payload->reflection_probe_import_count,
        async_payload->reflection_probe_prepared,
        async_payload->reflection_probe_prepared_ready);
    for (uint32_t i = 0u; i < async_payload->reflection_probe_import_count;
         ++i) {
      if (async_payload->reflection_probe_prepared_ready[i]) {
        vkr_texture_system_release_prepared_load(
            &async_payload->reflection_probe_prepared[i]);
        async_payload->reflection_probe_prepared_ready[i] = false_v;
      }
    }
    (void)vkr_world_resources_prepare_scene_reflection_probes(
        async_payload->assets, &async_payload->assets->world_resources, scene);
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
      vkr_scene_set_visibility(scene, entity, true_v, true_v);
      if (!vkr_entity_has_component(scene->world, entity,
                                    scene->comp_visibility)) {
        *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }
      const SceneSourceIdentity source_identity = {
          .scene_entity_index = i,
          .gltf_node_index = UINT32_MAX,
          .gltf_mesh_index = UINT32_MAX,
          .gltf_camera_index = UINT32_MAX,
          .gltf_skin_index = UINT32_MAX,
          .gltf_light_index = UINT32_MAX,
          .source_fingerprint = async_payload->scene_source_fingerprint};
      if (!vkr_scene_set_source_identity(scene, entity, &source_identity)) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
        return false_v;
      }

      if (!vkr_scene_set_name(scene, entity, async_payload->imports[i].name)) {
        *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        return false_v;
      }

      const SceneEntityImport *import = &async_payload->imports[i];
      const bool8_t transformed =
          import->has_matrix
              ? vkr_scene_set_local_matrix(scene, entity, import->matrix)
              : vkr_scene_set_transform(scene, entity, import->position,
                                        import->rotation, import->scale);
      if (!transformed) {
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
    async_payload->stage_cursor = 0;
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

  MemZero(out_cost, sizeof(*out_cost));
  VkrSceneLoaderAsyncPayload *async_payload =
      (VkrSceneLoaderAsyncPayload *)payload;
  if (async_payload->diffuse_volume_prepared_ready) {
    out_cost->gpu_upload_bytes +=
        async_payload->diffuse_volume_prepared.upload_data_size;
    out_cost->gpu_upload_ops += 1u;
  }
  if (!async_payload->subsurface_applied &&
      async_payload->subsurface_import.valid &&
      async_payload->subsurface_import.enabled &&
      async_payload->subsurface_import.profile_count > 0u) {
    out_cost->gpu_upload_bytes += VKR_SUBSURFACE_TABLE_BYTE_COUNT;
    out_cost->gpu_upload_ops += 1u;
  }
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
  if (payload->diffuse_volume_prepared_ready) {
    vkr_texture_system_release_prepared_load(&payload->diffuse_volume_prepared);
    payload->diffuse_volume_prepared_ready = false_v;
  }
  for (uint32_t i = 0u; i < payload->reflection_probe_import_count; ++i) {
    if (payload->reflection_probe_prepared_ready[i]) {
      vkr_texture_system_release_prepared_load(
          &payload->reflection_probe_prepared[i]);
      payload->reflection_probe_prepared_ready[i] = false_v;
    }
  }

  if (payload->imports) {
    if (payload->mesh_states) {
      for (uint32_t i = 0; i < payload->entity_count; ++i) {
        SceneMeshAsyncState *mesh_state = &payload->mesh_states[i];
        if (mesh_state->source_nodes) {
          vkr_allocator_free_ts(
              &payload->assets->scene_async_allocator, mesh_state->source_nodes,
              (uint64_t)mesh_state->source_node_count * sizeof(VkrEntityId),
              VKR_ALLOCATOR_MEMORY_TAG_ARRAY,
              payload->assets->scene_async_mutex);
          mesh_state->source_nodes = NULL;
        }

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
    vkr_scene_handle_destroy(payload->scene_handle, payload->assets);
    payload->scene_handle = VKR_SCENE_HANDLE_INVALID;
  }

  if (payload->shape_material_states) {
    vkr_allocator_free_ts(
        &payload->assets->scene_async_allocator, payload->shape_material_states,
        sizeof(SceneShapeMaterialAsyncState) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->assets->scene_async_mutex);
    payload->shape_material_states = NULL;
  }
  if (payload->mesh_states) {
    vkr_allocator_free_ts(
        &payload->assets->scene_async_allocator, payload->mesh_states,
        sizeof(SceneMeshAsyncState) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->assets->scene_async_mutex);
    payload->mesh_states = NULL;
  }
  if (payload->entity_ids) {
    vkr_allocator_free_ts(
        &payload->assets->scene_async_allocator, payload->entity_ids,
        sizeof(VkrEntityId) * payload->entity_count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->assets->scene_async_mutex);
    payload->entity_ids = NULL;
  }
  if (payload->imports) {
    vkr_allocator_free_ts(
        &payload->assets->scene_async_allocator, payload->imports,
        sizeof(SceneEntityImport) * payload->imports_capacity,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY, payload->assets->scene_async_mutex);
    payload->imports = NULL;
    payload->imports_capacity = 0;
  }
  if (payload->path_storage) {
    vkr_allocator_free_ts(&payload->assets->scene_async_allocator,
                          payload->path_storage, payload->path_storage_size,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING,
                          payload->assets->scene_async_mutex);
    payload->path_storage = NULL;
    payload->path_storage_size = 0;
  }
  if (payload->json_storage) {
    vkr_allocator_free_ts(&payload->assets->scene_async_allocator,
                          payload->json_storage, payload->json_length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING,
                          payload->assets->scene_async_mutex);
    payload->json_storage = NULL;
    payload->json_length = 0;
  }
}

vkr_internal void
scene_loader_destroy_async_payload(VkrSceneLoaderAsyncPayload *payload) {
  if (!payload) {
    return;
  }

  struct VkrRenderAssets *assets = payload->assets;
  scene_loader_destroy_async_payload_contents(payload);
  vkr_allocator_free_ts(&assets->scene_async_allocator, payload,
                        sizeof(*payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        assets->scene_async_mutex);
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

  struct VkrRenderAssets *assets =
      (struct VkrRenderAssets *)self->resource_system;
  if (!assets) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSceneError scene_err = VKR_SCENE_ERROR_NONE;
  VkrSceneHandle handle =
      vkr_scene_handle_create(&assets->allocator, 0, 64, 256, &scene_err);
  if (!handle) {
    *out_error = scene_error_to_renderer_error(scene_err);
    return false_v;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(temp_alloc);
  bool8_t scoped = vkr_allocator_scope_is_valid(&scope);

  VkrSceneLoadResult load_result = {0};
  VkrScene *scene = vkr_scene_handle_get_scene(handle);
  bool8_t loaded = vkr_scene_load_from_file(scene, assets, name, temp_alloc,
                                            &load_result, &scene_err);

  if (scoped) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (!loaded) {
    log_error("Scene loader: failed to load '%s' (error=%d)",
              string8_cstr(&name), (int)scene_err);
    vkr_scene_handle_destroy(handle, assets);
    *out_error = scene_error_to_renderer_error(scene_err);
    return false_v;
  }

  // Sync through the incremental path; async load uses staged partial syncs.
  vkr_scene_handle_update(handle, 0.0);
  vkr_scene_handle_sync(handle, assets);

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

  struct VkrRenderAssets *assets =
      (struct VkrRenderAssets *)self->resource_system;
  vkr_scene_handle_destroy(handle->as.scene, assets);
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

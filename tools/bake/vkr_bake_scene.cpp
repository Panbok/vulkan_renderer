#include "bake/vkr_bake_scene.h"

#include "bake/vkr_bake_mesh_decode.h"

extern "C" {
#include "core/vkr_json.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr uint32_t k_max_scene_entities = 65536u;
constexpr uint32_t k_max_transform_depth = 256u;

enum class ShapeKind : uint8_t { None, Cube, Unsupported };

struct EntityImport {
  int32_t parent = -1;
  Vec3 position = {0.0f, 0.0f, 0.0f};
  VkrQuat rotation = vkr_quat_identity();
  Vec3 scale = {1.0f, 1.0f, 1.0f};
  Mat4 matrix = {};
  bool has_matrix = false;
  std::string mesh_path;
  bool skip_geometry = false;
  ShapeKind shape = ShapeKind::None;
  Vec3 dimensions = {1.0f, 1.0f, 1.0f};
  Vec4 shape_color = {1.0f, 1.0f, 1.0f, 1.0f};
  std::string shape_material_path;
  bool has_point_light = false;
  VkrBakeSceneLight point_light = {};
  bool has_directional_light = false;
  VkrBakeSceneLight directional_light = {};
  bool has_rectangle_light = false;
  VkrBakeSceneLight rectangle_light = {};
};

bool finite_vec2(Vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}
bool finite_vec3(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}
bool finite_vec4(Vec4 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

void set_error(VkrBakeSceneError error, VkrBakeSceneError *out_error) {
  if (out_error)
    *out_error = error;
}

void reset_scene(VkrBakeScene *scene) {
  scene->materials.clear();
  scene->triangles.clear();
  scene->zero_area_triangle_count = 0u;
  scene->lights.clear();
  scene->dependency_paths.clear();
  scene->environment = {};
  scene->atmosphere = {};
  scene->subsurface_profile_count = 0u;
  if (scene->texture_store) {
    vkr_bake_texture_store_release(scene->texture_store);
    scene->texture_store = nullptr;
  }
}

bool append_unique_path(std::vector<std::string> *paths,
                        const std::string &path) {
  if (path.empty())
    return false;
  if (std::find(paths->begin(), paths->end(), path) == paths->end())
    paths->push_back(path);
  return true;
}

bool read_file(const char *path, std::vector<uint8_t> *out_bytes) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamsize size = file.tellg();
  if (size < 0)
    return false;
  out_bytes->resize(static_cast<size_t>(size));
  file.seekg(0, std::ios::beg);
  return size == 0 ||
         file.read(reinterpret_cast<char *>(out_bytes->data()), size);
}

bool parse_null(VkrJsonReader *reader) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos + 4u > reader->length ||
      MemCompare(reader->data + reader->pos, "null", 4u) != 0)
    return false;
  reader->pos += 4u;
  return true;
}

bool parse_float_array(VkrJsonReader *reader, float32_t *out, uint32_t count) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos++] != '[')
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    vkr_json_skip_whitespace(reader);
    if (!vkr_json_parse_float(reader, &out[i]))
      return false;
    vkr_json_skip_whitespace(reader);
    if (i + 1u < count) {
      if (reader->pos >= reader->length || reader->data[reader->pos++] != ',')
        return false;
    }
  }
  vkr_json_skip_whitespace(reader);
  return reader->pos < reader->length && reader->data[reader->pos++] == ']';
}

bool parse_vec3(VkrJsonReader *reader, Vec3 *out) {
  float32_t values[3] = {};
  if (!parse_float_array(reader, values, 3u) || !std::isfinite(values[0]) ||
      !std::isfinite(values[1]) || !std::isfinite(values[2]))
    return false;
  *out = vec3_new(values[0], values[1], values[2]);
  return true;
}

bool parse_vec2(VkrJsonReader *reader, Vec2 *out) {
  float32_t values[2] = {};
  if (!parse_float_array(reader, values, 2u) || !std::isfinite(values[0]) ||
      !std::isfinite(values[1]))
    return false;
  *out = vec2_new(values[0], values[1]);
  return true;
}

bool parse_vec4(VkrJsonReader *reader, Vec4 *out) {
  float32_t values[4] = {};
  if (!parse_float_array(reader, values, 4u) || !std::isfinite(values[0]) ||
      !std::isfinite(values[1]) || !std::isfinite(values[2]) ||
      !std::isfinite(values[3]))
    return false;
  *out = vec4_new(values[0], values[1], values[2], values[3]);
  return true;
}

bool read_string(const VkrJsonReader *object, const char *field,
                 std::string *out) {
  VkrJsonReader reader = *object;
  String8 value = {};
  if (!vkr_json_find_field(&reader, field) ||
      !vkr_json_parse_string(&reader, &value))
    return false;
  out->assign(reinterpret_cast<const char *>(value.str),
              static_cast<size_t>(value.length));
  return true;
}

bool read_float(const VkrJsonReader *object, const char *field,
                float32_t *out) {
  VkrJsonReader reader = *object;
  return vkr_json_find_field(&reader, field) &&
         vkr_json_parse_float(&reader, out) && std::isfinite(*out);
}

bool read_bool(const VkrJsonReader *object, const char *field, bool8_t *out) {
  VkrJsonReader reader = *object;
  return vkr_json_find_field(&reader, field) &&
         vkr_json_parse_bool(&reader, out);
}

bool read_vec3(const VkrJsonReader *object, const char *field, Vec3 *out) {
  VkrJsonReader reader = *object;
  return vkr_json_find_field(&reader, field) && parse_vec3(&reader, out);
}

bool read_vec4(const VkrJsonReader *object, const char *field, Vec4 *out) {
  VkrJsonReader reader = *object;
  return vkr_json_find_field(&reader, field) && parse_vec4(&reader, out);
}

bool read_optional_bool(const VkrJsonReader *object, const char *field,
                        bool8_t *out) {
  VkrJsonReader reader = *object;
  return !vkr_json_find_field(&reader, field) ||
         vkr_json_parse_bool(&reader, out);
}

bool read_optional_float(const VkrJsonReader *object, const char *field,
                         float32_t *out) {
  VkrJsonReader reader = *object;
  return !vkr_json_find_field(&reader, field) ||
         (vkr_json_parse_float(&reader, out) && std::isfinite(*out));
}

bool read_optional_vec2(const VkrJsonReader *object, const char *field,
                        Vec2 *out) {
  VkrJsonReader reader = *object;
  return !vkr_json_find_field(&reader, field) || parse_vec2(&reader, out);
}

bool read_optional_vec3(const VkrJsonReader *object, const char *field,
                        Vec3 *out) {
  VkrJsonReader reader = *object;
  return !vkr_json_find_field(&reader, field) || parse_vec3(&reader, out);
}

bool parse_parent(VkrJsonReader *reader, int32_t *out) {
  if (parse_null(reader)) {
    *out = -1;
    return true;
  }
  return vkr_json_parse_int(reader, out);
}

bool parse_transform(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "transform"))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  VkrJsonReader matrix = object;
  if (vkr_json_find_field(&matrix, "matrix")) {
    for (const char *field : {"pos", "rot", "scale"}) {
      VkrJsonReader member = object;
      if (vkr_json_find_field(&member, field)) {
        return false;
      }
    }
    if (!parse_float_array(&matrix, out->matrix.elements, 16u)) {
      return false;
    }
    for (float32_t value : out->matrix.elements) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
    out->has_matrix = true;
    const Mat4 local = out->matrix;
    out->position =
        vec3_new(local.elements[12], local.elements[13], local.elements[14]);
    const Vec3 y =
        vec3_new(local.elements[4], local.elements[5], local.elements[6]);
    const Vec3 z =
        vec3_new(local.elements[8], local.elements[9], local.elements[10]);
    const float32_t sy = vec3_length(y), sz = vec3_length(z);
    if (sy > 1e-8f && sz > 1e-8f) {
      out->rotation =
          vkr_quat_look_at(vec3_scale(z, -1.0f / sz), vec3_scale(y, 1.0f / sy));
    }
    return true;
  }
  Vec3 value3 = {};
  if (read_vec3(&object, "pos", &value3))
    out->position = value3;
  if (read_vec3(&object, "scale", &value3))
    out->scale = value3;
  VkrJsonReader rotation = object;
  if (vkr_json_find_field(&rotation, "rot")) {
    Vec4 quat = {};
    if (!parse_vec4(&rotation, &quat))
      return false;
    out->rotation = vkr_quat_normalize(quat);
  }
  return finite_vec3(out->position) && finite_vec3(out->scale) &&
         finite_vec4(out->rotation);
}

bool parse_mesh(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "mesh"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object) ||
      !read_string(&object, "path", &out->mesh_path) || out->mesh_path.empty())
    return false;
  VkrJsonReader gltf_source = object;
  VkrJsonReader range_overrides = object;
  /* Runtime rejects these obsolete authoring fields too: the cooked mesh is
   * the sole source of imported punctual lights for this offline path. */
  if (vkr_json_find_field(&gltf_source, "gltf_light_source") ||
      vkr_json_find_field(&range_overrides, "gltf_light_range_overrides"))
    return false;
  std::string domain;
  if (read_string(&object, "pipeline_domain", &domain)) {
    out->skip_geometry =
        domain == "ui" || domain == "post" || domain == "compute" ||
        domain == "skybox" || domain == "picking" ||
        domain == "picking_transparent" || domain == "picking_overlay";
    if (!out->skip_geometry && domain != "world" &&
        domain != "world_transparent" && domain != "world_overlay" &&
        domain != "shadow")
      return false;
  }
  return true;
}

bool parse_shape(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "shape"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  std::string type;
  if (!read_string(&object, "type", &type) || type != "cube") {
    out->shape = ShapeKind::Unsupported;
    return false;
  }
  out->shape = ShapeKind::Cube;
  Vec3 dimensions = {};
  if (read_vec3(&object, "dimensions", &dimensions))
    out->dimensions = dimensions;
  Vec4 color = {};
  if (read_vec4(&object, "color", &color))
    out->shape_color = color;
  VkrJsonReader material = object;
  if (vkr_json_find_field(&material, "material") && !parse_null(&material)) {
    VkrJsonReader material_object = {};
    if (!vkr_json_enter_object(&material, &material_object))
      return false;
    std::string ignored_name;
    const bool has_name = read_string(&material_object, "name", &ignored_name);
    const bool has_path =
        read_string(&material_object, "path", &out->shape_material_path);
    if ((has_name || has_path) &&
        (!has_name || !has_path || out->shape_material_path.empty()))
      return false;
  }
  return finite_vec3(out->dimensions) && out->dimensions.x > 0.0f &&
         out->dimensions.y > 0.0f && out->dimensions.z > 0.0f &&
         finite_vec4(out->shape_color);
}

bool parse_point_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "point_light"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  VkrBakeSceneLight light = {};
  light.kind = VkrBakeSceneLightKind::Polynomial;
  light.color = vec3_new(1.0f, 1.0f, 1.0f);
  light.intensity = 1.0f;
  light.constant = 1.0f;
  light.linear = 0.35f;
  light.quadratic = 0.44f;
  light.direction = vec3_new(0.0f, 0.0f, -1.0f);
  light.outer_cone_angle = 0.78539816339f;
  light.enabled = true_v;
  (void)read_bool(&object, "enabled", &light.enabled);
  (void)read_bool(&object, "casts_shadow", &light.casts_shadow);
  (void)read_vec3(&object, "color", &light.color);
  (void)read_float(&object, "intensity", &light.intensity);
  (void)read_float(&object, "range", &light.range);
  (void)read_vec3(&object, "direction_local", &light.direction);
  (void)read_float(&object, "inner_cone_angle", &light.inner_cone_angle);
  (void)read_float(&object, "outer_cone_angle", &light.outer_cone_angle);
  float32_t kind = 0.0f;
  if (read_float(&object, "kind", &kind)) {
    if (kind == 0.0f)
      light.kind = VkrBakeSceneLightKind::Polynomial;
    else if (kind == 1.0f)
      light.kind = VkrBakeSceneLightKind::Point;
    else if (kind == 2.0f)
      light.kind = VkrBakeSceneLightKind::Spot;
    else
      return false;
  }
  VkrJsonReader attenuation = object;
  if (vkr_json_find_field(&attenuation, "attenuation") &&
      !parse_null(&attenuation)) {
    VkrJsonReader attenuation_object = {};
    if (!vkr_json_enter_object(&attenuation, &attenuation_object))
      return false;
    (void)read_float(&attenuation_object, "constant", &light.constant);
    (void)read_float(&attenuation_object, "linear", &light.linear);
    (void)read_float(&attenuation_object, "quadratic", &light.quadratic);
  }
  if (!finite_vec3(light.color) || !finite_vec3(light.direction) ||
      !std::isfinite(light.intensity) || !std::isfinite(light.range) ||
      !std::isfinite(light.constant) || !std::isfinite(light.linear) ||
      !std::isfinite(light.quadratic) ||
      !std::isfinite(light.inner_cone_angle) ||
      !std::isfinite(light.outer_cone_angle))
    return false;
  out->point_light = light;
  out->has_point_light = true;
  return true;
}

bool parse_directional_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "directional_light"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  VkrBakeSceneLight light = {};
  light.kind = VkrBakeSceneLightKind::Directional;
  light.color = vec3_new(1.0f, 1.0f, 1.0f);
  light.intensity = 1.0f;
  light.direction = vec3_new(0.0f, -1.0f, 0.0f);
  light.enabled = true_v;
  (void)read_bool(&object, "enabled", &light.enabled);
  (void)read_vec3(&object, "color", &light.color);
  (void)read_float(&object, "intensity", &light.intensity);
  (void)read_vec3(&object, "direction_local", &light.direction);
  if (!finite_vec3(light.color) || !finite_vec3(light.direction) ||
      !std::isfinite(light.intensity))
    return false;
  out->directional_light = light;
  out->has_directional_light = true;
  return true;
}

bool parse_rectangle_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_field(&reader, "rectangle_light"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  VkrBakeSceneLight light = {};
  light.kind = VkrBakeSceneLightKind::Rectangle;
  light.enabled = true_v;
  light.color = vec3_new(1.0f, 1.0f, 1.0f);
  light.radiance = 1.0f;
  Vec2 size = vec2_new(1.0f, 1.0f);
  if (!read_optional_bool(&object, "enabled", &light.enabled) ||
      !read_optional_vec3(&object, "color", &light.color) ||
      !read_optional_float(&object, "radiance", &light.radiance) ||
      !read_optional_vec2(&object, "size", &size) ||
      !finite_vec3(light.color) || !finite_vec2(size) || light.color.x < 0.0f ||
      light.color.y < 0.0f || light.color.z < 0.0f || light.radiance < 0.0f ||
      size.x <= 0.0f || size.y <= 0.0f)
    return false;
  light.half_width = size.x * 0.5f;
  light.half_height = size.y * 0.5f;
  out->rectangle_light = light;
  out->has_rectangle_light = true;
  return true;
}

bool parse_entities(const std::vector<uint8_t> &bytes,
                    std::vector<EntityImport> *out) {
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader entities = root;
  if (!vkr_json_find_array(&entities, "entities"))
    return false;
  uint32_t rectangle_light_count = 0u;
  while (vkr_json_next_array_element(&entities)) {
    if (out->size() == k_max_scene_entities)
      return false;
    VkrJsonReader entity = {};
    if (!vkr_json_enter_object(&entities, &entity))
      return false;
    EntityImport imported = {};
    VkrJsonReader parent = entity;
    if (vkr_json_find_field(&parent, "parent") &&
        !parse_parent(&parent, &imported.parent))
      return false;
    if (!parse_transform(&entity, &imported) ||
        !parse_mesh(&entity, &imported) || !parse_shape(&entity, &imported) ||
        !parse_point_light(&entity, &imported) ||
        !parse_directional_light(&entity, &imported) ||
        !parse_rectangle_light(&entity, &imported))
      return false;
    if (imported.has_rectangle_light &&
        ++rectangle_light_count > VKR_MAX_SCENE_RECTANGLE_LIGHTS)
      return false;
    out->push_back(std::move(imported));
  }
  for (uint32_t i = 0; i < out->size(); ++i)
    if ((*out)[i].parent >= static_cast<int32_t>(out->size()))
      return false;
  return true;
}

bool parse_environment(const std::vector<uint8_t> &bytes,
                       VkrBakeSceneEnvironment *out,
                       std::vector<std::string> *dependencies) {
  (void)dependencies;
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader reader = root;
  if (!vkr_json_find_field(&reader, "environment"))
    return true;
  if (parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  out->enabled = true_v;
  (void)read_bool(&object, "enabled", &out->enabled);
  (void)read_float(&object, "intensity", &out->intensity);
  (void)read_float(&object, "diffuse_intensity", &out->diffuse_intensity);
  (void)read_float(&object, "specular_intensity", &out->specular_intensity);
  (void)read_float(&object, "sh_deringing", &out->sh_deringing);
  if (!out->enabled)
    return true;
  VkrJsonReader cube = object;
  const bool has_cube = vkr_json_find_field(&cube, "cubemap");
  std::string equirect;
  const bool has_equirect_field = read_string(&object, "equirect", &equirect);
  const bool has_equirect = has_equirect_field && !equirect.empty();
  if (has_cube && has_equirect)
    return false;
  if (has_equirect) {
    out->kind = VkrBakeSceneEnvironmentKind::Equirect;
    out->path = equirect;
    return true;
  }
  if (!has_cube || parse_null(&cube))
    return false;
  VkrJsonReader cube_object = {};
  if (!vkr_json_enter_object(&cube, &cube_object))
    return false;
  VkrJsonReader base_field = cube_object;
  VkrJsonReader extension_field = cube_object;
  const bool has_base_field = vkr_json_find_field(&base_field, "base_path");
  const bool has_extension_field =
      vkr_json_find_field(&extension_field, "extension");
  const bool has_path = read_string(&cube_object, "path", &out->path);
  const bool has_base = read_string(&cube_object, "base_path", &out->base_path);
  const bool has_extension =
      read_string(&cube_object, "extension", &out->extension);
  const bool direct = has_path && !out->path.empty();
  const bool faces = has_base && has_extension && !out->base_path.empty() &&
                     !out->extension.empty();
  if (direct == faces || (direct && (has_base_field || has_extension_field)))
    return false;
  if (direct) {
    out->kind = VkrBakeSceneEnvironmentKind::CubemapPath;
    return true;
  }
  out->kind = VkrBakeSceneEnvironmentKind::CubemapFaces;
  return true;
}

bool parse_subsurface(const std::vector<uint8_t> &bytes, VkrBakeScene *scene) {
  VkrJsonReader reader = vkr_json_reader_create(bytes.data(), bytes.size());
  if (!vkr_json_find_field(&reader, "subsurface") || parse_null(&reader)) return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object)) return false;
  bool8_t enabled = false_v;
  VkrJsonReader field = object;
  if (vkr_json_find_field(&field, "enabled") && !vkr_json_parse_bool(&field, &enabled)) return false;
  if (!enabled) return true;
  VkrJsonReader profiles = object;
  if (!vkr_json_find_array(&profiles, "profiles")) return false;
  uint32_t count = 0u;
  while (vkr_json_next_array_element(&profiles)) {
    if (count == VKR_SUBSURFACE_PROFILE_COUNT ||
        !parse_vec3(&profiles, &scene->subsurface_profiles[count].diffusion_distance) ||
        !vkr_subsurface_profile_valid(scene->subsurface_profiles[count])) return false;
    ++count;
  }
  scene->subsurface_profile_count = count;
  return count > 0u;
}

bool parse_atmosphere(const std::vector<uint8_t> &bytes,
                      VkrAtmosphereSettings *out, float32_t *out_sh_deringing) {
  *out = vkr_atmosphere_settings_defaults();
  *out_sh_deringing = 0.0f;
  out->enabled = false_v;
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader reader = root;
  if (!vkr_json_find_field(&reader, "atmosphere") || parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  out->enabled = true_v;
  auto optional_bool = [&](const char *field, bool8_t *value) {
    VkrJsonReader input = object;
    return !vkr_json_find_field(&input, field) ||
           vkr_json_parse_bool(&input, value);
  };
  auto optional_float = [&](const char *field, float32_t *value) {
    VkrJsonReader input = object;
    return !vkr_json_find_field(&input, field) ||
           (vkr_json_parse_float(&input, value) && std::isfinite(*value));
  };
  auto optional_vec3 = [&](const char *field, Vec3 *value) {
    VkrJsonReader input = object;
    return !vkr_json_find_field(&input, field) || parse_vec3(&input, value);
  };
  if (!optional_bool("enabled", &out->enabled) ||
      !optional_vec3("sun_direction", &out->sun_direction) ||
      !optional_vec3("solar_irradiance", &out->solar_irradiance) ||
      !optional_vec3("ground_albedo", &out->ground_albedo) ||
      !optional_float("observer_altitude_m", &out->observer_altitude_m) ||
      !optional_float("sun_angular_diameter_degrees",
                      &out->sun_angular_diameter_degrees) ||
      !optional_float("rayleigh_density_scale", &out->rayleigh_density_scale) ||
      !optional_float("mie_density_scale", &out->mie_density_scale) ||
      !optional_float("ozone_density_scale", &out->ozone_density_scale) ||
      !optional_float("mie_anisotropy", &out->mie_anisotropy) ||
      !optional_float("sh_deringing", out_sh_deringing) ||
      *out_sh_deringing < 0.0f)
    return false;
  VkrAtmosphereSettings validation = *out;
  validation.enabled = true_v;
  return vkr_atmosphere_settings_valid(&validation);
}

bool load_environment(VkrBakeScene *scene) {
  VkrBakeSceneEnvironment &environment = scene->environment;
  if (!environment.enabled)
    return true;
  VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
  switch (environment.kind) {
  case VkrBakeSceneEnvironmentKind::Equirect:
    return vkr_bake_texture_store_load_environment_equirect(
        scene->texture_store, environment.path.c_str(),
        &environment.texture_index, &error);
  case VkrBakeSceneEnvironmentKind::CubemapPath:
    return vkr_bake_texture_store_load_environment_cube_rgba16f(
        scene->texture_store, environment.path.c_str(),
        &environment.texture_index, &error);
  case VkrBakeSceneEnvironmentKind::CubemapFaces: {
    /* Runtime face order and image orientation: +X, -X, +Y, -Y, +Z, -Z. */
    static constexpr const char *suffixes[] = {"_r.", "_l.", "_u.",
                                               "_d.", "_f.", "_b."};
    for (uint32_t face = 0u; face < 6u; ++face) {
      const std::string path =
          environment.base_path + suffixes[face] + environment.extension;
      if (!vkr_bake_texture_store_load_environment_2d(
              scene->texture_store, path.c_str(), true_v,
              &environment.face_texture_indices[face], &error))
        return false;
    }
    return true;
  }
  case VkrBakeSceneEnvironmentKind::None:
    return false;
  }
  return false;
}

bool compute_entity_worlds(const std::vector<EntityImport> &entities,
                           bool include_scale, std::vector<Mat4> *out) {
  out->resize(entities.size());
  std::vector<uint8_t> state(entities.size(), 0u);
  std::array<uint32_t, k_max_transform_depth> chain = {};
  for (uint32_t i = 0; i < entities.size(); ++i) {
    uint32_t cursor = i, count = 0u;
    while (state[cursor] == 0u) {
      if (count == chain.size())
        return false;
      state[cursor] = 1u;
      chain[count++] = cursor;
      const int32_t parent = entities[cursor].parent;
      if (parent < 0)
        break;
      cursor = static_cast<uint32_t>(parent);
      if (state[cursor] == 1u)
        return false;
    }
    while (count) {
      const uint32_t index = chain[--count];
      const EntityImport &entity = entities[index];
      Mat4 local = mat4_mul(mat4_translate(entity.position),
                            vkr_quat_to_mat4(entity.rotation));
      if (include_scale) {
        local = entity.has_matrix ? entity.matrix
                                  : mat4_mul(local, mat4_scale(entity.scale));
      }
      (*out)[index] =
          entity.parent < 0 ? local : mat4_mul((*out)[entity.parent], local);
      for (float32_t value : (*out)[index].elements)
        if (!std::isfinite(value))
          return false;
      state[index] = 2u;
    }
  }
  return true;
}

Vec3 transform_direction(Mat4 world, Vec3 direction) {
  const Vec4 transformed = mat4_mul_vec4(
      world, vec4_new(direction.x, direction.y, direction.z, 0.0f));
  return vec3_normalize(vec3_new(transformed.x, transformed.y, transformed.z));
}

bool normal_matrix(Mat4 world, Mat4 *out_normal, bool *out_flipped) {
  const Vec3 x =
      vec3_new(world.elements[0], world.elements[1], world.elements[2]);
  const Vec3 y =
      vec3_new(world.elements[4], world.elements[5], world.elements[6]);
  const Vec3 z =
      vec3_new(world.elements[8], world.elements[9], world.elements[10]);
  const float32_t determinant = vec3_dot(vec3_cross(x, y), z);
  if (!std::isfinite(determinant) || std::fabs(determinant) <= 1.0e-8f)
    return false;
  *out_normal = mat4_transpose(mat4_inverse_affine(world));
  *out_flipped = determinant < 0.0f;
  return true;
}

VkrBakeMaterial default_material(Vec4 color) {
  VkrBakeMaterial material = {};
  material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_OPAQUE;
  material.base_color = color;
  material.metallic = 1.0f;
  material.roughness = 1.0f;
  material.normal_scale = 1.0f;
  material.occlusion_strength = 1.0f;
  material.dielectric_specular = vec3_new(0.04f, 0.04f, 0.04f);
  material.ior = 1.5f;
  material.attenuation_color = vec3_new(1.0f, 1.0f, 1.0f);
  return material;
}

bool append_material(VkrBakeScene *scene, const std::string &path,
                     Vec4 fallback_color, uint32_t *out_index) {
  if (path.empty()) {
    scene->materials.push_back(default_material(fallback_color));
    *out_index = static_cast<uint32_t>(scene->materials.size() - 1u);
    return true;
  }
  VkrBakeMaterial material = {};
  VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
  if (!vkr_bake_material_load(scene->texture_store, path.c_str(), &material,
                              &error))
    return false;
  append_unique_path(&scene->dependency_paths, path);
  scene->materials.push_back(material);
  *out_index = static_cast<uint32_t>(scene->materials.size() - 1u);
  return true;
}

/* Exact zero-area triangles have no transport measure. Non-finite geometry is
   still an input failure; never turn it into a silent omission. */
bool triangle_area_is_exactly_zero(const VkrBakeTriangle *triangle,
                                   bool *out_zero_area) {
  const Vec3 a = triangle->vertex[0].position;
  const Vec3 b = triangle->vertex[1].position;
  const Vec3 c = triangle->vertex[2].position;
  const float64_t ab_x = (float64_t)b.x - a.x;
  const float64_t ab_y = (float64_t)b.y - a.y;
  const float64_t ab_z = (float64_t)b.z - a.z;
  const float64_t ac_x = (float64_t)c.x - a.x;
  const float64_t ac_y = (float64_t)c.y - a.y;
  const float64_t ac_z = (float64_t)c.z - a.z;
  const float64_t normal_x = ab_y * ac_z - ab_z * ac_y;
  const float64_t normal_y = ab_z * ac_x - ab_x * ac_z;
  const float64_t normal_z = ab_x * ac_y - ab_y * ac_x;
  const float64_t normal_squared =
      normal_x * normal_x + normal_y * normal_y + normal_z * normal_z;
  if (!std::isfinite(normal_squared))
    return false;
  *out_zero_area = normal_squared == 0.0;
  return true;
}

struct MeshAppendContext {
  VkrBakeScene *scene;
  std::vector<uint32_t> material_by_range;
};

bool8_t append_mesh_triangle(void *user, const VkrBakeTriangle *triangle,
                             String8 material_path) {
  MeshAppendContext *context = static_cast<MeshAppendContext *>(user);
  try {
    bool zero_area = false;
    if (!triangle_area_is_exactly_zero(triangle, &zero_area))
      return false_v;
    if (zero_area) {
      ++context->scene->zero_area_triangle_count;
      return true_v;
    }
    const uint32_t range = triangle->material_index;
    if (range >= context->material_by_range.size())
      context->material_by_range.resize(static_cast<size_t>(range) + 1u,
                                        UINT32_MAX);
    uint32_t material_index = context->material_by_range[range];
    if (material_index == UINT32_MAX) {
      const std::string path =
          material_path.str && material_path.length > 0u
              ? std::string(reinterpret_cast<const char *>(material_path.str),
                            static_cast<size_t>(material_path.length))
              : std::string();
      if (!append_material(context->scene, path, vec4_one(), &material_index))
        return false_v;
      context->material_by_range[range] = material_index;
    }
    VkrBakeTriangle copied = *triangle;
    copied.material_index = material_index;
    context->scene->triangles.push_back(copied);
    return true_v;
  } catch (const std::bad_alloc &) {
    return false_v;
  }
}

bool8_t append_mesh_light(void *user, const VkrBakeMeshLight *source) {
  MeshAppendContext *context = static_cast<MeshAppendContext *>(user);
  try {
    VkrBakeSceneLight light = {};
    light.kind = source->kind == VKR_BAKE_MESH_LIGHT_DIRECTIONAL
                     ? VkrBakeSceneLightKind::Directional
                 : source->kind == VKR_BAKE_MESH_LIGHT_POINT
                     ? VkrBakeSceneLightKind::Point
                     : VkrBakeSceneLightKind::Spot;
    light.position = source->position;
    light.direction = source->direction;
    light.color = source->color;
    light.intensity = source->intensity;
    light.range = source->range;
    light.inner_cone_angle = source->inner_cone_angle;
    light.outer_cone_angle = source->outer_cone_angle;
    light.enabled = true_v;
    context->scene->lights.push_back(light);
    return true_v;
  } catch (const std::bad_alloc &) {
    return false_v;
  }
}

bool append_mesh(VkrBakeScene *scene, const std::string &path,
                 Mat4 entity_world, uint32_t *next_instance) {
  std::vector<uint8_t> bytes;
  if (!read_file(path.c_str(), &bytes))
    return false;
  MeshAppendContext context = {.scene = scene};
  const VkrBakeMeshDecodeCallbacks callbacks = {
      .emit_triangle = append_mesh_triangle,
      .emit_light = append_mesh_light,
  };
  String8 source_path = {.str = (uint8_t *)path.data(), .length = path.size()};
  if (!vkr_bake_mesh_decode_file(source_path, bytes.data(), bytes.size(),
                                 entity_world, next_instance, &callbacks,
                                 &context))
    return false;
  const std::string sidecar = path + ".remap.json";
  std::error_code sidecar_error;
  if (std::filesystem::is_regular_file(sidecar, sidecar_error) &&
      !append_unique_path(&scene->dependency_paths, sidecar)) {
    return false;
  }
  return append_unique_path(&scene->dependency_paths, path);
}

bool append_cube(VkrBakeScene *scene, const EntityImport &entity, Mat4 world,
                 uint32_t source_instance_index) {
  uint32_t material_index = 0u;
  if (!append_material(scene, entity.shape_material_path, entity.shape_color,
                       &material_index))
    return false;
  Mat4 normal = {};
  bool flipped = false;
  if (!normal_matrix(world, &normal, &flipped))
    return false;
  const float32_t x = entity.dimensions.x * 0.5f,
                  y = entity.dimensions.y * 0.5f,
                  z = entity.dimensions.z * 0.5f;
  const std::array<VkrBakeVertex, 24> vertices = {{
      {{-x, -y, z}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
      {{x, -y, z}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
      {{x, y, z}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
      {{-x, y, z}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
      {{-x, -y, -z}, {0, 0, -1}, {1, 0}, {1, 1, 1, 1}},
      {{x, -y, -z}, {0, 0, -1}, {0, 0}, {1, 1, 1, 1}},
      {{x, y, -z}, {0, 0, -1}, {0, 1}, {1, 1, 1, 1}},
      {{-x, y, -z}, {0, 0, -1}, {1, 1}, {1, 1, 1, 1}},
      {{-x, -y, -z}, {-1, 0, 0}, {0, 0}, {1, 1, 1, 1}},
      {{-x, -y, z}, {-1, 0, 0}, {1, 0}, {1, 1, 1, 1}},
      {{-x, y, z}, {-1, 0, 0}, {1, 1}, {1, 1, 1, 1}},
      {{-x, y, -z}, {-1, 0, 0}, {0, 1}, {1, 1, 1, 1}},
      {{x, -y, -z}, {1, 0, 0}, {0, 0}, {1, 1, 1, 1}},
      {{x, -y, z}, {1, 0, 0}, {1, 0}, {1, 1, 1, 1}},
      {{x, y, z}, {1, 0, 0}, {1, 1}, {1, 1, 1, 1}},
      {{x, y, -z}, {1, 0, 0}, {0, 1}, {1, 1, 1, 1}},
      {{-x, y, z}, {0, 1, 0}, {0, 0}, {1, 1, 1, 1}},
      {{x, y, z}, {0, 1, 0}, {1, 0}, {1, 1, 1, 1}},
      {{x, y, -z}, {0, 1, 0}, {1, 1}, {1, 1, 1, 1}},
      {{-x, y, -z}, {0, 1, 0}, {0, 1}, {1, 1, 1, 1}},
      {{-x, -y, -z}, {0, -1, 0}, {0, 0}, {1, 1, 1, 1}},
      {{x, -y, -z}, {0, -1, 0}, {1, 0}, {1, 1, 1, 1}},
      {{x, -y, z}, {0, -1, 0}, {1, 1}, {1, 1, 1, 1}},
      {{-x, -y, z}, {0, -1, 0}, {0, 1}, {1, 1, 1, 1}},
  }};
  static constexpr std::array<uint32_t, 36> indices = {
      0,  1,  2,  2,  3,  0,  4,  7,  6,  6,  5,  4,  8,  9,  10, 10, 11, 8,
      12, 15, 14, 14, 13, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};
  for (uint32_t index = 0; index < indices.size(); index += 3u) {
    VkrBakeTriangle triangle = {};
    triangle.material_index = material_index;
    triangle.source_instance_index = source_instance_index;
    for (uint32_t corner = 0; corner < 3u; ++corner) {
      const VkrBakeVertex &source = vertices[indices[index + corner]];
      const Vec4 transformed_normal =
          mat4_mul_vec4(normal, vec4_new(source.normal.x, source.normal.y,
                                         source.normal.z, 0.0f));
      triangle.vertex[corner] = {
          .position = mat4_mul_vec3(world, source.position),
          .normal = vec3_normalize(vec3_new(transformed_normal.x,
                                            transformed_normal.y,
                                            transformed_normal.z)),
          .uv = source.uv,
          .color = source.color};
      if (!finite_vec3(triangle.vertex[corner].position) ||
          !finite_vec3(triangle.vertex[corner].normal) ||
          vec3_length(triangle.vertex[corner].normal) <= 1.0e-8f)
        return false;
    }
    if (flipped)
      std::swap(triangle.vertex[1], triangle.vertex[2]);
    bool zero_area = false;
    if (!triangle_area_is_exactly_zero(&triangle, &zero_area))
      return false;
    if (zero_area) {
      ++scene->zero_area_triangle_count;
      continue;
    }
    scene->triangles.push_back(triangle);
  }
  return true;
}

bool append_authored_lights(VkrBakeScene *scene, const EntityImport &entity,
                            Mat4 world, Mat4 rigid_world,
                            bool suppress_directional) {
  auto append = [&](VkrBakeSceneLight light) {
    light.position = mat4_mul_vec3(world, vec3_zero());
    light.direction = transform_direction(world, light.direction);
    if (!finite_vec3(light.position) || !finite_vec3(light.direction) ||
        vec3_length(light.direction) <= 1.0e-8f)
      return false;
    scene->lights.push_back(light);
    return true;
  };
  auto append_rectangle = [&](VkrBakeSceneLight light) {
    light.position = mat4_mul_vec3(world, vec3_zero());
    light.right = transform_direction(rigid_world, vec3_new(1.0f, 0.0f, 0.0f));
    light.up = transform_direction(rigid_world, vec3_new(0.0f, 1.0f, 0.0f));
    const Vec3 normal = vec3_cross(light.right, light.up);
    light.direction = vec3_normalize(vec3_new(-normal.x, -normal.y, -normal.z));
    if (!finite_vec3(light.position) || !finite_vec3(light.right) ||
        !finite_vec3(light.up) || !finite_vec3(light.direction) ||
        vec3_length(light.right) <= 1.0e-8f ||
        vec3_length(light.up) <= 1.0e-8f ||
        vec3_length(light.direction) <= 1.0e-8f)
      return false;
    scene->lights.push_back(light);
    return true;
  };
  return (!entity.has_point_light || append(entity.point_light)) &&
         (suppress_directional || !entity.has_directional_light ||
          append(entity.directional_light)) &&
         (!entity.has_rectangle_light ||
          append_rectangle(entity.rectangle_light));
}

} // namespace

VkrBakeScene::VkrBakeScene(VkrAllocator *scene_allocator)
    : allocator(scene_allocator) {}

VkrBakeScene::~VkrBakeScene() { reset_scene(this); }

bool vkr_bake_scene_load(VkrBakeScene *scene, const char *scene_path,
                         VkrBakeSceneError *out_error) {
  set_error(VkrBakeSceneError::None, out_error);
  if (!scene || !scene->allocator || !scene_path || scene_path[0] == '\0') {
    set_error(VkrBakeSceneError::InvalidArgument, out_error);
    return false;
  }
  reset_scene(scene);
  try {
    std::vector<uint8_t> json;
    if (!read_file(scene_path, &json)) {
      set_error(VkrBakeSceneError::Io, out_error);
      return false;
    }
    std::vector<EntityImport> entities;
    if (!parse_entities(json, &entities) || entities.empty()) {
      set_error(VkrBakeSceneError::Parse, out_error);
      return false;
    }
    VkrAtmosphereSettings atmosphere_settings = {};
    float32_t atmosphere_sh_deringing = 0.0f;
    if (!append_unique_path(&scene->dependency_paths, scene_path) ||
        !parse_subsurface(json, scene) ||
        !parse_atmosphere(json, &atmosphere_settings,
                          &atmosphere_sh_deringing) ||
        !vkr_bake_atmosphere_build(&scene->atmosphere, &atmosphere_settings) ||
        (!scene->atmosphere.enabled &&
         !parse_environment(json, &scene->environment,
                            &scene->dependency_paths))) {
      set_error(VkrBakeSceneError::Parse, out_error);
      reset_scene(scene);
      return false;
    }
    scene->atmosphere.sh_deringing = atmosphere_sh_deringing;
    std::vector<Mat4> worlds;
    if (!compute_entity_worlds(entities, true, &worlds)) {
      set_error(VkrBakeSceneError::Parse, out_error);
      reset_scene(scene);
      return false;
    }
    std::vector<Mat4> rigid_worlds;
    if (!compute_entity_worlds(entities, false, &rigid_worlds)) {
      set_error(VkrBakeSceneError::Parse, out_error);
      reset_scene(scene);
      return false;
    }
    scene->texture_store = vkr_bake_texture_store_create(scene->allocator);
    if (!scene->texture_store) {
      set_error(VkrBakeSceneError::OutOfMemory, out_error);
      reset_scene(scene);
      return false;
    }
    if (!scene->atmosphere.enabled && !load_environment(scene)) {
      set_error(VkrBakeSceneError::Unsupported, out_error);
      reset_scene(scene);
      return false;
    }
    uint32_t next_instance = 0u;
    for (uint32_t i = 0; i < entities.size(); ++i) {
      const EntityImport &entity = entities[i];
      if (!append_authored_lights(scene, entity, worlds[i], rigid_worlds[i],
                                  scene->atmosphere.enabled)) {
        set_error(VkrBakeSceneError::Parse, out_error);
        reset_scene(scene);
        return false;
      }
      if (!entity.skip_geometry && !entity.mesh_path.empty() &&
          !append_mesh(scene, entity.mesh_path, worlds[i], &next_instance)) {
        set_error(VkrBakeSceneError::CookedMesh, out_error);
        reset_scene(scene);
        return false;
      }
      if (entity.shape == ShapeKind::Unsupported ||
          (entity.shape == ShapeKind::Cube &&
           !append_cube(scene, entity, worlds[i], next_instance++))) {
        set_error(VkrBakeSceneError::Unsupported, out_error);
        reset_scene(scene);
        return false;
      }
    }
    if (scene->atmosphere.enabled) {
      const Vec3 sun = vec3_new(scene->atmosphere.params.sun.x,
                                scene->atmosphere.params.sun.y,
                                scene->atmosphere.params.sun.z);
      scene->lights.push_back((VkrBakeSceneLight){
          .kind = VkrBakeSceneLightKind::Directional,
          .direction = vec3_new(-sun.x, -sun.y, -sun.z),
          .color = scene->atmosphere.observer_irradiance,
          .intensity = 1.0f,
          .enabled = true_v,
      });
    }
    for (uint32_t texture = 0u;
         texture < vkr_bake_texture_store_count(scene->texture_store);
         ++texture) {
      const char *path = nullptr;
      const char *sha256 = nullptr;
      uint64_t byte_count = 0u;
      if (!vkr_bake_texture_store_dependency(scene->texture_store, texture,
                                             &path, &sha256, &byte_count) ||
          !path || !append_unique_path(&scene->dependency_paths, path)) {
        set_error(VkrBakeSceneError::Material, out_error);
        reset_scene(scene);
        return false;
      }
    }
    if (scene->triangles.empty()) {
      set_error(VkrBakeSceneError::Unsupported, out_error);
      reset_scene(scene);
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    reset_scene(scene);
    set_error(VkrBakeSceneError::OutOfMemory, out_error);
    return false;
  }
}

Vec3 vkr_bake_scene_sample_environment(const VkrBakeScene *scene,
                                       Vec3 direction) {
  const Vec3 black = vec3_zero();
  if (scene->atmosphere.enabled)
    return vkr_bake_atmosphere_sample(&scene->atmosphere, direction);
  if (!scene->environment.enabled ||
      scene->environment.kind == VkrBakeSceneEnvironmentKind::None)
    return black;
  const VkrBakeSceneEnvironment &environment = scene->environment;
  Vec4 sample = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
  if (environment.kind == VkrBakeSceneEnvironmentKind::Equirect) {
    constexpr float32_t inverse_two_pi = 0.15915494309189535f;
    constexpr float32_t inverse_pi = 0.3183098861837907f;
    const Vec2 uv =
        vec2_new(atan2f(direction.z, direction.x) * inverse_two_pi + 0.5f,
                 acosf(fmaxf(-1.0f, fminf(direction.y, 1.0f))) * inverse_pi);
    sample = vkr_bake_texture_store_sample_environment_2d(
        scene->texture_store, environment.texture_index, uv, true_v);
  } else {
    const float32_t x = direction.x;
    const float32_t y = direction.y;
    const float32_t z = direction.z;
    const float32_t ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    uint32_t face = 0u;
    Vec2 uv = vec2_new(0.5f, 0.5f);
    if (ax >= ay && ax >= az) {
      const float32_t inverse = 0.5f / ax;
      if (x >= 0.0f) {
        face = 0u;
        uv = vec2_new(0.5f - z * inverse, 0.5f - y * inverse);
      } else {
        face = 1u;
        uv = vec2_new(0.5f + z * inverse, 0.5f - y * inverse);
      }
    } else if (ay >= az) {
      const float32_t inverse = 0.5f / ay;
      if (y >= 0.0f) {
        face = 2u;
        uv = vec2_new(0.5f + x * inverse, 0.5f + z * inverse);
      } else {
        face = 3u;
        uv = vec2_new(0.5f + x * inverse, 0.5f - z * inverse);
      }
    } else {
      const float32_t inverse = 0.5f / az;
      if (z >= 0.0f) {
        face = 4u;
        uv = vec2_new(0.5f + x * inverse, 0.5f - y * inverse);
      } else {
        face = 5u;
        uv = vec2_new(0.5f - x * inverse, 0.5f - y * inverse);
      }
    }
    if (environment.kind == VkrBakeSceneEnvironmentKind::CubemapPath) {
      sample = vkr_bake_texture_store_sample_environment_cube_face(
          scene->texture_store, environment.texture_index, face, uv);
    } else if (environment.kind == VkrBakeSceneEnvironmentKind::CubemapFaces) {
      sample = vkr_bake_texture_store_sample_environment_2d(
          scene->texture_store, environment.face_texture_indices[face], uv,
          false_v);
    }
  }
  return vec3_new(sample.x * environment.intensity,
                  sample.y * environment.intensity,
                  sample.z * environment.intensity);
}

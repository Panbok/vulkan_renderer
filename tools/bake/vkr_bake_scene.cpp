#include "bake/vkr_bake_scene.h"
#include "filesystem/vkr_filesystem_cpp.h"

#include "bake/vkr_bake_mesh_decode.h"

extern "C" {
#include "core/vkr_json.h"
#include "level/vkr_blockout.h"
#include "level/vkr_brush.h"
#include "level/vkr_surface.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr uint32_t k_max_scene_entities = 65536u;
constexpr uint32_t k_max_transform_depth = 256u;
/* The runtime's BRUSH_MOVER_DEPTH_MAX: a brush deeper than this below its
   mover finds none and stays static there. */
constexpr uint32_t k_max_mover_depth = 64u;

enum class ShapeKind : uint8_t { None, Cube, Unsupported };

/* A brush face's surface tag, mark, art-owned material and texture
   placement, with the runtime's brush_face defaults. */
struct BrushFaceStyle {
  uint32_t surface = VKR_SURFACE_NONE;
  uint32_t mark = VKR_SURFACE_MARK_NONE;
  std::string material;
  Vec2 uv_offset = {0.0f, 0.0f};
  Vec2 uv_scale = {1.0f, 1.0f};
  float32_t uv_rotation = 0.0f;
  bool8_t uv_world = true_v;
};

struct EntityImport {
  int32_t parent = -1;
  /* The authored name and id text, for diagnostics only. */
  std::string name;
  std::string id_text;
  /* The document id, the stable key of the entity's lightmaps. */
  std::array<uint8_t, 16> document_id = {};
  bool has_document_id = false;
  /* A solid or visual brush draws; clip and trigger brushes do not. */
  bool brush = false;
  bool brush_draws = false;
  bool brush_face = false;
  /* A `surface_theme` component's theme file; empty without one. */
  std::string surface_theme;
  VkrBrushPlane face_plane = {};
  BrushFaceStyle face;
  /* A blockout shape's settings (ADR-084); null when it has none. */
  std::unique_ptr<SceneBlockout> blockout;
  /* It carries a `mover`, which moves it and the brushes below it. */
  bool mover = false;
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
  // A dynamic point or rectangle light is never baked; a stationary point
  // light bakes its bounce (VkrBakeSceneLight::stationary).
  bool point_light_static = true;
  VkrBakeSceneLight point_light = {};
  bool has_directional_light = false;
  VkrBakeSceneLight directional_light = {};
  float32_t directional_sun_angular_diameter_degrees =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;
  bool8_t directional_atmosphere_sun = true_v;
  bool8_t directional_atmosphere_moon = false_v;
  bool has_rectangle_light = false;
  bool rectangle_light_static = true;
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
  scene->lightmap_instances.clear();
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
  std::ifstream file(vkr_filesystem_native_utf8_path(path),
                     std::ios::binary | std::ios::ate);
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

/*
 * Finds an entity block by key: at the entity's root or, for a scene type the
 * runtime also accepts as a component, in its `components` object. A field
 * of the same name inside another block (a blockout's "shape", say) is not
 * that block.
 */
bool find_block(const VkrJsonReader *entity, const char *name,
                VkrJsonReader *out) {
  VkrJsonReader reader = *entity;
  if (vkr_json_find_root_field(&reader, name)) {
    *out = reader;
    return true;
  }
  reader = *entity;
  VkrJsonReader components = {};
  if (!vkr_json_find_root_field(&reader, "components") ||
      !vkr_json_enter_object(&reader, &components) ||
      !vkr_json_find_root_field(&components, name)) {
    return false;
  }
  *out = components;
  return true;
}

bool parse_transform(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_root_field(&reader, "transform"))
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
  if (!vkr_json_find_root_field(&reader, "mesh"))
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
  VkrJsonReader reader = {};
  if (!find_block(entity, "shape", &reader))
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

/*
 * The baking fields of a point or rectangle light block (ADR-088): its
 * group, the default group when it names none, and whether it bakes: static
 * and stationary lights bake, and only a point light may be stationary
 * (ADR-107).
 */
bool parse_light_baking(const VkrJsonReader *object, VkrBakeSceneLight *light,
                        bool allow_stationary, bool *out_static) {
  std::string text;
  *out_static = true;
  if (read_string(object, "mobility", &text)) {
    if (text == "dynamic") {
      *out_static = false;
    } else if (text == "stationary" && allow_stationary) {
      light->stationary = true_v;
    } else if (text != "static") {
      return false;
    }
  }
  if (!read_string(object, "light_group", &text) || text.empty()) {
    text = VKR_LIGHT_GROUP_DEFAULT;
  }
  if (!vkr_light_group_name_valid(text.data(), text.size())) {
    return false;
  }
  std::copy(text.begin(), text.end(), light->group);
  light->group[text.size()] = '\0';
  return true;
}

bool parse_point_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (!find_block(entity, "point_light", &reader))
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
  if (!parse_light_baking(&object, &light, true, &out->point_light_static))
    return false;
  out->point_light = light;
  out->has_point_light = true;
  return true;
}

bool parse_directional_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (!find_block(entity, "directional_light", &reader))
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
  // The runtime sun always casts its cascaded shadows (ADR-041).
  light.casts_shadow = true_v;
  light.enabled = true_v;
  (void)read_bool(&object, "enabled", &light.enabled);
  (void)read_vec3(&object, "color", &light.color);
  (void)read_float(&object, "intensity", &light.intensity);
  (void)read_vec3(&object, "direction_local", &light.direction);
  float32_t temperature = 0.0f;
  float32_t diameter =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;
  bool8_t atmosphere_sun = true_v;
  bool8_t atmosphere_moon = false_v;
  if (!finite_vec3(light.color) || !finite_vec3(light.direction) ||
      !std::isfinite(light.intensity) ||
      !read_optional_float(&object, "temperature_kelvin", &temperature) ||
      !read_optional_float(&object, "sun_angular_diameter_degrees",
                           &diameter) ||
      !read_optional_bool(&object, "atmosphere_sun", &atmosphere_sun) ||
      !read_optional_bool(&object, "atmosphere_moon", &atmosphere_moon))
    return false;
  if (temperature != 0.0f &&
      (temperature < VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K ||
       temperature > VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K))
    return false;
  // A colour temperature tints the authored colour, as in the runtime.
  if (temperature > 0.0f)
    light.color =
        vec3_mul(light.color, vkr_atmosphere_blackbody_rgb(temperature));
  out->directional_light = light;
  out->has_directional_light = true;
  out->directional_sun_angular_diameter_degrees = diameter;
  out->directional_atmosphere_sun = atmosphere_sun;
  out->directional_atmosphere_moon = atmosphere_moon;
  return true;
}

bool parse_rectangle_light(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (!find_block(entity, "rectangle_light", &reader))
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
  if (!parse_light_baking(&object, &light, false, &out->rectangle_light_static))
    return false;
  out->rectangle_light = light;
  out->has_rectangle_light = true;
  return true;
}

/* A UUID's 32 hex digits, hyphens ignored. */
bool parse_uuid(const std::string &text, std::array<uint8_t, 16> *out) {
  uint32_t digits = 0u;
  std::array<uint8_t, 16> bytes = {};
  for (char c : text) {
    if (c == '-') {
      continue;
    }
    uint32_t value = 0u;
    if (c >= '0' && c <= '9') {
      value = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value = (uint32_t)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      value = (uint32_t)(c - 'A') + 10u;
    } else {
      return false;
    }
    if (digits == 32u) {
      return false;
    }
    bytes[digits / 2u] |= (uint8_t)(value << ((digits % 2u) ? 0u : 4u));
    ++digits;
  }
  if (digits != 32u) {
    return false;
  }
  *out = bytes;
  return true;
}

bool parse_document_id(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = *entity;
  if (!vkr_json_find_root_field(&reader, "id")) {
    return true;
  }
  String8 text = {};
  if (!vkr_json_parse_string(&reader, &text)) {
    return true;
  }
  out->id_text.assign((const char *)text.str, text.length);
  out->has_document_id = parse_uuid(out->id_text, &out->document_id);
  return true;
}

/* The index of a JSON string field's value in `names`, or `out` unchanged
   when the field is absent; false for another name. */
bool read_optional_enum(const VkrJsonReader *object, const char *field,
                        const char *const *names, uint32_t *out);

/* `brush` and `brush_face` components (ADR-084): a brush's role, and one
   face plane with its surface, mark, material and texture placement. */
bool parse_brush(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (find_block(entity, "brush", &reader) && !parse_null(&reader)) {
    VkrJsonReader object = {};
    if (!vkr_json_enter_object(&reader, &object)) {
      return false;
    }
    std::string role = "solid";
    (void)read_string(&object, "role", &role);
    out->brush = true;
    out->brush_draws = role == "solid" || role == "visual";
  }
  return true;
}

bool parse_brush_face(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (find_block(entity, "brush_face", &reader) && !parse_null(&reader)) {
    VkrJsonReader object = {};
    if (!vkr_json_enter_object(&reader, &object) ||
        !read_vec3(&object, "normal", &out->face_plane.normal) ||
        !read_float(&object, "distance", &out->face_plane.distance) ||
        !read_optional_vec2(&object, "uv_offset", &out->face.uv_offset) ||
        !read_optional_vec2(&object, "uv_scale", &out->face.uv_scale) ||
        !read_optional_float(&object, "uv_rotation", &out->face.uv_rotation) ||
        !read_optional_bool(&object, "uv_world", &out->face.uv_world)) {
      return false;
    }
    (void)read_string(&object, "material", &out->face.material);
    if (!read_optional_enum(&object, "surface", vkr_surface_names,
                            &out->face.surface) ||
        !read_optional_enum(&object, "mark", vkr_surface_mark_names,
                            &out->face.mark)) {
      return false;
    }
    /* Older documents name a retired dev material, as the runtime reads
       them (brush_face_migrate). */
    VkrSurface surface = VKR_SURFACE_NONE;
    VkrSurfaceMark mark = VKR_SURFACE_MARK_NONE;
    if (vkr_surface_from_legacy_material(out->face.material.c_str(), &surface,
                                         &mark)) {
      out->face.surface = (uint32_t)surface;
      out->face.mark = (uint32_t)mark;
      out->face.material.clear();
    }
    out->brush_face = true;
  }
  return true;
}

/* A `surface_theme` component: the theme file that binds the scene's
   surface tags to materials. */
bool parse_surface_theme(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (find_block(entity, "surface_theme", &reader) && !parse_null(&reader)) {
    VkrJsonReader object = {};
    if (!vkr_json_enter_object(&reader, &object)) {
      return false;
    }
    (void)read_string(&object, "theme", &out->surface_theme);
  }
  return true;
}

/* A `mover` component: only its presence matters to the bake, which leaves
   the brushes it moves out. */
bool parse_mover(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  out->mover = find_block(entity, "mover", &reader) && !parse_null(&reader);
  return true;
}

/* The blockout type's enum names, as the scene writes them. */
constexpr const char *k_blockout_shape_names[] = {"Stairs", "Corridor",
                                                  nullptr};
constexpr const char *k_stairs_kind_names[] = {"Straight", "L turn", "U turn",
                                               "Curved",   "Spiral", nullptr};

bool read_optional_enum(const VkrJsonReader *object, const char *field,
                        const char *const *names, uint32_t *out) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return true;
  }
  String8 text = {};
  if (!vkr_json_parse_string(&reader, &text)) {
    return false;
  }
  for (uint32_t i = 0u; names[i]; ++i) {
    if (std::string(names[i]) ==
        std::string((const char *)text.str, text.length)) {
      *out = i;
      return true;
    }
  }
  return false;
}

bool read_optional_count(const VkrJsonReader *object, const char *field,
                         uint32_t max, uint32_t *out) {
  VkrJsonReader reader = *object;
  if (!vkr_json_find_field(&reader, field)) {
    return true;
  }
  float64_t value = 0.0;
  if (!vkr_json_parse_double(&reader, &value) || value != std::floor(value) ||
      value < 0.0 || value > (float64_t)max) {
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

/* A `blockout` component (ADR-084) read into its settings with the runtime
   type's defaults. The runtime loader validates ranges, and
   vkr_blockout_layout rejects a shape it cannot lay out. */
bool parse_blockout(const VkrJsonReader *entity, EntityImport *out) {
  VkrJsonReader reader = {};
  if (!find_block(entity, "blockout", &reader) || parse_null(&reader)) {
    return true;
  }
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object)) {
    return false;
  }
  std::unique_ptr<SceneBlockout> shape(new SceneBlockout());
  *shape = SceneBlockout{
      .shape = SCENE_BLOCKOUT_STAIRS,
      .stairs = SCENE_STAIRS_STRAIGHT,
      .height = 3.0f,
      .width = 1.5f,
      .length = 4.5f,
      .step_height = 0.1875f,
      .turn = 180.0f,
      .radius = 1.0f,
      .thickness = 0.0f,
      .ceiling = true_v,
  };
  for (float32_t &corner : shape->corners) {
    corner = -1.0f;
  }
  uint32_t shape_kind = (uint32_t)shape->shape;
  uint32_t stairs_kind = (uint32_t)shape->stairs;
  uint32_t surface = VKR_SURFACE_NONE;
  uint32_t floor_surface = VKR_SURFACE_NONE;
  uint32_t mark = VKR_SURFACE_MARK_NONE;
  if (!read_optional_enum(&object, "shape", k_blockout_shape_names,
                          &shape_kind) ||
      !read_optional_enum(&object, "stairs", k_stairs_kind_names,
                          &stairs_kind) ||
      !read_optional_float(&object, "height", &shape->height) ||
      !read_optional_float(&object, "width", &shape->width) ||
      !read_optional_float(&object, "length", &shape->length) ||
      !read_optional_float(&object, "step_height", &shape->step_height) ||
      !read_optional_float(&object, "turn", &shape->turn) ||
      !read_optional_float(&object, "radius", &shape->radius) ||
      !read_optional_float(&object, "thickness", &shape->thickness) ||
      !read_optional_bool(&object, "left", &shape->left) ||
      !read_optional_bool(&object, "ceiling", &shape->ceiling) ||
      !read_optional_enum(&object, "surface", vkr_surface_names, &surface) ||
      !read_optional_enum(&object, "floor_surface", vkr_surface_names,
                          &floor_surface) ||
      !read_optional_enum(&object, "mark", vkr_surface_mark_names, &mark) ||
      !read_optional_count(&object, "point_count", SCENE_BLOCKOUT_POINT_MAX,
                           &shape->point_count) ||
      !read_optional_count(&object, "opening_count", SCENE_BLOCKOUT_OPENING_MAX,
                           &shape->opening_count)) {
    return false;
  }
  shape->shape = (SceneBlockoutShape)shape_kind;
  shape->stairs = (SceneStairsKind)stairs_kind;
  shape->surface = (VkrSurface)surface;
  shape->floor_surface = (VkrSurface)floor_surface;
  shape->mark = (VkrSurfaceMark)mark;
  for (uint32_t i = 0u; i < SCENE_BLOCKOUT_POINT_MAX; ++i) {
    const std::string point = "point_" + std::to_string(i);
    const std::string corner = "corner_" + std::to_string(i);
    if (!read_optional_vec3(&object, point.c_str(), &shape->points[i]) ||
        !read_optional_float(&object, corner.c_str(), &shape->corners[i])) {
      return false;
    }
  }
  for (uint32_t i = 0u; i < SCENE_BLOCKOUT_OPENING_MAX; ++i) {
    const std::string wall = "wall_" + std::to_string(i);
    const std::string opening = "opening_" + std::to_string(i);
    VkrJsonReader value = object;
    if (!read_optional_count(&object, wall.c_str(), UINT32_MAX,
                             &shape->walls[i]) ||
        (vkr_json_find_field(&value, opening.c_str()) &&
         !parse_vec4(&value, &shape->openings[i]))) {
      return false;
    }
  }
  out->blockout = std::move(shape);
  return true;
}

/* "entity 12 \"name\" (id ...)": how a diagnostic names an entity. */
std::string describe_entity(const EntityImport &entity, uint32_t index) {
  std::string text = "entity " + std::to_string(index);
  if (!entity.name.empty()) {
    text += " \"" + entity.name + "\"";
  }
  if (!entity.id_text.empty()) {
    text += " (id " + entity.id_text + ")";
  }
  return text;
}

/* An entity block the bake reads. Its parser fails when the block is
   malformed or authors something the bake cannot reproduce. */
struct BlockParser {
  const char *block;
  bool (*parse)(const VkrJsonReader *entity, EntityImport *out);
};

constexpr BlockParser k_block_parsers[] = {
    {"brush", parse_brush},
    {"brush_face", parse_brush_face},
    {"surface_theme", parse_surface_theme},
    {"blockout", parse_blockout},
    {"mover", parse_mover},
    {"transform", parse_transform},
    {"mesh", parse_mesh},
    {"shape", parse_shape},
    {"point_light", parse_point_light},
    {"directional_light", parse_directional_light},
    {"rectangle_light", parse_rectangle_light},
};

/* Blocks the bake does not read (gameplay, scripts, IO) are ignored. */
bool parse_entities(const std::vector<uint8_t> &bytes,
                    std::vector<EntityImport> *out,
                    std::string *out_diagnostic) {
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader entities = root;
  if (!vkr_json_find_array(&entities, "entities")) {
    *out_diagnostic = "the scene has no \"entities\" array";
    return false;
  }
  uint32_t rectangle_light_count = 0u;
  while (vkr_json_next_array_element(&entities)) {
    const uint32_t index = (uint32_t)out->size();
    if (index == k_max_scene_entities) {
      *out_diagnostic = "the scene has more than " +
                        std::to_string(k_max_scene_entities) + " entities";
      return false;
    }
    VkrJsonReader entity = {};
    if (!vkr_json_enter_object(&entities, &entity)) {
      *out_diagnostic =
          "entity " + std::to_string(index) + " is not a JSON object";
      return false;
    }
    EntityImport imported = {};
    VkrJsonReader name = entity;
    String8 name_text = {};
    if (vkr_json_find_root_field(&name, "name") &&
        vkr_json_parse_string(&name, &name_text)) {
      imported.name.assign((const char *)name_text.str, name_text.length);
    }
    (void)parse_document_id(&entity, &imported);
    VkrJsonReader parent = entity;
    if (vkr_json_find_root_field(&parent, "parent") &&
        !parse_parent(&parent, &imported.parent)) {
      *out_diagnostic = describe_entity(imported, index) +
                        ": \"parent\" is not an entity index or null";
      return false;
    }
    for (const BlockParser &parser : k_block_parsers) {
      if (!parser.parse(&entity, &imported)) {
        *out_diagnostic = describe_entity(imported, index) + ": its \"" +
                          parser.block +
                          "\" block is malformed or not bakeable";
        return false;
      }
    }
    if (imported.has_rectangle_light &&
        ++rectangle_light_count > VKR_MAX_SCENE_RECTANGLE_LIGHTS) {
      *out_diagnostic =
          describe_entity(imported, index) + ": the scene has more than " +
          std::to_string(VKR_MAX_SCENE_RECTANGLE_LIGHTS) + " rectangle lights";
      return false;
    }
    out->push_back(std::move(imported));
  }
  for (uint32_t i = 0; i < out->size(); ++i) {
    if ((*out)[i].parent >= static_cast<int32_t>(out->size())) {
      *out_diagnostic = describe_entity((*out)[i], i) + ": parent " +
                        std::to_string((*out)[i].parent) +
                        " is past the last entity";
      return false;
    }
  }
  return true;
}

/* Largest finite RGBA16F value: the runtime constant-source bound. */
constexpr float32_t k_constant_radiance_max = 65504.0f;

bool constant_radiance_valid(Vec3 radiance) {
  const float32_t channels[] = {radiance.x, radiance.y, radiance.z};
  for (float32_t channel : channels) {
    if (!std::isfinite(channel) || channel < 0.0f ||
        channel > k_constant_radiance_max) {
      return false;
    }
  }
  return true;
}

/* Mirrors the runtime loader's sky-light rules; any stale or conflicting
   field fails the bake instead of silently changing its transport. */
bool parse_environment(const std::vector<uint8_t> &bytes,
                       bool8_t atmosphere_enabled,
                       VkrBakeSceneEnvironment *out) {
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader reader = root;
  const bool has_block = vkr_json_find_root_field(&reader, "environment");
  if (has_block && !parse_null(&reader)) {
    VkrJsonReader object = {};
    if (!vkr_json_enter_object(&reader, &object)) {
      return false;
    }
    out->enabled = true_v;
    (void)read_bool(&object, "enabled", &out->enabled);
    (void)read_float(&object, "intensity", &out->intensity);
    (void)read_float(&object, "diffuse_intensity", &out->diffuse_intensity);
    (void)read_float(&object, "specular_intensity", &out->specular_intensity);
    (void)read_float(&object, "sh_deringing", &out->sh_deringing);
    if (!std::isfinite(out->sh_deringing) || out->sh_deringing < 0.0f) {
      return false;
    }

    for (const char *removed : {"equirect", "cubemap"}) {
      VkrJsonReader field = object;
      if (vkr_json_find_field(&field, removed)) {
        return false;
      }
    }

    VkrJsonReader constant = object;
    if (vkr_json_find_field(&constant, "constant")) {
      if (!parse_vec3(&constant, &out->constant_radiance) ||
          !constant_radiance_valid(out->constant_radiance)) {
        return false;
      }
      out->kind = VkrBakeSceneEnvironmentKind::Constant;
    }
  }

  if (atmosphere_enabled) {
    if (out->kind == VkrBakeSceneEnvironmentKind::Constant) {
      return false;
    }
    if (!has_block) {
      out->enabled = true_v;
    }
    out->kind = VkrBakeSceneEnvironmentKind::Atmosphere;
    return true;
  }
  return !out->enabled || out->kind == VkrBakeSceneEnvironmentKind::Constant;
}

bool parse_subsurface(const std::vector<uint8_t> &bytes, VkrBakeScene *scene) {
  VkrJsonReader reader = vkr_json_reader_create(bytes.data(), bytes.size());
  if (!vkr_json_find_root_field(&reader, "subsurface") || parse_null(&reader))
    return true;
  VkrJsonReader object = {};
  if (!vkr_json_enter_object(&reader, &object))
    return false;
  bool8_t enabled = false_v;
  VkrJsonReader field = object;
  if (vkr_json_find_field(&field, "enabled") &&
      !vkr_json_parse_bool(&field, &enabled))
    return false;
  if (!enabled)
    return true;
  VkrJsonReader profiles = object;
  if (!vkr_json_find_array(&profiles, "profiles"))
    return false;
  uint32_t count = 0u;
  while (vkr_json_next_array_element(&profiles)) {
    if (count == VKR_SUBSURFACE_PROFILE_COUNT ||
        !parse_vec3(&profiles,
                    &scene->subsurface_profiles[count].diffusion_distance) ||
        !vkr_subsurface_profile_valid(scene->subsurface_profiles[count]))
      return false;
    ++count;
  }
  scene->subsurface_profile_count = count;
  return count > 0u;
}

bool parse_atmosphere(const std::vector<uint8_t> &bytes,
                      VkrAtmosphereSettings *out, bool *authored_sun) {
  *authored_sun = false;
  *out = vkr_atmosphere_settings_defaults();
  out->enabled = false_v;
  VkrJsonReader root = vkr_json_reader_create(bytes.data(), bytes.size());
  VkrJsonReader reader = root;
  if (!vkr_json_find_root_field(&reader, "atmosphere") || parse_null(&reader))
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
  VkrJsonReader solar = object;
  const bool has_solar_irradiance =
      vkr_json_find_field(&solar, "solar_irradiance");
  VkrAtmosphereSunAuthoring sun = {};
  VkrJsonReader temperature = object;
  sun.has_temperature =
      vkr_json_find_field(&temperature, "sun_temperature_kelvin");
  VkrJsonReader illuminance = object;
  sun.has_illuminance = vkr_json_find_field(&illuminance, "sun_illuminance");
  if (!optional_bool("enabled", &out->enabled) ||
      !optional_vec3("sun_direction", &out->sun_direction) ||
      !optional_vec3("solar_irradiance", &out->solar_irradiance) ||
      !optional_float("sun_temperature_kelvin", &sun.temperature_kelvin) ||
      !optional_float("sun_illuminance", &sun.illuminance) ||
      !optional_vec3("ground_albedo", &out->ground_albedo) ||
      !optional_float("observer_altitude_m", &out->observer_altitude_m) ||
      !optional_float("sun_angular_diameter_degrees",
                      &out->sun_angular_diameter_degrees) ||
      !optional_float("sun_glow", &out->sun_glow) ||
      !optional_float("star_intensity", &out->star_intensity) ||
      !optional_vec3("celestial_pole", &out->celestial_pole) ||
      !optional_float("rayleigh_density_scale", &out->rayleigh_density_scale) ||
      !optional_float("mie_density_scale", &out->mie_density_scale) ||
      !optional_float("ozone_density_scale", &out->ozone_density_scale) ||
      !optional_float("mie_anisotropy", &out->mie_anisotropy) ||
      !optional_float("metres_per_world_unit", &out->metres_per_world_unit))
    return false;
  /* The SH window moved to $.environment.sh_deringing with the runtime. */
  VkrJsonReader moved = object;
  if (vkr_json_find_field(&moved, "sh_deringing")) {
    return false;
  }
  if (has_solar_irradiance && (sun.has_temperature || sun.has_illuminance)) {
    return false;
  }
  if (!vkr_atmosphere_apply_sun_authoring(out, &sun)) {
    return false;
  }
  VkrJsonReader direction = object;
  *authored_sun = vkr_json_find_field(&direction, "sun_direction") ||
                  has_solar_irradiance || sun.has_temperature ||
                  sun.has_illuminance;
  VkrAtmosphereSettings validation = *out;
  validation.enabled = true_v;
  return vkr_atmosphere_settings_valid(&validation);
}

/* Mirrors vkr_scene_sync_sun for a scene with one atmosphere sun light and
   at most one moon light: the first enabled one of each drives an enabled
   atmosphere's sun or moon through its local rotation, and an unusable light
   keeps the authored one. Only a light is a sun (ADR-058): without one the
   sky has none, unless a legacy atmosphere block authors it, for which the
   runtime loader generates that light. A moon light is never the sun, and
   without one the sky has no moon (ADR-081). */
void apply_sun_light(const std::vector<EntityImport> &entities,
                     bool authored_sun, VkrAtmosphereSettings *settings) {
  if (!settings->enabled)
    return;
  settings->lunar_irradiance = vec3_zero();
  for (const EntityImport &entity : entities) {
    const VkrBakeSceneLight &light = entity.directional_light;
    if (!entity.has_directional_light || !light.enabled ||
        !entity.directional_atmosphere_moon)
      continue;
    (void)vkr_atmosphere_apply_moon_light(
        settings, vkr_quat_rotate_vec3(entity.rotation, light.direction),
        vec3_scale(light.color, light.intensity),
        entity.directional_sun_angular_diameter_degrees);
    break;
  }
  for (const EntityImport &entity : entities) {
    const VkrBakeSceneLight &light = entity.directional_light;
    if (!entity.has_directional_light || !light.enabled ||
        !entity.directional_atmosphere_sun ||
        entity.directional_atmosphere_moon)
      continue;
    (void)vkr_atmosphere_apply_sun_light(
        settings, vkr_quat_rotate_vec3(entity.rotation, light.direction),
        vec3_scale(light.color, light.intensity),
        entity.directional_sun_angular_diameter_degrees);
    return;
  }
  if (!authored_sun) {
    settings->solar_irradiance = vec3_zero();
  }
}

/* On failure `out_diagnostic` names the entity and why. */
bool compute_entity_worlds(const std::vector<EntityImport> &entities,
                           bool include_scale, std::vector<Mat4> *out,
                           std::string *out_diagnostic) {
  out->resize(entities.size());
  std::vector<uint8_t> state(entities.size(), 0u);
  std::array<uint32_t, k_max_transform_depth> chain = {};
  for (uint32_t i = 0; i < entities.size(); ++i) {
    uint32_t cursor = i, count = 0u;
    while (state[cursor] == 0u) {
      if (count == chain.size()) {
        *out_diagnostic = describe_entity(entities[i], i) +
                          ": its parent chain is deeper than " +
                          std::to_string(k_max_transform_depth);
        return false;
      }
      state[cursor] = 1u;
      chain[count++] = cursor;
      const int32_t parent = entities[cursor].parent;
      if (parent < 0)
        break;
      cursor = static_cast<uint32_t>(parent);
      if (state[cursor] == 1u) {
        *out_diagnostic =
            describe_entity(entities[i], i) + ": its parent chain is a cycle";
        return false;
      }
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
      for (float32_t value : (*out)[index].elements) {
        if (!std::isfinite(value)) {
          *out_diagnostic = describe_entity(entity, index) +
                            ": its world transform is not finite";
          return false;
        }
      }
      state[index] = 2u;
    }
  }
  return true;
}

/* Whether the entity or one of its first ancestors (k_max_mover_depth
   entities in all) carries a mover, as the runtime's brush_mover_of finds
   one. compute_entity_worlds has proven the parent chains acyclic and in
   range. */
bool moved_by_mover(const std::vector<EntityImport> &entities, uint32_t index) {
  int32_t at = (int32_t)index;
  for (uint32_t depth = 0u; at >= 0 && depth < k_max_mover_depth; ++depth) {
    if (entities[(size_t)at].mover) {
      return true;
    }
    at = entities[(size_t)at].parent;
  }
  return false;
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
  const EntityImport *entity;
  uint32_t entity_index;
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
  // The runtime lights an atmosphere scene with its sun alone.
  if (source->kind == VKR_BAKE_MESH_LIGHT_DIRECTIONAL &&
      context->scene->atmosphere.enabled)
    return true_v;
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
    // A model's own lights are static members of the default group; a
    // directional one is shadowed like the runtime sun.
    if (light.kind == VkrBakeSceneLightKind::Directional)
      light.casts_shadow = true_v;
    else
      std::snprintf(light.group, sizeof(light.group), "%s",
                    VKR_LIGHT_GROUP_DEFAULT);
    context->scene->lights.push_back(light);
    return true_v;
  } catch (const std::bad_alloc &) {
    return false_v;
  }
}

bool8_t append_mesh_instance(void *user, const VkrBakeMeshInstance *instance) {
  MeshAppendContext *context = static_cast<MeshAppendContext *>(user);
  if (instance->atlas_width == 0u || instance->atlas_height == 0u)
    return true_v;
  try {
    VkrBakeLightmapInstance lightmap;
    lightmap.source_instance_index = instance->source_instance_index;
    lightmap.entity_index = context->entity_index;
    lightmap.document_id = context->entity->document_id;
    lightmap.has_document_id = context->entity->has_document_id;
    lightmap.source_node_index = instance->source_node_index;
    lightmap.world = instance->world;
    lightmap.atlas_width = instance->atlas_width;
    lightmap.atlas_height = instance->atlas_height;
    lightmap.texels_per_unit = instance->texels_per_unit;
    context->scene->lightmap_instances.push_back(lightmap);
    return true_v;
  } catch (const std::bad_alloc &) {
    return false_v;
  }
}

/* Why appending an entity's geometry failed. */
struct AppendFailure {
  VkrBakeSceneError error = VkrBakeSceneError::Parse;
  std::string reason;
};

/* The UVs of every greybox look, as the runtime projects them
   (s_greybox_uv): the metric grid in world space, one repeat per
   VKR_SURFACE_GREYBOX_REPEAT meters. */
BrushFaceStyle greybox_style(const char *material) {
  BrushFaceStyle style;
  style.material = material;
  style.uv_scale =
      vec2_new(VKR_SURFACE_GREYBOX_REPEAT, VKR_SURFACE_GREYBOX_REPEAT);
  return style;
}

/* The scene material of brush material `path`; each path loads once per
   scene. */
bool brush_material(VkrBakeScene *scene, const std::string &path,
                    std::map<std::string, uint32_t> *materials,
                    uint32_t *out_index, AppendFailure *out_failure) {
  const auto cached = materials->find(path);
  if (cached != materials->end()) {
    *out_index = cached->second;
    return true;
  }
  if (!append_material(scene, path, vec4_new(1.0f, 1.0f, 1.0f, 1.0f),
                       out_index)) {
    out_failure->error = VkrBakeSceneError::Material;
    out_failure->reason = "material " + path + " does not load";
    return false;
  }
  materials->emplace(path, *out_index);
  return true;
}

/* Face `f` of built `geometry` placed by `world`, as the runtime writes it
   (brush_write_face): a triangle fan with texture UVs from `style` and,
   when `layout` is set, lightmap UVs from the brush layout. */
bool append_brush_polygon(VkrBakeScene *scene, const VkrBrushGeometry *geometry,
                          uint32_t f, const BrushFaceStyle &style,
                          uint32_t material_index, Mat4 world,
                          Mat4 normal_world, bool flipped,
                          const VkrBrushLightmapLayout *layout,
                          uint32_t source_instance_index) {
  const VkrBrushPolygon polygon = geometry->polygons[f];
  if (polygon.count < 3u) {
    return true;
  }
  const Vec3 local_normal = geometry->normals[f];
  const Vec4 transformed =
      mat4_mul_vec4(normal_world, vec4_new(local_normal.x, local_normal.y,
                                           local_normal.z, 0.0f));
  const Vec3 world_normal =
      vec3_normalize(vec3_new(transformed.x, transformed.y, transformed.z));
  const Vec3 projection_normal = style.uv_world ? world_normal : local_normal;
  VkrBakeVertex corners[VKR_BRUSH_POLYGON_MAX];
  for (uint32_t i = 0u; i < polygon.count; ++i) {
    const Vec3 local = geometry->vertices[polygon.first + i];
    const Vec3 position = mat4_mul_vec3(world, local);
    corners[i] = VkrBakeVertex{};
    corners[i].position = position;
    corners[i].normal = world_normal;
    corners[i].uv =
        vkr_brush_uv(style.uv_world ? position : local, projection_normal,
                     style.uv_offset, style.uv_scale, style.uv_rotation);
    corners[i].color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
    if (layout) {
      corners[i].lightmap_uv =
          vkr_brush_lightmap_uv(layout, geometry, f, local);
    }
    if (!finite_vec3(position) || !finite_vec3(world_normal)) {
      return false;
    }
  }
  for (uint32_t i = 2u; i < polygon.count; ++i) {
    VkrBakeTriangle triangle = {};
    triangle.material_index = material_index;
    triangle.source_instance_index = source_instance_index;
    triangle.vertex[0] = corners[0];
    triangle.vertex[1] = corners[i - 1u];
    triangle.vertex[2] = corners[i];
    if (flipped) {
      std::swap(triangle.vertex[1], triangle.vertex[2]);
    }
    bool zero_area = false;
    if (!triangle_area_is_exactly_zero(&triangle, &zero_area)) {
      return false;
    }
    if (zero_area) {
      ++scene->zero_area_triangle_count;
      continue;
    }
    scene->triangles.push_back(triangle);
  }
  return true;
}

/* A solid or visual brush built from its brush_face children, as the
   runtime builds its mesh: one triangle fan per face in its art-owned
   material or the material `theme` binds to its surface, with UVs from the
   face placement counting repeats of the material's world size, or in its
   surface's greybox look with the greybox UVs, and lightmap UVs from the
   shared brush layout. The brush is one lightmap
   instance. A brush that does not build is left out, as the editor reports
   it. */
bool append_brush(VkrBakeScene *scene,
                  const std::vector<EntityImport> &entities,
                  const std::vector<uint32_t> &faces, uint32_t entity_index,
                  Mat4 world, uint32_t source_instance_index,
                  const VkrSurfaceTheme *theme,
                  std::map<std::string, uint32_t> *materials,
                  AppendFailure *out_failure) {
  if (faces.empty() || faces.size() > VKR_BRUSH_FACE_MAX) {
    return true;
  }
  VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
  for (size_t i = 0u; i < faces.size(); ++i) {
    planes[i] = entities[faces[i]].face_plane;
  }
  std::unique_ptr<VkrBrushGeometry> geometry(new VkrBrushGeometry());
  if (vkr_brush_build(planes, (uint32_t)faces.size(), geometry.get(),
                      nullptr) != VKR_BRUSH_OK) {
    return true;
  }
  VkrBrushLightmapLayout layout = {};
  const bool lightmapped =
      vkr_brush_lightmap_layout(geometry.get(), &layout) != false_v;
  Mat4 normal_world = {};
  bool flipped = false;
  if (!normal_matrix(world, &normal_world, &flipped)) {
    out_failure->reason = "its world transform is singular";
    return false;
  }
  for (uint32_t f = 0u; f < geometry->face_count; ++f) {
    const BrushFaceStyle &face = entities[faces[f]].face;
    const Vec3 local_normal = geometry->normals[f];
    const Vec4 normal =
        mat4_mul_vec4(normal_world, vec4_new(local_normal.x, local_normal.y,
                                             local_normal.z, 0.0f));
    const Vec3 world_normal =
        vec3_normalize(vec3_new(normal.x, normal.y, normal.z));
    const char *look = vkr_surface_face_material(
        (VkrSurface)face.surface, (VkrSurfaceMark)face.mark,
        face.material.c_str(), theme, nullptr, world_normal.y, false_v);
    const bool art = look == face.material.c_str() ||
                     look == vkr_surface_theme_material(
                                 theme, nullptr, (VkrSurface)face.surface);
    BrushFaceStyle style = art ? face : greybox_style(look);
    style.material = look;
    uint32_t material_index = 0u;
    if (!brush_material(scene, style.material, materials, &material_index,
                        out_failure)) {
      return false;
    }
    const Vec2 world_size = scene->materials[material_index].world_size;
    if (art && world_size.x > 0.0f && world_size.y > 0.0f) {
      style.uv_scale = vec2_new(style.uv_scale.x * world_size.x,
                                style.uv_scale.y * world_size.y);
    }
    if (!append_brush_polygon(scene, geometry.get(), f, style, material_index,
                              world, normal_world, flipped,
                              lightmapped ? &layout : nullptr,
                              source_instance_index)) {
      out_failure->reason = "face " + std::to_string(f) + " is not finite";
      return false;
    }
  }
  if (lightmapped) {
    VkrBakeLightmapInstance instance;
    instance.source_instance_index = source_instance_index;
    instance.document_id = entities[entity_index].document_id;
    instance.has_document_id = entities[entity_index].has_document_id;
    instance.entity_index = entity_index;
    instance.source_node_index = 0u;
    instance.world = world;
    instance.atlas_width = layout.width;
    instance.atlas_height = layout.height;
    instance.texels_per_unit = layout.texels_per_unit;
    scene->lightmap_instances.push_back(instance);
  }
  return true;
}

/* A blockout shape (ADR-084) built as the runtime builds it
   (brush_rebuild_shape): each laid-out piece is the brush of its hull, in the
   wall look of the shape's surface, floors in the floor look of its floor
   surface, with the greybox UVs.
   Shapes take no lightmap at runtime, so their faces occlude and bounce
   light without becoming a lightmap instance. A shape that does not lay out,
   or a piece that does not build, is left out as the runtime leaves it. */
bool append_blockout(VkrBakeScene *scene, const SceneBlockout &shape,
                     Mat4 world, uint32_t source_instance_index,
                     std::map<std::string, uint32_t> *materials,
                     AppendFailure *out_failure) {
  std::vector<VkrBlockoutPiece> pieces(vkr_blockout_piece_capacity(&shape));
  char error[96] = {};
  const uint32_t count = vkr_blockout_layout(
      &shape, pieces.data(), (uint32_t)pieces.size(), error, sizeof(error));
  if (count == 0u) {
    return true;
  }
  Mat4 normal_world = {};
  bool flipped = false;
  if (!normal_matrix(world, &normal_world, &flipped)) {
    out_failure->reason = "its world transform is singular";
    return false;
  }
  const VkrSurface floor_surface = shape.floor_surface != VKR_SURFACE_NONE
                                       ? shape.floor_surface
                                       : shape.surface;
  const BrushFaceStyle styles[2] = {
      greybox_style(vkr_surface_greybox_material(shape.surface, shape.mark,
                                                 VKR_SURFACE_WALL)),
      greybox_style(vkr_surface_greybox_material(floor_surface, shape.mark,
                                                 VKR_SURFACE_FLOOR)),
  };
  uint32_t material_indices[2] = {};
  for (uint32_t i = 0u; i < 2u; ++i) {
    if (!brush_material(scene, styles[i].material, materials,
                        &material_indices[i], out_failure)) {
      return false;
    }
  }
  std::unique_ptr<VkrBrushGeometry> geometry(new VkrBrushGeometry());
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrBlockoutPiece &piece = pieces[i];
    VkrBrushPlane planes[VKR_BRUSH_FACE_MAX];
    const uint32_t plane_count = vkr_brush_hull(piece.points, piece.point_count,
                                                planes, VKR_BRUSH_FACE_MAX);
    if (plane_count == 0u ||
        vkr_brush_build(planes, plane_count, geometry.get(), nullptr) !=
            VKR_BRUSH_OK) {
      continue;
    }
    const uint32_t style = piece.kind == VKR_BLOCKOUT_PIECE_FLOOR ? 1u : 0u;
    for (uint32_t f = 0u; f < geometry->face_count; ++f) {
      if (!append_brush_polygon(scene, geometry.get(), f, styles[style],
                                material_indices[style], world, normal_world,
                                flipped, nullptr, source_instance_index)) {
        out_failure->reason = "piece " + std::to_string(i) + " is not finite";
        return false;
      }
    }
  }
  return true;
}

bool append_mesh(VkrBakeScene *scene, const std::string &path,
                 const EntityImport &entity, uint32_t entity_index,
                 Mat4 entity_world, uint32_t *next_instance) {
  std::vector<uint8_t> bytes;
  if (!read_file(path.c_str(), &bytes))
    return false;
  MeshAppendContext context = {
      .scene = scene, .entity = &entity, .entity_index = entity_index};
  const VkrBakeMeshDecodeCallbacks callbacks = {
      .emit_triangle = append_mesh_triangle,
      .emit_light = append_mesh_light,
      .emit_instance = append_mesh_instance,
  };
  String8 source_path = {.str = (uint8_t *)path.data(), .length = path.size()};
  if (!vkr_bake_mesh_decode_file(source_path, bytes.data(), bytes.size(),
                                 entity_world, next_instance, &callbacks,
                                 &context))
    return false;
  const std::string sidecar = path + ".remap.json";
  std::error_code sidecar_error;
  if (std::filesystem::is_regular_file(vkr_filesystem_native_utf8_path(sidecar),
                                       sidecar_error) &&
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

/* A dynamic point or rectangle light stays out: no bake holds it. */
bool append_authored_lights(VkrBakeScene *scene, const EntityImport &entity,
                            Mat4 world, Mat4 rigid_world,
                            bool suppress_directional) {
  auto append = [&](VkrBakeSceneLight light) {
    light.position = mat4_mul_vec3(world, vec3_zero());
    light.document_id = entity.document_id;
    light.has_document_id = entity.has_document_id;
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
  return (!entity.has_point_light || !entity.point_light_static ||
          append(entity.point_light)) &&
         (suppress_directional || !entity.has_directional_light ||
          append(entity.directional_light)) &&
         (!entity.has_rectangle_light || !entity.rectangle_light_static ||
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
  scene->diagnostic.clear();
  /* Leaves the scene empty with the cause for the caller to report. */
  auto fail = [&](VkrBakeSceneError error, const std::string &message) {
    reset_scene(scene);
    set_error(error, out_error);
    scene->diagnostic = message;
    return false;
  };
  try {
    std::vector<uint8_t> json;
    if (!read_file(scene_path, &json)) {
      return fail(VkrBakeSceneError::Io,
                  std::string("cannot read ") + scene_path);
    }
    std::vector<EntityImport> entities;
    std::string diagnostic;
    if (!parse_entities(json, &entities, &diagnostic)) {
      return fail(VkrBakeSceneError::Parse, diagnostic);
    }
    VkrAtmosphereSettings atmosphere_settings = {};
    bool authored_sun = false;
    if (!append_unique_path(&scene->dependency_paths, scene_path) ||
        !parse_subsurface(json, scene)) {
      return fail(VkrBakeSceneError::Parse,
                  "the subsurface block is malformed or invalid");
    }
    if (!parse_atmosphere(json, &atmosphere_settings, &authored_sun)) {
      return fail(VkrBakeSceneError::Parse,
                  "the atmosphere block is malformed, invalid or authors a "
                  "moved field");
    }
    apply_sun_light(entities, authored_sun, &atmosphere_settings);
    scene->atmosphere_settings = atmosphere_settings;
    if (!vkr_bake_atmosphere_build(&scene->atmosphere, &atmosphere_settings)) {
      return fail(VkrBakeSceneError::Parse,
                  "the atmosphere does not build from its settings");
    }
    if (!parse_environment(json, scene->atmosphere.enabled,
                           &scene->environment)) {
      return fail(VkrBakeSceneError::Parse,
                  "the environment block is malformed, authors a removed "
                  "source, or a constant sky beside an atmosphere");
    }
    std::vector<Mat4> worlds;
    std::vector<Mat4> rigid_worlds;
    if (!compute_entity_worlds(entities, true, &worlds, &diagnostic) ||
        !compute_entity_worlds(entities, false, &rigid_worlds, &diagnostic)) {
      return fail(VkrBakeSceneError::Parse, diagnostic);
    }
    scene->texture_store = vkr_bake_texture_store_create(scene->allocator);
    if (!scene->texture_store) {
      return fail(VkrBakeSceneError::OutOfMemory,
                  "the texture store does not allocate");
    }
    uint32_t next_instance = 0u;
    /* The scene's surface theme, the first one, as the runtime resolves a
       singleton; a theme that does not read binds nothing, as at runtime.
       The World's theme, like its sun and sky, does not reach the bake. */
    VkrSurfaceTheme surface_theme = {};
    for (const EntityImport &entity : entities) {
      if (entity.surface_theme.empty()) {
        continue;
      }
      std::vector<uint8_t> theme_json;
      if (read_file(entity.surface_theme.c_str(), &theme_json) &&
          vkr_surface_theme_read(
              string8_create_from_cstr(theme_json.data(), theme_json.size()),
              &surface_theme, nullptr, 0u)) {
        append_unique_path(&scene->dependency_paths, entity.surface_theme);
      } else {
        surface_theme = {};
      }
      break;
    }
    /* A brush's faces are its brush_face children. */
    std::vector<std::vector<uint32_t>> brush_faces(entities.size());
    for (uint32_t i = 0; i < entities.size(); ++i) {
      if (entities[i].brush_face && entities[i].parent >= 0) {
        brush_faces[(size_t)entities[i].parent].push_back(i);
      }
    }
    std::map<std::string, uint32_t> brush_materials;
    for (uint32_t i = 0; i < entities.size(); ++i) {
      const EntityImport &entity = entities[i];
      if (!append_authored_lights(scene, entity, worlds[i], rigid_worlds[i],
                                  scene->atmosphere.enabled)) {
        return fail(VkrBakeSceneError::Parse,
                    describe_entity(entity, i) +
                        ": its light's world position or direction is not "
                        "finite");
      }
      if (!entity.skip_geometry && !entity.mesh_path.empty() &&
          !append_mesh(scene, entity.mesh_path, entity, i, worlds[i],
                       &next_instance)) {
        return fail(VkrBakeSceneError::CookedMesh,
                    describe_entity(entity, i) + ": cooked mesh " +
                        entity.mesh_path +
                        " does not read or decode, or a material it names "
                        "does not load");
      }
      if (entity.shape == ShapeKind::Unsupported ||
          (entity.shape == ShapeKind::Cube &&
           !append_cube(scene, entity, worlds[i], next_instance++))) {
        return fail(VkrBakeSceneError::Unsupported,
                    describe_entity(entity, i) +
                        ": its cube shape's material does not load or its "
                        "world transform is singular");
      }
      /* A brush or blockout shape a mover moves is out of every bake: it
         takes no lightmap and neither blocks nor bounces baked light, since
         the bake would hold it at its saved pose. Runtime lights light it. */
      const bool moves =
          (entity.brush || entity.blockout) && moved_by_mover(entities, i);
      AppendFailure failure;
      if (entity.brush && entity.brush_draws && !moves &&
          !append_brush(scene, entities, brush_faces[i], i, worlds[i],
                        next_instance++, &surface_theme, &brush_materials,
                        &failure)) {
        return fail(failure.error,
                    describe_entity(entity, i) + ": brush: " + failure.reason);
      }
      if (entity.blockout && !moves &&
          !append_blockout(scene, *entity.blockout, worlds[i], next_instance++,
                           &brush_materials, &failure)) {
        return fail(failure.error, describe_entity(entity, i) +
                                       ": blockout: " + failure.reason);
      }
    }
    if (scene->atmosphere.enabled) {
      const Vec3 key = scene->atmosphere.key_light_direction;
      scene->atmosphere_light = (uint32_t)scene->lights.size();
      scene->lights.push_back((VkrBakeSceneLight){
          .kind = VkrBakeSceneLightKind::Directional,
          .direction = vec3_new(-key.x, -key.y, -key.z),
          .color = scene->atmosphere.observer_irradiance,
          .intensity = 1.0f,
          .casts_shadow = true_v,
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
        return fail(VkrBakeSceneError::Material, "texture dependency " +
                                                     std::to_string(texture) +
                                                     " has no path");
      }
    }
    return true;
  } catch (const std::bad_alloc &) {
    return fail(VkrBakeSceneError::OutOfMemory, "out of memory");
  }
}

bool vkr_bake_scene_build_sun_atmosphere(const VkrBakeScene *scene,
                                         Vec3 sun_direction,
                                         VkrBakeAtmosphere *out) {
  if (!scene || !out || !scene->atmosphere.enabled ||
      !finite_vec3(sun_direction)) {
    return false;
  }
  VkrAtmosphereSettings settings = scene->atmosphere_settings;
  settings.sun_direction = vec3_normalize(sun_direction);
  return vkr_bake_atmosphere_build(out, &settings);
}

void vkr_bake_scene_use_atmosphere(VkrBakeScene *scene,
                                   const VkrBakeAtmosphere &atmosphere) {
  scene->atmosphere = atmosphere;
  if (scene->atmosphere_light < scene->lights.size()) {
    VkrBakeSceneLight &light = scene->lights[scene->atmosphere_light];
    const Vec3 key = atmosphere.key_light_direction;
    light.direction = vec3_new(-key.x, -key.y, -key.z);
    light.color = atmosphere.observer_irradiance;
  }
}

Vec3 vkr_bake_scene_sample_environment(const VkrBakeScene *scene,
                                       Vec3 direction) {
  const VkrBakeSceneEnvironment &environment = scene->environment;
  if (!environment.enabled) {
    return vec3_zero();
  }

  /* Transport scales sky radiance by the overall sky-light intensity, the
     convention the image environment used before the atmosphere replaced it. */
  if (environment.kind == VkrBakeSceneEnvironmentKind::Atmosphere) {
    return vec3_scale(vkr_bake_atmosphere_sample(&scene->atmosphere, direction),
                      environment.intensity);
  }
  if (environment.kind == VkrBakeSceneEnvironmentKind::Constant) {
    return vec3_scale(environment.constant_radiance, environment.intensity);
  }
  return vec3_zero();
}

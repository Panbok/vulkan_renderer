#include "renderer/systems/vkr_scene_types.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/vkr_text.h"
#include "vkr_bloom.h"
#include "vkr_dof.h"
#include "vkr_exposure.h"
#include "vkr_gtao.h"
#include "vkr_motion_blur.h"

#define TYPE_OFFSET(type, field) (uint32_t)offsetof(type, field)

/* ---- Transform and visibility ---- */

static const VkrPropertyDesc s_transform_properties[] = {
    {.name = "position",
     .label = "Location",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneTransform, position),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.01f},
    {.name = "rotation",
     .label = "Rotation",
     .unit = "deg",
     .offset = TYPE_OFFSET(SceneTransform, rotation),
     .kind = VKR_PROPERTY_QUAT,
     .step = 0.5f},
    {.name = "scale",
     .label = "Scale",
     .offset = TYPE_OFFSET(SceneTransform, scale),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.005f},
};

static bool8_t transform_validate(const void *value, char *error,
                                  uint32_t capacity) {
  const SceneTransform *transform = value;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (fabsf(transform->scale.elements[axis]) < 0.000001f) {
      snprintf(error, capacity, "Scale components must not be zero.");
      return false_v;
    }
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_transform_type = {
    .name = "transform",
    .label = "Transform",
    .properties = s_transform_properties,
    .property_count = ArrayCount(s_transform_properties),
    .size = sizeof(SceneTransform),
    .align = AlignOf(SceneTransform),
    .validate = transform_validate,
};

static const VkrPropertyDesc s_visibility_properties[] = {
    {.name = "visible",
     .label = "Visible",
     .offset = TYPE_OFFSET(SceneVisibility, visible),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "inherit_parent",
     .label = "Inherit parent visibility",
     .tooltip = "Hidden when any ancestor is hidden",
     .offset = TYPE_OFFSET(SceneVisibility, inherit_parent),
     .kind = VKR_PROPERTY_BOOL},
};

const VkrTypeDesc vkr_scene_visibility_type = {
    .name = "visibility",
    .label = "Visibility",
    .properties = s_visibility_properties,
    .property_count = ArrayCount(s_visibility_properties),
    .size = sizeof(SceneVisibility),
    .align = AlignOf(SceneVisibility),
};

/* ---- Point and spot lights ---- */

static const char *const s_point_light_kind_names[] = {"polynomial", "point",
                                                       "spot", NULL};

static const char *const s_light_mobility_names[] = {"static", "dynamic", NULL};

_Static_assert(sizeof(VkrLightMobility) == sizeof(uint32_t),
               "ENUM properties store four bytes");

/* The light-baking properties of point and rectangle lights (ADR-088). */
#define SCENE_LIGHT_MOBILITY_PROPERTY(type)                                    \
  {                                                                            \
      .name = "mobility",                                                      \
      .label = "Mobility",                                                     \
      .group = "Baking",                                                       \
      .tooltip = "Static lights bake into their group's lightmaps on tiled "   \
                 "GPUs; dynamic lights light at runtime",                      \
      .names = s_light_mobility_names,                                         \
      .offset = TYPE_OFFSET(type, mobility),                                   \
      .kind = VKR_PROPERTY_ENUM,                                               \
  }

#define SCENE_LIGHT_GROUP_PROPERTY(type)                                       \
  {                                                                            \
      .name = "light_group",                                                   \
      .label = "Light group",                                                  \
      .tooltip = "Baked lights of one group switch and dim together; empty "   \
                 "is the default group",                                       \
      .offset = TYPE_OFFSET(type, light_group),                                \
      .capacity = sizeof(((type *)0)->light_group),                            \
      .kind = VKR_PROPERTY_STRING,                                             \
  }

/* A group name the validators accept, or the reason it is not. */
static bool8_t scene_light_baking_validate(const char *group, char *error,
                                           uint32_t capacity) {
  if (!vkr_light_group_name_valid(group, strlen(group))) {
    snprintf(error, capacity,
             "Name light groups with letters, digits, '_' or '-'.");
    return false_v;
  }
  return true_v;
}

_Static_assert(sizeof(VkrPointLightKind) == sizeof(uint32_t),
               "ENUM properties store four bytes");

static const VkrPropertyDesc s_point_light_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(ScenePointLight, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "casts_shadow",
     .label = "Cast shadows",
     .tooltip = "Requires a finite positive range",
     .offset = TYPE_OFFSET(ScenePointLight, casts_shadow),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "source_radius",
     .label = "Source radius",
     .tooltip = "Soften shadow edges with distance from the caster",
     .unit = "m",
     .offset = TYPE_OFFSET(ScenePointLight, source_radius),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = VKR_POINT_LIGHT_SOURCE_RADIUS_MAX,
     .step = 0.005f},
    {.name = "kind",
     .label = "Kind",
     .tooltip = "Attenuation model and cone, fixed by the source light",
     .names = s_point_light_kind_names,
     .offset = TYPE_OFFSET(ScenePointLight, kind),
     .kind = VKR_PROPERTY_ENUM,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "color",
     .label = "Color",
     .tooltip = "Linear RGB color",
     .offset = TYPE_OFFSET(ScenePointLight, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.005f},
    {.name = "intensity",
     .label = "Intensity",
     .offset = TYPE_OFFSET(ScenePointLight, intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "range",
     .label = "Range",
     .tooltip = "Attenuation range; 0 is unlimited",
     .unit = "m",
     .zero_label = "Unlimited",
     .offset = TYPE_OFFSET(ScenePointLight, range),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "direction",
     .label = "Direction",
     .tooltip = "Local light direction; node rotation also applies",
     .offset = TYPE_OFFSET(ScenePointLight, direction_local),
     .kind = VKR_PROPERTY_DIRECTION,
     .step = 0.5f},
    {.name = "inner_cone",
     .label = "Inner cone",
     .tooltip = "Cone half-angle; inner must be less than outer",
     .unit = "deg",
     .offset = TYPE_OFFSET(ScenePointLight, inner_cone_angle),
     .kind = VKR_PROPERTY_ANGLE,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 90.0f,
     .step = 0.25f},
    {.name = "outer_cone",
     .label = "Outer cone",
     .tooltip = "Cone half-angle; inner must be less than outer",
     .unit = "deg",
     .offset = TYPE_OFFSET(ScenePointLight, outer_cone_angle),
     .kind = VKR_PROPERTY_ANGLE,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 90.0f,
     .step = 0.25f},
    {.name = "constant",
     .label = "Constant attenuation",
     .offset = TYPE_OFFSET(ScenePointLight, constant),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_HIDDEN,
     .min = 0.0f,
     .max = FLT_MAX},
    {.name = "linear",
     .label = "Linear attenuation",
     .offset = TYPE_OFFSET(ScenePointLight, linear),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_HIDDEN,
     .min = 0.0f,
     .max = FLT_MAX},
    {.name = "quadratic",
     .label = "Quadratic attenuation",
     .offset = TYPE_OFFSET(ScenePointLight, quadratic),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_HIDDEN,
     .min = 0.0f,
     .max = FLT_MAX},
    SCENE_LIGHT_MOBILITY_PROPERTY(ScenePointLight),
    SCENE_LIGHT_GROUP_PROPERTY(ScenePointLight),
};

static bool8_t point_light_validate(const void *value, char *error,
                                    uint32_t capacity) {
  const ScenePointLight *light = value;
  const bool8_t spot = light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  if (light->outer_cone_angle < light->inner_cone_angle ||
      light->outer_cone_angle > 1.5707964f ||
      !(cosf(light->inner_cone_angle) > cosf(light->outer_cone_angle))) {
    snprintf(error, capacity,
             "Use 0 <= inner < outer <= 90 degrees; increase the gap between "
             "cone angles.");
    return false_v;
  }
  if (light->casts_shadow &&
      (light->range <= 0.0f ||
       (spot && (light->outer_cone_angle <= 0.0f ||
                 light->outer_cone_angle >= 1.57079632679f)))) {
    snprintf(error, capacity,
             "Shadows require positive range and a spot outer angle below 90 "
             "degrees.");
    return false_v;
  }
  if (spot && !(vec3_dot(light->direction_local, light->direction_local) >=
                0.000001f)) {
    snprintf(error, capacity, "A spot light needs a nonzero direction.");
    return false_v;
  }
  return scene_light_baking_validate(light->light_group, error, capacity);
}

static VkrPropertyState point_light_state(const void *value, uint32_t property,
                                          const void *context) {
  (void)context;
  const ScenePointLight *light = value;
  const uint32_t offset = s_point_light_properties[property].offset;
  VkrPropertyState state = {0};
  if (light->kind != VKR_POINT_LIGHT_KIND_GLTF_SPOT &&
      (offset == offsetof(ScenePointLight, direction_local) ||
       offset == offsetof(ScenePointLight, inner_cone_angle) ||
       offset == offsetof(ScenePointLight, outer_cone_angle))) {
    state.flags |= VKR_PROPERTY_STATE_HIDDEN;
  }
  /* The radius only shapes shadow penumbrae. */
  if (!light->casts_shadow &&
      offset == offsetof(ScenePointLight, source_radius)) {
    state.flags |= VKR_PROPERTY_STATE_DISABLED;
  }
  /* Only baked lights belong to a group. */
  if (light->mobility == VKR_LIGHT_MOBILITY_DYNAMIC &&
      offset == offsetof(ScenePointLight, light_group)) {
    state.flags |= VKR_PROPERTY_STATE_DISABLED;
  }
  return state;
}

/* A new light the editor places: bright enough to read in a lit scene and
   inside the cone rules spot lights need. */
static void point_light_defaults(void *value) {
  *(ScenePointLight *)value = (ScenePointLight){
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
      .color = vec3_one(),
      .intensity = 5.0f,
      .constant = 1.0f,
      .range = 10.0f,
      .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
      .inner_cone_angle = 0.35f,
      .outer_cone_angle = 0.6f,
      .enabled = true_v,
      .casts_shadow = true_v,
  };
}

const VkrTypeDesc vkr_scene_point_light_type = {
    .name = "point_light",
    .label = "Point light",
    .properties = s_point_light_properties,
    .property_count = ArrayCount(s_point_light_properties),
    .size = sizeof(ScenePointLight),
    .align = AlignOf(ScenePointLight),
    .defaults = point_light_defaults,
    .validate = point_light_validate,
    .state = point_light_state,
};

/* ---- Directional lights ---- */

static const VkrPropertyDesc s_directional_light_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneDirectionalLight, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "atmosphere_sun",
     .label = "Atmosphere sun",
     .tooltip = "Drives the atmosphere's sun direction and color",
     .offset = TYPE_OFFSET(SceneDirectionalLight, atmosphere_sun),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "atmosphere_moon",
     .label = "Atmosphere moon",
     .tooltip = "Drives the atmosphere's moon, its night light; a moon is "
                "never the sun",
     .offset = TYPE_OFFSET(SceneDirectionalLight, atmosphere_moon),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "color",
     .label = "Color",
     .tooltip = "Linear RGB color",
     .offset = TYPE_OFFSET(SceneDirectionalLight, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.005f},
    {.name = "intensity",
     .label = "Intensity",
     .offset = TYPE_OFFSET(SceneDirectionalLight, intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "direction",
     .label = "Direction",
     .tooltip = "Local light direction; node rotation also applies",
     .offset = TYPE_OFFSET(SceneDirectionalLight, direction_local),
     .kind = VKR_PROPERTY_DIRECTION,
     .step = 0.5f},
    {.name = "sun_diameter",
     .label = "Sun diameter",
     .tooltip = "Visible sun-disc diameter and shadow softness; zero keeps a "
                "hard PCF edge and the default disc",
     .unit = "deg",
     .offset = TYPE_OFFSET(SceneDirectionalLight, sun_angular_diameter_degrees),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = VKR_DIRECTIONAL_LIGHT_MAX_SUN_ANGULAR_DIAMETER_DEGREES,
     .step = 0.01f},
    {.name = "temperature",
     .label = "Temperature",
     .tooltip = "Color temperature; 0 uses the authored color",
     .unit = "K",
     .zero_label = "Off",
     .offset = TYPE_OFFSET(SceneDirectionalLight, temperature_kelvin),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K,
     .max = VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K,
     .step = 10.0f},
};

static bool8_t directional_light_validate(const void *value, char *error,
                                          uint32_t capacity) {
  const SceneDirectionalLight *light = value;
  if (!(vec3_dot(light->direction_local, light->direction_local) >=
        0.000001f)) {
    snprintf(error, capacity, "A directional light needs a nonzero direction.");
    return false_v;
  }
  if (light->temperature_kelvin != 0.0f &&
      light->temperature_kelvin < VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K) {
    snprintf(error, capacity, "Temperature must be 0 or 1000..40000 K.");
    return false_v;
  }
  return true_v;
}

/* A moon light is never the sun, so its sun flag has no effect, and its disc
   is the moon's. */
static VkrPropertyState directional_light_state(const void *value,
                                                uint32_t property,
                                                const void *context) {
  (void)context;
  const SceneDirectionalLight *light = value;
  const uint32_t offset = s_directional_light_properties[property].offset;
  VkrPropertyState state = {0};
  if (!light->atmosphere_moon) {
    return state;
  }
  if (offset == offsetof(SceneDirectionalLight, atmosphere_sun)) {
    state.flags |= VKR_PROPERTY_STATE_DISABLED;
  } else if (offset ==
             offsetof(SceneDirectionalLight, sun_angular_diameter_degrees)) {
    state.label = string8_lit("Moon diameter");
  }
  return state;
}

static void directional_light_defaults(void *value) {
  *(SceneDirectionalLight *)value = (SceneDirectionalLight){
      .color = vec3_one(),
      .intensity = 3.0f,
      .direction_local = vec3_normalize(vec3_new(-0.3f, -0.8f, -0.5f)),
      .sun_angular_diameter_degrees =
          VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES,
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
}

const VkrTypeDesc vkr_scene_directional_light_type = {
    .name = "directional_light",
    .label = "Directional light",
    .properties = s_directional_light_properties,
    .property_count = ArrayCount(s_directional_light_properties),
    .size = sizeof(SceneDirectionalLight),
    .align = AlignOf(SceneDirectionalLight),
    .defaults = directional_light_defaults,
    .validate = directional_light_validate,
    .state = directional_light_state,
};

/* ---- Rectangle lights ---- */

static const VkrPropertyDesc s_rectangle_light_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneRectangleLight, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "color",
     .label = "Color",
     .tooltip = "Linear RGB color",
     .offset = TYPE_OFFSET(SceneRectangleLight, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.005f},
    {.name = "radiance",
     .label = "Radiance",
     .offset = TYPE_OFFSET(SceneRectangleLight, radiance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "size",
     .label = "Size",
     .tooltip = "Width and height of the emitting rectangle",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneRectangleLight, size),
     .kind = VKR_PROPERTY_VEC2,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    SCENE_LIGHT_MOBILITY_PROPERTY(SceneRectangleLight),
    SCENE_LIGHT_GROUP_PROPERTY(SceneRectangleLight),
};

static bool8_t rectangle_light_validate(const void *value, char *error,
                                        uint32_t capacity) {
  const SceneRectangleLight *light = value;
  if (!(light->size.x > 0.0f) || !(light->size.y > 0.0f)) {
    snprintf(error, capacity, "Rectangle size must be positive.");
    return false_v;
  }
  return scene_light_baking_validate(light->light_group, error, capacity);
}

static void rectangle_light_defaults(void *value) {
  *(SceneRectangleLight *)value = (SceneRectangleLight){
      .color = vec3_one(),
      .radiance = 5.0f,
      .size = vec2_new(1.0f, 1.0f),
      .enabled = true_v,
  };
}

const VkrTypeDesc vkr_scene_rectangle_light_type = {
    .name = "rectangle_light",
    .label = "Rectangle light",
    .properties = s_rectangle_light_properties,
    .property_count = ArrayCount(s_rectangle_light_properties),
    .size = sizeof(SceneRectangleLight),
    .align = AlignOf(SceneRectangleLight),
    .defaults = rectangle_light_defaults,
    .validate = rectangle_light_validate,
};

/* ---- World components (ADR-076) ---- */

#define PROPERTY_BOOL(type, field, key, text, tip)                             \
  {.name = key,                                                                \
   .label = text,                                                              \
   .tooltip = tip,                                                             \
   .offset = TYPE_OFFSET(type, field),                                         \
   .kind = VKR_PROPERTY_BOOL}

static const char *const s_environment_source_names[] = {"none", "constant",
                                                         "atmosphere", NULL};

_Static_assert(sizeof(VkrSceneEnvironmentSourceKind) == sizeof(uint32_t),
               "ENUM properties store four bytes");

static const VkrPropertyDesc s_environment_properties[] = {
    PROPERTY_BOOL(SceneEnvironmentSettings, enabled, "enabled", "Sky lighting",
                  "Light the scene from the environment; the sky stays "
                  "visible when off"),
    {.name = "source",
     .label = "Source",
     .tooltip = "Producer of the sky light; applies on the next load",
     .names = s_environment_source_names,
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, source_kind),
     .kind = VKR_PROPERTY_ENUM,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "constant_radiance",
     .label = "Constant radiance",
     .tooltip = "Scene-linear radiance of a constant source; applies on the "
                "next load",
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, constant_radiance),
     .kind = VKR_PROPERTY_COLOR,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY,
     .min = 0.0f,
     .max = VKR_SCENE_ENVIRONMENT_CONSTANT_MAX},
    {.name = "intensity",
     .label = "Intensity",
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "diffuse_intensity",
     .label = "Diffuse intensity",
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, diffuse_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "specular_intensity",
     .label = "Specular intensity",
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, specular_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "sh_deringing",
     .label = "SH deringing",
     .tooltip = "L2 deringing exponent; applies on the next bake",
     .offset = TYPE_OFFSET(SceneEnvironmentSettings, sh_deringing),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY,
     .min = 0.0f,
     .max = FLT_MAX},
};

static void environment_defaults(void *value) {
  SceneEnvironmentSettings *environment = value;
  environment->enabled = true_v;
  environment->source_kind = VKR_SCENE_ENV_SOURCE_NONE;
  environment->intensity = 1.0f;
  environment->diffuse_intensity = 1.0f;
  environment->specular_intensity = 1.0f;
}

const VkrTypeDesc vkr_scene_environment_type = {
    .name = "environment",
    .label = "Sky light",
    .category = "Lighting",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_environment_properties,
    .property_count = ArrayCount(s_environment_properties),
    .size = sizeof(SceneEnvironmentSettings),
    .align = AlignOf(SceneEnvironmentSettings),
    .defaults = environment_defaults,
};

static const VkrPropertyDesc s_atmosphere_properties[] = {
    PROPERTY_BOOL(VkrAtmosphereSettings, enabled, "enabled", "Enabled", NULL),
    {.name = "ground_albedo",
     .label = "Ground albedo",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, ground_albedo),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "observer_altitude_m",
     .label = "Observer altitude",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, observer_altitude_m),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100000.0f,
     .step = 1.0f},
    /* The directional light owns the visible disc; this saved value is only
       the disc of a hard-shadow light, whose diameter is zero. */
    {.name = "sun_angular_diameter_degrees",
     .label = "Sun diameter",
     .unit = "deg",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, sun_angular_diameter_degrees),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER | VKR_PROPERTY_FLAG_HIDDEN,
     .min = 0.001f,
     .max = 5.0f,
     .step = 0.005f},
    {.name = "sun_glow",
     .label = "Sun glow",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, sun_glow),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = VKR_ATMOSPHERE_SUN_GLOW_MAX,
     .step = 0.05f},
    {.name = "star_intensity",
     .label = "Stars",
     .group = "Night",
     .tooltip = "Star-field brightness; 1 is physical beside the default sun",
     .unit = "x",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, star_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = VKR_ATMOSPHERE_STAR_INTENSITY_MAX,
     .step = 0.05f},
    {.name = "celestial_pole",
     .label = "Celestial pole",
     .tooltip = "Direction the stars turn about with the sun; its elevation "
                "is the latitude",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, celestial_pole),
     .kind = VKR_PROPERTY_DIRECTION,
     .step = 0.5f},
    {.name = "rayleigh_density_scale",
     .label = "Rayleigh density",
     .group = "Medium",
     .unit = "x",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, rayleigh_density_scale),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100.0f,
     .step = 0.01f},
    {.name = "mie_density_scale",
     .label = "Mie density",
     .unit = "x",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, mie_density_scale),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100.0f,
     .step = 0.01f},
    {.name = "ozone_density_scale",
     .label = "Ozone density",
     .unit = "x",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, ozone_density_scale),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100.0f,
     .step = 0.01f},
    {.name = "mie_anisotropy",
     .label = "Mie anisotropy",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, mie_anisotropy),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -0.95f,
     .max = 0.95f,
     .step = 0.005f},
    {.name = "metres_per_world_unit",
     .label = "Metres per unit",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrAtmosphereSettings, metres_per_world_unit),
     .kind = VKR_PROPERTY_F32,
     .min = VKR_ATMOSPHERE_METRES_PER_UNIT_MIN,
     .max = VKR_ATMOSPHERE_METRES_PER_UNIT_MAX,
     .step = 0.01f},
};

static bool8_t atmosphere_validate(const void *value, char *error,
                                   uint32_t capacity) {
  if (!vkr_atmosphere_settings_valid(value)) {
    snprintf(error, capacity, "The atmosphere rejects these values.");
    return false_v;
  }
  return true_v;
}

static void atmosphere_defaults(void *value) {
  *(VkrAtmosphereSettings *)value = vkr_atmosphere_settings_defaults();
}

/* Only a directional light is a sun (ADR-058); documents from before keep
   loading and drop the atmosphere's own sun. */
static const char *const s_atmosphere_retired[] = {"sun_direction",
                                                   "solar_irradiance", NULL};

const VkrTypeDesc vkr_scene_atmosphere_type = {
    .name = "atmosphere",
    .label = "Sky atmosphere",
    .category = "Sky",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .retired = s_atmosphere_retired,
    .properties = s_atmosphere_properties,
    .property_count = ArrayCount(s_atmosphere_properties),
    .size = sizeof(VkrAtmosphereSettings),
    .align = AlignOf(VkrAtmosphereSettings),
    .defaults = atmosphere_defaults,
    .validate = atmosphere_validate,
};

static const VkrPropertyDesc s_cloud_properties[] = {
    PROPERTY_BOOL(VkrCloudSettings, enabled, "enabled", "Enabled",
                  "Requires an enabled atmosphere"),
    {.name = "base_altitude_m",
     .label = "Base altitude",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrCloudSettings, base_altitude_m),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = VKR_CLOUD_ALTITUDE_MAX_M,
     .step = 5.0f},
    {.name = "top_altitude_m",
     .label = "Top altitude",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrCloudSettings, top_altitude_m),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = VKR_CLOUD_ALTITUDE_MAX_M,
     .step = 5.0f},
    {.name = "coverage",
     .label = "Coverage",
     .offset = TYPE_OFFSET(VkrCloudSettings, coverage),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "density",
     .label = "Density",
     .offset = TYPE_OFFSET(VkrCloudSettings, density),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = VKR_CLOUD_DENSITY_MAX,
     .step = 0.001f},
    {.name = "wind_mps",
     .label = "Wind",
     .tooltip = "Horizontal wind in metres per second",
     .unit = "m/s",
     .offset = TYPE_OFFSET(VkrCloudSettings, wind_mps),
     .kind = VKR_PROPERTY_VEC2,
     .min = -VKR_CLOUD_WIND_MAX_MPS,
     .max = VKR_CLOUD_WIND_MAX_MPS,
     .step = 0.1f},
};

static bool8_t clouds_validate(const void *value, char *error,
                               uint32_t capacity) {
  if (!vkr_cloud_settings_valid(value)) {
    snprintf(error, capacity,
             "Clouds need a top at least %.0f m above their base.",
             VKR_CLOUD_THICKNESS_MIN_M);
    return false_v;
  }
  return true_v;
}

static void clouds_defaults(void *value) {
  *(VkrCloudSettings *)value = vkr_cloud_settings_defaults();
}

const VkrTypeDesc vkr_scene_clouds_type = {
    .name = "clouds",
    .label = "Volumetric clouds",
    .category = "Sky",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_cloud_properties,
    .property_count = ArrayCount(s_cloud_properties),
    .size = sizeof(VkrCloudSettings),
    .align = AlignOf(VkrCloudSettings),
    .defaults = clouds_defaults,
    .validate = clouds_validate,
};

static const VkrPropertyDesc s_fog_properties[] = {
    PROPERTY_BOOL(VkrFogSettings, enabled, "enabled", "Enabled", NULL),
    {.name = "color",
     .label = "Color",
     .tooltip = "In-scattered color when sky lighting is zero",
     .offset = TYPE_OFFSET(VkrFogSettings, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.005f},
    {.name = "density",
     .label = "Density",
     .offset = TYPE_OFFSET(VkrFogSettings, density),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.0005f},
    {.name = "base_height",
     .label = "Base height",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFogSettings, base_height),
     .kind = VKR_PROPERTY_F32,
     .step = 0.05f},
    {.name = "height_falloff",
     .label = "Height falloff",
     .offset = TYPE_OFFSET(VkrFogSettings, height_falloff),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.001f},
    {.name = "max_distance",
     .label = "Max distance",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFogSettings, max_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 1.0f},
    {.name = "sky_distance",
     .label = "Sky distance",
     .tooltip = "Distance assigned to sky pixels; at most the max distance",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFogSettings, sky_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 1.0f},
    {.name = "sky_lighting",
     .label = "Sky lighting",
     .tooltip = "Albedo of a medium lit by the atmosphere's sun and sky",
     .offset = TYPE_OFFSET(VkrFogSettings, sky_lighting),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "anisotropy",
     .label = "Anisotropy",
     .offset = TYPE_OFFSET(VkrFogSettings, anisotropy),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -0.95f,
     .max = 0.95f,
     .step = 0.005f},
};

static bool8_t fog_validate(const void *value, char *error, uint32_t capacity) {
  if (!vkr_fog_settings_valid(value)) {
    snprintf(error, capacity,
             "Fog needs positive distances with the sky distance at most the "
             "max distance.");
    return false_v;
  }
  return true_v;
}

static void fog_defaults(void *value) {
  *(VkrFogSettings *)value = vkr_fog_settings_defaults();
}

const VkrTypeDesc vkr_scene_fog_type = {
    .name = "fog",
    .label = "Height fog",
    .category = "Fog",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_fog_properties,
    .property_count = ArrayCount(s_fog_properties),
    .size = sizeof(VkrFogSettings),
    .align = AlignOf(VkrFogSettings),
    .defaults = fog_defaults,
    .validate = fog_validate,
};

static const VkrPropertyDesc s_froxel_fog_properties[] = {
    PROPERTY_BOOL(VkrFroxelFogSettings, enabled, "enabled", "Enabled", NULL),
    {.name = "color",
     .label = "Albedo",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, color),
     .kind = VKR_PROPERTY_COLOR,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "density",
     .label = "Density",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, density),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.0005f},
    {.name = "base_height",
     .label = "Base height",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, base_height),
     .kind = VKR_PROPERTY_F32,
     .step = 0.05f},
    {.name = "height_falloff",
     .label = "Height falloff",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, height_falloff),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.001f},
    {.name = "max_distance",
     .label = "Max distance",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, max_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.5f},
    {.name = "sky_lighting",
     .label = "Sky lighting",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, sky_lighting),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "anisotropy",
     .label = "Anisotropy",
     .offset = TYPE_OFFSET(VkrFroxelFogSettings, anisotropy),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -0.95f,
     .max = 0.95f,
     .step = 0.005f},
};

static bool8_t froxel_fog_validate(const void *value, char *error,
                                   uint32_t capacity) {
  const VkrFroxelFogSettings *fog = value;
  if (fog->box_count != 0u || !vkr_froxel_fog_settings_valid(fog)) {
    snprintf(error, capacity,
             "Volumetric fog needs a positive max distance; density boxes are "
             "their own components.");
    return false_v;
  }
  return true_v;
}

static void froxel_fog_defaults(void *value) {
  *(VkrFroxelFogSettings *)value = vkr_froxel_fog_settings_defaults();
}

const VkrTypeDesc vkr_scene_froxel_fog_type = {
    .name = "volumetric_fog",
    .label = "Volumetric fog",
    .category = "Fog",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_froxel_fog_properties,
    .property_count = ArrayCount(s_froxel_fog_properties),
    .size = sizeof(VkrFroxelFogSettings),
    .align = AlignOf(VkrFroxelFogSettings),
    .defaults = froxel_fog_defaults,
    .validate = froxel_fog_validate,
};

static const VkrPropertyDesc s_fog_box_properties[] = {
    {.name = "minimum",
     .label = "Minimum",
     .tooltip = "World-space box corner",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFroxelDensityBox, minimum),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.05f},
    {.name = "maximum",
     .label = "Maximum",
     .tooltip = "World-space box corner",
     .unit = "m",
     .offset = TYPE_OFFSET(VkrFroxelDensityBox, maximum),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.05f},
    {.name = "density_multiplier",
     .label = "Density multiplier",
     .unit = "x",
     .offset = TYPE_OFFSET(VkrFroxelDensityBox, density_multiplier),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
};

static bool8_t fog_box_validate(const void *value, char *error,
                                uint32_t capacity) {
  const VkrFroxelDensityBox *box = value;
  if (!(box->minimum.x < box->maximum.x) ||
      !(box->minimum.y < box->maximum.y) ||
      !(box->minimum.z < box->maximum.z)) {
    snprintf(error, capacity, "Each minimum must be below its maximum.");
    return false_v;
  }
  return true_v;
}

static void fog_box_defaults(void *value) {
  VkrFroxelDensityBox *box = value;
  box->minimum = vec3_new(-1.0f, 0.0f, -1.0f);
  box->maximum = vec3_new(1.0f, 2.0f, 1.0f);
  box->density_multiplier = 1.0f;
}

const VkrTypeDesc vkr_scene_fog_box_type = {
    .name = "fog_density_box",
    .label = "Fog density box",
    .category = "Fog",
    .properties = s_fog_box_properties,
    .property_count = ArrayCount(s_fog_box_properties),
    .size = sizeof(VkrFroxelDensityBox),
    .align = AlignOf(VkrFroxelDensityBox),
    .defaults = fog_box_defaults,
    .validate = fog_box_validate,
};

static const char *const s_exposure_mode_names[] = {"manual", "automatic",
                                                    NULL};

static const VkrPropertyDesc s_post_process_properties[] = {
    {.name = "exposure_mode",
     .label = "Exposure",
     .group = "Exposure",
     .names = s_exposure_mode_names,
     .offset = TYPE_OFFSET(ScenePostProcess, exposure_mode),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "manual_exposure",
     .label = "Manual exposure",
     .tooltip = "Linear multiplier in manual mode",
     .offset = TYPE_OFFSET(ScenePostProcess, manual_exposure),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0001f,
     .max = 1000.0f,
     .step = 0.005f},
    {.name = "exposure_compensation_ev",
     .label = "Compensation",
     .tooltip = "Exposure bias in automatic mode",
     .unit = "EV",
     .offset = TYPE_OFFSET(ScenePostProcess, exposure_compensation_ev),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -6.0f,
     .max = 6.0f,
     .step = 0.01f},
    {.name = "white_balance_temperature",
     .label = "Temperature",
     .group = "Color",
     .tooltip = "Shift color toward cool or warm tones",
     .offset = TYPE_OFFSET(ScenePostProcess, white_balance_temperature),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -1.0f,
     .max = 1.0f},
    {.name = "white_balance_tint",
     .label = "Tint",
     .tooltip = "Shift color toward green or magenta",
     .offset = TYPE_OFFSET(ScenePostProcess, white_balance_tint),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -1.0f,
     .max = 1.0f},
    {.name = "contrast",
     .label = "Contrast",
     .unit = "x",
     .offset = TYPE_OFFSET(ScenePostProcess, contrast),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.5f,
     .max = 1.5f},
    {.name = "saturation",
     .label = "Saturation",
     .unit = "x",
     .offset = TYPE_OFFSET(ScenePostProcess, saturation),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.5f},
    {.name = "sharpness",
     .label = "Sharpness",
     .unit = "%",
     .offset = TYPE_OFFSET(ScenePostProcess, sharpness),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .display_scale = 100.0f},
    {.name = "bloom_threshold",
     .label = "Threshold",
     .group = "Bloom",
     .tooltip = "Scene-linear luminance where bloom starts",
     .offset = TYPE_OFFSET(ScenePostProcess, bloom_threshold),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "bloom_knee",
     .label = "Knee",
     .offset = TYPE_OFFSET(ScenePostProcess, bloom_knee),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f},
    {.name = "bloom_intensity",
     .label = "Intensity",
     .unit = "%",
     .offset = TYPE_OFFSET(ScenePostProcess, bloom_intensity),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .display_scale = 100.0f},
    {.name = "dof_focus_distance",
     .label = "Focus distance",
     .group = "Depth of field",
     .unit = "m",
     .offset = TYPE_OFFSET(ScenePostProcess, dof_focus_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.01f,
     .max = 100000.0f,
     .step = 0.05f},
    {.name = "dof_f_stop",
     .label = "Aperture",
     .unit = "f",
     .offset = TYPE_OFFSET(ScenePostProcess, dof_f_stop),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.5f,
     .max = 32.0f,
     .step = 0.05f},
    {.name = "motion_blur_shutter_angle",
     .label = "Shutter angle",
     .group = "Motion blur",
     .unit = "deg",
     .offset = TYPE_OFFSET(ScenePostProcess, motion_blur_shutter_angle),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 360.0f,
     .step = 1.0f},
    {.name = "gtao_radius",
     .label = "Radius",
     .group = "Ambient occlusion",
     .unit = "m",
     .offset = TYPE_OFFSET(ScenePostProcess, gtao_radius),
     .kind = VKR_PROPERTY_F32,
     .min = VKR_GTAO_RADIUS_MIN,
     .max = VKR_GTAO_RADIUS_MAX,
     .step = 0.01f},
    {.name = "gtao_power",
     .label = "Power",
     .offset = TYPE_OFFSET(ScenePostProcess, gtao_power),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.1f,
     .max = 8.0f,
     .step = 0.01f},
};

_Static_assert(VKR_EXPOSURE_MODE_MANUAL == 0u &&
                   VKR_EXPOSURE_MODE_AUTOMATIC == 1u,
               "Exposure names index VkrExposureMode");

static void post_process_defaults(void *value) {
  *(ScenePostProcess *)value = vkr_scene_post_process_defaults();
}

const VkrTypeDesc vkr_scene_post_process_type = {
    .name = "post_process",
    .label = "Post process",
    .category = "Camera",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_post_process_properties,
    .property_count = ArrayCount(s_post_process_properties),
    .size = sizeof(ScenePostProcess),
    .align = AlignOf(ScenePostProcess),
    .defaults = post_process_defaults,
};

ScenePostProcess vkr_scene_post_process_defaults(void) {
  return (ScenePostProcess){
      .exposure_mode = VKR_EXPOSURE_MODE_AUTOMATIC,
      .manual_exposure = VKR_DEFAULT_EXPOSURE,
      .contrast = 1.0f,
      .saturation = 1.0f,
      .sharpness = 0.25f,
      .bloom_threshold = VKR_BLOOM_DEFAULT_THRESHOLD,
      .bloom_knee = VKR_BLOOM_DEFAULT_KNEE,
      .bloom_intensity = VKR_BLOOM_DEFAULT_INTENSITY,
      .dof_focus_distance = VKR_DOF_DEFAULT_FOCUS_DISTANCE,
      .dof_f_stop = VKR_DOF_DEFAULT_F_STOP,
      .motion_blur_shutter_angle = VKR_MOTION_BLUR_DEFAULT_SHUTTER_ANGLE,
      .gtao_radius = VKR_GTAO_DEFAULT_RADIUS,
      .gtao_power = VKR_GTAO_DEFAULT_POWER,
  };
}

static const VkrPropertyDesc s_probe_properties[] = {
    PROPERTY_BOOL(SceneReflectionProbeSettings, enabled, "enabled", "Enabled",
                  NULL),
    {.name = "center",
     .label = "Center",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, center),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.05f},
    {.name = "extents",
     .label = "Extents",
     .tooltip = "Half size of the world-space influence box",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, extents),
     .kind = VKR_PROPERTY_VEC3,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "blend_distance",
     .label = "Blend distance",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, blend_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "intensity",
     .label = "Intensity",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "diffuse_intensity",
     .label = "Diffuse intensity",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, diffuse_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "specular_intensity",
     .label = "Specular intensity",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, specular_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
    {.name = "sh_deringing",
     .label = "SH deringing",
     .tooltip = "L2 deringing exponent; applies on the next bake",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, sh_deringing),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY,
     .min = 0.0f,
     .max = FLT_MAX},
    {.name = "cubemap",
     .label = "Cubemap",
     .tooltip = "Captured source; empty uses the scene's sky. Applies on the "
                "next load",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, cubemap),
     .capacity = sizeof(((SceneReflectionProbeSettings *)0)->cubemap),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "slot",
     .label = "Runtime slot",
     .offset = TYPE_OFFSET(SceneReflectionProbeSettings, slot),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_TRANSIENT | VKR_PROPERTY_FLAG_HIDDEN},
};

static bool8_t probe_validate(const void *value, char *error,
                              uint32_t capacity) {
  const SceneReflectionProbeSettings *probe = value;
  if (!(probe->extents.x > 0.0f) || !(probe->extents.y > 0.0f) ||
      !(probe->extents.z > 0.0f)) {
    snprintf(error, capacity, "Probe extents must be positive.");
    return false_v;
  }
  return true_v;
}

static void probe_defaults(void *value) {
  SceneReflectionProbeSettings *probe = value;
  probe->enabled = true_v;
  probe->extents = vec3_new(5.0f, 5.0f, 5.0f);
  probe->blend_distance = 1.0f;
  probe->intensity = 1.0f;
  probe->diffuse_intensity = 1.0f;
  probe->specular_intensity = 1.0f;
  probe->slot = UINT32_MAX;
}

const VkrTypeDesc vkr_scene_reflection_probe_type = {
    .name = "reflection_probe",
    .label = "Reflection probe",
    .category = "Lighting",
    .properties = s_probe_properties,
    .property_count = ArrayCount(s_probe_properties),
    .size = sizeof(SceneReflectionProbeSettings),
    .align = AlignOf(SceneReflectionProbeSettings),
    .defaults = probe_defaults,
    .validate = probe_validate,
};

static const VkrPropertyDesc s_diffuse_volume_properties[] = {
    PROPERTY_BOOL(SceneDiffuseVolumeSettings, enabled, "enabled", "Enabled",
                  NULL),
    {.name = "path",
     .label = "Volume",
     .tooltip = "Baked irradiance volume; applies on the next load",
     .offset = TYPE_OFFSET(SceneDiffuseVolumeSettings, path),
     .capacity = sizeof(((SceneDiffuseVolumeSettings *)0)->path),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
};

static void diffuse_volume_defaults(void *value) {
  ((SceneDiffuseVolumeSettings *)value)->enabled = true_v;
}

const VkrTypeDesc vkr_scene_diffuse_volume_type = {
    .name = "diffuse_volume",
    .label = "Diffuse volume",
    .category = "Lighting",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_diffuse_volume_properties,
    .property_count = ArrayCount(s_diffuse_volume_properties),
    .size = sizeof(SceneDiffuseVolumeSettings),
    .align = AlignOf(SceneDiffuseVolumeSettings),
    .defaults = diffuse_volume_defaults,
};

static const VkrPropertyDesc s_subsurface_properties[] = {
    PROPERTY_BOOL(SceneSubsurfaceSettings, enabled, "enabled", "Enabled", NULL),
    {.name = "profile_count",
     .label = "Profiles",
     .tooltip = "Profiles loaded with the scene",
     .offset = TYPE_OFFSET(SceneSubsurfaceSettings, profile_count),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_TRANSIENT},
};

static void subsurface_defaults(void *value) {
  ((SceneSubsurfaceSettings *)value)->enabled = true_v;
}

const VkrTypeDesc vkr_scene_subsurface_type = {
    .name = "subsurface",
    .label = "Subsurface scattering",
    .category = "Lighting",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_subsurface_properties,
    .property_count = ArrayCount(s_subsurface_properties),
    .size = sizeof(SceneSubsurfaceSettings),
    .align = AlignOf(SceneSubsurfaceSettings),
    .defaults = subsurface_defaults,
};

/* ---- Shape and text ---- */

_Static_assert(sizeof(SceneShapeType) == sizeof(uint32_t),
               "ENUM properties store four bytes");

static const char *const s_shape_type_names[] = {"cube", NULL};

static const VkrPropertyDesc s_shape_properties[] = {
    {.name = "type",
     .label = "Shape",
     .names = s_shape_type_names,
     .offset = TYPE_OFFSET(SceneShapeSettings, type),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "dimensions",
     .label = "Size",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneShapeSettings, dimensions),
     .kind = VKR_PROPERTY_VEC3,
     .min = 0.001f,
     .max = 10000.0f,
     .step = 0.01f},
    {.name = "color",
     .label = "Color",
     .tooltip = "Linear RGBA; used when no material is named",
     .offset = TYPE_OFFSET(SceneShapeSettings, color),
     .kind = VKR_PROPERTY_VEC4,
     .flags = VKR_PROPERTY_FLAG_COLOR,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "material_name",
     .label = "Material",
     .offset = TYPE_OFFSET(SceneShapeSettings, material_name),
     .capacity = sizeof(((SceneShapeSettings *)0)->material_name),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "material_path",
     .label = "Material file",
     .offset = TYPE_OFFSET(SceneShapeSettings, material_path),
     .capacity = sizeof(((SceneShapeSettings *)0)->material_path),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
};

static void shape_defaults(void *value) {
  *(SceneShapeSettings *)value =
      (SceneShapeSettings){.type = SCENE_SHAPE_TYPE_CUBE,
                           .dimensions = vec3_new(1.0f, 1.0f, 1.0f),
                           .color = {0.8f, 0.8f, 0.8f, 1.0f}};
}

const VkrTypeDesc vkr_scene_shape_type = {
    .name = "shape",
    .label = "Shape",
    .category = "Geometry",
    .properties = s_shape_properties,
    .property_count = ArrayCount(s_shape_properties),
    .size = sizeof(SceneShapeSettings),
    .align = _Alignof(SceneShapeSettings),
    .defaults = shape_defaults,
};

static const char *const s_text_align_names[] = {"left", "center", "right",
                                                 NULL};

/* Engine text fonts, named by their configuration `<name>.fontcfg` in the
   font directory, which also registers them in the font system. The first
   is the font system's default MTSDF font. A package carries another one
   only when a scene's text uses it. */
static const char *const s_text_font_names[] = {
    "UbuntuMono-cooked", "UbuntuMono-Bold-cooked", "Inter-Regular-cooked",
    "Inter-SemiBold-cooked", NULL};
static const char *const s_text_font_labels[] = {
    "Ubuntu Mono", "Ubuntu Mono Bold", "Inter", "Inter SemiBold", NULL};

_Static_assert(ArrayCount(s_text_font_names) - 1u == VKR_SCENE_TEXT_FONT_COUNT,
               "every engine text font has a scene reference slot");

const char *vkr_scene_text_font_name(uint32_t font) {
  return font < ArrayCount(s_text_font_names) - 1u ? s_text_font_names[font]
                                                   : NULL;
}

static const VkrPropertyDesc s_text_properties[] = {
    {.name = "content",
     .label = "Text",
     .tooltip = "Shown as you type; Enter starts a new line",
     .offset = TYPE_OFFSET(SceneTextSettings, content),
     .capacity = sizeof(((SceneTextSettings *)0)->content),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_MULTILINE},
    {.name = "font",
     .label = "Font",
     .group = "Style",
     .names = s_text_font_names,
     .labels = s_text_font_labels,
     .offset = TYPE_OFFSET(SceneTextSettings, font),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "size",
     .label = "Size",
     .tooltip = "Em height at unit scale",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneTextSettings, size),
     .kind = VKR_PROPERTY_F32,
     .min = 0.01f,
     .max = 100.0f,
     .step = 0.005f},
    {.name = "color",
     .label = "Color",
     .tooltip = "sRGB color and linear alpha",
     .offset = TYPE_OFFSET(SceneTextSettings, color),
     .kind = VKR_PROPERTY_VEC4,
     .flags = VKR_PROPERTY_FLAG_COLOR | VKR_PROPERTY_FLAG_SRGB,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
    {.name = "align",
     .label = "Alignment",
     .tooltip = "Alignment of each line within the text block",
     .names = s_text_align_names,
     .offset = TYPE_OFFSET(SceneTextSettings, align),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "line_spacing",
     .label = "Line spacing",
     .tooltip = "Multiple of the font's line height",
     .offset = TYPE_OFFSET(SceneTextSettings, line_spacing),
     .kind = VKR_PROPERTY_F32,
     .min = 0.5f,
     .max = 4.0f,
     .step = 0.01f},
    {.name = "letter_spacing",
     .label = "Letter spacing",
     .tooltip = "Extra space after each character",
     .unit = "em",
     .offset = TYPE_OFFSET(SceneTextSettings, letter_spacing),
     .kind = VKR_PROPERTY_F32,
     .min = -0.5f,
     .max = 2.0f,
     .step = 0.002f},
};

/* Font pixels per em in the slot's texture box, before size in meters. */
static const char *const s_text_retired[] = {"font_size", NULL};

static void text_defaults(void *value) {
  SceneTextSettings *text = value;
  MemZero(text, sizeof(*text));
  snprintf(text->content, sizeof(text->content), "Text");
  text->size = 0.5f;
  text->color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
  text->align = VKR_TEXT_ALIGN_CENTER;
  text->line_spacing = 1.0f;
}

const VkrTypeDesc vkr_scene_text_type = {
    .name = "text",
    .label = "Text",
    .category = "Geometry",
    .properties = s_text_properties,
    .property_count = ArrayCount(s_text_properties),
    .size = sizeof(SceneTextSettings),
    .align = _Alignof(SceneTextSettings),
    .retired = s_text_retired,
    .defaults = text_defaults,
};

static const VkrPropertyDesc s_animation_properties[] = {
    {.name = "clip",
     .label = "Clip",
     .tooltip = "Clip index in the bound animation bank",
     .offset = TYPE_OFFSET(SceneAnimationSettings, clip),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 65535.0f,
     .step = 1.0f},
    {.name = "clip_name",
     .label = "Clip name",
     .offset = TYPE_OFFSET(SceneAnimationSettings, clip_name),
     .capacity = sizeof(((SceneAnimationSettings *)0)->clip_name),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "rate",
     .label = "Rate",
     .tooltip = "Playback speed; negative plays in reverse",
     .unit = "x",
     .offset = TYPE_OFFSET(SceneAnimationSettings, rate),
     .kind = VKR_PROPERTY_F32,
     .min = -16.0f,
     .max = 16.0f,
     .step = 0.01f},
    {.name = "loop",
     .label = "Loop",
     .offset = TYPE_OFFSET(SceneAnimationSettings, loop),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "playing",
     .label = "Playing",
     .tooltip = "Advances the clip over time",
     .offset = TYPE_OFFSET(SceneAnimationSettings, playing),
     .kind = VKR_PROPERTY_BOOL},
};

static void animation_defaults(void *value) {
  *(SceneAnimationSettings *)value =
      (SceneAnimationSettings){.rate = 1.0f, .loop = true_v, .playing = true_v};
}

/* Not live: only an animation binding adds it, and removing it would leave
   the player without authored values. */
const VkrTypeDesc vkr_scene_animation_type = {
    .name = "animation",
    .label = "Animation",
    .category = "Animation",
    .properties = s_animation_properties,
    .property_count = ArrayCount(s_animation_properties),
    .size = sizeof(SceneAnimationSettings),
    .align = _Alignof(SceneAnimationSettings),
    .defaults = animation_defaults,
};

static const char *const s_mesh_state_names[] = {"Not loaded", "Loading",
                                                 "Loaded", "Failed", NULL};
static const char *const s_mesh_mobility_names[] = {"Dynamic", "Static", NULL};

static const VkrPropertyDesc s_mesh_info_properties[] = {
    {.name = "asset",
     .label = "Asset",
     .offset = TYPE_OFFSET(SceneMeshInfo, asset),
     .capacity = sizeof(((SceneMeshInfo *)0)->asset),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "material",
     .label = "Material",
     .tooltip = "First submesh material; the count names the other distinct "
                "materials",
     .offset = TYPE_OFFSET(SceneMeshInfo, material),
     .capacity = sizeof(((SceneMeshInfo *)0)->material),
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "submeshes",
     .label = "Submeshes",
     .offset = TYPE_OFFSET(SceneMeshInfo, submeshes),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "vertices",
     .label = "Vertices",
     .offset = TYPE_OFFSET(SceneMeshInfo, vertices),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "triangles",
     .label = "Triangles",
     .offset = TYPE_OFFSET(SceneMeshInfo, triangles),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "radius",
     .label = "Bounds radius",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneMeshInfo, radius),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "state",
     .label = "State",
     .names = s_mesh_state_names,
     .offset = TYPE_OFFSET(SceneMeshInfo, state),
     .kind = VKR_PROPERTY_ENUM,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "mobility",
     .label = "Shadow mobility",
     .tooltip = "Static casters keep cached shadows until an edit "
                "invalidates them",
     .names = s_mesh_mobility_names,
     .offset = TYPE_OFFSET(SceneMeshInfo, mobility),
     .kind = VKR_PROPERTY_ENUM,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
};

const VkrTypeDesc vkr_scene_mesh_info_type = {
    .name = "mesh",
    .label = "Mesh",
    .category = "Geometry",
    .properties = s_mesh_info_properties,
    .property_count = ArrayCount(s_mesh_info_properties),
    .size = sizeof(SceneMeshInfo),
    .align = _Alignof(SceneMeshInfo),
};

/* ---- Physics settings ---- */

static const VkrPropertyDesc s_physics_settings_properties[] = {
    {.name = "gravity",
     .label = "Gravity",
     .tooltip = "Acceleration of every body and character in the world",
     .unit = "m/s\xc2\xb2",
     .offset = TYPE_OFFSET(ScenePhysicsSettings, gravity),
     .kind = VKR_PROPERTY_VEC3,
     .min = -1000.0f,
     .max = 1000.0f,
     .step = 0.05f},
};

static void physics_settings_defaults(void *value) {
  *(ScenePhysicsSettings *)value =
      (ScenePhysicsSettings){.gravity = vec3_new(0.0f, -9.81f, 0.0f)};
}

const VkrTypeDesc vkr_scene_physics_settings_type = {
    .name = "physics_settings",
    .label = "Physics settings",
    .category = "Physics",
    .flags = VKR_TYPE_FLAG_SINGLETON | VKR_TYPE_FLAG_WORLD_ONLY,
    .properties = s_physics_settings_properties,
    .property_count = ArrayCount(s_physics_settings_properties),
    .size = sizeof(ScenePhysicsSettings),
    .align = _Alignof(ScenePhysicsSettings),
    .defaults = physics_settings_defaults,
};

static const VkrPropertyDesc s_animation_settings_properties[] = {
    {.name = "time_scale",
     .label = "Time scale",
     .tooltip = "Multiplies the clock of every animation in the scene",
     .unit = "x",
     .offset = TYPE_OFFSET(SceneAnimationWorldSettings, time_scale),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 4.0f,
     .step = 0.01f},
};

static void animation_settings_defaults(void *value) {
  *(SceneAnimationWorldSettings *)value =
      (SceneAnimationWorldSettings){.time_scale = 1.0f};
}

const VkrTypeDesc vkr_scene_animation_settings_type = {
    .name = "animation_settings",
    .label = "Animation settings",
    .category = "Animation",
    .flags = VKR_TYPE_FLAG_SINGLETON | VKR_TYPE_FLAG_WORLD_ONLY,
    .properties = s_animation_settings_properties,
    .property_count = ArrayCount(s_animation_settings_properties),
    .size = sizeof(SceneAnimationWorldSettings),
    .align = _Alignof(SceneAnimationWorldSettings),
    .defaults = animation_settings_defaults,
};

/* ---- Time of day (ADR-090) ---- */

static const VkrPropertyDesc s_time_of_day_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .tooltip = "Turns the sun and moon to the hour",
     .offset = TYPE_OFFSET(SceneTimeOfDay, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "hour",
     .label = "Hour",
     .tooltip = "Hour of the day; at 12 the sun and moon keep their authored "
                "directions",
     .unit = "h",
     .offset = TYPE_OFFSET(SceneTimeOfDay, hour),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 24.0f,
     .step = 0.05f},
    {.name = "day_minutes",
     .label = "Day length",
     .tooltip = "Real minutes per day while the game runs",
     .unit = "min",
     .zero_label = "Paused",
     .offset = TYPE_OFFSET(SceneTimeOfDay, day_minutes),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100000.0f,
     .step = 1.0f},
    {.name = "night_groups",
     .label = "Night groups",
     .tooltip = "Light groups, separated by commas, that light only while the "
                "sun is down",
     .offset = TYPE_OFFSET(SceneTimeOfDay, night_groups),
     .capacity = sizeof(((SceneTimeOfDay *)0)->night_groups),
     .kind = VKR_PROPERTY_STRING},
};

static void time_of_day_defaults(void *value) {
  *(SceneTimeOfDay *)value = (SceneTimeOfDay){
      .enabled = true_v,
      .hour = 12.0f,
  };
}

static bool8_t time_of_day_validate(const void *value, char *error,
                                    uint32_t capacity) {
  const SceneTimeOfDay *time = (const SceneTimeOfDay *)value;
  String8 name = {0};
  uint32_t cursor = 0u;
  while (vkr_scene_night_group_next(time->night_groups, &cursor, &name)) {
    if (!vkr_light_group_name_valid((const char *)name.str, name.length)) {
      snprintf(error, capacity,
               "Night groups are light group names separated by commas.");
      return false_v;
    }
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_time_of_day_type = {
    .name = "time_of_day",
    .label = "Time of day",
    .category = "Environment",
    .flags = VKR_TYPE_FLAG_SINGLETON | VKR_TYPE_FLAG_WORLD_ONLY,
    .properties = s_time_of_day_properties,
    .property_count = ArrayCount(s_time_of_day_properties),
    .size = sizeof(SceneTimeOfDay),
    .align = _Alignof(SceneTimeOfDay),
    .defaults = time_of_day_defaults,
    .validate = time_of_day_validate,
};

static const VkrPropertyDesc s_player_start_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .tooltip = "A disabled start is skipped; the first enabled one spawns "
                "the player",
     .offset = TYPE_OFFSET(ScenePlayerStart, enabled),
     .kind = VKR_PROPERTY_BOOL},
};

static void player_start_defaults(void *value) {
  ScenePlayerStart *start = value;
  start->enabled = true_v;
}

const VkrTypeDesc vkr_scene_player_start_type = {
    .name = "player_start",
    .label = "Player start",
    .category = "Gameplay",
    .properties = s_player_start_properties,
    .property_count = ArrayCount(s_player_start_properties),
    .size = sizeof(ScenePlayerStart),
    .align = AlignOf(ScenePlayerStart),
    .defaults = player_start_defaults,
};

/* ---- Decals (ADR-092) ---- */

static const VkrPropertyDesc s_decal_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneDecal, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "material",
     .label = "Material file",
     .tooltip = "Its base colour texture times its tint, weighted by alpha, "
                "covers the surfaces in the box; empty uses the dev decal "
                "material",
     .offset = TYPE_OFFSET(SceneDecal, material),
     .capacity = sizeof(((SceneDecal *)0)->material),
     .kind = VKR_PROPERTY_STRING},
    {.name = "opacity",
     .label = "Opacity",
     .offset = TYPE_OFFSET(SceneDecal, opacity),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "fade_angle_start",
     .label = "Fade start angle",
     .tooltip = "Angle between a surface's normal and the box's +Y where "
                "the decal starts to fade",
     .unit = "deg",
     .offset = TYPE_OFFSET(SceneDecal, fade_angle_start),
     .kind = VKR_PROPERTY_ANGLE,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 180.0f,
     .step = 0.5f},
    {.name = "fade_angle_end",
     .label = "Fade end angle",
     .tooltip = "Angle where the decal is gone; above the start angle",
     .unit = "deg",
     .offset = TYPE_OFFSET(SceneDecal, fade_angle_end),
     .kind = VKR_PROPERTY_ANGLE,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 180.0f,
     .step = 0.5f},
    {.name = "depth_fade",
     .label = "Depth fade",
     .tooltip = "Share of the box's half depth over which the decal fades "
                "toward its near and far faces",
     .offset = TYPE_OFFSET(SceneDecal, depth_fade),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "sort_order",
     .label = "Sort order",
     .tooltip = "Overlapping decals draw in ascending order, the highest on "
                "top",
     .offset = TYPE_OFFSET(SceneDecal, sort_order),
     .kind = VKR_PROPERTY_I32,
     .min = -1000.0f,
     .max = 1000.0f,
     .step = 1.0f},
};

static void decal_defaults(void *value) {
  SceneDecal *decal = value;
  MemZero(decal, sizeof(*decal));
  decal->enabled = true_v;
  decal->opacity = 1.0f;
  decal->fade_angle_start = vkr_to_radians(60.0f);
  decal->fade_angle_end = vkr_to_radians(80.0f);
  decal->depth_fade = 0.25f;
}

static bool8_t decal_validate(const void *value, char *error,
                              uint32_t capacity) {
  const SceneDecal *decal = value;
  if (!(decal->opacity >= 0.0f && decal->opacity <= 1.0f) ||
      !(decal->depth_fade >= 0.0f && decal->depth_fade <= 1.0f)) {
    snprintf(error, capacity, "Opacity and depth fade lie in [0, 1].");
    return false_v;
  }
  if (!(decal->fade_angle_start >= 0.0f &&
        decal->fade_angle_start < decal->fade_angle_end &&
        decal->fade_angle_end <= VKR_PI)) {
    snprintf(error, capacity,
             "The fade angles lie in [0, 180] degrees, start below end.");
    return false_v;
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_decal_type = {
    .name = "decal",
    .label = "Decal",
    .category = "Geometry",
    .properties = s_decal_properties,
    .property_count = ArrayCount(s_decal_properties),
    .size = sizeof(SceneDecal),
    .align = AlignOf(SceneDecal),
    .defaults = decal_defaults,
    .validate = decal_validate,
};

/* ---- Look volumes ---- */

static const VkrPropertyDesc s_look_volume_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneLookVolume, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "priority",
     .label = "Priority",
     .tooltip = "Overlapping volumes blend in ascending priority, the "
                "highest last",
     .offset = TYPE_OFFSET(SceneLookVolume, priority),
     .kind = VKR_PROPERTY_I32,
     .min = -1000.0f,
     .max = 1000.0f,
     .step = 1.0f},
    {.name = "blend_distance",
     .label = "Blend distance",
     .tooltip = "Distance outside the box over which the volume fades out",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneLookVolume, blend_distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.05f},
    {.name = "override_exposure",
     .label = "Override exposure",
     .tooltip = "Replace the post process exposure compensation",
     .offset = TYPE_OFFSET(SceneLookVolume, override_exposure),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "exposure_compensation_ev",
     .label = "Compensation",
     .unit = "EV",
     .offset = TYPE_OFFSET(SceneLookVolume, exposure_compensation_ev),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -6.0f,
     .max = 6.0f,
     .step = 0.01f},
    {.name = "override_metering",
     .label = "Override metering range",
     .tooltip = "Limit the EV automatic exposure settles at",
     .offset = TYPE_OFFSET(SceneLookVolume, override_metering),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "metering_min_ev",
     .label = "Metering minimum",
     .unit = "EV",
     .offset = TYPE_OFFSET(SceneLookVolume, metering_min_ev),
     .kind = VKR_PROPERTY_F32,
     .min = -16.0f,
     .max = 32.0f,
     .step = 0.1f},
    {.name = "metering_max_ev",
     .label = "Metering maximum",
     .unit = "EV",
     .offset = TYPE_OFFSET(SceneLookVolume, metering_max_ev),
     .kind = VKR_PROPERTY_F32,
     .min = -16.0f,
     .max = 32.0f,
     .step = 0.1f},
    {.name = "override_white_balance",
     .label = "Override white balance",
     .tooltip = "Replace the post process white balance",
     .offset = TYPE_OFFSET(SceneLookVolume, override_white_balance),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "white_balance_temperature",
     .label = "Temperature",
     .offset = TYPE_OFFSET(SceneLookVolume, white_balance_temperature),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -1.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "white_balance_tint",
     .label = "Tint",
     .offset = TYPE_OFFSET(SceneLookVolume, white_balance_tint),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = -1.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "override_contrast",
     .label = "Override contrast",
     .tooltip = "Replace the post process contrast",
     .offset = TYPE_OFFSET(SceneLookVolume, override_contrast),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "contrast",
     .label = "Contrast",
     .unit = "x",
     .offset = TYPE_OFFSET(SceneLookVolume, contrast),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.5f,
     .max = 1.5f,
     .step = 0.01f},
    {.name = "override_saturation",
     .label = "Override saturation",
     .tooltip = "Replace the post process saturation",
     .offset = TYPE_OFFSET(SceneLookVolume, override_saturation),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "saturation",
     .label = "Saturation",
     .unit = "x",
     .offset = TYPE_OFFSET(SceneLookVolume, saturation),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.5f,
     .step = 0.01f},
    {.name = "override_bloom",
     .label = "Override bloom",
     .tooltip = "Replace the post process bloom intensity",
     .offset = TYPE_OFFSET(SceneLookVolume, override_bloom),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "bloom_intensity",
     .label = "Bloom intensity",
     .offset = TYPE_OFFSET(SceneLookVolume, bloom_intensity),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "override_fog_color",
     .label = "Override fog colour",
     .tooltip = "Replace the height fog colour",
     .offset = TYPE_OFFSET(SceneLookVolume, override_fog_color),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "fog_color",
     .label = "Fog colour",
     .offset = TYPE_OFFSET(SceneLookVolume, fog_color),
     .kind = VKR_PROPERTY_COLOR},
    {.name = "override_fog_density",
     .label = "Override fog density",
     .tooltip = "Replace the height fog density",
     .offset = TYPE_OFFSET(SceneLookVolume, override_fog_density),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "fog_density",
     .label = "Fog density",
     .offset = TYPE_OFFSET(SceneLookVolume, fog_density),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.0005f},
    {.name = "override_sky_light",
     .label = "Override sky light",
     .tooltip = "Replace the sky light intensity",
     .offset = TYPE_OFFSET(SceneLookVolume, override_sky_light),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "sky_light_intensity",
     .label = "Sky light intensity",
     .offset = TYPE_OFFSET(SceneLookVolume, sky_light_intensity),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = FLT_MAX,
     .step = 0.01f},
};

static void look_volume_defaults(void *value) {
  SceneLookVolume *volume = value;
  MemZero(volume, sizeof(*volume));
  const ScenePostProcess post = vkr_scene_post_process_defaults();
  const VkrFogSettings fog = vkr_fog_settings_defaults();
  const VkrExposureMeteringConfig metering =
      vkr_exposure_metering_config_default();
  volume->enabled = true_v;
  volume->blend_distance = 1.0f;
  volume->exposure_compensation_ev = post.exposure_compensation_ev;
  volume->metering_min_ev = metering.min_ev;
  volume->metering_max_ev = metering.max_ev;
  volume->white_balance_temperature = post.white_balance_temperature;
  volume->white_balance_tint = post.white_balance_tint;
  volume->contrast = post.contrast;
  volume->saturation = post.saturation;
  volume->bloom_intensity = post.bloom_intensity;
  volume->fog_color = fog.color;
  volume->fog_density = fog.density;
  volume->sky_light_intensity = 1.0f;
}

static bool8_t look_volume_validate(const void *value, char *error,
                                    uint32_t capacity) {
  const SceneLookVolume *volume = value;
  if (!(volume->blend_distance >= 0.0f) || !(volume->fog_density >= 0.0f) ||
      !(volume->sky_light_intensity >= 0.0f)) {
    snprintf(error, capacity,
             "Blend distance, fog density and sky light are not negative.");
    return false_v;
  }
  if (!(volume->metering_min_ev < volume->metering_max_ev)) {
    snprintf(error, capacity, "The metering minimum lies below its maximum.");
    return false_v;
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_look_volume_type = {
    .name = "look_volume",
    .label = "Look volume",
    .category = "Camera",
    .properties = s_look_volume_properties,
    .property_count = ArrayCount(s_look_volume_properties),
    .size = sizeof(SceneLookVolume),
    .align = AlignOf(SceneLookVolume),
    .defaults = look_volume_defaults,
    .validate = look_volume_validate,
};

/* ---- Material overrides ---- */

/* One path per submesh, `material_0` naming submesh 0's material. */
#define MATERIAL_OVERRIDE_PROPERTY(index)                                      \
  {.name = "material_" #index,                                                 \
   .label = "Submesh " #index,                                                 \
   .tooltip =                                                                  \
       "Material file for submesh " #index "; empty keeps the mesh's own",     \
   .offset = TYPE_OFFSET(SceneMaterialOverride, materials[index]),             \
   .capacity = SCENE_MATERIAL_OVERRIDE_PATH_CAPACITY,                          \
   .kind = VKR_PROPERTY_STRING}

static const VkrPropertyDesc s_material_override_properties[] = {
    MATERIAL_OVERRIDE_PROPERTY(0), MATERIAL_OVERRIDE_PROPERTY(1),
    MATERIAL_OVERRIDE_PROPERTY(2), MATERIAL_OVERRIDE_PROPERTY(3),
    MATERIAL_OVERRIDE_PROPERTY(4), MATERIAL_OVERRIDE_PROPERTY(5),
    MATERIAL_OVERRIDE_PROPERTY(6), MATERIAL_OVERRIDE_PROPERTY(7),
};

#undef MATERIAL_OVERRIDE_PROPERTY

_Static_assert(ArrayCount(s_material_override_properties) ==
                   VKR_MESH_MATERIAL_OVERRIDE_MAX,
               "One material_override property per override entry");
_Static_assert(sizeof(SceneMaterialOverride) <= VKR_TYPE_VALUE_MAX,
               "A material_override value fits the typed value bound");

const VkrTypeDesc vkr_scene_material_override_type = {
    .name = "material_override",
    .label = "Material overrides",
    .category = "Rendering",
    .properties = s_material_override_properties,
    .property_count = ArrayCount(s_material_override_properties),
    .size = sizeof(SceneMaterialOverride),
    .align = AlignOf(SceneMaterialOverride),
};

/* ---- Surface themes ---- */

static const VkrPropertyDesc s_surface_theme_properties[] = {
    {.name = "theme",
     .label = "Theme",
     .tooltip = "A .surfaces file binding surface tags to materials for this "
                "container's brush faces; tags it leaves unbound take the "
                "World's theme, then their greybox look",
     .offset = TYPE_OFFSET(SceneSurfaceTheme, theme),
     .capacity = VKR_SURFACE_THEME_PATH_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
};

const VkrTypeDesc vkr_scene_surface_theme_type = {
    .name = "surface_theme",
    .label = "Surface theme",
    .category = "Level",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_surface_theme_properties,
    .property_count = ArrayCount(s_surface_theme_properties),
    .size = sizeof(SceneSurfaceTheme),
    .align = AlignOf(SceneSurfaceTheme),
};

/* ---- Brushes ---- */

_Static_assert(sizeof(SceneBrushRole) == sizeof(uint32_t),
               "ENUM properties store four bytes");

static const char *const s_brush_role_names[] = {"solid", "visual", "clip",
                                                 "trigger", NULL};
static const char *const s_brush_role_labels[] = {
    "Solid", "Visual only", "Clip (collision only)", "Trigger", NULL};

static const VkrPropertyDesc s_brush_properties[] = {
    {.name = "role",
     .label = "Role",
     .tooltip = "Solid renders and collides; Visual only renders; Clip only "
                "collides; Trigger is a sensor volume",
     .names = s_brush_role_names,
     .labels = s_brush_role_labels,
     .offset = TYPE_OFFSET(SceneBrushSettings, role),
     .kind = VKR_PROPERTY_ENUM},
};

static void brush_defaults(void *value) {
  *(SceneBrushSettings *)value =
      (SceneBrushSettings){.role = SCENE_BRUSH_ROLE_SOLID};
}

const VkrTypeDesc vkr_scene_brush_type = {
    .name = "brush",
    .label = "Brush",
    .category = "Level",
    .properties = s_brush_properties,
    .property_count = ArrayCount(s_brush_properties),
    .size = sizeof(SceneBrushSettings),
    .align = _Alignof(SceneBrushSettings),
    .defaults = brush_defaults,
};

static const VkrPropertyDesc s_brush_face_properties[] = {
    {.name = "normal",
     .label = "Normal",
     .tooltip = "Outward plane normal in the brush's space",
     .group = "Plane",
     .offset = TYPE_OFFSET(SceneBrushFace, normal),
     .kind = VKR_PROPERTY_VEC3,
     .min = -1.0f,
     .max = 1.0f,
     .step = 0.01f},
    {.name = "distance",
     .label = "Distance",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneBrushFace, distance),
     .kind = VKR_PROPERTY_F32,
     .step = 0.0625f},
    {.name = "surface",
     .label = "Surface",
     .group = "Surface",
     .tooltip = "What the face is made of; it shows the surface's fixed "
                "greybox look until the art pass gives it a material",
     .names = vkr_surface_names,
     .labels = vkr_surface_labels,
     .offset = TYPE_OFFSET(SceneBrushFace, surface),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "mark",
     .label = "Mark",
     .tooltip = "A level-design accent that replaces the surface's greybox "
                "tone: hazard stripes or a wayfinding colour",
     .names = vkr_surface_mark_names,
     .labels = vkr_surface_mark_labels,
     .offset = TYPE_OFFSET(SceneBrushFace, mark),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "material",
     .label = "Material file",
     .tooltip = "Art-owned material; empty shows the greybox look",
     .offset = TYPE_OFFSET(SceneBrushFace, material),
     .capacity = sizeof(((SceneBrushFace *)0)->material),
     .kind = VKR_PROPERTY_STRING},
    {.name = "uv_offset",
     .label = "Texture offset",
     .offset = TYPE_OFFSET(SceneBrushFace, uv_offset),
     .kind = VKR_PROPERTY_VEC2,
     .step = 0.01f},
    {.name = "uv_scale",
     .label = "Texture size",
     .tooltip = "Meters one texture repeat covers",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneBrushFace, uv_scale),
     .kind = VKR_PROPERTY_VEC2,
     .min = 0.001f,
     .max = 10000.0f,
     .step = 0.01f},
    {.name = "uv_rotation",
     .label = "Texture rotation",
     .offset = TYPE_OFFSET(SceneBrushFace, uv_rotation),
     .kind = VKR_PROPERTY_ANGLE,
     .min = -360.0f,
     .max = 360.0f},
    {.name = "uv_world",
     .label = "World-aligned texture",
     .offset = TYPE_OFFSET(SceneBrushFace, uv_world),
     .kind = VKR_PROPERTY_BOOL},
};

static void brush_face_defaults(void *value) {
  *(SceneBrushFace *)value = (SceneBrushFace){
      .normal = vec3_new(0.0f, 1.0f, 0.0f),
      .distance = 0.5f,
      .uv_scale = vec2_new(1.0f, 1.0f),
      .uv_world = true_v,
  };
}

/* Older documents name a retired dev palette material where the face now
   stores its surface and mark. */
static void brush_face_migrate(void *value) {
  SceneBrushFace *face = value;
  if (vkr_surface_from_legacy_material(face->material, &face->surface,
                                       &face->mark)) {
    face->material[0] = '\0';
  }
}

static bool8_t brush_face_validate(const void *value, char *error,
                                   uint32_t capacity) {
  const SceneBrushFace *face = value;
  if (vec3_length(face->normal) < 1.0e-4f) {
    if (error) {
      snprintf(error, capacity, "A face normal must not be zero");
    }
    return false_v;
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_brush_face_type = {
    .name = "brush_face",
    .label = "Brush face",
    .category = "Level",
    .properties = s_brush_face_properties,
    .property_count = ArrayCount(s_brush_face_properties),
    .size = sizeof(SceneBrushFace),
    .align = _Alignof(SceneBrushFace),
    .defaults = brush_face_defaults,
    .migrate = brush_face_migrate,
    .validate = brush_face_validate,
};

// =============================================================================
// Terrain (ADR-084)
// =============================================================================

static const VkrPropertyDesc s_terrain_properties[] = {
    {.name = "heightfield",
     .label = "Heightfield file",
     .tooltip = "Heights and layer weights, relative to the scene's asset "
                "root; saving the scene writes it",
     .offset = TYPE_OFFSET(SceneTerrain, heightfield),
     .capacity = SCENE_TERRAIN_PATH_CAPACITY,
     .kind = VKR_PROPERTY_STRING,
     .flags = VKR_PROPERTY_FLAG_READ_ONLY},
    {.name = "layer0",
     .label = "Layer 1",
     .group = "Layers",
     .tooltip = "Material of the first layer; empty uses the dev grid",
     .offset = TYPE_OFFSET(SceneTerrain, layer0),
     .capacity = SCENE_TERRAIN_MATERIAL_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "layer1",
     .label = "Layer 2",
     .offset = TYPE_OFFSET(SceneTerrain, layer1),
     .capacity = SCENE_TERRAIN_MATERIAL_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "layer2",
     .label = "Layer 3",
     .offset = TYPE_OFFSET(SceneTerrain, layer2),
     .capacity = SCENE_TERRAIN_MATERIAL_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "layer3",
     .label = "Layer 4",
     .offset = TYPE_OFFSET(SceneTerrain, layer3),
     .capacity = SCENE_TERRAIN_MATERIAL_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "texture_size",
     .label = "Texture size",
     .unit = "m",
     .tooltip = "Metres one texture repeat covers",
     .offset = TYPE_OFFSET(SceneTerrain, texture_size),
     .kind = VKR_PROPERTY_F32,
     .min = 0.01f,
     .max = 1000.0f,
     .step = 0.1f},
    {.name = "stream_radius",
     .label = "Detail radius",
     .group = "Streaming",
     .unit = "m",
     .tooltip = "Distance from the camera or a player within which a terrain "
                "larger than 1024 cells loads full detail; beyond it the "
                "overview draws",
     .offset = TYPE_OFFSET(SceneTerrain, stream_radius),
     .kind = VKR_PROPERTY_F32,
     .min = 128.0f,
     .max = 4096.0f,
     .step = 64.0f},
};

static void terrain_defaults(void *value) {
  *(SceneTerrain *)value =
      (SceneTerrain){.texture_size = 4.0f, .stream_radius = 512.0f};
}

/* Older documents name retired dev palette materials as layers; each takes
   the floor greybox look of the surface and mark that replaced it. */
static void terrain_migrate(void *value) {
  SceneTerrain *terrain = value;
  char *const layers[] = {terrain->layer0, terrain->layer1, terrain->layer2,
                          terrain->layer3};
  for (uint32_t i = 0; i < ArrayCount(layers); ++i) {
    VkrSurface surface = VKR_SURFACE_NONE;
    VkrSurfaceMark mark = VKR_SURFACE_MARK_NONE;
    if (vkr_surface_from_legacy_material(layers[i], &surface, &mark)) {
      snprintf(layers[i], SCENE_TERRAIN_MATERIAL_CAPACITY, "%s",
               vkr_surface_greybox_material(surface, mark, VKR_SURFACE_FLOOR));
    }
  }
}

const VkrTypeDesc vkr_scene_terrain_type = {
    .name = "terrain",
    .label = "Terrain",
    .category = "Level",
    .properties = s_terrain_properties,
    .property_count = ArrayCount(s_terrain_properties),
    .size = sizeof(SceneTerrain),
    .align = _Alignof(SceneTerrain),
    .defaults = terrain_defaults,
    .migrate = terrain_migrate,
};

// =============================================================================
// Population (ADR-084)
// =============================================================================

static const VkrPropertyDesc s_spline_properties[] = {
    {.name = "closed",
     .label = "Closed",
     .tooltip = "The curve returns from its last point to its first",
     .offset = TYPE_OFFSET(SceneSpline, closed),
     .kind = VKR_PROPERTY_BOOL},
};

const VkrTypeDesc vkr_scene_spline_type = {
    .name = "spline",
    .label = "Spline",
    .category = "Level",
    .properties = s_spline_properties,
    .property_count = ArrayCount(s_spline_properties),
    .size = sizeof(SceneSpline),
    .align = _Alignof(SceneSpline),
};

static const VkrPropertyDesc s_spline_point_properties[] = {
    {.name = "order",
     .label = "Order",
     .tooltip = "The curve visits its points from the lowest order up",
     .offset = TYPE_OFFSET(SceneSplinePoint, order),
     .kind = VKR_PROPERTY_F32,
     .min = -1.0e6f,
     .max = 1.0e6f,
     .step = 1.0f},
};

const VkrTypeDesc vkr_scene_spline_point_type = {
    .name = "spline_point",
    .label = "Spline point",
    .category = "Level",
    .properties = s_spline_point_properties,
    .property_count = ArrayCount(s_spline_point_properties),
    .size = sizeof(SceneSplinePoint),
    .align = _Alignof(SceneSplinePoint),
};

static const VkrPropertyDesc s_spline_mesh_properties[] = {
    {.name = "mesh",
     .label = "Mesh",
     .tooltip = "Cooked .vkb whose mesh repeats along the spline",
     .offset = TYPE_OFFSET(SceneSplineMesh, mesh),
     .capacity = SCENE_POPULATION_MESH_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "mesh_index",
     .label = "Mesh index",
     .tooltip = "Source mesh in the file, counting from 0",
     .offset = TYPE_OFFSET(SceneSplineMesh, mesh_index),
     .kind = VKR_PROPERTY_U32,
     .max = 65535.0f},
    {.name = "spacing",
     .label = "Spacing",
     .unit = "m",
     .tooltip = "Distance between copies along the curve",
     .offset = TYPE_OFFSET(SceneSplineMesh, spacing),
     .kind = VKR_PROPERTY_F32,
     .min = 0.05f,
     .max = 1000.0f,
     .step = 0.1f},
    {.name = "scale",
     .label = "Scale",
     .offset = TYPE_OFFSET(SceneSplineMesh, scale),
     .kind = VKR_PROPERTY_VEC3,
     .min = 0.001f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "offset",
     .label = "Offset",
     .unit = "m",
     .tooltip = "Offset in each copy's frame: x right, y up, z along",
     .offset = TYPE_OFFSET(SceneSplineMesh, offset),
     .kind = VKR_PROPERTY_VEC3,
     .min = -1000.0f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "rotation",
     .label = "Rotation",
     .unit = "\xc2\xb0",
     .tooltip = "Turn of the mesh within each copy's frame, about X, Y, Z",
     .offset = TYPE_OFFSET(SceneSplineMesh, rotation),
     .kind = VKR_PROPERTY_VEC3,
     .min = -360.0f,
     .max = 360.0f,
     .step = 1.0f},
    {.name = "follow_slope",
     .label = "Follow slope",
     .tooltip = "Copies tilt with the curve instead of standing upright",
     .offset = TYPE_OFFSET(SceneSplineMesh, follow_slope),
     .kind = VKR_PROPERTY_BOOL},
};

static void spline_mesh_defaults(void *value) {
  *(SceneSplineMesh *)value =
      (SceneSplineMesh){.spacing = 2.0f, .scale = vec3_new(1.0f, 1.0f, 1.0f)};
}

const VkrTypeDesc vkr_scene_spline_mesh_type = {
    .name = "spline_mesh",
    .label = "Spline mesh",
    .category = "Level",
    .properties = s_spline_mesh_properties,
    .property_count = ArrayCount(s_spline_mesh_properties),
    .size = sizeof(SceneSplineMesh),
    .align = _Alignof(SceneSplineMesh),
    .defaults = spline_mesh_defaults,
};

static const VkrPropertyDesc s_scatter_properties[] = {
    {.name = "mesh",
     .label = "Mesh",
     .tooltip = "Cooked .vkb whose mesh the scatter places",
     .offset = TYPE_OFFSET(SceneScatter, mesh),
     .capacity = SCENE_POPULATION_MESH_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "mesh_index",
     .label = "Mesh index",
     .tooltip = "Source mesh in the file, counting from 0",
     .offset = TYPE_OFFSET(SceneScatter, mesh_index),
     .kind = VKR_PROPERTY_U32,
     .max = 65535.0f},
    {.name = "count",
     .label = "Count",
     .tooltip = "Copies to place; at most 2048",
     .offset = TYPE_OFFSET(SceneScatter, count),
     .kind = VKR_PROPERTY_U32,
     .max = 2048.0f},
    {.name = "seed",
     .label = "Seed",
     .tooltip = "Another seed places the copies anew",
     .offset = TYPE_OFFSET(SceneScatter, seed),
     .kind = VKR_PROPERTY_U32},
    {.name = "extents",
     .label = "Extents",
     .unit = "m",
     .tooltip = "Half sizes of the box copies land in",
     .offset = TYPE_OFFSET(SceneScatter, extents),
     .kind = VKR_PROPERTY_VEC3,
     .min = 0.01f,
     .max = 10000.0f,
     .step = 0.5f},
    {.name = "scale_min",
     .label = "Scale min",
     .offset = TYPE_OFFSET(SceneScatter, scale_min),
     .kind = VKR_PROPERTY_F32,
     .min = 0.001f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "scale_max",
     .label = "Scale max",
     .offset = TYPE_OFFSET(SceneScatter, scale_max),
     .kind = VKR_PROPERTY_F32,
     .min = 0.001f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "rotation",
     .label = "Rotation",
     .unit = "\xc2\xb0",
     .tooltip = "Turn of the mesh within each copy's frame, about X, Y, Z",
     .offset = TYPE_OFFSET(SceneScatter, rotation),
     .kind = VKR_PROPERTY_VEC3,
     .min = -360.0f,
     .max = 360.0f,
     .step = 1.0f},
    {.name = "align_to_surface",
     .label = "Align to surface",
     .tooltip = "Copies lean to the ground's normal",
     .offset = TYPE_OFFSET(SceneScatter, align_to_surface),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "random_yaw",
     .label = "Random yaw",
     .tooltip = "Copies turn about their up axis at random",
     .offset = TYPE_OFFSET(SceneScatter, random_yaw),
     .kind = VKR_PROPERTY_BOOL},
};

static void scatter_defaults(void *value) {
  *(SceneScatter *)value = (SceneScatter){
      .count = 64u,
      .seed = 1u,
      .extents = vec3_new(10.0f, 10.0f, 10.0f),
      .scale_min = 0.8f,
      .scale_max = 1.2f,
      .random_yaw = true_v,
  };
}

const VkrTypeDesc vkr_scene_scatter_type = {
    .name = "scatter",
    .label = "Scatter",
    .category = "Level",
    .properties = s_scatter_properties,
    .property_count = ArrayCount(s_scatter_properties),
    .size = sizeof(SceneScatter),
    .align = _Alignof(SceneScatter),
    .defaults = scatter_defaults,
};

static const VkrPropertyDesc s_scatter_area_properties[] = {
    {.name = "radius",
     .label = "Radius",
     .unit = "m",
     .tooltip = "Copies fill a disc this wide about the area's position",
     .offset = TYPE_OFFSET(SceneScatterArea, radius),
     .kind = VKR_PROPERTY_F32,
     .min = 0.05f,
     .max = 100.0f,
     .step = 0.1f},
    {.name = "density",
     .label = "Density",
     .unit = "/m\xc2\xb2",
     .tooltip = "Copies per square metre",
     .offset = TYPE_OFFSET(SceneScatterArea, density),
     .kind = VKR_PROPERTY_F32,
     .min = 0.001f,
     .max = 100.0f,
     .step = 0.05f},
    {.name = "spacing",
     .label = "Spacing",
     .unit = "m",
     .tooltip = "A copy this close to an earlier copy of the scatter is "
                "left out",
     .offset = TYPE_OFFSET(SceneScatterArea, spacing),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100.0f,
     .step = 0.05f},
    {.name = "seed",
     .label = "Seed",
     .tooltip = "Another seed places the area's copies anew",
     .offset = TYPE_OFFSET(SceneScatterArea, seed),
     .kind = VKR_PROPERTY_U32},
    {.name = "order",
     .label = "Order",
     .tooltip = "Areas place copies from the lowest order up; later ones "
                "leave room for earlier copies",
     .offset = TYPE_OFFSET(SceneScatterArea, order),
     .kind = VKR_PROPERTY_U32},
};

static void scatter_area_defaults(void *value) {
  *(SceneScatterArea *)value =
      (SceneScatterArea){.radius = 2.0f, .density = 1.0f, .seed = 1u};
}

const VkrTypeDesc vkr_scene_scatter_area_type = {
    .name = "scatter_area",
    .label = "Scatter area",
    .category = "Level",
    .properties = s_scatter_area_properties,
    .property_count = ArrayCount(s_scatter_area_properties),
    .size = sizeof(SceneScatterArea),
    .align = _Alignof(SceneScatterArea),
    .defaults = scatter_area_defaults,
};

// =============================================================================
// Entity IO (ADR-084)
// =============================================================================

static const VkrIoPort s_trigger_outputs[] = {
    {"on_enter", "On enter", VKR_PROPERTY_ENTITY},
    {"on_exit", "On exit", VKR_PROPERTY_ENTITY},
    {"on_empty", "On empty", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};
static const VkrIoPort s_switch_inputs[] = {
    {"enable", "Enable", VKR_IO_PORT_NONE},
    {"disable", "Disable", VKR_IO_PORT_NONE},
    {"toggle", "Toggle", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_trigger_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneTrigger, enabled),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "once",
     .label = "Once",
     .tooltip = "Disable after the first entry",
     .offset = TYPE_OFFSET(SceneTrigger, once),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "filter",
     .label = "Only with",
     .tooltip = "A component the entering entity must carry, such as "
                "fps_player; empty takes any",
     .offset = TYPE_OFFSET(SceneTrigger, filter),
     .capacity = SCENE_IO_NAME_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
};

static void trigger_defaults(void *value) {
  *(SceneTrigger *)value = (SceneTrigger){.enabled = true_v};
}

const VkrTypeDesc vkr_scene_trigger_type = {
    .name = "trigger",
    .label = "Trigger",
    .category = "Level",
    .properties = s_trigger_properties,
    .property_count = ArrayCount(s_trigger_properties),
    .size = sizeof(SceneTrigger),
    .align = _Alignof(SceneTrigger),
    .defaults = trigger_defaults,
    .outputs = s_trigger_outputs,
    .inputs = s_switch_inputs,
};

static const VkrIoPort s_relay_outputs[] = {
    {"on_trigger", "On trigger", VKR_IO_PORT_NONE}, {NULL, NULL, 0u}};
static const VkrIoPort s_relay_inputs[] = {
    {"trigger", "Trigger", VKR_IO_PORT_NONE},
    {"enable", "Enable", VKR_IO_PORT_NONE},
    {"disable", "Disable", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_relay_properties[] = {
    {.name = "enabled",
     .label = "Enabled",
     .offset = TYPE_OFFSET(SceneRelay, enabled),
     .kind = VKR_PROPERTY_BOOL},
};

static void relay_defaults(void *value) {
  *(SceneRelay *)value = (SceneRelay){.enabled = true_v};
}

const VkrTypeDesc vkr_scene_relay_type = {
    .name = "relay",
    .label = "Relay",
    .category = "Level",
    .properties = s_relay_properties,
    .property_count = ArrayCount(s_relay_properties),
    .size = sizeof(SceneRelay),
    .align = _Alignof(SceneRelay),
    .defaults = relay_defaults,
    .outputs = s_relay_outputs,
    .inputs = s_relay_inputs,
};

static const VkrIoPort s_button_outputs[] = {
    {"on_pressed", "On pressed", VKR_PROPERTY_ENTITY},
    {"on_refused", "On refused", VKR_PROPERTY_ENTITY},
    {NULL, NULL, 0u}};
static const VkrIoPort s_button_inputs[] = {
    {"press", "Press", VKR_PROPERTY_ENTITY},
    {"lock", "Lock", VKR_IO_PORT_NONE},
    {"unlock", "Unlock", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_button_properties[] = {
    {.name = "wait",
     .label = "Wait",
     .tooltip = "Seconds before it takes another press; -1 takes one press "
                "a session",
     .unit = "s",
     .offset = TYPE_OFFSET(SceneButton, wait),
     .kind = VKR_PROPERTY_F32,
     .min = -1.0f,
     .max = 86400.0f,
     .step = 0.05f},
    {.name = "locked",
     .label = "Locked",
     .tooltip = "A press fires On refused instead until unlocked",
     .offset = TYPE_OFFSET(SceneButton, locked),
     .kind = VKR_PROPERTY_BOOL},
};

static void button_defaults(void *value) {
  *(SceneButton *)value = (SceneButton){.wait = 1.0f};
}

const VkrTypeDesc vkr_scene_button_type = {
    .name = "button",
    .label = "Button",
    .category = "Level",
    .properties = s_button_properties,
    .property_count = ArrayCount(s_button_properties),
    .size = sizeof(SceneButton),
    .align = _Alignof(SceneButton),
    .defaults = button_defaults,
    .outputs = s_button_outputs,
    .inputs = s_button_inputs,
};

static const VkrIoPort s_timer_outputs[] = {
    {"on_timer", "On timer", VKR_IO_PORT_NONE}, {NULL, NULL, 0u}};
static const VkrIoPort s_timer_inputs[] = {
    {"start", "Start", VKR_IO_PORT_NONE},
    {"stop", "Stop", VKR_IO_PORT_NONE},
    {"set_interval", "Set interval", VKR_PROPERTY_F32},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_timer_properties[] = {
    {.name = "interval",
     .label = "Interval",
     .unit = "s",
     .offset = TYPE_OFFSET(SceneTimer, interval),
     .kind = VKR_PROPERTY_F32,
     .min = 0.01f,
     .max = 86400.0f,
     .step = 0.05f},
    {.name = "start_running",
     .label = "Start running",
     .offset = TYPE_OFFSET(SceneTimer, start_running),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "once",
     .label = "Once",
     .tooltip = "Stop after one firing",
     .offset = TYPE_OFFSET(SceneTimer, once),
     .kind = VKR_PROPERTY_BOOL},
};

static void timer_defaults(void *value) {
  *(SceneTimer *)value =
      (SceneTimer){.interval = 1.0f, .start_running = true_v};
}

const VkrTypeDesc vkr_scene_timer_type = {
    .name = "timer",
    .label = "Timer",
    .category = "Level",
    .properties = s_timer_properties,
    .property_count = ArrayCount(s_timer_properties),
    .size = sizeof(SceneTimer),
    .align = _Alignof(SceneTimer),
    .defaults = timer_defaults,
    .outputs = s_timer_outputs,
    .inputs = s_timer_inputs,
};

static const VkrIoPort s_counter_outputs[] = {
    {"on_changed", "On changed", VKR_PROPERTY_I32},
    {"on_max", "On max", VKR_IO_PORT_NONE},
    {"on_min", "On min", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};
static const VkrIoPort s_counter_inputs[] = {
    {"add", "Add", VKR_PROPERTY_I32},
    {"subtract", "Subtract", VKR_PROPERTY_I32},
    {"set", "Set", VKR_PROPERTY_I32},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_counter_properties[] = {
    {.name = "start",
     .label = "Start",
     .offset = TYPE_OFFSET(SceneCounter, start),
     .kind = VKR_PROPERTY_I32},
    {.name = "min",
     .label = "Minimum",
     .offset = TYPE_OFFSET(SceneCounter, min),
     .kind = VKR_PROPERTY_I32},
    {.name = "max",
     .label = "Maximum",
     .offset = TYPE_OFFSET(SceneCounter, max),
     .kind = VKR_PROPERTY_I32},
};

static void counter_defaults(void *value) {
  *(SceneCounter *)value = (SceneCounter){.start = 0, .min = 0, .max = 1};
}

static bool8_t counter_validate(const void *value, char *error,
                                uint32_t capacity) {
  const SceneCounter *counter = value;
  if (counter->min > counter->max || counter->start < counter->min ||
      counter->start > counter->max) {
    if (error) {
      snprintf(error, capacity, "A counter needs min <= start <= max");
    }
    return false_v;
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_counter_type = {
    .name = "counter",
    .label = "Counter",
    .category = "Level",
    .properties = s_counter_properties,
    .property_count = ArrayCount(s_counter_properties),
    .size = sizeof(SceneCounter),
    .align = _Alignof(SceneCounter),
    .defaults = counter_defaults,
    .validate = counter_validate,
    .outputs = s_counter_outputs,
    .inputs = s_counter_inputs,
};

static const VkrIoPort s_mover_outputs[] = {
    {"on_open", "On open", VKR_IO_PORT_NONE},
    {"on_opened", "On opened", VKR_IO_PORT_NONE},
    {"on_close", "On close", VKR_IO_PORT_NONE},
    {"on_closed", "On closed", VKR_IO_PORT_NONE},
    {NULL, NULL, 0u}};
static const VkrIoPort s_mover_inputs[] = {
    {"open", "Open", VKR_IO_PORT_NONE},
    {"close", "Close", VKR_IO_PORT_NONE},
    {"toggle", "Toggle", VKR_IO_PORT_NONE},
    {"lock", "Lock", VKR_IO_PORT_NONE},
    {"unlock", "Unlock", VKR_IO_PORT_NONE},
    {"set_position", "Set position", VKR_PROPERTY_F32},
    {NULL, NULL, 0u}};

static const VkrPropertyDesc s_mover_properties[] = {
    {.name = "direction",
     .label = "Direction",
     .tooltip = "Where it opens, in its own space",
     .offset = TYPE_OFFSET(SceneMover, direction),
     .kind = VKR_PROPERTY_DIRECTION,
     .step = 1.0f},
    {.name = "distance",
     .label = "Distance",
     .tooltip = "How far it opens; automatic takes the size of what it moves "
                "along the direction, less the lip",
     .unit = "m",
     .zero_label = "Automatic",
     .offset = TYPE_OFFSET(SceneMover, distance),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 10000.0f,
     .step = 0.05f},
    {.name = "lip",
     .label = "Lip",
     .tooltip = "What stays in view when the distance is automatic",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneMover, lip),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100.0f,
     .step = 0.01f},
    {.name = "angle",
     .label = "Angle",
     .tooltip = "Degrees it turns to open about the axis through the pivot; "
                "zero slides along the direction",
     .unit = "deg",
     .offset = TYPE_OFFSET(SceneMover, angle),
     .kind = VKR_PROPERTY_F32,
     .min = -3600.0f,
     .max = 3600.0f,
     .step = 1.0f},
    {.name = "axis",
     .label = "Axis",
     .tooltip = "What it turns about, in its own space",
     .offset = TYPE_OFFSET(SceneMover, axis),
     .kind = VKR_PROPERTY_DIRECTION,
     .step = 1.0f},
    {.name = "pivot",
     .label = "Pivot",
     .tooltip = "Where the axis passes, in its own space: a door's hinge",
     .unit = "m",
     .offset = TYPE_OFFSET(SceneMover, pivot),
     .kind = VKR_PROPERTY_VEC3,
     .step = 0.05f},
    {.name = "spin",
     .label = "Spin",
     .tooltip = "Turn about the axis without end while open, as a fan, at "
                "the speed in degrees per second",
     .offset = TYPE_OFFSET(SceneMover, spin),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "speed",
     .label = "Speed",
     .tooltip = "Metres per second, or degrees per second when it turns",
     .offset = TYPE_OFFSET(SceneMover, speed),
     .kind = VKR_PROPERTY_F32,
     .min = 0.01f,
     .max = 1000.0f,
     .step = 0.05f},
    {.name = "acceleration",
     .label = "Acceleration",
     .tooltip = "Speed it gains setting off and loses before it stops, per "
                "second; 0 moves at full speed at once",
     .offset = TYPE_OFFSET(SceneMover, acceleration),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 100000.0f,
     .step = 0.1f},
    {.name = "wait",
     .label = "Wait",
     .tooltip = "Seconds it stays open before it closes by itself; -1 stays "
                "open",
     .unit = "s",
     .offset = TYPE_OFFSET(SceneMover, wait),
     .kind = VKR_PROPERTY_F32,
     .min = -1.0f,
     .max = 86400.0f,
     .step = 0.05f},
    {.name = "start_open",
     .label = "Start open",
     .offset = TYPE_OFFSET(SceneMover, start_open),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "loop",
     .label = "Loop",
     .tooltip = "Move back and forth without inputs, resting the wait at "
                "each end",
     .offset = TYPE_OFFSET(SceneMover, loop),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "locked",
     .label = "Locked",
     .tooltip = "Refuse open, toggle and set position until unlocked",
     .offset = TYPE_OFFSET(SceneMover, locked),
     .kind = VKR_PROPERTY_BOOL},
};

static void mover_defaults(void *value) {
  *(SceneMover *)value = (SceneMover){.direction = vec3_new(1.0f, 0.0f, 0.0f),
                                      .speed = 2.0f,
                                      .wait = -1.0f,
                                      .axis = vec3_new(0.0f, 1.0f, 0.0f)};
}

static bool8_t mover_validate(const void *value, char *error,
                              uint32_t capacity) {
  const SceneMover *mover = value;
  const bool8_t turns = mover->angle != 0.0f || mover->spin;
  const Vec3 along = turns ? mover->axis : mover->direction;
  if (!isfinite(mover->acceleration) || mover->acceleration < 0.0f) {
    if (error) {
      snprintf(error, capacity, "A mover's acceleration is zero or more");
    }
    return false_v;
  }
  if (!isfinite(vec3_length(along)) || vec3_length(along) < 1.0e-4f ||
      !isfinite(mover->angle) || !isfinite(vec3_length(mover->pivot))) {
    if (error) {
      snprintf(error, capacity,
               turns ? "A turning mover needs an axis and a finite pivot"
                     : "A mover needs a direction");
    }
    return false_v;
  }
  return true_v;
}

const VkrTypeDesc vkr_scene_mover_type = {
    .name = "mover",
    .label = "Mover",
    .category = "Level",
    .properties = s_mover_properties,
    .property_count = ArrayCount(s_mover_properties),
    .size = sizeof(SceneMover),
    .align = _Alignof(SceneMover),
    .defaults = mover_defaults,
    .validate = mover_validate,
    .outputs = s_mover_outputs,
    .inputs = s_mover_inputs,
};

static const VkrPropertyDesc s_io_connection_properties[] = {
    {.name = "output",
     .label = "Output",
     .tooltip = "The source's output, as on_enter or trigger.on_enter",
     .offset = TYPE_OFFSET(SceneIoConnection, output),
     .capacity = SCENE_IO_NAME_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "target",
     .label = "Target",
     .offset = TYPE_OFFSET(SceneIoConnection, target),
     .kind = VKR_PROPERTY_ENTITY},
    {.name = "input",
     .label = "Input",
     .tooltip = "The target's input, as open or door.open",
     .offset = TYPE_OFFSET(SceneIoConnection, input),
     .capacity = SCENE_IO_NAME_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "value",
     .label = "Value",
     .tooltip = "Replaces the output's value; empty passes it on",
     .offset = TYPE_OFFSET(SceneIoConnection, value),
     .capacity = SCENE_IO_NAME_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
    {.name = "delay",
     .label = "Delay",
     .unit = "s",
     .offset = TYPE_OFFSET(SceneIoConnection, delay),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 3600.0f,
     .step = 0.05f},
    {.name = "limit",
     .label = "Times",
     .tooltip = "Firings per session; 0 is unlimited",
     .zero_label = "Unlimited",
     .offset = TYPE_OFFSET(SceneIoConnection, limit),
     .kind = VKR_PROPERTY_U32,
     .min = 1.0f,
     .max = 1000000.0f},
};

const VkrTypeDesc vkr_scene_io_connection_type = {
    .name = "io_connection",
    .label = "Connection",
    .category = "Level",
    .properties = s_io_connection_properties,
    .property_count = ArrayCount(s_io_connection_properties),
    .size = sizeof(SceneIoConnection),
    .align = _Alignof(SceneIoConnection),
};

// =============================================================================
// Tags (ADR-084)
// =============================================================================

static bool8_t tags_separator(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

static bool8_t tags_character(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

bool8_t vkr_scene_tags_next(const SceneTags *tags, uint32_t *cursor,
                            String8 *out) {
  uint32_t at = *cursor;
  while (at < SCENE_TAGS_CAPACITY && tags->text[at] == ' ') {
    ++at;
  }
  if (at >= SCENE_TAGS_CAPACITY || !tags->text[at]) {
    *cursor = at;
    return false_v;
  }
  uint32_t end = at;
  while (end < SCENE_TAGS_CAPACITY && tags->text[end] &&
         tags->text[end] != ' ') {
    ++end;
  }
  *out = string8_create((uint8_t *)&tags->text[at], end - at);
  *cursor = end;
  return true_v;
}

bool8_t vkr_scene_tags_has(const SceneTags *tags, String8 tag) {
  uint32_t cursor = 0u;
  String8 held = {0};
  while (vkr_scene_tags_next(tags, &cursor, &held)) {
    if (held.length == tag.length &&
        MemCompare(held.str, tag.str, tag.length) == 0) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_scene_tags_parse(String8 text, SceneTags *out, char *error,
                             uint32_t capacity) {
  SceneTags tags = {0};
  uint32_t length = 0u;
  uint32_t count = 0u;
  uint64_t at = 0u;
  while (at < text.length) {
    if (tags_separator(text.str[at])) {
      ++at;
      continue;
    }
    uint64_t end = at;
    while (end < text.length && !tags_separator(text.str[end])) {
      ++end;
    }
    const String8 word = string8_create(text.str + at, end - at);
    const uint64_t start = text.str[at] == '#' ? at + 1u : at;
    const uint64_t name_length = end - start;
    if (!name_length) {
      if (error) {
        snprintf(error, capacity, "A tag needs a name after '#'");
      }
      return false_v;
    }
    if (name_length > SCENE_TAG_LENGTH_MAX) {
      if (error) {
        snprintf(error, capacity, "Tag '%.*s' is longer than %u characters",
                 (int)word.length, word.str, SCENE_TAG_LENGTH_MAX);
      }
      return false_v;
    }
    char tag[SCENE_TAG_LENGTH_MAX + 1u];
    tag[0] = '#';
    for (uint64_t i = 0; i < name_length; ++i) {
      uint8_t c = text.str[start + i];
      if (c >= 'A' && c <= 'Z') {
        c = (uint8_t)(c - 'A' + 'a');
      }
      if (!tags_character(c)) {
        if (error) {
          snprintf(error, capacity,
                   "Tag '%.*s' may hold only a-z, 0-9, _ and - after its '#'",
                   (int)word.length, word.str);
        }
        return false_v;
      }
      tag[1u + i] = (char)c;
    }
    const String8 canonical =
        string8_create((uint8_t *)tag, (uint64_t)name_length + 1u);
    at = end;
    if (vkr_scene_tags_has(&tags, canonical)) {
      continue;
    }
    if (count == SCENE_TAG_COUNT_MAX) {
      if (error) {
        snprintf(error, capacity, "An object holds at most %u tags",
                 SCENE_TAG_COUNT_MAX);
      }
      return false_v;
    }
    if (length) {
      tags.text[length++] = ' ';
    }
    MemCopy(&tags.text[length], canonical.str, canonical.length);
    length += (uint32_t)canonical.length;
    ++count;
  }
  *out = tags;
  return true_v;
}

/* Stored tags are canonical, so every reader compares them byte for byte. */
static bool8_t tags_validate(const void *value, char *error,
                             uint32_t capacity) {
  const SceneTags *tags = value;
  SceneTags canonical;
  if (!vkr_scene_tags_parse(
          string8_create_from_cstr((const uint8_t *)tags->text,
                                   strlen(tags->text)),
          &canonical, error, capacity)) {
    return false_v;
  }
  if (strcmp(canonical.text, tags->text) != 0) {
    if (error) {
      snprintf(error, capacity,
               "Tags are stored lowercase with a '#', one space apart and "
               "without repeats, as \"%s\"",
               canonical.text);
    }
    return false_v;
  }
  return true_v;
}

static const VkrPropertyDesc s_tags_properties[] = {
    {.name = "tags",
     .label = "Tags",
     .tooltip = "Categories to find this object by, as #labs #chair",
     .offset = TYPE_OFFSET(SceneTags, text),
     .capacity = SCENE_TAGS_CAPACITY,
     .kind = VKR_PROPERTY_STRING},
};

const VkrTypeDesc vkr_scene_tags_type = {
    .name = "tags",
    .label = "Tags",
    .category = "Level",
    .properties = s_tags_properties,
    .property_count = ArrayCount(s_tags_properties),
    .size = sizeof(SceneTags),
    .align = _Alignof(SceneTags),
    .validate = tags_validate,
};

// =============================================================================
// World partition (ADR-086)
// =============================================================================

static const VkrPropertyDesc s_world_partition_properties[] = {
    {.name = "cell_size",
     .label = "Cell size",
     .unit = "m",
     .tooltip = "Side of one square cell of the ground plane; created root "
                "objects stream with the cell holding their position",
     .offset = TYPE_OFFSET(SceneWorldPartition, cell_size),
     .kind = VKR_PROPERTY_F32,
     .min = 64.0f,
     .max = 1024.0f,
     .step = 64.0f},
    {.name = "load_radius",
     .label = "Load radius",
     .unit = "m",
     .tooltip = "Cells within this distance of the camera or a player load",
     .offset = TYPE_OFFSET(SceneWorldPartition, load_radius),
     .kind = VKR_PROPERTY_F32,
     .min = 64.0f,
     .max = 8192.0f,
     .step = 64.0f},
    {.name = "proxy_radius",
     .label = "Proxy radius",
     .unit = "m",
     .tooltip = "Unloaded cells within this distance draw their proxy",
     .offset = TYPE_OFFSET(SceneWorldPartition, proxy_radius),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 16384.0f,
     .step = 256.0f},
    {.name = "cell_budget",
     .label = "Cell budget",
     .tooltip = "Cells loaded at once; past it the farthest unload first",
     .offset = TYPE_OFFSET(SceneWorldPartition, cell_budget),
     .kind = VKR_PROPERTY_U32,
     .min = 1.0f,
     .max = 4096.0f,
     .step = 1.0f},
};

static void world_partition_defaults(void *value) {
  *(SceneWorldPartition *)value = (SceneWorldPartition){.cell_size = 128.0f,
                                                        .load_radius = 384.0f,
                                                        .proxy_radius = 2048.0f,
                                                        .cell_budget = 256u};
}

const VkrTypeDesc vkr_scene_world_partition_type = {
    .name = "world_partition",
    .label = "World partition",
    .category = "Level",
    .flags = VKR_TYPE_FLAG_SINGLETON,
    .properties = s_world_partition_properties,
    .property_count = ArrayCount(s_world_partition_properties),
    .size = sizeof(SceneWorldPartition),
    .align = _Alignof(SceneWorldPartition),
    .defaults = world_partition_defaults,
};

static const VkrPropertyDesc s_always_loaded_properties[] = {
    {.name = "enabled",
     .label = "Always loaded",
     .tooltip = "Keeps this object and its children loaded in a partitioned "
                "scene instead of streaming with its cell",
     .offset = TYPE_OFFSET(SceneAlwaysLoaded, enabled),
     .kind = VKR_PROPERTY_BOOL},
};

static void always_loaded_defaults(void *value) {
  *(SceneAlwaysLoaded *)value = (SceneAlwaysLoaded){.enabled = true_v};
}

const VkrTypeDesc vkr_scene_always_loaded_type = {
    .name = "always_loaded",
    .label = "Always loaded",
    .category = "Level",
    .properties = s_always_loaded_properties,
    .property_count = ArrayCount(s_always_loaded_properties),
    .size = sizeof(SceneAlwaysLoaded),
    .align = _Alignof(SceneAlwaysLoaded),
    .defaults = always_loaded_defaults,
};

static const VkrPropertyDesc s_free_placement_properties[] = {
    {.name = "enabled",
     .label = "Free placement",
     .tooltip = "The editor never snaps this object: moving it, Snap and "
                "parenting it in the Outliner leave it where it is placed",
     .offset = TYPE_OFFSET(SceneFreePlacement, enabled),
     .kind = VKR_PROPERTY_BOOL},
};

static void free_placement_defaults(void *value) {
  *(SceneFreePlacement *)value = (SceneFreePlacement){.enabled = true_v};
}

const VkrTypeDesc vkr_scene_free_placement_type = {
    .name = "free_placement",
    .label = "Free placement",
    .category = "Level",
    .properties = s_free_placement_properties,
    .property_count = ArrayCount(s_free_placement_properties),
    .size = sizeof(SceneFreePlacement),
    .align = _Alignof(SceneFreePlacement),
    .defaults = free_placement_defaults,
};

static const char *const s_blockout_shape_names[] = {"Stairs", "Corridor",
                                                     NULL};
static const char *const s_stairs_kind_names[] = {
    "Straight", "L turn", "U turn", "Curved", "Spiral", NULL};

enum {
  BLOCKOUT_SHAPE = 0,
  BLOCKOUT_STAIRS,
  BLOCKOUT_HEIGHT,
  BLOCKOUT_WIDTH,
  BLOCKOUT_LENGTH,
  BLOCKOUT_STEP_HEIGHT,
  BLOCKOUT_TURN,
  BLOCKOUT_RADIUS,
  BLOCKOUT_THICKNESS,
  BLOCKOUT_LEFT,
  BLOCKOUT_CEILING,
  BLOCKOUT_SURFACE,
  BLOCKOUT_FLOOR_SURFACE,
  BLOCKOUT_MARK,
  BLOCKOUT_POINT_COUNT,
  BLOCKOUT_OPENING_COUNT,
  BLOCKOUT_POINT_FIRST,
  BLOCKOUT_CORNER_FIRST = BLOCKOUT_POINT_FIRST + SCENE_BLOCKOUT_POINT_MAX,
  BLOCKOUT_WALL_FIRST = BLOCKOUT_CORNER_FIRST + SCENE_BLOCKOUT_POINT_MAX,
  BLOCKOUT_OPENING_FIRST = BLOCKOUT_WALL_FIRST + SCENE_BLOCKOUT_OPENING_MAX,
};

static const VkrPropertyDesc s_blockout_properties[] = {
    [BLOCKOUT_SHAPE] = {.name = "shape",
                        .label = "Shape",
                        .names = s_blockout_shape_names,
                        .offset = TYPE_OFFSET(SceneBlockout, shape),
                        .kind = VKR_PROPERTY_ENUM},
    [BLOCKOUT_STAIRS] = {.name = "stairs",
                         .label = "Stairs",
                         .tooltip = "Straight; two flights turning at a "
                                    "landing; two flights running back; "
                                    "steps along an arc; or steps around a "
                                    "pole",
                         .names = s_stairs_kind_names,
                         .offset = TYPE_OFFSET(SceneBlockout, stairs),
                         .kind = VKR_PROPERTY_ENUM},
    [BLOCKOUT_HEIGHT] = {.name = "height",
                         .label = "Height",
                         .unit = "m",
                         .offset = TYPE_OFFSET(SceneBlockout, height),
                         .kind = VKR_PROPERTY_F32,
                         .min = 0.0625f,
                         .max = VKR_BLOCKOUT_HEIGHT_MAX,
                         .step = 0.0625f},
    [BLOCKOUT_WIDTH] = {.name = "width",
                        .label = "Width",
                        .unit = "m",
                        .offset = TYPE_OFFSET(SceneBlockout, width),
                        .kind = VKR_PROPERTY_F32,
                        .min = 0.25f,
                        .max = 64.0f,
                        .step = 0.0625f},
    [BLOCKOUT_LENGTH] = {.name = "length",
                         .label = "Length",
                         .tooltip = "The run; for L and U stairs the first "
                                    "flight with its landing",
                         .unit = "m",
                         .offset = TYPE_OFFSET(SceneBlockout, length),
                         .kind = VKR_PROPERTY_F32,
                         .min = 0.25f,
                         .max = 128.0f,
                         .step = 0.0625f},
    [BLOCKOUT_STEP_HEIGHT] = {.name = "step_height",
                              .label = "Step height",
                              .tooltip = "The highest a step may rise; the "
                                         "steps share the height evenly",
                              .unit = "m",
                              .offset = TYPE_OFFSET(SceneBlockout, step_height),
                              .kind = VKR_PROPERTY_F32,
                              .min = 0.05f,
                              .max = 2.0f,
                              .step = 0.0125f},
    [BLOCKOUT_TURN] = {.name = "turn",
                       .label = "Turn",
                       .tooltip = "Degrees the steps sweep; a spiral may "
                                  "turn more than once",
                       .unit = "deg",
                       .offset = TYPE_OFFSET(SceneBlockout, turn),
                       .kind = VKR_PROPERTY_F32,
                       .min = 15.0f,
                       .max = VKR_BLOCKOUT_TURN_MAX,
                       .step = 5.0f},
    [BLOCKOUT_RADIUS] = {.name = "radius",
                         .label = "Radius",
                         .tooltip = "Stairs: the inner radius; corridors: "
                                    "the radius of the arcs at the corners, "
                                    "zero for sharp corners",
                         .unit = "m",
                         .offset = TYPE_OFFSET(SceneBlockout, radius),
                         .kind = VKR_PROPERTY_F32,
                         .min = 0.0f,
                         .max = 64.0f,
                         .step = 0.0625f},
    [BLOCKOUT_THICKNESS] = {.name = "thickness",
                            .label = "Thickness",
                            .tooltip = "Corridors: walls, floor and ceiling; "
                                       "stairs: each step's slab, zero for "
                                       "steps solid down to the floor",
                            .unit = "m",
                            .offset = TYPE_OFFSET(SceneBlockout, thickness),
                            .kind = VKR_PROPERTY_F32,
                            .min = 0.0f,
                            .max = 4.0f,
                            .step = 0.0125f},
    [BLOCKOUT_LEFT] = {.name = "left",
                       .label = "Turn left",
                       .offset = TYPE_OFFSET(SceneBlockout, left),
                       .kind = VKR_PROPERTY_BOOL},
    [BLOCKOUT_CEILING] = {.name = "ceiling",
                          .label = "Ceiling",
                          .offset = TYPE_OFFSET(SceneBlockout, ceiling),
                          .kind = VKR_PROPERTY_BOOL},
    [BLOCKOUT_SURFACE] = {.name = "surface",
                          .label = "Surface",
                          .tooltip = "Surface tag of the steps or the walls, "
                                     "and of a corridor floor without its own",
                          .names = vkr_surface_names,
                          .labels = vkr_surface_labels,
                          .offset = TYPE_OFFSET(SceneBlockout, surface),
                          .kind = VKR_PROPERTY_ENUM},
    [BLOCKOUT_FLOOR_SURFACE] = {.name = "floor_surface",
                                .label = "Floor surface",
                                .tooltip = "None takes the walls' surface",
                                .names = vkr_surface_names,
                                .labels = vkr_surface_labels,
                                .offset =
                                    TYPE_OFFSET(SceneBlockout, floor_surface),
                                .kind = VKR_PROPERTY_ENUM},
    [BLOCKOUT_MARK] = {.name = "mark",
                       .label = "Mark",
                       .tooltip = "Level-design accent of every piece",
                       .names = vkr_surface_mark_names,
                       .labels = vkr_surface_mark_labels,
                       .offset = TYPE_OFFSET(SceneBlockout, mark),
                       .kind = VKR_PROPERTY_ENUM},
    [BLOCKOUT_POINT_COUNT] = {.name = "point_count",
                              .label = "Points",
                              .offset = TYPE_OFFSET(SceneBlockout, point_count),
                              .kind = VKR_PROPERTY_U32,
                              .max = (float32_t)SCENE_BLOCKOUT_POINT_MAX},
    [BLOCKOUT_POINT_FIRST + 0u] = {.name = "point_0",
                                   .label = "Point 1",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[0]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 1u] = {.name = "point_1",
                                   .label = "Point 2",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[1]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 2u] = {.name = "point_2",
                                   .label = "Point 3",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[2]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 3u] = {.name = "point_3",
                                   .label = "Point 4",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[3]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 4u] = {.name = "point_4",
                                   .label = "Point 5",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[4]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 5u] = {.name = "point_5",
                                   .label = "Point 6",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[5]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 6u] = {.name = "point_6",
                                   .label = "Point 7",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[6]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 7u] = {.name = "point_7",
                                   .label = "Point 8",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[7]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 8u] = {.name = "point_8",
                                   .label = "Point 9",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[8]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 9u] = {.name = "point_9",
                                   .label = "Point 10",
                                   .offset =
                                       TYPE_OFFSET(SceneBlockout, points[9]),
                                   .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 10u] = {.name = "point_10",
                                    .label = "Point 11",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[10]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 11u] = {.name = "point_11",
                                    .label = "Point 12",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[11]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 12u] = {.name = "point_12",
                                    .label = "Point 13",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[12]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 13u] = {.name = "point_13",
                                    .label = "Point 14",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[13]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 14u] = {.name = "point_14",
                                    .label = "Point 15",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[14]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_POINT_FIRST + 15u] = {.name = "point_15",
                                    .label = "Point 16",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, points[15]),
                                    .kind = VKR_PROPERTY_VEC3},
    [BLOCKOUT_OPENING_COUNT] = {.name = "opening_count",
                                .label = "Openings",
                                .offset =
                                    TYPE_OFFSET(SceneBlockout, opening_count),
                                .kind = VKR_PROPERTY_U32,
                                .max = (float32_t)SCENE_BLOCKOUT_OPENING_MAX},
    [BLOCKOUT_CORNER_FIRST + 0u] = {.name = "corner_0",
                                    .label = "Corner 1",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[0]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 1u] = {.name = "corner_1",
                                    .label = "Corner 2",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[1]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 2u] = {.name = "corner_2",
                                    .label = "Corner 3",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[2]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 3u] = {.name = "corner_3",
                                    .label = "Corner 4",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[3]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 4u] = {.name = "corner_4",
                                    .label = "Corner 5",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[4]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 5u] = {.name = "corner_5",
                                    .label = "Corner 6",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[5]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 6u] = {.name = "corner_6",
                                    .label = "Corner 7",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[6]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 7u] = {.name = "corner_7",
                                    .label = "Corner 8",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[7]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 8u] = {.name = "corner_8",
                                    .label = "Corner 9",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[8]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST + 9u] = {.name = "corner_9",
                                    .label = "Corner 10",
                                    .offset =
                                        TYPE_OFFSET(SceneBlockout, corners[9]),
                                    .kind = VKR_PROPERTY_F32,
                                    .min = -1.0f,
                                    .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        10u] = {.name = "corner_10",
                .label = "Corner 11",
                .offset = TYPE_OFFSET(SceneBlockout, corners[10]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        11u] = {.name = "corner_11",
                .label = "Corner 12",
                .offset = TYPE_OFFSET(SceneBlockout, corners[11]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        12u] = {.name = "corner_12",
                .label = "Corner 13",
                .offset = TYPE_OFFSET(SceneBlockout, corners[12]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        13u] = {.name = "corner_13",
                .label = "Corner 14",
                .offset = TYPE_OFFSET(SceneBlockout, corners[13]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        14u] = {.name = "corner_14",
                .label = "Corner 15",
                .offset = TYPE_OFFSET(SceneBlockout, corners[14]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_CORNER_FIRST +
        15u] = {.name = "corner_15",
                .label = "Corner 16",
                .offset = TYPE_OFFSET(SceneBlockout, corners[15]),
                .kind = VKR_PROPERTY_F32,
                .min = -1.0f,
                .max = 64.0f},
    [BLOCKOUT_WALL_FIRST + 0u] = {.name = "wall_0",
                                  .label = "Opening wall 1",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[0]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 1u] = {.name = "wall_1",
                                  .label = "Opening wall 2",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[1]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 2u] = {.name = "wall_2",
                                  .label = "Opening wall 3",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[2]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 3u] = {.name = "wall_3",
                                  .label = "Opening wall 4",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[3]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 4u] = {.name = "wall_4",
                                  .label = "Opening wall 5",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[4]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 5u] = {.name = "wall_5",
                                  .label = "Opening wall 6",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[5]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 6u] = {.name = "wall_6",
                                  .label = "Opening wall 7",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[6]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_WALL_FIRST + 7u] = {.name = "wall_7",
                                  .label = "Opening wall 8",
                                  .offset =
                                      TYPE_OFFSET(SceneBlockout, walls[7]),
                                  .kind = VKR_PROPERTY_U32},
    [BLOCKOUT_OPENING_FIRST +
        0u] = {.name = "opening_0",
               .label = "Opening 1",
               .offset = TYPE_OFFSET(SceneBlockout, openings[0]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        1u] = {.name = "opening_1",
               .label = "Opening 2",
               .offset = TYPE_OFFSET(SceneBlockout, openings[1]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        2u] = {.name = "opening_2",
               .label = "Opening 3",
               .offset = TYPE_OFFSET(SceneBlockout, openings[2]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        3u] = {.name = "opening_3",
               .label = "Opening 4",
               .offset = TYPE_OFFSET(SceneBlockout, openings[3]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        4u] = {.name = "opening_4",
               .label = "Opening 5",
               .offset = TYPE_OFFSET(SceneBlockout, openings[4]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        5u] = {.name = "opening_5",
               .label = "Opening 6",
               .offset = TYPE_OFFSET(SceneBlockout, openings[5]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        6u] = {.name = "opening_6",
               .label = "Opening 7",
               .offset = TYPE_OFFSET(SceneBlockout, openings[6]),
               .kind = VKR_PROPERTY_VEC4},
    [BLOCKOUT_OPENING_FIRST +
        7u] = {.name = "opening_7",
               .label = "Opening 8",
               .offset = TYPE_OFFSET(SceneBlockout, openings[7]),
               .kind = VKR_PROPERTY_VEC4},
};

/* Material files shapes named before surface tags replaced them. */
static const char *const s_blockout_retired[] = {"material", "floor_material",
                                                 NULL};

static void blockout_defaults(void *value) {
  *(SceneBlockout *)value = (SceneBlockout){
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
  for (uint32_t i = 0; i < SCENE_BLOCKOUT_POINT_MAX; ++i) {
    ((SceneBlockout *)value)->corners[i] = -1.0f;
  }
}

/* Each shape shows only what shapes it; corridor points are edited in the
   Scene. */
static VkrPropertyState blockout_state(const void *value, uint32_t property,
                                       const void *context) {
  (void)context;
  const SceneBlockout *blockout = value;
  const bool8_t stairs = blockout->shape == SCENE_BLOCKOUT_STAIRS;
  const SceneStairsKind kind = blockout->stairs;
  const bool8_t round =
      kind == SCENE_STAIRS_CURVED || kind == SCENE_STAIRS_SPIRAL;
  bool8_t shown = true_v;
  switch (property) {
  case BLOCKOUT_STAIRS:
  case BLOCKOUT_STEP_HEIGHT:
    shown = stairs;
    break;
  case BLOCKOUT_LENGTH:
    shown = stairs && !round;
    break;
  case BLOCKOUT_TURN:
    shown = stairs && round;
    break;
  case BLOCKOUT_RADIUS:
    shown = !stairs || round;
    break;
  case BLOCKOUT_LEFT:
    shown = stairs && kind != SCENE_STAIRS_STRAIGHT;
    break;
  case BLOCKOUT_CEILING:
  case BLOCKOUT_FLOOR_SURFACE:
    shown = !stairs;
    break;
  default:
    shown = property < BLOCKOUT_POINT_COUNT;
    break;
  }
  return (VkrPropertyState){
      .flags = (shown ? 0u : VKR_PROPERTY_STATE_HIDDEN) |
               (property == BLOCKOUT_SHAPE ? VKR_PROPERTY_STATE_DISABLED : 0u)};
}

const VkrTypeDesc vkr_scene_blockout_type = {
    .name = "blockout",
    .label = "Blockout shape",
    .category = "Level",
    .properties = s_blockout_properties,
    .property_count = ArrayCount(s_blockout_properties),
    .size = sizeof(SceneBlockout),
    .align = _Alignof(SceneBlockout),
    .retired = s_blockout_retired,
    .defaults = blockout_defaults,
    .state = blockout_state,
};

static const VkrTypeDesc *const s_world_types[] = {
    &vkr_scene_environment_type,
    &vkr_scene_atmosphere_type,
    &vkr_scene_clouds_type,
    &vkr_scene_fog_type,
    &vkr_scene_froxel_fog_type,
    &vkr_scene_fog_box_type,
    &vkr_scene_post_process_type,
    &vkr_scene_reflection_probe_type,
    &vkr_scene_diffuse_volume_type,
    &vkr_scene_subsurface_type,
    &vkr_scene_physics_settings_type,
    &vkr_scene_animation_settings_type,
    &vkr_scene_time_of_day_type,
    &vkr_scene_shape_type,
    &vkr_scene_text_type,
    &vkr_scene_animation_type,
    &vkr_scene_player_start_type,
    &vkr_scene_decal_type,
    &vkr_scene_look_volume_type,
    &vkr_scene_brush_type,
    &vkr_scene_brush_face_type,
    &vkr_scene_trigger_type,
    &vkr_scene_relay_type,
    &vkr_scene_button_type,
    &vkr_scene_timer_type,
    &vkr_scene_counter_type,
    &vkr_scene_io_connection_type,
    &vkr_scene_terrain_type,
    &vkr_scene_spline_type,
    &vkr_scene_spline_point_type,
    &vkr_scene_spline_mesh_type,
    &vkr_scene_scatter_type,
    &vkr_scene_world_partition_type,
    &vkr_scene_always_loaded_type,
    &vkr_scene_free_placement_type,
    &vkr_scene_blockout_type,
    &vkr_scene_mover_type,
    &vkr_scene_tags_type,
    &vkr_scene_material_override_type,
    &vkr_scene_surface_theme_type,
    &vkr_scene_scatter_area_type,
};

/* Types registered at startup by modules outside the renderer. */
static const VkrTypeDesc *s_registered_types[VKR_SCENE_REGISTERED_TYPE_MAX];
static uint32_t s_registered_type_count;

const VkrTypeDesc *vkr_scene_world_type(uint32_t index) {
  if (index < ArrayCount(s_world_types)) {
    return s_world_types[index];
  }
  index -= ArrayCount(s_world_types);
  return index < s_registered_type_count ? s_registered_types[index] : NULL;
}

const VkrTypeDesc *vkr_scene_world_type_named(String8 name) {
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)); ++i) {
    const uint64_t length = strlen(type->name);
    if (length == name.length &&
        MemCompare(type->name, name.str, length) == 0) {
      return type;
    }
  }
  return NULL;
}

const VkrTypeDesc *vkr_scene_registered_type(uint32_t index) {
  return index < s_registered_type_count ? s_registered_types[index] : NULL;
}

bool8_t vkr_scene_world_type_registered(const VkrTypeDesc *type) {
  for (uint32_t i = 0; i < s_registered_type_count; ++i) {
    if (s_registered_types[i] == type) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_scene_register_world_type(const VkrTypeDesc *type) {
  if (!type || !type->name || !type->name[0] || !type->size ||
      type->size > VKR_TYPE_VALUE_MAX || !type->align ||
      s_registered_type_count == VKR_SCENE_REGISTERED_TYPE_MAX ||
      vkr_scene_world_type_named(string8_create_from_cstr(
          (const uint8_t *)type->name, strlen(type->name)))) {
    return false_v;
  }
  s_registered_types[s_registered_type_count++] = type;
  return true_v;
}

bool8_t vkr_scene_world_type_live(const VkrTypeDesc *type) {
  /* Registered types have no load-baked products; they apply when added. */
  if (vkr_scene_world_type_registered(type)) {
    return true_v;
  }
  return type == &vkr_scene_atmosphere_type || type == &vkr_scene_clouds_type ||
         type == &vkr_scene_fog_type || type == &vkr_scene_froxel_fog_type ||
         type == &vkr_scene_fog_box_type ||
         type == &vkr_scene_post_process_type ||
         type == &vkr_scene_physics_settings_type ||
         type == &vkr_scene_animation_settings_type ||
         type == &vkr_scene_time_of_day_type || type == &vkr_scene_shape_type ||
         type == &vkr_scene_text_type || type == &vkr_scene_player_start_type ||
         type == &vkr_scene_decal_type || type == &vkr_scene_look_volume_type ||
         type == &vkr_scene_material_override_type ||
         type == &vkr_scene_surface_theme_type ||
         type == &vkr_scene_brush_type || type == &vkr_scene_brush_face_type ||
         type == &vkr_scene_trigger_type || type == &vkr_scene_relay_type ||
         type == &vkr_scene_button_type || type == &vkr_scene_timer_type ||
         type == &vkr_scene_counter_type || type == &vkr_scene_mover_type ||
         type == &vkr_scene_io_connection_type ||
         type == &vkr_scene_terrain_type || type == &vkr_scene_spline_type ||
         type == &vkr_scene_spline_point_type ||
         type == &vkr_scene_spline_mesh_type ||
         type == &vkr_scene_scatter_type ||
         type == &vkr_scene_scatter_area_type ||
         type == &vkr_scene_world_partition_type ||
         type == &vkr_scene_always_loaded_type ||
         type == &vkr_scene_free_placement_type ||
         type == &vkr_scene_blockout_type || type == &vkr_scene_tags_type;
}

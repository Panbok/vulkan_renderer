#include "renderer/systems/vkr_scene_types.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
  return true_v;
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
};

static bool8_t rectangle_light_validate(const void *value, char *error,
                                        uint32_t capacity) {
  const SceneRectangleLight *light = value;
  if (!(light->size.x > 0.0f) || !(light->size.y > 0.0f)) {
    snprintf(error, capacity, "Rectangle size must be positive.");
    return false_v;
  }
  return true_v;
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
     .tooltip = "RGBA; used when no material is named",
     .offset = TYPE_OFFSET(SceneShapeSettings, color),
     .kind = VKR_PROPERTY_VEC4,
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

static const VkrPropertyDesc s_text_properties[] = {
    {.name = "content",
     .label = "Text",
     .offset = TYPE_OFFSET(SceneTextSettings, content),
     .capacity = sizeof(((SceneTextSettings *)0)->content),
     .kind = VKR_PROPERTY_STRING},
    {.name = "font_size",
     .label = "Font size",
     .tooltip = "Text texture pixels per em",
     .unit = "px",
     .offset = TYPE_OFFSET(SceneTextSettings, font_size),
     .kind = VKR_PROPERTY_F32,
     .min = 4.0f,
     .max = 512.0f,
     .step = 0.5f},
    {.name = "color",
     .label = "Color",
     .tooltip = "RGBA",
     .offset = TYPE_OFFSET(SceneTextSettings, color),
     .kind = VKR_PROPERTY_VEC4,
     .min = 0.0f,
     .max = 1.0f,
     .step = 0.005f},
};

static void text_defaults(void *value) {
  SceneTextSettings *text = value;
  MemZero(text, sizeof(*text));
  snprintf(text->content, sizeof(text->content), "Text");
  text->font_size = 32.0f;
  text->color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};
}

const VkrTypeDesc vkr_scene_text_type = {
    .name = "text",
    .label = "Text",
    .category = "Geometry",
    .properties = s_text_properties,
    .property_count = ArrayCount(s_text_properties),
    .size = sizeof(SceneTextSettings),
    .align = _Alignof(SceneTextSettings),
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
    &vkr_scene_shape_type,
    &vkr_scene_text_type,
    &vkr_scene_animation_type,
    &vkr_scene_player_start_type,
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
         type == &vkr_scene_shape_type || type == &vkr_scene_text_type ||
         type == &vkr_scene_player_start_type;
}

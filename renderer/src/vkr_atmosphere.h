#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "vkr_clouds.h"

#define VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH 256u
#define VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT 64u
#define VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE 32u
#define VKR_ATMOSPHERE_SOURCE_SIZE 256u

/** Camera-dependent sky resources rebuilt every frame. The aerial-perspective
 * volume spans the view frustum with squared slice spacing. */
#define VKR_ATMOSPHERE_SKY_VIEW_WIDTH 192u
#define VKR_ATMOSPHERE_SKY_VIEW_HEIGHT 108u
#define VKR_ATMOSPHERE_AERIAL_SIZE 32u
#define VKR_ATMOSPHERE_AERIAL_KM_PER_SLICE 4.0f

/** Authorable world scale range, in metres per world unit. */
#define VKR_ATMOSPHERE_METRES_PER_UNIT_MIN 0.001f
#define VKR_ATMOSPHERE_METRES_PER_UNIT_MAX 1000.0f

/** Midpoint samples of a transmittance integral and the optical-depth clamp,
 * shared with atmosphere_kernel.slangh. */
#define VKR_ATMOSPHERE_TRANSMITTANCE_SAMPLES 40u
#define VKR_ATMOSPHERE_MAX_OPTICAL_DEPTH 80.0f

/** Visible sun glow: veiling radiance `sun_glow * E / theta^2` around the
 * disc, with the attenuated irradiance E and theta in degrees, after the
 * Stiles-Holladay glare formula whose human-eye coefficient is about 10,
 * faded to zero 25 degrees out. It only changes the drawn sky, never
 * lighting. */
#define VKR_ATMOSPHERE_SUN_GLOW_DEFAULT 2.0f
#define VKR_ATMOSPHERE_SUN_GLOW_MAX 100.0f

/** Earth atmosphere baked at a fixed observer altitude, in metres.
 * Density multipliers are in [0,100], solar diameter in [1e-16,5] degrees,
 * altitude in [0,100000] metres, Mie anisotropy in [-.95,.95] and the sun
 * glow in [0,100]. The camera-dependent sky places the camera at the observer
 * altitude plus its world height times `metres_per_world_unit`. The sun
 * direction, irradiance, disc diameter and glow form the sun; the rest is the
 * medium, which the revision bake alone depends on. */
typedef struct VkrAtmosphereSettings {
  bool8_t enabled;
  Vec3 sun_direction;
  Vec3 solar_irradiance;
  Vec3 ground_albedo;
  float32_t observer_altitude_m;
  float32_t sun_angular_diameter_degrees;
  float32_t sun_glow;
  float32_t rayleigh_density_scale;
  float32_t mie_density_scale;
  float32_t ozone_density_scale;
  float32_t mie_anisotropy;
  float32_t metres_per_world_unit;
} VkrAtmosphereSettings;

/** Shared bake constants. Lengths are kilometres; extinction is per kilometre.
 */
typedef struct VkrAtmosphereGpuParams {
  Vec4 planet;
  Vec4 rayleigh;
  Vec4 mie_scattering;
  Vec4 mie_extinction;
  Vec4 ozone;
  Vec4 ground;
  Vec4 sun;
  Vec4 solar;
} VkrAtmosphereGpuParams;

_Static_assert(sizeof(VkrAtmosphereGpuParams) == 128u,
               "Atmosphere parameter ABI size drift");

/** Deferred background selected by the frame's sky payload. */
typedef enum VkrSkyMode {
  VKR_SKY_MODE_NONE = 0,
  VKR_SKY_MODE_CONSTANT = 1,
  VKR_SKY_MODE_ATMOSPHERE = 2,
} VkrSkyMode;

/** Per-frame camera-dependent sky record owned by frame-slot storage.
 * `atmosphere.planet.z` holds the camera altitude in kilometres and
 * `atmosphere.mie_extinction.w` the sun glow, which the bake ignores. The
 * matrices are the unjittered packet view-projection and its inverse; both
 * native backends use them only to map world positions to aerial-perspective
 * froxels, so the raster clip convention does not matter. `camera_position.w`
 * is kilometres per world unit. `aerial` holds kilometres per slice, the
 * slice count, its reciprocal, and 1 when aerial perspective applies.
 * `clouds` is zero unless the cloud layer renders this frame. */
typedef struct VkrSkyGpuParams {
  VkrAtmosphereGpuParams atmosphere;
  Mat4 view_projection;
  Mat4 inverse_view_projection;
  Vec4 camera_position;
  Vec4 aerial;
  VkrCloudGpuParams clouds;
} VkrSkyGpuParams;

_Static_assert(sizeof(VkrSkyGpuParams) == 352u, "Sky parameter ABI size drift");

typedef enum VkrAtmosphereBakeStatus {
  VKR_ATMOSPHERE_BAKE_PENDING = 0,
  VKR_ATMOSPHERE_BAKE_READY,
  VKR_ATMOSPHERE_BAKE_FAILED,
} VkrAtmosphereBakeStatus;

/** Authorable sun colour-temperature range, in kelvin. */
#define VKR_ATMOSPHERE_SUN_TEMPERATURE_MIN_K 1000.0f
#define VKR_ATMOSPHERE_SUN_TEMPERATURE_MAX_K 40000.0f

/** Optional sun authoring that replaces a raw top-of-atmosphere
 * `solar_irradiance`. Illuminance is scene-linear luminance, not lux. */
typedef struct VkrAtmosphereSunAuthoring {
  bool8_t has_temperature;
  float32_t temperature_kelvin;
  bool8_t has_illuminance;
  float32_t illuminance;
} VkrAtmosphereSunAuthoring;

VkrAtmosphereSettings vkr_atmosphere_settings_defaults(void);
bool8_t vkr_atmosphere_settings_valid(const VkrAtmosphereSettings *settings);

/** Linear Rec.709 colour of a blackbody at `kelvin`, clamped to the gamut and
 * normalized to unit luminance. `kelvin` lies in the authorable range. */
Vec3 vkr_atmosphere_blackbody_rgb(float32_t kelvin);

/** Resolves temperature and illuminance authoring into `solar_irradiance`.
 * A missing temperature keeps the default colour; a missing illuminance keeps
 * the default luminance. Returns false for values outside their domains. */
bool8_t
vkr_atmosphere_apply_sun_authoring(VkrAtmosphereSettings *settings,
                                   const VkrAtmosphereSunAuthoring *authoring);

/** Returns `medium` lit by the sun of `sun`: direction, irradiance, disc and
 * glow. The frame's sky pairs the published medium, which its lookups baked,
 * with the scene's current sun. */
VkrAtmosphereSettings
vkr_atmosphere_with_sun(const VkrAtmosphereSettings *medium,
                        const VkrAtmosphereSettings *sun);

/** Makes a directional light the sun. `light_direction` points along incoming
 * light and `irradiance`, the light's colour times intensity, becomes the
 * top-of-atmosphere irradiance. A diameter in (0, 5] degrees replaces the
 * visible disc; zero, a hard-shadow light, keeps the authored disc. Returns
 * false and leaves `settings` unchanged when the result is invalid. */
bool8_t vkr_atmosphere_apply_sun_light(VkrAtmosphereSettings *settings,
                                       Vec3 light_direction, Vec3 irradiance,
                                       float32_t sun_angular_diameter_degrees);
VkrAtmosphereGpuParams
vkr_atmosphere_prepare(const VkrAtmosphereSettings *settings);

/** Scattering and extinction per kilometre at a planet-centred position in
 * kilometres; scalar mirror of the shader kernel's medium. */
typedef struct VkrAtmosphereMedium {
  Vec3 rayleigh_scattering;
  Vec3 mie_scattering;
  Vec3 scattering;
  Vec3 extinction;
} VkrAtmosphereMedium;

VkrAtmosphereMedium vkr_atmosphere_medium(const VkrAtmosphereGpuParams *params,
                                          Vec3 position);

/** Distance along a unit ray from a point inside the atmosphere to its top,
 * or to the ground when `out_hits_ground` reports that the planet blocks it. */
float32_t vkr_atmosphere_segment_limit(const VkrAtmosphereGpuParams *params,
                                       Vec3 origin, Vec3 direction,
                                       bool8_t *out_hits_ground);

/** True when the planet blocks the sun from `position`. */
bool8_t vkr_atmosphere_sun_occluded(const VkrAtmosphereGpuParams *params,
                                    Vec3 position, Vec3 sun_direction);

/** Transmittance from `position` to the top of the atmosphere along a unit
 * direction, the integral every transmittance-lookup texel holds; zero when
 * the ray reaches the ground. */
Vec3 vkr_atmosphere_transmittance(const VkrAtmosphereGpuParams *params,
                                  Vec3 position, Vec3 direction);

/** The direct light: top-of-atmosphere irradiance attenuated to the observer
 * altitude, or zero below the horizon. */
Vec3 vkr_atmosphere_observer_irradiance(const VkrAtmosphereGpuParams *params);

/** Prepares the camera-dependent sky from validated enabled settings. The
 * camera altitude is clamped to the supported observer-altitude domain.
 * Clouds render only where aerial perspective applies. */
VkrSkyGpuParams
vkr_atmosphere_prepare_sky(const VkrAtmosphereSettings *settings,
                           const VkrCloudSettings *clouds,
                           Vec2 cloud_wind_offset_m, Vec3 camera_position,
                           Mat4 view_projection, bool8_t aerial_perspective);

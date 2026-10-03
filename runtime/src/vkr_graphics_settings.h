#pragma once

#include "containers/str.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_type_desc.h"
#include "vkr_renderer.h"

/* Machine-local quality gates and presentation preferences. Art direction
 * (exposure, grading, bloom and motion blur strength) is the scene's
 * post_process component (ADR-076). */
typedef struct VkrGraphicsSettings {
  bool8_t vsync, hdr, temporal_upscaling, dynamic_resolution, anti_aliasing;
  float32_t render_scale;
  uint32_t frame_limit;
  /* 0 off, 1 balanced, 2 high, 3 ultra: ten shadowed local lights on Vulkan,
   * High on Metal. */
  uint32_t shadow_quality;
  bool8_t soft_shadows, local_shadows, ambient_occlusion;
  /* Screen-space contact shadows for the nearest local lights, on either
   * backend; the Epic preset enables them. */
  bool8_t contact_shadows;
  /* Largest loaded texture extent: 0 is 1024, 1 is 2048, 2 is full
   * resolution. Metal defaults to 2048 for the unified-memory floor
   * (ADR-083); a change applies at the next start. */
  uint32_t texture_resolution;
  bool8_t screen_space_reflections, screen_space_gi, reflection_probes;
  bool8_t subsurface_scattering, fog, volumetric_fog;
  bool8_t bloom, depth_of_field, motion_blur;
  /* Captured mouse look turns down for upward motion, as flight controls do. */
  bool8_t invert_mouse_y;
} VkrGraphicsSettings;

typedef struct VkrGraphicsSettingsState {
  VkrGraphicsSettings settings;
  bool8_t restart_required;
  bool8_t temporal_upscaling_available;
  bool8_t dynamic_resolution_available;
  /* Whether render scale applies without temporal upscaling; otherwise the
     Scene renders at unit scale until temporal upscaling is enabled. */
  bool8_t spatial_render_scale_available;
  String8 temporal_upscaling_name;
  String8 message;
} VkrGraphicsSettingsState;

/* Borrowed during UI build, consumed at the runtime update boundary. */
typedef struct VkrGraphicsSettingsRequest {
  VkrGraphicsSettings settings;
  bool8_t apply;
  bool8_t reset_defaults;
} VkrGraphicsSettingsRequest;

/* Scalability presets: each sets every quality gate (Quality, Lighting and
 * Effects groups) and leaves display settings and render scale alone. */
typedef enum VkrGraphicsPreset {
  VKR_GRAPHICS_PRESET_LOW = 0,
  VKR_GRAPHICS_PRESET_MEDIUM,
  VKR_GRAPHICS_PRESET_HIGH,
  VKR_GRAPHICS_PRESET_EPIC,
  /* No preset matches: the gates were set one by one. */
  VKR_GRAPHICS_PRESET_CUSTOM,
} VkrGraphicsPreset;

const char *vkr_graphics_preset_name(VkrGraphicsPreset preset);
void vkr_graphics_settings_apply_preset(VkrGraphicsSettings *settings,
                                        VkrGraphicsPreset preset);
/* The preset whose quality gates `settings` match, else CUSTOM. */
VkrGraphicsPreset
vkr_graphics_settings_preset(const VkrGraphicsSettings *settings);

/** Machine-local graphics preferences. The state hook's context is a
 * `const VkrGraphicsSettingsState *` or NULL. */
extern const VkrTypeDesc vkr_graphics_settings_type;

VkrGraphicsSettings
vkr_graphics_settings_defaults(VkrRendererBackendType backend);
bool8_t vkr_graphics_settings_valid(const VkrGraphicsSettings *settings);
/* The texture load limit in texels that `settings` selects; zero is none. */
uint32_t vkr_graphics_settings_texture_max_dimension(
    const VkrGraphicsSettings *settings);
bool8_t
vkr_graphics_settings_restart_required(const VkrGraphicsSettings *requested,
                                       const VkrGraphicsSettings *started);
/* The Scene render scale `settings` selects under `state`'s capabilities: unit
 * scale where the backend cannot scale without temporal upscaling. */
float32_t
vkr_graphics_settings_render_scale(const VkrGraphicsSettingsState *state,
                                   const VkrGraphicsSettings *settings);
/* Missing files preserve defaults. Invalid files leave the destination intact.
 */
bool8_t vkr_graphics_settings_load(const char *path,
                                   VkrGraphicsSettings *settings);
bool8_t vkr_graphics_settings_save(const char *path,
                                   const VkrGraphicsSettings *settings);
bool8_t vkr_graphics_settings_read_json(String8 json,
                                        VkrGraphicsSettings *settings);
bool8_t vkr_graphics_settings_write_json(VkrJsonWriter *writer,
                                         const VkrGraphicsSettings *settings);

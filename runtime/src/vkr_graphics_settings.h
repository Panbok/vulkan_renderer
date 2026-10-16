#pragma once

#include "containers/str.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_type_desc.h"
#include "vkr_frame_input.h"
#include "vkr_renderer.h"

/* Machine-local quality gates and presentation preferences. Art direction
 * (exposure, grading, bloom and motion blur strength) is the scene's
 * post_process component (ADR-076). */
typedef struct VkrGraphicsSettings {
  bool8_t vsync, hdr, dynamic_resolution, anti_aliasing;
  /* VkrAntialiasingMode of the desktop pipeline's temporal anti-aliasing:
   * FSR 3.1 native anti-aliasing (the default) or SMAA T2x. It applies only
   * while anti_aliasing is on; the tiled pipeline ignores it (ADR-087). */
  uint32_t antialiasing;
  /* Render the display's physical pixels; off renders one pixel per point,
   * a quarter of the pixels on a 2x Retina display. macOS only. */
  bool8_t high_dpi;
  float32_t render_scale;
  /* VkrTiledUpscaler of the tiled pipeline: MetalFX (the default) or the
   * spatial upscale. It takes effect at the next start; the desktop pipeline
   * ignores it. */
  uint32_t upscaler;
  uint32_t frame_limit;
  /* The frame rate dynamic resolution holds: the controller's GPU budget is
   * VKR_GRAPHICS_DYNAMIC_RESOLUTION_BUDGET_SHARE of its frame time. */
  uint32_t dynamic_resolution_target_fps;
  /* 0 off, 1 balanced, 2 high, 3 ultra: ten shadowed local lights on Vulkan,
   * High on Metal. */
  uint32_t shadow_quality;
  bool8_t soft_shadows, local_shadows, ambient_occlusion;
  /* Screen-space contact shadows: for the nearest local lights on the
   * desktop pipeline and for the sun on the tiled pipeline; the Epic preset
   * enables them. */
  bool8_t contact_shadows;
  /* Exponential variance moments filter the two farthest cascades instead of
   * PCF on the desktop pipeline; the High and Epic presets enable them. */
  bool8_t filtered_far_shadows;
  /* Largest loaded texture extent: 0 is 1024, 1 is 2048, 2 is full
   * resolution. Metal defaults to 2048 for the unified-memory floor
   * (ADR-083). Scene material textures reload when it changes; other
   * textures take it at the next start. */
  uint32_t texture_resolution;
  /* How far objects draw: 0 near, 1 medium, 2 far, 3 epic scale each
   * object's draw distance by vkr_graphics_settings_draw_distance_scale. */
  uint32_t view_distance;
  bool8_t reflection_probes;
  bool8_t subsurface_scattering, fog, volumetric_fog;
  bool8_t bloom, depth_of_field, motion_blur;
  /* Captured mouse look turns down for upward motion, as flight controls do. */
  bool8_t invert_mouse_y;
} VkrGraphicsSettings;

typedef struct VkrGraphicsSettingsState {
  VkrGraphicsSettings settings;
  bool8_t restart_required;
  bool8_t dynamic_resolution_available;
  /* The running renderer's class (ADR-087); the tiled pipeline leaves the
     desktop pipeline's effects out. */
  VkrGraphicsPipelineClass graphics_pipeline;
  /* Whether render scale applies; otherwise the Scene renders at unit
     scale. */
  bool8_t spatial_render_scale_available;
  /* Whether the platform window can turn high-DPI rendering off. */
  bool8_t high_dpi_available;
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
/* The multiplier of objects' draw distances that `settings` selects. */
float32_t
vkr_graphics_settings_draw_distance_scale(const VkrGraphicsSettings *settings);
/* A frame that runs past its interval waits for the next one, so dynamic
   resolution keeps this share of the target frame time as headroom, as
   adaptive-quality controllers for VR do. */
#define VKR_GRAPHICS_DYNAMIC_RESOLUTION_BUDGET_SHARE 0.9f
/* The dynamic-resolution GPU budget in milliseconds that `settings` selects:
   VKR_GRAPHICS_DYNAMIC_RESOLUTION_BUDGET_SHARE of the target frame time. */
float32_t vkr_graphics_settings_dynamic_resolution_budget_ms(
    const VkrGraphicsSettings *settings);
/* The texture load limit in texels that `settings` selects; zero is none. */
uint32_t vkr_graphics_settings_texture_max_dimension(
    const VkrGraphicsSettings *settings);
bool8_t
vkr_graphics_settings_restart_required(const VkrGraphicsSettings *requested,
                                       const VkrGraphicsSettings *started);
/* Copies the settings that take effect only at start from `running` into
 * `settings`, so a project's stored values cannot override the machine's. */
void vkr_graphics_settings_keep_restart(VkrGraphicsSettings *settings,
                                        const VkrGraphicsSettings *running);
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

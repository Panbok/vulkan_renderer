#pragma once

#include "containers/str.h"
#include "core/vkr_json_writer.h"
#include "vkr_renderer.h"

/* Player preferences. Scene and material authoring values remain scene-owned.
 */
typedef struct VkrGraphicsSettings {
  bool8_t vsync, hdr, temporal_upscaling, dynamic_resolution, anti_aliasing;
  float32_t render_scale;
  uint32_t frame_limit;
  uint32_t shadow_quality; /* 0 off, 1 balanced, 2 high. */
  bool8_t soft_shadows, local_shadows, ambient_occlusion;
  bool8_t screen_space_reflections, screen_space_gi, reflection_probes;
  bool8_t subsurface_scattering, fog, volumetric_fog;
  bool8_t bloom, depth_of_field, motion_blur;
  float32_t brightness, contrast, saturation, temperature, tint, sharpness;
  float32_t bloom_intensity, motion_blur_amount;
} VkrGraphicsSettings;

typedef struct VkrGraphicsSettingsState {
  VkrGraphicsSettings settings;
  bool8_t restart_required;
  bool8_t temporal_upscaling_available;
  bool8_t dynamic_resolution_available;
  String8 temporal_upscaling_name;
  String8 message;
} VkrGraphicsSettingsState;

/* Borrowed during UI build, consumed at the runtime update boundary. */
typedef struct VkrGraphicsSettingsRequest {
  VkrGraphicsSettings settings;
  bool8_t apply;
  bool8_t reset_defaults;
} VkrGraphicsSettingsRequest;

VkrGraphicsSettings
vkr_graphics_settings_defaults(VkrRendererBackendType backend);
bool8_t vkr_graphics_settings_valid(const VkrGraphicsSettings *settings);
bool8_t
vkr_graphics_settings_restart_required(const VkrGraphicsSettings *requested,
                                       const VkrGraphicsSettings *started);
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

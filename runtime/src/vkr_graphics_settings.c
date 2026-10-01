#include "vkr_graphics_settings.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const s_shadow_quality_names[] = {"off", "balanced", "high",
                                                     "ultra", NULL};

#define GRAPHICS_OFFSET(field) (uint32_t)offsetof(VkrGraphicsSettings, field)

static const VkrPropertyDesc s_graphics_properties[] = {
    {.name = "vsync",
     .label = "Vertical sync",
     .tooltip = "Match presentation to the display refresh rate",
     .group = "Display",
     .offset = GRAPHICS_OFFSET(vsync),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "hdr",
     .label = "High dynamic range",
     .tooltip = "Use the display's extended brightness range when available",
     .offset = GRAPHICS_OFFSET(hdr),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "frame_limit",
     .label = "Frame limit",
     .tooltip = "0 leaves the frame rate unrestricted",
     .unit = "FPS",
     .zero_label = "Unlimited",
     .offset = GRAPHICS_OFFSET(frame_limit),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 0.0f,
     .max = 240.0f,
     .step = 1.0f},
    {.name = "temporal_upscaling",
     .label = "Temporal upscaling",
     .tooltip = "Reconstruct a higher-resolution image from temporal data",
     .offset = GRAPHICS_OFFSET(temporal_upscaling),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "dynamic_resolution",
     .label = "Dynamic resolution",
     .tooltip = "Adjust internal resolution to keep frame pacing stable",
     .offset = GRAPHICS_OFFSET(dynamic_resolution),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "render_scale",
     .label = "Render scale",
     .tooltip = "Lower values render fewer pixels",
     .unit = "%",
     .offset = GRAPHICS_OFFSET(render_scale),
     .kind = VKR_PROPERTY_F32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 1.0f / 3.0f,
     .max = 1.0f,
     .step = 0.5f,
     .display_scale = 100.0f},
    {.name = "anti_aliasing",
     .label = "Anti-aliasing",
     .tooltip = "Smooth visible edges; temporal upscaling requires it",
     .group = "Quality",
     .offset = GRAPHICS_OFFSET(anti_aliasing),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "shadow_quality",
     .label = "Shadows",
     .tooltip = "Shadow map resolution and filtering",
     .names = s_shadow_quality_names,
     .offset = GRAPHICS_OFFSET(shadow_quality),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 3.0f},
    {.name = "soft_shadows",
     .label = "Soft shadows",
     .tooltip = "Soften shadow edges",
     .offset = GRAPHICS_OFFSET(soft_shadows),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "local_shadows",
     .label = "Local light shadows",
     .tooltip = "Enable shadows from nearby lights",
     .offset = GRAPHICS_OFFSET(local_shadows),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "ambient_occlusion",
     .label = "Ambient occlusion",
     .tooltip = "Add contact shading where surfaces meet",
     .group = "Lighting",
     .offset = GRAPHICS_OFFSET(ambient_occlusion),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "screen_space_gi",
     .label = "Screen-space global illumination",
     .tooltip = "Add indirect light from visible surfaces",
     .offset = GRAPHICS_OFFSET(screen_space_gi),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "screen_space_reflections",
     .label = "Screen-space reflections",
     .tooltip = "Reflect visible surroundings on glossy surfaces",
     .offset = GRAPHICS_OFFSET(screen_space_reflections),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "reflection_probes",
     .label = "Reflection probes",
     .tooltip = "Use authored environment reflections",
     .offset = GRAPHICS_OFFSET(reflection_probes),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "subsurface_scattering",
     .label = "Subsurface scattering",
     .tooltip = "Soften light through skin, wax, and similar surfaces",
     .offset = GRAPHICS_OFFSET(subsurface_scattering),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "fog",
     .label = "Fog",
     .tooltip = "Add distance and height fog",
     .offset = GRAPHICS_OFFSET(fog),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "volumetric_fog",
     .label = "Volumetric fog",
     .tooltip = "Render light through fog",
     .offset = GRAPHICS_OFFSET(volumetric_fog),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "bloom",
     .label = "Bloom",
     .tooltip = "Glow around bright image details",
     .group = "Effects",
     .offset = GRAPHICS_OFFSET(bloom),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "depth_of_field",
     .label = "Depth of field",
     .tooltip = "Blur distant and near image regions",
     .offset = GRAPHICS_OFFSET(depth_of_field),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "motion_blur",
     .label = "Motion blur",
     .tooltip = "Blur fast movement",
     .offset = GRAPHICS_OFFSET(motion_blur),
     .kind = VKR_PROPERTY_BOOL},
};
#undef GRAPHICS_OFFSET

/* Grading and effect strengths moved to the post_process component; files
 * written before the move still load. */
static const char *const s_graphics_retired[] = {
    "brightness", "contrast",  "saturation",      "temperature",
    "tint",       "sharpness", "bloom_intensity", "motion_blur_amount",
    NULL};

static bool8_t graphics_validate(const void *value, char *error,
                                 uint32_t capacity) {
  const VkrGraphicsSettings *settings = value;
  if (settings->temporal_upscaling && !settings->anti_aliasing) {
    snprintf(error, capacity, "Temporal upscaling requires anti-aliasing");
    return false_v;
  }
  if (settings->dynamic_resolution && !settings->temporal_upscaling) {
    snprintf(error, capacity, "Dynamic resolution requires temporal upscaling");
    return false_v;
  }
  return true_v;
}

static void graphics_normalize(void *value) {
  VkrGraphicsSettings *settings = value;
  if (settings->temporal_upscaling) {
    settings->anti_aliasing = true_v;
  } else {
    settings->dynamic_resolution = false_v;
  }
}

/* Context is the runtime's VkrGraphicsSettingsState, or NULL. */
static VkrPropertyState graphics_state(const void *value, uint32_t property,
                                       const void *context) {
  const VkrGraphicsSettings *settings = value;
  const VkrGraphicsSettingsState *state = context;
  VkrPropertyState result = {0};
  const uint32_t offset = s_graphics_properties[property].offset;
  if (offset == offsetof(VkrGraphicsSettings, temporal_upscaling)) {
    if (state && !state->temporal_upscaling_available) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
    if (state && state->temporal_upscaling_name.length) {
      result.label = state->temporal_upscaling_name;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, dynamic_resolution)) {
    if (!settings->temporal_upscaling ||
        (state && !state->dynamic_resolution_available)) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, anti_aliasing)) {
    if (settings->temporal_upscaling) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, render_scale)) {
    if (!settings->temporal_upscaling && state &&
        !state->spatial_render_scale_available) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, soft_shadows) ||
             offset == offsetof(VkrGraphicsSettings, local_shadows)) {
    if (settings->shadow_quality == 0u) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, volumetric_fog)) {
    if (!settings->fog) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  }
  return result;
}

const VkrTypeDesc vkr_graphics_settings_type = {
    .name = "graphics_preferences",
    .label = "Graphics",
    .properties = s_graphics_properties,
    .property_count = ArrayCount(s_graphics_properties),
    .size = sizeof(VkrGraphicsSettings),
    .align = AlignOf(VkrGraphicsSettings),
    .version = 1u,
    .retired = s_graphics_retired,
    .validate = graphics_validate,
    .normalize = graphics_normalize,
    .state = graphics_state,
};

VkrGraphicsSettings
vkr_graphics_settings_defaults(VkrRendererBackendType backend) {
  return (VkrGraphicsSettings){
      .vsync = true_v,
      .hdr = false_v,
      .temporal_upscaling = true_v,
      .dynamic_resolution = backend == VKR_RENDERER_BACKEND_TYPE_METAL,
      .anti_aliasing = true_v,
      /* With dynamic resolution the scale caps the controller, so Metal
         starts uncapped and lets it choose. */
      .render_scale =
          backend == VKR_RENDERER_BACKEND_TYPE_METAL ? 1.0f : 2.0f / 3.0f,
      .shadow_quality = 2,
      .soft_shadows = true_v,
      .local_shadows = true_v,
      .ambient_occlusion = true_v,
      .screen_space_reflections = true_v,
      .reflection_probes = true_v,
      .subsurface_scattering = true_v,
      .fog = true_v,
      .volumetric_fog = true_v,
      .bloom = true_v,
  };
}

static const char *const s_graphics_preset_names[] = {"Low", "Medium", "High",
                                                      "Epic", "Custom"};

const char *vkr_graphics_preset_name(VkrGraphicsPreset preset) {
  return s_graphics_preset_names[Min((uint32_t)preset,
                                     (uint32_t)VKR_GRAPHICS_PRESET_CUSTOM)];
}

/* Quality gates of one preset over `base`, whose display settings stay. */
static VkrGraphicsSettings
graphics_preset_gates(const VkrGraphicsSettings *base,
                      VkrGraphicsPreset preset) {
  const bool8_t medium = preset >= VKR_GRAPHICS_PRESET_MEDIUM;
  const bool8_t high = preset >= VKR_GRAPHICS_PRESET_HIGH;
  const bool8_t epic = preset >= VKR_GRAPHICS_PRESET_EPIC;
  VkrGraphicsSettings out = *base;
  out.anti_aliasing = true_v;
  out.shadow_quality = epic ? 3u : high ? 2u : 1u;
  out.soft_shadows = medium;
  out.local_shadows = medium;
  out.ambient_occlusion = medium;
  out.screen_space_gi = epic;
  out.screen_space_reflections = high;
  out.reflection_probes = true_v;
  out.subsurface_scattering = medium;
  out.fog = true_v;
  out.volumetric_fog = high;
  out.bloom = true_v;
  out.depth_of_field = epic;
  out.motion_blur = epic;
  return out;
}

void vkr_graphics_settings_apply_preset(VkrGraphicsSettings *settings,
                                        VkrGraphicsPreset preset) {
  if (settings && preset < VKR_GRAPHICS_PRESET_CUSTOM) {
    *settings = graphics_preset_gates(settings, preset);
  }
}

VkrGraphicsPreset
vkr_graphics_settings_preset(const VkrGraphicsSettings *settings) {
  for (uint32_t i = 0; i < VKR_GRAPHICS_PRESET_CUSTOM; ++i) {
    const VkrGraphicsSettings gates =
        graphics_preset_gates(settings, (VkrGraphicsPreset)i);
    /* Field by field: struct padding is unspecified. */
    if (gates.anti_aliasing == settings->anti_aliasing &&
        gates.shadow_quality == settings->shadow_quality &&
        gates.soft_shadows == settings->soft_shadows &&
        gates.local_shadows == settings->local_shadows &&
        gates.ambient_occlusion == settings->ambient_occlusion &&
        gates.screen_space_gi == settings->screen_space_gi &&
        gates.screen_space_reflections == settings->screen_space_reflections &&
        gates.reflection_probes == settings->reflection_probes &&
        gates.subsurface_scattering == settings->subsurface_scattering &&
        gates.fog == settings->fog &&
        gates.volumetric_fog == settings->volumetric_fog &&
        gates.bloom == settings->bloom &&
        gates.depth_of_field == settings->depth_of_field &&
        gates.motion_blur == settings->motion_blur) {
      return (VkrGraphicsPreset)i;
    }
  }
  return VKR_GRAPHICS_PRESET_CUSTOM;
}

bool8_t vkr_graphics_settings_valid(const VkrGraphicsSettings *settings) {
  return settings &&
         vkr_type_validate(&vkr_graphics_settings_type, settings, NULL, 0u);
}

bool8_t vkr_graphics_settings_restart_required(const VkrGraphicsSettings *a,
                                               const VkrGraphicsSettings *b) {
  return a->vsync != b->vsync || a->hdr != b->hdr ||
         a->temporal_upscaling != b->temporal_upscaling ||
         a->dynamic_resolution != b->dynamic_resolution ||
         a->render_scale != b->render_scale;
}

float32_t
vkr_graphics_settings_render_scale(const VkrGraphicsSettingsState *state,
                                   const VkrGraphicsSettings *settings) {
  if (!settings->temporal_upscaling && state &&
      !state->spatial_render_scale_available) {
    return 1.0f;
  }
  return settings->render_scale;
}

bool8_t vkr_graphics_settings_load(const char *path,
                                   VkrGraphicsSettings *settings) {
  if (!path || !path[0])
    return true_v;
  FILE *file = file_fopen(path, "rb");
  if (!file)
    return errno == ENOENT;
  uint8_t bytes[4096];
  const size_t length = fread(bytes, 1, sizeof(bytes), file);
  const bool8_t read_ok = !ferror(file) && length < sizeof(bytes);
  fclose(file);
  if (!read_ok)
    return false_v;
  return vkr_graphics_settings_read_json(string8_create(bytes, length),
                                         settings);
}

bool8_t vkr_graphics_settings_read_json(String8 json,
                                        VkrGraphicsSettings *settings) {
  if (!settings) {
    return false_v;
  }
  return vkr_type_read_json_document(json, &vkr_graphics_settings_type,
                                     settings, NULL, NULL, 0u);
}

bool8_t vkr_graphics_settings_write_json(VkrJsonWriter *writer,
                                         const VkrGraphicsSettings *settings) {
  return settings &&
         vkr_type_write_json(writer, &vkr_graphics_settings_type, settings);
}

bool8_t vkr_graphics_settings_save(const char *path,
                                   const VkrGraphicsSettings *settings) {
  if (!settings || !vkr_graphics_settings_valid(settings)) {
    return false_v;
  }
  if (!path || !path[0]) {
    return true_v;
  }
  VkrJsonFileWriter writer = {0};
  if (!vkr_json_file_writer_begin(
          &writer,
          string8_create_from_cstr((const uint8_t *)path, strlen(path)))) {
    return false_v;
  }
  if (!vkr_graphics_settings_write_json(&writer.writer, settings) ||
      !vkr_json_file_writer_commit(&writer)) {
    vkr_json_file_writer_abort(&writer);
    return false_v;
  }
  return true_v;
}

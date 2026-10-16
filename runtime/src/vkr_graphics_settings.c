#include "vkr_graphics_settings.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include <stddef.h>

static const char *const s_shadow_quality_names[] = {"off", "balanced", "high",
                                                     "ultra", NULL};
static const char *const s_texture_resolution_names[] = {"1024", "2048", "full",
                                                         NULL};
/* Texel limits of `texture_resolution`; zero loads every mip. */
static const uint32_t s_texture_resolution_limits[] = {1024u, 2048u, 0u};
static const char *const s_view_distance_names[] = {"near", "medium", "far",
                                                    "epic", NULL};
/* Draw distance multipliers of `view_distance`. */
static const float32_t s_view_distance_scales[] = {0.5f, 0.75f, 1.0f, 1.5f};
/* Indexed by VkrAntialiasingMode. */
static const char *const s_antialiasing_names[] = {"fsr", "smaa_t2x", NULL};
static const char *const s_antialiasing_labels[] = {"FSR 3.1", "SMAA T2x",
                                                    NULL};
_Static_assert(ArrayCount(s_antialiasing_names) - 1u ==
                       VKR_ANTIALIASING_MODE_COUNT &&
                   ArrayCount(s_antialiasing_labels) - 1u ==
                       VKR_ANTIALIASING_MODE_COUNT,
               "Name every anti-aliasing mode");

/* Indexed by VkrTiledUpscaler. */
static const char *const s_upscaler_names[] = {"metalfx", "spatial", NULL};
static const char *const s_upscaler_labels[] = {"MetalFX", "Spatial", NULL};
_Static_assert(ArrayCount(s_upscaler_names) - 1u == VKR_TILED_UPSCALER_COUNT &&
                   ArrayCount(s_upscaler_labels) - 1u ==
                       VKR_TILED_UPSCALER_COUNT,
               "Name every tiled upscaler");

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
    {.name = "high_dpi",
     .label = "High-DPI rendering",
     .tooltip = "Render every physical pixel of a Retina display; off renders "
                "one pixel per point, a quarter of the pixels at 2x, with a "
                "softer Scene and UI",
     .offset = GRAPHICS_OFFSET(high_dpi),
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
    {.name = "invert_mouse_y",
     .label = "Invert mouse Y",
     .tooltip = "Moving the mouse up looks down, in the Scene camera and in "
                "gameplay",
     .offset = GRAPHICS_OFFSET(invert_mouse_y),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "dynamic_resolution",
     .label = "Dynamic resolution",
     .tooltip = "Adjust internal resolution to keep frame pacing stable",
     .offset = GRAPHICS_OFFSET(dynamic_resolution),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "dynamic_resolution_target_fps",
     .label = "Target frame rate",
     .tooltip = "Dynamic resolution lowers the render scale until the GPU "
                "keeps this frame rate, with headroom",
     .unit = "FPS",
     .offset = GRAPHICS_OFFSET(dynamic_resolution_target_fps),
     .kind = VKR_PROPERTY_U32,
     .flags = VKR_PROPERTY_FLAG_SLIDER,
     .min = 30.0f,
     .max = 240.0f,
     .step = 1.0f},
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
    {.name = "upscaler",
     .label = "Upscaler",
     .tooltip = "MetalFX rebuilds full-resolution detail from many jittered "
                "frames; Spatial stretches each frame and smooths edges with "
                "four samples. Applies after restart",
     .names = s_upscaler_names,
     .labels = s_upscaler_labels,
     .offset = GRAPHICS_OFFSET(upscaler),
     .kind = VKR_PROPERTY_ENUM},
    {.name = "anti_aliasing",
     .label = "Anti-aliasing",
     .tooltip = "Smooth visible edges",
     .group = "Quality",
     .offset = GRAPHICS_OFFSET(anti_aliasing),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "anti_aliasing_method",
     .label = "Anti-aliasing method",
     .tooltip = "FSR 3.1 rebuilds edges and fine detail from many frames; "
                "SMAA T2x costs less and blends two frames",
     .names = s_antialiasing_names,
     .labels = s_antialiasing_labels,
     .offset = GRAPHICS_OFFSET(antialiasing),
     .kind = VKR_PROPERTY_ENUM},
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
    {.name = "filtered_far_shadows",
     .label = "Filtered far shadows",
     .tooltip = "Smooth distant sun shadows with filtered shadow maps",
     .offset = GRAPHICS_OFFSET(filtered_far_shadows),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "local_shadows",
     .label = "Local light shadows",
     .tooltip = "Enable shadows from nearby lights",
     .offset = GRAPHICS_OFFSET(local_shadows),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "contact_shadows",
     .label = "Contact shadows",
     .tooltip = "Add fine shadows where objects meet surfaces",
     .offset = GRAPHICS_OFFSET(contact_shadows),
     .kind = VKR_PROPERTY_BOOL},
    {.name = "texture_resolution",
     .label = "Texture resolution",
     .tooltip = "Largest texture size to load; lower values use less memory. "
                "Scene materials reload at once; other textures change after "
                "restart",
     .names = s_texture_resolution_names,
     .offset = GRAPHICS_OFFSET(texture_resolution),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 2.0f},
    {.name = "view_distance",
     .label = "View distance",
     .tooltip = "How far away objects keep drawing; nearer settings draw "
                "fewer distant objects",
     .names = s_view_distance_names,
     .offset = GRAPHICS_OFFSET(view_distance),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 3.0f},
    {.name = "ambient_occlusion",
     .label = "Ambient occlusion",
     .tooltip = "Add contact shading where surfaces meet",
     .group = "Lighting",
     .offset = GRAPHICS_OFFSET(ambient_occlusion),
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

/* Grading and effect strengths moved to the post_process component, and the
 * pipeline class follows the backend (ADR-087); files written before still
 * load. */
static const char *const s_graphics_retired[] = {"brightness",
                                                 "contrast",
                                                 "saturation",
                                                 "temperature",
                                                 "tint",
                                                 "sharpness",
                                                 "bloom_intensity",
                                                 "motion_blur_amount",
                                                 "tiled_pipeline",
                                                 "temporal_upscaling",
                                                 "screen_space_gi",
                                                 "screen_space_reflections",
                                                 NULL};

/* Context is the runtime's VkrGraphicsSettingsState, or NULL. */
static VkrPropertyState graphics_state(const void *value, uint32_t property,
                                       const void *context) {
  const VkrGraphicsSettings *settings = value;
  const VkrGraphicsSettingsState *state = context;
  VkrPropertyState result = {0};
  const uint32_t offset = s_graphics_properties[property].offset;
  /* Effects the tiled pipeline does not draw (ADR-087); its four-sample
     edges need no anti-aliasing setting. */
  const bool8_t tiled_unused =
      offset == offsetof(VkrGraphicsSettings, anti_aliasing) ||
      offset == offsetof(VkrGraphicsSettings, antialiasing) ||
      offset == offsetof(VkrGraphicsSettings, filtered_far_shadows) ||
      offset == offsetof(VkrGraphicsSettings, ambient_occlusion) ||
      offset == offsetof(VkrGraphicsSettings, reflection_probes) ||
      offset == offsetof(VkrGraphicsSettings, subsurface_scattering) ||
      offset == offsetof(VkrGraphicsSettings, volumetric_fog) ||
      offset == offsetof(VkrGraphicsSettings, depth_of_field) ||
      offset == offsetof(VkrGraphicsSettings, motion_blur);
  if (state && state->graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED &&
      tiled_unused) {
    result.flags |= VKR_PROPERTY_STATE_DISABLED;
  }
  /* Only the tiled pipeline upscales its Scene (ADR-087). */
  if (offset == offsetof(VkrGraphicsSettings, upscaler) && state &&
      state->graphics_pipeline != VKR_GRAPHICS_PIPELINE_TILED) {
    result.flags |= VKR_PROPERTY_STATE_HIDDEN;
  }
  if (offset == offsetof(VkrGraphicsSettings, dynamic_resolution)) {
    if (state && !state->dynamic_resolution_available) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset ==
             offsetof(VkrGraphicsSettings, dynamic_resolution_target_fps)) {
    if (!settings->dynamic_resolution ||
        (state && !state->dynamic_resolution_available)) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, render_scale)) {
    if (state && !state->spatial_render_scale_available) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, high_dpi)) {
    if (state && !state->high_dpi_available) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, soft_shadows) ||
             offset == offsetof(VkrGraphicsSettings, filtered_far_shadows) ||
             offset == offsetof(VkrGraphicsSettings, local_shadows)) {
    if (settings->shadow_quality == 0u) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, contact_shadows)) {
    /* The desktop pipeline marches them for local lights, the tiled
       pipeline for the sun. */
    const bool8_t tiled =
        state && state->graphics_pipeline == VKR_GRAPHICS_PIPELINE_TILED;
    if (settings->shadow_quality == 0u ||
        (!tiled && !settings->local_shadows)) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, volumetric_fog)) {
    if (!settings->fog) {
      result.flags |= VKR_PROPERTY_STATE_DISABLED;
    }
  } else if (offset == offsetof(VkrGraphicsSettings, antialiasing)) {
    if (!settings->anti_aliasing) {
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
    .state = graphics_state,
};

VkrGraphicsSettings
vkr_graphics_settings_defaults(VkrRendererBackendType backend) {
  /* Metal's tiled pipeline renders 0.6 of each axis and MetalFX rebuilds the
     output, dynamic resolution lowering the scale to 0.5 for its frame
     budget (ADR-087); Vulkan's desktop pipeline renders at unit scale. */
  return (VkrGraphicsSettings){
      .vsync = true_v,
      .hdr = false_v,
      .high_dpi = true_v,
      .dynamic_resolution = backend == VKR_RENDERER_BACKEND_TYPE_METAL,
      .dynamic_resolution_target_fps = 60u,
      .anti_aliasing = true_v,
      .antialiasing = VKR_ANTIALIASING_MODE_FSR_NATIVE,
      /* With dynamic resolution the scale caps the controller. */
      .render_scale = backend == VKR_RENDERER_BACKEND_TYPE_METAL ? 0.6f : 1.0f,
      .upscaler = VKR_TILED_UPSCALER_METALFX,
      .shadow_quality = 2,
      .view_distance = 2,
      /* Full-resolution Bistro textures take 3.2 GB of a 16 GB Mac's 8 GiB
         managed budget (ADR-083). */
      .texture_resolution =
          backend == VKR_RENDERER_BACKEND_TYPE_METAL ? 1u : 2u,
      .soft_shadows = true_v,
      .filtered_far_shadows = true_v,
      .local_shadows = true_v,
      .ambient_occlusion = true_v,
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
  out.view_distance = epic ? 3u : high ? 2u : medium ? 1u : 0u;
  out.soft_shadows = medium;
  out.filtered_far_shadows = high;
  out.local_shadows = medium;
  out.contact_shadows = epic;
  out.ambient_occlusion = medium;
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
        gates.view_distance == settings->view_distance &&
        gates.soft_shadows == settings->soft_shadows &&
        gates.filtered_far_shadows == settings->filtered_far_shadows &&
        gates.local_shadows == settings->local_shadows &&
        gates.contact_shadows == settings->contact_shadows &&
        gates.ambient_occlusion == settings->ambient_occlusion &&
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
         a->dynamic_resolution != b->dynamic_resolution ||
         a->render_scale != b->render_scale || a->upscaler != b->upscaler;
}

void vkr_graphics_settings_keep_restart(VkrGraphicsSettings *settings,
                                        const VkrGraphicsSettings *running) {
  /* The fields vkr_graphics_settings_restart_required compares. */
  settings->vsync = running->vsync;
  settings->hdr = running->hdr;
  settings->dynamic_resolution = running->dynamic_resolution;
  settings->render_scale = running->render_scale;
  settings->upscaler = running->upscaler;
}

float32_t vkr_graphics_settings_dynamic_resolution_budget_ms(
    const VkrGraphicsSettings *settings) {
  const uint32_t fps = Max(settings->dynamic_resolution_target_fps, 1u);
  return VKR_GRAPHICS_DYNAMIC_RESOLUTION_BUDGET_SHARE * 1000.0f /
         (float32_t)fps;
}

float32_t
vkr_graphics_settings_draw_distance_scale(const VkrGraphicsSettings *settings) {
  const uint32_t index = Min(settings->view_distance,
                             (uint32_t)ArrayCount(s_view_distance_scales) - 1u);
  return s_view_distance_scales[index];
}

uint32_t vkr_graphics_settings_texture_max_dimension(
    const VkrGraphicsSettings *settings) {
  const uint32_t index =
      Min(settings->texture_resolution,
          (uint32_t)ArrayCount(s_texture_resolution_limits) - 1u);
  return s_texture_resolution_limits[index];
}

float32_t
vkr_graphics_settings_render_scale(const VkrGraphicsSettingsState *state,
                                   const VkrGraphicsSettings *settings) {
  if (state && !state->spatial_render_scale_available) {
    return 1.0f;
  }
  return settings->render_scale;
}

bool8_t vkr_graphics_settings_load(const char *path,
                                   VkrGraphicsSettings *settings) {
  if (!path || !path[0])
    return true_v;
  const FilePath file_path = file_path_view(path);
  FileHandle file = {0};
  const FileError open_error = file_open(
      &file_path, (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY}, &file);
  if (open_error != FILE_ERROR_NONE)
    return open_error == FILE_ERROR_NOT_FOUND;
  uint8_t bytes[4096];
  uint64_t length = 0u;
  const bool8_t read_ok =
      file_read_into(&file, bytes, sizeof(bytes), &length) == FILE_ERROR_NONE &&
      length < sizeof(bytes);
  file_close(&file);
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
          &writer, string8_create_from_cstr((const uint8_t *)path,
                                            string_length(path)))) {
    return false_v;
  }
  if (!vkr_graphics_settings_write_json(&writer.writer, settings) ||
      !vkr_json_file_writer_commit(&writer)) {
    vkr_json_file_writer_abort(&writer);
    return false_v;
  }
  return true_v;
}

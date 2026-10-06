#include "vkr_sample_runtime_config.h"

#include "containers/str.h"
#include "core/vkr_window.h"
#include "filesystem/vkr_vfs.h"
#include "platform/vkr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_PATH "assets/scenes/bistro.scene.json"

/* Rejected input prints to stderr because it is found before the standard
 * runtime creates the logger; the caller exits with code 2. Diagnostics after
 * runtime creation go through the logger. The other direct stderr writes are
 * the same kind of pre-logger bootstrap failure, or explicitly requested
 * diagnostic streams that must survive Release builds, where LOG_LEVEL
 * compiles informational logging out. */

/**
 * @brief Parses common truthy/falsy environment values.
 *
 * Unknown values keep the provided default to avoid brittle automation.
 */
vkr_internal bool8_t sample_env_flag(const char *name, bool8_t default_value) {
  if (!name || name[0] == '\0') {
    return default_value;
  }

  const char *value = getenv(name);
  if (!value || value[0] == '\0') {
    return default_value;
  }

  switch (value[0]) {
  case '1':
  case 'y':
  case 'Y':
  case 't':
  case 'T':
    return true_v;
  case '0':
  case 'n':
  case 'N':
  case 'f':
  case 'F':
    return false_v;
  default:
    return default_value;
  }
}

/* Unset yields zero. A value that is not a positive number also yields zero
 * and is returned through `out_rejected` for the caller's warning. */
vkr_internal float64_t sample_env_seconds(const char *name,
                                          const char **out_rejected) {
  *out_rejected = NULL;
  const char *value = getenv(name);
  if (!value || value[0] == '\0') {
    return 0.0;
  }

  char *end = NULL;
  const float64_t seconds = strtod(value, &end);
  if (end == value || !(seconds > 0.0)) {
    *out_rejected = value;
    return 0.0;
  }
  return seconds;
}

vkr_internal bool8_t sample_parse_arguments(int argc, char **argv,
                                            VkrSampleRuntimeOptions *options) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--gameplay") == 0) {
      options->gameplay_enabled = true_v;
    } else if (strcmp(argv[i], "--scene") == 0) {
      if (i + 1 >= argc || !argv[i + 1][0]) {
        fprintf(stderr, "--scene requires a scene JSON path\n");
        return false_v;
      }
      options->scene_path = argv[++i];
      options->scene_requested = true_v;
    } else if (strcmp(argv[i], "--metrics-json") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--metrics-json requires an output path\n");
        return false_v;
      }
      options->metrics_json_path = argv[++i];
    } else if (strcmp(argv[i], "--renderer") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        fprintf(stderr, "--renderer requires 'vulkan' or 'metal'\n");
        return false_v;
      }
      const char *renderer_name = argv[++i];
      if (strcmp(renderer_name, "vulkan") == 0) {
        options->renderer_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN;
      } else if (strcmp(renderer_name, "metal") == 0) {
        options->renderer_backend = VKR_RENDERER_BACKEND_TYPE_METAL;
      } else {
        fprintf(stderr, "unknown renderer '%s'\n", renderer_name);
        return false_v;
      }
    }
  }

  return true_v;
}

/* Project mode loads the fonts installed beside the executable. */
vkr_internal bool8_t sample_bootstrap_font_directory(char *path,
                                                     uint64_t capacity) {
  if (!vkr_platform_executable_path(path, (uint32_t)capacity)) {
    fprintf(stderr, "Cannot locate installed editor resources\n");
    return false_v;
  }

  char *separator = strrchr(path, '/');
  return separator &&
         vkr_string_copy_bounded(separator + 1,
                                 capacity - (uint64_t)(separator + 1 - path),
                                 "resources/editor/fonts");
}

/* Missing files keep defaults; an invalid file is reported and ignored.
 * Vulkan's desktop pipeline offers FSR 3.1 temporal upscaling; Metal's tiled
 * pipeline offers dynamic resolution of its spatial upscale (ADR-087). */
vkr_internal bool8_t
sample_load_graphics(const VkrSampleRuntimeConfig *runtime_config,
                     VkrSampleRuntimeOptions *options) {
  const bool8_t project_managed = runtime_config->project_managed;
  const char *graphics_path = project_managed
                                  ? runtime_config->graphics_settings_path
                                  : getenv("VKR_GRAPHICS_SETTINGS_PATH");
  if (project_managed && !graphics_path) {
    graphics_path = "";
  }
  if (!graphics_path) {
    graphics_path = runtime_config->graphics_settings_path;
  }
  /* The options keep this pointer for the process's lifetime. */
  static char default_path[VKR_SAMPLE_RUNTIME_PATH_CAPACITY];
  if (!graphics_path) {
    snprintf(default_path, sizeof(default_path),
             "%s.vkr-graphics-settings.json", vkr_content_root());
    graphics_path = default_path;
  }
  if (strlen(graphics_path) >= VKR_SAMPLE_RUNTIME_PATH_CAPACITY) {
    fprintf(stderr, "Graphics settings path is too long\n");
    return false_v;
  }

  const VkrRendererBackendType backend = options->renderer_backend;
  VkrGraphicsSettings settings = vkr_graphics_settings_defaults(backend);
  if (runtime_config->graphics_defaults.length &&
      !vkr_graphics_settings_read_json(runtime_config->graphics_defaults,
                                       &settings)) {
    fprintf(stderr, "Ignoring invalid default Graphics settings\n");
    settings = vkr_graphics_settings_defaults(backend);
  }
  options->graphics_loaded =
      vkr_graphics_settings_load(graphics_path, &settings);
  if (!options->graphics_loaded) {
    fprintf(stderr, "Ignoring invalid Graphics settings: %s\n", graphics_path);
  }

  const bool8_t temporal_available =
      backend == VKR_RENDERER_BACKEND_TYPE_VULKAN;
  const bool8_t dynamic_available = backend == VKR_RENDERER_BACKEND_TYPE_METAL;
  if (!temporal_available) {
    settings.temporal_upscaling = false_v;
  }
  if (!dynamic_available) {
    settings.dynamic_resolution = false_v;
  }
  const bool8_t high_dpi_available = vkr_window_high_dpi_switchable();
  if (!high_dpi_available) {
    settings.high_dpi = true_v;
  }

  options->graphics_path = graphics_path;
  options->graphics = (VkrGraphicsSettingsState){
      .settings = settings,
      .temporal_upscaling_available = temporal_available,
      .dynamic_resolution_available = dynamic_available,
      .graphics_pipeline = vkr_graphics_pipeline_for_backend(backend),
      .spatial_render_scale_available =
          backend == VKR_RENDERER_BACKEND_TYPE_METAL,
      .high_dpi_available = high_dpi_available,
      .temporal_upscaling_name =
          temporal_available ? string8_lit("FSR 3.1") : (String8){0},
  };
  return true_v;
}

VkrSampleRuntimeConfig vkr_sample_runtime_config_default(void) {
  return (VkrSampleRuntimeConfig){
      .presentation =
          {
              .render_scale = 1.0f,
              .paneled = false_v,
              .scene_only = false_v,
          },
  };
}

bool8_t
vkr_sample_runtime_options_parse(int argc, char **argv,
                                 const VkrSampleRuntimeConfig *runtime_config,
                                 VkrSampleRuntimeOptions *options) {
  if (!runtime_config || !runtime_config->title ||
      !runtime_config->ui.initialize || !runtime_config->ui.handle_input ||
      !runtime_config->ui.build || !runtime_config->ui.shutdown) {
    fprintf(stderr, "Invalid sample runtime configuration\n");
    return false_v;
  }

  *options = (VkrSampleRuntimeOptions){
      .scene_path = getenv("VKR_SCENE_PATH"),
#if defined(_WIN32)
      .renderer_backend = VKR_RENDERER_BACKEND_TYPE_VULKAN,
#else
      .renderer_backend = VKR_RENDERER_BACKEND_TYPE_METAL,
#endif
  };
  options->scene_requested = options->scene_path && options->scene_path[0];
  if (!options->scene_requested && vkr_vfs_bundle_scene()) {
    /* A bundle opens its own scene, as if it were requested. */
    options->scene_path = vkr_vfs_bundle_scene();
    options->scene_requested = true_v;
  }
  if (!options->scene_requested) {
    options->scene_path = SCENE_PATH;
  }
  if (!sample_parse_arguments(argc, argv, options)) {
    return false_v;
  }
  // The runtime copies the scene path and derives `<path>.editor.json`.
  if (strlen(vkr_content_root()) + strlen(options->scene_path) +
          sizeof(".editor.json") >
      VKR_SAMPLE_RUNTIME_PATH_CAPACITY) {
    fprintf(stderr, "Scene path is too long\n");
    return false_v;
  }

  options->rg_gpu_timing = sample_env_flag("VKR_RG_GPU_TIMING", false_v);
  options->submission_gpu_timing =
      sample_env_flag("VKR_GPU_SUBMISSION_TIMING", false_v);
  options->metrics_event_subjects =
      sample_env_flag("VKR_METRICS_EVENT_SUBJECTS", false_v);

  if (runtime_config->project_managed &&
      !sample_bootstrap_font_directory(
          options->bootstrap_font_directory,
          sizeof(options->bootstrap_font_directory))) {
    return false_v;
  }
  if (!sample_load_graphics(runtime_config, options)) {
    return false_v;
  }

  // Headless metrics capture. The knobs are opt-in so interactive runs are
  // unchanged; together with VKR_AUTOCLOSE_SECONDS they make a baseline
  // reproducible without a human driving the app.
  options->autoload_scene =
      !runtime_config->project_managed &&
      (options->gameplay_enabled || options->scene_requested ||
       sample_env_flag("VKR_AUTOLOAD_SCENE", false_v));
  options->auto_close_seconds = sample_env_seconds(
      "VKR_AUTOCLOSE_SECONDS", &options->auto_close_rejected);
  options->metrics_interval_seconds = sample_env_seconds(
      "VKR_METRICS_INTERVAL_SECONDS", &options->metrics_interval_rejected);
  options->rg_slow_frame_ms = sample_env_seconds(
      "VKR_RG_SLOW_FRAME_MS", &options->rg_slow_frame_rejected);
  options->assert_no_upload_waits =
      sample_env_flag("VKR_ASSERT_NO_UPLOAD_WAITS", false_v);
  options->scene_memory_verbose =
      sample_env_flag("VKR_SCENE_MEM_VERBOSE", false_v);
  return true_v;
}

VkrStandardSceneRuntimeConfig
vkr_sample_runtime_scene_config(const VkrSampleRuntimeConfig *runtime_config,
                                const VkrSampleRuntimeOptions *options) {
  const VkrGraphicsSettings *graphics = &options->graphics.settings;
  const bool8_t paneled = runtime_config->presentation.paneled;
  /* Only Vulkan offers temporal upscaling (sample_load_graphics). */
  const VkrUpscaleMode upscale_mode = graphics->temporal_upscaling
                                          ? VKR_UPSCALE_MODE_FSR31
                                          : VKR_UPSCALE_MODE_SPATIAL;
  const uint32_t width = runtime_config->presentation.window_width_pt
                             ? runtime_config->presentation.window_width_pt
                         : paneled ? 1280
                                   : 800;
  const uint32_t height = runtime_config->presentation.window_height_pt
                              ? runtime_config->presentation.window_height_pt
                          : paneled ? 800
                                    : 600;
  /* Headless renders into an offscreen target of the window's size in
     pixels; offscreen output is always SDR. */
  const bool8_t headless = runtime_config->presentation.headless;

  return (VkrStandardSceneRuntimeConfig){
      .title = runtime_config->title,
      .x = 100,
      .y = 100,
      .width = width,
      .height = height,
      .target_frame_rate = graphics->frame_limit,
      .unified_title_bar = paneled,
      .window_high_dpi_disabled = !graphics->high_dpi,
      .app_arena_size = MB(1),
      .device_requirements =
          {
              .supported_stages =
                  VKR_SHADER_STAGE_VERTEX_BIT | VKR_SHADER_STAGE_FRAGMENT_BIT,
              .supported_queues = VKR_DEVICE_QUEUE_GRAPHICS_BIT |
                                  VKR_DEVICE_QUEUE_TRANSFER_BIT |
                                  VKR_DEVICE_QUEUE_PRESENT_BIT,
              .allowed_device_types =
                  VKR_DEVICE_TYPE_DISCRETE_BIT | VKR_DEVICE_TYPE_INTEGRATED_BIT,
              .supported_sampler_filters = VKR_SAMPLER_FILTER_ANISOTROPIC_BIT,
          },
      .metrics_config =
          {
              .pass_gpu_timings = options->rg_gpu_timing,
              .submission_gpu_timings = options->submission_gpu_timing,
              .event_subjects = options->metrics_event_subjects,
          },
      .disable_camera_controller = options->gameplay_enabled,
      .renderer_backend = options->renderer_backend,
      /* A windowed target takes its extent and image count from WSI. */
      .present_target =
          headless
              ? (VkrPresentTargetConfig){.kind = VKR_PRESENT_TARGET_OFFSCREEN,
                                         .width = width,
                                         .height = height,
                                         .image_count = 2u}
              : (VkrPresentTargetConfig){0},
      .requested_present_mode =
          graphics->vsync ? VKR_PRESENT_MODE_FIFO : VKR_PRESENT_MODE_IMMEDIATE,
      .display_output_mode = graphics->hdr && !headless
                                 ? VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR
                                 : VKR_DISPLAY_OUTPUT_SDR,
      .render_scale =
          vkr_graphics_settings_render_scale(&options->graphics, graphics),
      .upscale_mode = upscale_mode,
      /* The tiled pipeline's adaptive quality upscales spatially, so it
         keeps 0.65 of each axis at least and holds the 60 Hz frame
         (ADR-087). */
      .dynamic_resolution =
          {
              .min_scale = 0.65f,
              .max_scale = 1.0f,
              .target_frame_ms = 16.0f,
              .enabled = graphics->dynamic_resolution,
          },
      /* The UI may capture the window (VkrSampleCaptureRequest), one capture
         at a time; Vulkan reserves its capture ring only when enabled. */
      .capture_enabled = true_v,
      .capture_ring_capacity = 1u,
      .bootstrap_font_directory = options->bootstrap_font_directory[0]
                                      ? options->bootstrap_font_directory
                                      : NULL,
      .texture_max_load_dimension =
          vkr_graphics_settings_texture_max_dimension(graphics),
  };
}

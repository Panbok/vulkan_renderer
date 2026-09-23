#pragma once

#include "application/vkr_standard_scene_runtime.h"
#include "vkr_sample_runtime.h"

/* Capacity of the runtime's copied scene, sidecar, and Graphics paths. */
#define VKR_SAMPLE_RUNTIME_PATH_CAPACITY 1024u

/* Command-line, environment, and saved Graphics inputs, resolved before the
 * standard runtime and its logger exist. Paths borrow argv, the environment,
 * or static defaults for the process lifetime. */
typedef struct VkrSampleRuntimeOptions {
  const char *scene_path;
  const char *metrics_json_path;
  const char *graphics_path; /* Empty in project mode. */
  VkrRendererBackendType renderer_backend;
  bool8_t scene_requested;
  bool8_t autoload_scene;
  bool8_t gameplay_enabled;
  bool8_t rg_gpu_timing;
  bool8_t submission_gpu_timing;
  bool8_t metrics_event_subjects;
  bool8_t metal_validation_enabled;
  bool8_t assert_no_upload_waits;
  bool8_t scene_memory_verbose;
  /* Zero disables. A rejected value is kept for the warning logged once the
   * logger exists. */
  float64_t auto_close_seconds;
  const char *auto_close_rejected;
  float64_t metrics_interval_seconds;
  const char *metrics_interval_rejected;
  /* Saved preferences clamped to what this backend and validation mode can
   * start with. */
  VkrGraphicsSettingsState graphics;
  bool8_t graphics_loaded;
  char bootstrap_font_directory[2048]; /* Empty outside project mode. */
} VkrSampleRuntimeOptions;

/* Writes each rejection to stderr; false maps to exit code 2. */
bool8_t
vkr_sample_runtime_options_parse(int argc, char **argv,
                                 const VkrSampleRuntimeConfig *runtime_config,
                                 VkrSampleRuntimeOptions *options);

/* The standard runtime borrows the result, and through it `options`, until
 * shutdown. */
VkrStandardSceneRuntimeConfig
vkr_sample_runtime_scene_config(const VkrSampleRuntimeConfig *runtime_config,
                                const VkrSampleRuntimeOptions *options);

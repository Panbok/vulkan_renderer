/**
 * @file vkr_harness_summary_layout.h
 * @brief Byte-exact layouts of every stored capture-summary version.
 *
 * A capture summary is the raw header below followed by its capture results
 * and artifacts. Stored summaries are an ABI: a published layout never
 * changes. Adding a field to a stored type freezes the previous layout here
 * under its version and bumps the writer version. The reader and the tests
 * that forge legacy summaries share these definitions.
 */
#pragma once

#include "vkr_harness.h"

#include <stddef.h>

/** Stored version the writer emits (VkrHarnessCaptureSummaryHeaderV15). */
#define VKR_HARNESS_CAPTURE_SUMMARY_VERSION 15u

/* Version 2 embedded the profile struct directly. Keep its exact layout so
 * accepted capture summaries remain readable when the in-memory profile grows.
 */
typedef struct VkrHarnessProfileV2 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char description[VKR_HARNESS_TEXT_MAX];
  bool8_t authoritative;
  bool8_t allow_dirty;
  VkrHarnessTarget target;
  VkrHarnessPresentMode required_present;
  bool8_t require_actual_present;
  bool8_t gpu_timing;
  bool8_t event_subjects;
  uint32_t minimum_repetitions;
  uint32_t warmup_stability_window;
  float64_t warmup_max_drift_ratio;
  bool8_t require_warmup_stability;
  bool8_t require_exclusive_gpu_lane;
  char required_os[64];
  char required_cpu[128];
  char required_gpu[128];
  char required_driver[128];
  uint32_t required_gpu_vendor_id;
  uint32_t required_gpu_device_id;
  char required_power_mode[32];
  char required_thermal_state[32];
  int32_t required_process_priority;
  bool8_t has_required_process_priority;
  char required_metrics[VKR_HARNESS_MAX_REQUIRED_METRICS][128];
  uint32_t required_metric_count;
} VkrHarnessProfileV2;

/* Versions 2 and 3 embedded this case layout before GTAO became authored
 * harness state. Keep it byte-exact; stored summaries are an ABI.
 */
typedef struct VkrHarnessRendererConfigV3 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  uint32_t shadow_debug_mode;
} VkrHarnessRendererConfigV3;

typedef struct VkrHarnessCaseV3 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV3 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
} VkrHarnessCaseV3;

/* Version 4 embedded the complete case before render_scale was added. Keep
 * this layout byte-exact so existing summaries remain readable. */
typedef struct VkrHarnessRendererConfigV4 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
} VkrHarnessRendererConfigV4;

typedef struct VkrHarnessCaseV4 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV4 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
} VkrHarnessCaseV4;

/* Version 5 embedded the complete case before logical UI content scale became
 * explicit. Keep both nested layouts byte-exact for accepted summaries. */
typedef struct VkrHarnessRendererConfigV5 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
  float32_t render_scale;
  uint32_t render_width;
  uint32_t render_height;
  char upscaler[24];
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
} VkrHarnessRendererConfigV5;

typedef struct VkrHarnessCaseV5 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV5 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
} VkrHarnessCaseV5;

/* Version 6 added UI content scale, before editor transport controls. */
typedef struct VkrHarnessCaseV6 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV5 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV6;

/* Version 7 embedded the complete case before image sharpness became an
 * authored renderer control. Keep this layout byte-exact when reading stored
 * summaries. */
typedef struct VkrHarnessRendererConfigV7 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
  float32_t render_scale;
  uint32_t render_width;
  uint32_t render_height;
  char upscaler[24];
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
} VkrHarnessRendererConfigV7;

typedef struct VkrHarnessCaseV7 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV7 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV7;

/* Version 8 embedded the complete case before display transform became an
 * authored renderer control. Keep this layout byte-exact for stored summaries.
 */
typedef struct VkrHarnessRendererConfigV8 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
  float32_t render_scale;
  uint32_t render_width;
  uint32_t render_height;
  char upscaler[24];
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  float32_t image_sharpness;
} VkrHarnessRendererConfigV8;

typedef struct VkrHarnessCaseV8 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV8 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV8;

_Static_assert(offsetof(VkrHarnessCaseV3, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Legacy case prefix drift");
_Static_assert(offsetof(VkrHarnessRendererConfigV3, shadow_debug_mode) ==
                   offsetof(VkrHarnessRendererConfig, gtao_enabled),
               "Legacy renderer prefix drift");
_Static_assert(offsetof(VkrHarnessCaseV4, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-4 case prefix drift");
_Static_assert(sizeof(VkrHarnessRendererConfigV4) ==
                   offsetof(VkrHarnessRendererConfig, render_scale),
               "Version-4 renderer layout drift");
_Static_assert(sizeof(VkrHarnessRendererConfigV5) ==
                   offsetof(VkrHarnessRendererConfig, editor_stop_frame),
               "Version-5 renderer layout drift");
_Static_assert(sizeof(VkrHarnessCaseV5) ==
                   offsetof(VkrHarnessCaseV6, content_scale),
               "Version-5 case layout drift");
_Static_assert(sizeof(VkrHarnessRendererConfigV7) ==
                   offsetof(VkrHarnessRendererConfig, image_sharpness),
               "Version-7 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV7, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-7 case prefix drift");
_Static_assert(offsetof(VkrHarnessCaseV7, camera) ==
                   offsetof(VkrHarnessCaseV8, camera),
               "Version-7 case camera alignment drift");
_Static_assert(sizeof(VkrHarnessCaseV7) == sizeof(VkrHarnessCaseV8),
               "Version-7/8 case layout drift");
_Static_assert(sizeof(VkrHarnessRendererConfigV8) ==
                   offsetof(VkrHarnessRendererConfig, display_transform),
               "Version-8 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV8, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-8 case prefix drift");
typedef struct VkrHarnessCaptureSummaryHeaderV2 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV3 case_manifest;
  VkrHarnessProfileV2 profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV2;

typedef struct VkrHarnessCaptureSummaryHeaderV3 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV3 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV3;

typedef struct VkrHarnessCaptureSummaryHeaderV4 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV4 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV4;

typedef struct VkrHarnessCaptureSummaryHeaderV5 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV5 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV5;

typedef struct VkrHarnessCaptureSummaryHeaderV6 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV6 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV6;

typedef struct VkrHarnessCaptureSummaryHeaderV7 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV7 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV7;

typedef struct VkrHarnessCaptureSummaryHeaderV8 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV8 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV8;

/* Version 9 added display transform and color grading, before SSR. */
typedef struct VkrHarnessRendererConfigV9 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  /** Whether temporal reconstruction and camera jitter are enabled. */
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  /** Effective receiver tap count after the optional case field is resolved. */
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  /** Measure-relative frame that explicitly resets automatic adaptation. */
  uint32_t exposure_reset_frame;
  /** Bloom is opt-in for deterministic cases; production defaults do not leak
   * into a harness workload. */
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  /** GTAO is opt-in and must carry its complete deterministic control tuple. */
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  /** Cold probe-count control used by the SH scaling fixture. UINT32_MAX means
   * "do not clamp". */
  uint32_t ibl_probe_limit;
  /** Whether the fullscreen ACES tonemap stage is enabled. */
  bool8_t tonemap_enabled;
  /** Whether the fullscreen FXAA stage is enabled. */
  bool8_t fxaa_enabled;
  /** Enables the capture-only fifth transmission peel on every case frame. */
  bool8_t transmission_depth_diagnostic_enabled;
  /** Internal renderer resolution relative to the present target. */
  float32_t render_scale;
  /** Renderer-reported scene extent. Output-only; manifests cannot author it.
   */
  uint32_t render_width;
  uint32_t render_height;
  /** Reconstruction implementation: `spatial`, `metalfx_temporal`, or `fsr31`.
   */
  char upscaler[24];
  /** Completion-driven MetalFX resolution policy. FSR 3.1 uses fixed scale. */
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  /** Authored case-frame indices including warmup, excluding bootstrap.
   * UINT32_MAX disables the action. Stop at zero also stops bootstrap, before
   * the first scene frame. Resume must follow the configured stop. */
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  /** Image-space sharpness after reconstruction. Zero disables the control. */
  float32_t image_sharpness;
  /** `agx` is the default; `aces_fitted` preserves the prior presentation. */
  char display_transform[16];
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t color_contrast;
  float32_t color_saturation;
} VkrHarnessRendererConfigV9;

typedef struct VkrHarnessCaseV9 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV9 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV9;

_Static_assert(sizeof(VkrHarnessRendererConfigV9) ==
                   offsetof(VkrHarnessRendererConfig, ssr_enabled),
               "Version-9 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV9, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-9 case prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV9 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV9 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV9;

/* Version 10 appended the SSR and SSGI toggles, before display output. */
typedef struct VkrHarnessRendererConfigV10 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  /** Whether temporal reconstruction and camera jitter are enabled. */
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  /** Effective receiver tap count after the optional case field is resolved. */
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  /** Measure-relative frame that explicitly resets automatic adaptation. */
  uint32_t exposure_reset_frame;
  /** Bloom is opt-in for deterministic cases; production defaults do not leak
   * into a harness workload. */
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  /** GTAO is opt-in and must carry its complete deterministic control tuple. */
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  /** Cold probe-count control used by the SH scaling fixture. UINT32_MAX means
   * "do not clamp". */
  uint32_t ibl_probe_limit;
  /** Whether the fullscreen ACES tonemap stage is enabled. */
  bool8_t tonemap_enabled;
  /** Whether the fullscreen FXAA stage is enabled. */
  bool8_t fxaa_enabled;
  /** Enables the capture-only fifth transmission peel on every case frame. */
  bool8_t transmission_depth_diagnostic_enabled;
  /** Internal renderer resolution relative to the present target. */
  float32_t render_scale;
  /** Renderer-reported scene extent. Output-only; manifests cannot author it.
   */
  uint32_t render_width;
  uint32_t render_height;
  /** Reconstruction implementation: `spatial`, `metalfx_temporal`, or `fsr31`.
   */
  char upscaler[24];
  /** Completion-driven MetalFX resolution policy. FSR 3.1 uses fixed scale. */
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  /** Authored case-frame indices including warmup, excluding bootstrap.
   * UINT32_MAX disables the action. Stop at zero also stops bootstrap, before
   * the first scene frame. Resume must follow the configured stop. */
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  /** Image-space sharpness after reconstruction. Zero disables the control. */
  float32_t image_sharpness;
  /** `agx` is the default; `aces_fitted` preserves the prior presentation. */
  char display_transform[16];
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t color_contrast;
  float32_t color_saturation;
  /** Half-resolution opaque reflections; opt-in for deterministic cases. */
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
} VkrHarnessRendererConfigV10;

typedef struct VkrHarnessCaseV10 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV10 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  /** Explicit offscreen logical-UI scale; effective OS scale for reports. */
  float32_t content_scale;
} VkrHarnessCaseV10;

_Static_assert(offsetof(VkrHarnessRendererConfigV10, ssgi_enabled) ==
                   offsetof(VkrHarnessRendererConfig, ssgi_enabled),
               "Version-10 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV10, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-10 case prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV10 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV10 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV10;

/* Version 11 appended display-output policy. Version 12 appends DoF controls.
 * Keep this concrete layout for summaries published before the new controls. */
typedef struct VkrHarnessRendererConfigV11 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
  float32_t render_scale;
  uint32_t render_width;
  uint32_t render_height;
  char upscaler[24];
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  float32_t image_sharpness;
  char display_transform[16];
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t color_contrast;
  float32_t color_saturation;
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
  char display_output[24];
} VkrHarnessRendererConfigV11;

typedef struct VkrHarnessCaseV11 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV11 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV11;

_Static_assert(sizeof(VkrHarnessRendererConfigV11) ==
                   offsetof(VkrHarnessRendererConfig, dof_focus_distance),
               "Version-11 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV11, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-11 case prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV11 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV11 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV11;

/* Version 13 appends motion-blur and deterministic entity-motion controls. */
typedef struct VkrHarnessRendererConfigV12 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  uint32_t exposure_reset_frame;
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  uint32_t ibl_probe_limit;
  bool8_t tonemap_enabled;
  bool8_t fxaa_enabled;
  bool8_t transmission_depth_diagnostic_enabled;
  float32_t render_scale;
  uint32_t render_width;
  uint32_t render_height;
  char upscaler[24];
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  float32_t image_sharpness;
  char display_transform[16];
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t color_contrast;
  float32_t color_saturation;
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
  char display_output[24];
  bool8_t dof_enabled;
  float32_t dof_focus_distance;
  float32_t dof_f_stop;
} VkrHarnessRendererConfigV12;

typedef struct VkrHarnessCaseV12 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV12 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  float32_t content_scale;
} VkrHarnessCaseV12;

_Static_assert(sizeof(VkrHarnessRendererConfigV12) ==
                   offsetof(VkrHarnessRendererConfig, motion_blur_enabled),
               "Version-12 renderer layout drift");
_Static_assert(offsetof(VkrHarnessCaseV12, renderer) ==
                   offsetof(VkrHarnessCase, renderer),
               "Version-12 case prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV12 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV12 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV12;

/* Frozen pre-physics renderer layout, shared by summary versions 13 and 14. */
typedef struct VkrHarnessRendererConfigV14 {
  bool8_t editor;
  bool8_t skybox;
  bool8_t text_fixture;
  /** Whether temporal reconstruction and camera jitter are enabled. */
  bool8_t taa_enabled;
  bool8_t shadow_pcf_early_out;
  bool8_t shadow_sdsm;
  char backend[16];
  char shadow_preset[32];
  uint32_t shadow_cascades;
  /** Effective receiver tap count after the optional case field is resolved. */
  uint32_t shadow_pcf_samples;
  uint32_t shadow_map_size;
  float32_t shadow_split_lambda;
  char render_mode[24];
  char exposure_mode[16];
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  /** Measure-relative frame that explicitly resets automatic adaptation. */
  uint32_t exposure_reset_frame;
  /** Bloom is opt-in for deterministic cases; production defaults do not leak
   * into a harness workload. */
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  /** GTAO is opt-in and must carry its complete deterministic control tuple. */
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  uint32_t shadow_debug_mode;
  /** Cold probe-count control used by the SH scaling fixture. UINT32_MAX means
   * "do not clamp". */
  uint32_t ibl_probe_limit;
  /** Whether the fullscreen ACES tonemap stage is enabled. */
  bool8_t tonemap_enabled;
  /** Whether the fullscreen FXAA stage is enabled. */
  bool8_t fxaa_enabled;
  /** Enables the capture-only fifth transmission peel on every case frame. */
  bool8_t transmission_depth_diagnostic_enabled;
  /** Internal renderer resolution relative to the present target. */
  float32_t render_scale;
  /** Renderer-reported scene extent. Output-only; manifests cannot author it.
   */
  uint32_t render_width;
  uint32_t render_height;
  /** Reconstruction implementation: `spatial`, `metalfx_temporal`, or `fsr31`.
   */
  char upscaler[24];
  /** Completion-driven MetalFX resolution policy. FSR 3.1 uses fixed scale. */
  bool8_t dynamic_resolution;
  float32_t dynamic_resolution_min_scale;
  float32_t dynamic_resolution_max_scale;
  float32_t dynamic_resolution_target_frame_ms;
  /** Authored case-frame indices including warmup, excluding bootstrap.
   * UINT32_MAX disables the action. Stop at zero also stops bootstrap, before
   * the first scene frame. Resume must follow the configured stop. */
  uint32_t editor_stop_frame;
  uint32_t editor_resume_frame;
  /** Image-space sharpness after reconstruction. Zero disables the control. */
  float32_t image_sharpness;
  /** `agx` is the default; `aces_fitted` preserves the prior presentation. */
  char display_transform[16];
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t color_contrast;
  float32_t color_saturation;
  /** Half-resolution opaque reflections; opt-in for deterministic cases. */
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
  /** Requested presentation policy; offscreen targets remain SDR. */
  char display_output[24];
  /** Opaque-depth depth of field. Disabled cases leave scene color unchanged.
   */
  bool8_t dof_enabled;
  /** Focus plane distance in metres. */
  float32_t dof_focus_distance;
  /** Photographic aperture denominator. */
  float32_t dof_f_stop;
  /** Optional velocity-based opaque motion blur. */
  bool8_t motion_blur_enabled;
  /** Shutter interval in degrees; zero bypasses motion blur. */
  float32_t motion_blur_shutter_angle;
  /** Optional scene entity translated deterministically before each frame. */
  char motion_blur_entity[VKR_HARNESS_ID_MAX];
  /** Entity translation velocity in metres per second. */
  float32_t motion_blur_entity_velocity_x;
  float32_t motion_blur_entity_velocity_y;
  float32_t motion_blur_entity_velocity_z;
} VkrHarnessRendererConfigV14;

typedef struct VkrHarnessCaseV14 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV14 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  /** Explicit offscreen logical-UI scale; effective OS scale for reports. */
  float32_t content_scale;
  VkrHarnessAssetContext asset_context;
} VkrHarnessCaseV14;

typedef struct VkrHarnessCaseV13 {
  uint32_t schema_version;
  char manifest_path[VKR_HARNESS_PATH_MAX];
  char manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char id[VKR_HARNESS_ID_MAX];
  char suite[64];
  char description[VKR_HARNESS_TEXT_MAX];
  char scene[VKR_HARNESS_PATH_MAX];
  uint64_t seed;
  uint32_t width;
  uint32_t height;
  bool8_t resize_round_trip;
  uint32_t resize_width;
  uint32_t resize_height;
  VkrHarnessBootProfile boot;
  VkrHarnessTarget target;
  VkrHarnessPresentMode present;
  uint32_t target_image_count;
  VkrHarnessCacheMode cache;
  float64_t fixed_delta_seconds;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t repetitions;
  uint32_t repetition_timeout_ms;
  uint32_t asset_ready_timeout_ms;
  VkrHarnessRendererConfigV14 renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  /** Explicit offscreen logical-UI scale; effective OS scale for reports. */
  float32_t content_scale;
} VkrHarnessCaseV13;

_Static_assert(sizeof(VkrHarnessRendererConfigV14) ==
                   offsetof(VkrHarnessRendererConfig, physics_fixture),
               "Version-14 renderer prefix drift");
_Static_assert(offsetof(VkrHarnessCaseV13, content_scale) ==
                   offsetof(VkrHarnessCaseV14, content_scale),
               "Version-13/14 frozen case prefix drift");

typedef struct VkrHarnessCaptureSummaryHeaderV13 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV13 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV13;

typedef struct VkrHarnessCaptureSummaryHeaderV14 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCaseV14 case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV14;

typedef struct VkrHarnessCaptureSummaryHeaderV15 {
  uint8_t magic[8];
  uint32_t version;
  uint32_t capture_count;
  uint32_t artifact_count;
  uint32_t tool;
  uint32_t exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
  uint8_t reserved[2];
  char status[24];
  char case_id[VKR_HARNESS_ID_MAX];
  char case_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char profile_manifest_sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCase case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV15;

/* Stored summaries are an ABI, so their sizes are pinned. Every stored type
 * uses fixed-width members and Vec3 is explicitly 16-byte aligned, so the
 * sizes hold on every supported compiler. A failing pin means a stored type
 * changed: freeze its previous layout above and bump the writer version. The
 * reader also relies on the case starting at the same offset in every
 * version. */
#define VKR_HARNESS_SUMMARY_ABI(header, size)                                  \
  _Static_assert(                                                              \
      sizeof(header) == (size) &&                                              \
          offsetof(header, case_manifest) ==                                   \
              offsetof(VkrHarnessCaptureSummaryHeaderV2, case_manifest),       \
      #header " stored layout drift")

VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV2, 77904u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV3, 78032u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV4, 78048u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV5, 78112u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV6, 78128u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV7, 78128u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV8, 78128u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV9, 78160u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV10, 78176u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV11, 78192u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV12, 78208u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV13, 78320u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV14, 78320u);
VKR_HARNESS_SUMMARY_ABI(VkrHarnessCaptureSummaryHeaderV15, 78320u);
_Static_assert(sizeof(VkrHarnessCaptureResult) == 2072u &&
                   sizeof(VkrHarnessArtifact) == 512u,
               "Capture summary record drift");

#undef VKR_HARNESS_SUMMARY_ABI

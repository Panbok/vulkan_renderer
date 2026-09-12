/**
 * @file vkr_harness_capture.c
 * @brief Canonicalizes a ready capture batch into comparable artifacts.
 *
 * Comparison always reads the canonical payload, never the preview. Display
 * color becomes a top-left RGBA8 PNG. Packed normals, depth, and identifier
 * channels retain a tightly packed little-endian payload beside a PNG a
 * reviewer can inspect. Each row is published with a sidecar describing
 * exactly how it was produced.
 */
#include "vkr_harness_runtime.h"

#include <float.h>
#include <stb_image_write.h>

typedef struct VkrHarnessPngBuffer {
  uint8_t *data;
  uint64_t count;
  uint64_t capacity;
  bool8_t failed;
} VkrHarnessPngBuffer;

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
 * harness state. Keep it private and byte-exact; stored summaries are an ABI.
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
  VkrHarnessRendererConfig renderer;
  VkrHarnessCamera camera;
  VkrHarnessCapture captures[VKR_HARNESS_MAX_CAPTURES];
  uint32_t capture_count;
  VkrHarnessAssertion assertions[VKR_HARNESS_MAX_ASSERTIONS];
  uint32_t assertion_count;
  VkrHarnessCompareConfig compare;
  /** Explicit offscreen logical-UI scale; effective OS scale for reports. */
  float32_t content_scale;
} VkrHarnessCaseV13;

_Static_assert(offsetof(VkrHarnessCaseV13, content_scale) ==
                       offsetof(VkrHarnessCase, content_scale) &&
                   sizeof(VkrHarnessCaseV13) <= sizeof(VkrHarnessCase),
               "Version 13 capture case prefix must remain readable");

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
  VkrHarnessCase case_manifest;
  VkrHarnessProfile profile;
  VkrHarnessProvenance provenance;
} VkrHarnessCaptureSummaryHeaderV14;

_Static_assert(
    offsetof(VkrHarnessCaptureSummaryHeaderV2, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV3, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV3, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV4, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV4, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV5, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV5, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV6, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV6, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV7, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV7, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV8, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV8, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV9, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV9, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV10, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV10, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV11, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV11, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV12, case_manifest) &&
        offsetof(VkrHarnessCaptureSummaryHeaderV12, case_manifest) ==
            offsetof(VkrHarnessCaptureSummaryHeaderV13, case_manifest),
    "Capture summary common prefix drift");

static const uint8_t s_capture_summary_magic[8] = {'V', 'K', 'R', 'C',
                                                   'A', 'P', '0', '1'};

static void vkr_harness_profile_from_v2(const VkrHarnessProfileV2 *source,
                                        VkrHarnessProfile *destination) {
  MemZero(destination, sizeof(*destination));
  destination->schema_version = source->schema_version;
  string_format(destination->manifest_path, sizeof(destination->manifest_path),
                "%s", source->manifest_path);
  string_format(destination->manifest_sha256,
                sizeof(destination->manifest_sha256), "%s",
                source->manifest_sha256);
  string_format(destination->id, sizeof(destination->id), "%s", source->id);
  string_format(destination->description, sizeof(destination->description),
                "%s", source->description);
  destination->authoritative = source->authoritative;
  destination->allow_dirty = source->allow_dirty;
  destination->target = source->target;
  destination->required_present = source->required_present;
  destination->require_actual_present = source->require_actual_present;
  destination->gpu_timing = source->gpu_timing;
  destination->event_subjects = source->event_subjects;
  destination->minimum_repetitions = source->minimum_repetitions;
  destination->warmup_stability_window = source->warmup_stability_window;
  string_format(destination->warmup_stability_metric,
                sizeof(destination->warmup_stability_metric), "%s",
                "cpu.render_submit");
  destination->warmup_max_drift_ratio = source->warmup_max_drift_ratio;
  destination->require_warmup_stability = source->require_warmup_stability;
  destination->require_exclusive_gpu_lane = source->require_exclusive_gpu_lane;
  string_format(destination->required_os, sizeof(destination->required_os),
                "%s", source->required_os);
  string_format(destination->required_cpu, sizeof(destination->required_cpu),
                "%s", source->required_cpu);
  string_format(destination->required_gpu, sizeof(destination->required_gpu),
                "%s", source->required_gpu);
  string_format(destination->required_driver,
                sizeof(destination->required_driver), "%s",
                source->required_driver);
  destination->required_gpu_vendor_id = source->required_gpu_vendor_id;
  destination->required_gpu_device_id = source->required_gpu_device_id;
  string_format(destination->required_power_mode,
                sizeof(destination->required_power_mode), "%s",
                source->required_power_mode);
  string_format(destination->required_thermal_state,
                sizeof(destination->required_thermal_state), "%s",
                source->required_thermal_state);
  destination->required_process_priority = source->required_process_priority;
  destination->has_required_process_priority =
      source->has_required_process_priority;
  MemCopy(destination->required_metrics, source->required_metrics,
          sizeof(destination->required_metrics));
  destination->required_metric_count = source->required_metric_count;
}

static void
vkr_harness_case_legacy_display_defaults(VkrHarnessCase *destination) {
  string_copy(destination->renderer.display_transform, "aces_fitted");
  destination->renderer.color_contrast = 1.0f;
  destination->renderer.color_saturation = 1.0f;
}

static void vkr_harness_case_from_v3(const VkrHarnessCaseV3 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV3, renderer));
  MemCopy(&destination->renderer, &source->renderer,
          offsetof(VkrHarnessRendererConfigV3, shadow_debug_mode));
  destination->renderer.gtao_enabled = false_v;
  destination->renderer.gtao_radius = VKR_GTAO_DEFAULT_RADIUS;
  destination->renderer.gtao_power = VKR_GTAO_DEFAULT_POWER;
  destination->renderer.shadow_debug_mode = source->renderer.shadow_debug_mode;
  destination->renderer.render_scale = 1.0f;
  string_copy(destination->renderer.upscaler, "spatial");
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = 1.0f;
  destination->renderer.editor_stop_frame = UINT32_MAX;
  destination->renderer.editor_resume_frame = UINT32_MAX;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v4(const VkrHarnessCaseV4 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV4, renderer));
  MemCopy(&destination->renderer, &source->renderer,
          sizeof(VkrHarnessRendererConfigV4));
  destination->renderer.render_scale = 1.0f;
  string_copy(destination->renderer.upscaler, "spatial");
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = 1.0f;
  destination->renderer.editor_stop_frame = UINT32_MAX;
  destination->renderer.editor_resume_frame = UINT32_MAX;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v5(const VkrHarnessCaseV5 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV5, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = 1.0f;
  destination->renderer.editor_stop_frame = UINT32_MAX;
  destination->renderer.editor_resume_frame = UINT32_MAX;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v6(const VkrHarnessCaseV6 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV6, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
  destination->renderer.editor_stop_frame = UINT32_MAX;
  destination->renderer.editor_resume_frame = UINT32_MAX;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v7(const VkrHarnessCaseV7 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV7, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
  destination->renderer.image_sharpness = 0.0f;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v8(const VkrHarnessCaseV8 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV8, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
  vkr_harness_case_legacy_display_defaults(destination);
}

static void vkr_harness_case_from_v9(const VkrHarnessCaseV9 *source,
                                     VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV9, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
}

static void vkr_harness_case_from_v10(const VkrHarnessCaseV10 *source,
                                      VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV10, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
}

static void vkr_harness_case_from_v11(const VkrHarnessCaseV11 *source,
                                      VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV11, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->renderer.dof_enabled = false_v;
  destination->renderer.dof_focus_distance = 5.0f;
  destination->renderer.dof_f_stop = 2.8f;
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures,
          sizeof(destination->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(destination->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
}

static void vkr_harness_case_from_v12(const VkrHarnessCaseV12 *source,
                                      VkrHarnessCase *destination) {
  MemZero(destination, sizeof(*destination));
  MemCopy(destination, source, offsetof(VkrHarnessCaseV12, renderer));
  MemCopy(&destination->renderer, &source->renderer, sizeof(source->renderer));
  destination->renderer.motion_blur_enabled = false_v;
  destination->renderer.motion_blur_shutter_angle = 180.0f;
  destination->camera = source->camera;
  MemCopy(destination->captures, source->captures, sizeof(source->captures));
  destination->capture_count = source->capture_count;
  MemCopy(destination->assertions, source->assertions,
          sizeof(source->assertions));
  destination->assertion_count = source->assertion_count;
  destination->compare = source->compare;
  destination->content_scale = source->content_scale;
}

static void vkr_harness_png_write(void *context, void *data, int size) {
  VkrHarnessPngBuffer *buffer = context;
  if (!buffer || size < 0 ||
      buffer->count + (uint64_t)size > buffer->capacity) {
    if (buffer) {
      buffer->failed = true_v;
    }
    return;
  }
  MemCopy(buffer->data + buffer->count, data, (uint64_t)size);
  buffer->count += (uint64_t)size;
}

static const char *vkr_harness_capture_format_name(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return "R8G8B8A8_UNORM";
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return "R8G8B8A8_SRGB";
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return "B8G8R8A8_UNORM";
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return "B8G8R8A8_SRGB";
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return "R8_UNORM";
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return "R16G16B16A16_SFLOAT";
  case VKR_TEXTURE_FORMAT_R16G16_SFLOAT:
    return "R16G16_SFLOAT";
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return "R16_SFLOAT";
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
    return "R32_SFLOAT";
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return "D32_SFLOAT";
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return "D16_UNORM";
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return "R32_UINT";
  case VKR_TEXTURE_FORMAT_R32G32_UINT:
    return "R32G32_UINT";
  case VKR_TEXTURE_FORMAT_R16G16_SNORM:
    return "R16G16_SNORM";
  default:
    return "UNSUPPORTED";
  }
}

static const char *
vkr_harness_capture_value_name(VkrCaptureValueKind value_kind) {
  return value_kind == VKR_CAPTURE_VALUE_COLOR   ? "color"
         : value_kind == VKR_CAPTURE_VALUE_DEPTH ? "depth"
                                                 : "uint";
}

static const char *
vkr_harness_capture_color_space_name(VkrCaptureColorSpace color_space) {
  return color_space == VKR_CAPTURE_COLOR_SPACE_SRGB ? "srgb"
         : color_space == VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR
             ? "extended_srgb_linear"
             : "none";
}

/** Canonical output is top-left; a bottom-left source is flipped on read. */
static const uint8_t *vkr_harness_capture_row(const VkrCaptureItemResult *item,
                                              uint32_t y) {
  const uint32_t source_y = item->origin == VKR_CAPTURE_ORIGIN_BOTTOM_LEFT
                                ? item->height - 1u - y
                                : y;
  return (const uint8_t *)item->data + source_y * item->row_pitch;
}

static uint16_t vkr_harness_capture_read_u16(const uint8_t *bytes) {
  uint16_t value = 0u;
  MemCopy(&value, bytes, sizeof(value));
  return value;
}

static uint32_t vkr_harness_capture_read_u32(const uint8_t *bytes) {
  uint32_t value = 0u;
  MemCopy(&value, bytes, sizeof(value));
  return value;
}

static float32_t vkr_harness_capture_read_f32(const uint8_t *bytes) {
  const uint32_t bits = vkr_harness_capture_read_u32(bytes);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static void vkr_harness_capture_write_u32_le(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void vkr_harness_capture_write_u16_le(uint8_t *bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
}

bool8_t vkr_harness_capture_png_write(const char *path, const uint8_t *rgba,
                                      uint32_t width, uint32_t height,
                                      const VkrHarnessArenas *arenas,
                                      VkrHarnessError *error) {
  const uint64_t capacity = (uint64_t)width * height * 5u + KB(64);
  Scratch scratch = scratch_create(arenas->transient);
  VkrHarnessPngBuffer output = {
      .data = arena_alloc(arenas->transient, capacity, ARENA_MEMORY_TAG_ARRAY),
      .capacity = capacity,
  };
  bool8_t ok =
      output.data &&
      stbi_write_png_to_func(vkr_harness_png_write, &output, (int)width,
                             (int)height, 4, rgba, (int)(width * 4u)) != 0 &&
      !output.failed &&
      vkr_harness_atomic_write(path, output.data, output.count, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

static float32_t vkr_harness_capture_half_to_float(uint16_t half) {
  const uint32_t sign = half >> 15u;
  const uint32_t exponent = (half >> 10u) & 0x1fu;
  const uint32_t mantissa = half & 0x3ffu;
  float32_t value = 0.0f;
  if (exponent == 0u) {
    value = (float32_t)mantissa * (1.0f / 16777216.0f);
  } else if (exponent < 31u) {
    value = (1.0f + (float32_t)mantissa * (1.0f / 1024.0f)) *
            vkr_pow_f32(2.0f, (float32_t)((int32_t)exponent - 15));
  }
  return sign ? -value : value;
}

static float32_t vkr_harness_capture_aces(float32_t value) {
  const float32_t nonnegative = Max(value, 0.0f);
  return Clamp((nonnegative * (2.51f * nonnegative + 0.03f)) /
                   (nonnegative * (2.43f * nonnegative + 0.59f) + 0.14f),
               0.0f, 1.0f);
}

static uint8_t vkr_harness_capture_linear_to_srgb8(float32_t value,
                                                   float32_t exposure) {
  const float32_t linear = vkr_harness_capture_aces(value * exposure);
  const float32_t srgb =
      linear <= 0.0031308f ? linear * 12.92f
                           : 1.055f * vkr_pow_f32(linear, 1.0f / 2.4f) - 0.055f;
  return (uint8_t)(Clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static uint8_t vkr_harness_capture_display_to_srgb8(float32_t value,
                                                    float32_t output_scale) {
  const float32_t linear = Clamp(value / output_scale, 0.0f, 1.0f);
  const float32_t srgb =
      linear <= 0.0031308f ? linear * 12.92f
                           : 1.055f * vkr_pow_f32(linear, 1.0f / 2.4f) - 0.055f;
  return (uint8_t)(Clamp(srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static void vkr_harness_capture_color_rgba(const VkrCaptureItemResult *item,
                                           uint8_t *rgba) {
  const bool8_t bgra = item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM ||
                       item->format == VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  const bool8_t hdr = item->format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT;
  const bool8_t opaque_scene = string_equals(
      vkr_renderer_capture_channel_get(item->channel)->name, "scene_color");
  const bool8_t oct_normal = item->format == VKR_TEXTURE_FORMAT_R16G16_SNORM;
  const bool8_t grayscale = item->format == VKR_TEXTURE_FORMAT_R8_UNORM;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      if (grayscale) {
        const uint8_t value = source[x];
        target[x * 4u + 0u] = value;
        target[x * 4u + 1u] = value;
        target[x * 4u + 2u] = value;
        target[x * 4u + 3u] = 255u;
        continue;
      }
      if (oct_normal) {
        const uint8_t *texel = source + (uint64_t)x * 4u;
        const int16_t encoded_x =
            (int16_t)vkr_harness_capture_read_u16(texel + 0u);
        const int16_t encoded_y =
            (int16_t)vkr_harness_capture_read_u16(texel + 2u);
        float32_t nx = Clamp((float32_t)encoded_x / 32767.0f, -1.0f, 1.0f);
        float32_t ny = Clamp((float32_t)encoded_y / 32767.0f, -1.0f, 1.0f);
        float32_t nz = 1.0f - vkr_abs_f32(nx) - vkr_abs_f32(ny);
        const float32_t fold = Clamp(-nz, 0.0f, 1.0f);
        nx += nx >= 0.0f ? -fold : fold;
        ny += ny >= 0.0f ? -fold : fold;
        const float32_t length =
            vkr_sqrt_f32(Max(nx * nx + ny * ny + nz * nz, 1e-12f));
        nx /= length;
        ny /= length;
        nz /= length;
        target[x * 4u + 0u] =
            (uint8_t)(Clamp(nx * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 1u] =
            (uint8_t)(Clamp(ny * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 2u] =
            (uint8_t)(Clamp(nz * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        target[x * 4u + 3u] = 255u;
        continue;
      }
      if (hdr) {
        const uint8_t *texel = source + (uint64_t)x * 8u;
        if (item->color_space == VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR) {
          for (uint32_t component = 0; component < 3u; ++component)
            target[x * 4u + component] = vkr_harness_capture_display_to_srgb8(
                vkr_harness_capture_half_to_float(
                    vkr_harness_capture_read_u16(texel + component * 2u)),
                item->display_output.output_scale);
          target[x * 4u + 3u] =
              (uint8_t)(Clamp(vkr_harness_capture_half_to_float(
                                  vkr_harness_capture_read_u16(texel + 6u)),
                              0.0f, 1.0f) *
                            255.0f +
                        0.5f);
          continue;
        }
        target[x * 4u + 0u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 0u)),
            item->display_exposure);
        target[x * 4u + 1u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 2u)),
            item->display_exposure);
        target[x * 4u + 2u] = vkr_harness_capture_linear_to_srgb8(
            vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 4u)),
            item->display_exposure);
        target[x * 4u + 3u] =
            opaque_scene
                ? 255u
                : (uint8_t)(Clamp(vkr_harness_capture_half_to_float(
                                      vkr_harness_capture_read_u16(texel + 6u)),
                                  0.0f, 1.0f) *
                                255.0f +
                            0.5f);
        continue;
      }
      target[x * 4u + 0u] = source[x * 4u + (bgra ? 2u : 0u)];
      target[x * 4u + 1u] = source[x * 4u + 1u];
      target[x * 4u + 2u] = source[x * 4u + (bgra ? 0u : 2u)];
      target[x * 4u + 3u] = source[x * 4u + 3u];
    }
  }
}

/**
 * One texel of a depth source as a floating-point depth value. The accepted
 * source formats are the only ones capture initialization permits, so this is
 * the single place each encoding is interpreted.
 */
static float32_t vkr_harness_capture_depth_at(const VkrCaptureItemResult *item,
                                              const uint8_t *row, uint32_t x) {
  if (item->format == VKR_TEXTURE_FORMAT_D16_UNORM) {
    return (float32_t)vkr_harness_capture_read_u16(row + (uint64_t)x * 2u) /
           65535.0f;
  }
  if (item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT) {
    return vkr_harness_capture_half_to_float(
        vkr_harness_capture_read_u16(row + (uint64_t)x * 2u));
  }
  return vkr_harness_capture_read_f32(row + (uint64_t)x * 4u);
}

/**
 * Depth previews normalize over the observed range, which the sidecar records:
 * a fixed [0,1] ramp would render almost every scene as flat white. The
 * canonical payload is unnormalized, so comparison is unaffected.
 */
static void vkr_harness_capture_depth_preview(const VkrCaptureItemResult *item,
                                              uint8_t *rgba, float32_t *out_min,
                                              float32_t *out_max) {
  const bool8_t view_depth = item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT ||
                             item->format == VKR_TEXTURE_FORMAT_R32_SFLOAT;
  float32_t min_value = view_depth ? FLT_MAX : 1.0f;
  float32_t max_value = view_depth ? -FLT_MAX : 0.0f;
  bool8_t observed_value = false_v;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    for (uint32_t x = 0; x < item->width; ++x) {
      const float32_t depth = vkr_harness_capture_depth_at(item, row, x);
      /* NaN compares unequal to itself and must not poison the range. */
      if (depth == depth) {
        min_value = Min(min_value, depth);
        max_value = Max(max_value, depth);
        observed_value = true_v;
      }
    }
  }
  if (view_depth && !observed_value) {
    min_value = 0.0f;
    max_value = 0.0f;
  }
  const float32_t range = max_value > min_value ? max_value - min_value : 1.0f;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const float32_t depth = vkr_harness_capture_depth_at(item, row, x);
      const float32_t normalized =
          Clamp((depth - min_value) / range, 0.0f, 1.0f);
      const uint8_t value = (uint8_t)(normalized * 255.0f + 0.5f);
      target[x * 4u + 0u] = value;
      target[x * 4u + 1u] = value;
      target[x * 4u + 2u] = value;
      target[x * 4u + 3u] = 255u;
    }
  }
  if (out_min) {
    *out_min = min_value;
  }
  if (out_max) {
    *out_max = max_value;
  }
}

/**
 * Identifiers have no meaningful ordering, so the preview hashes each one into
 * a stable colour. Zero stays black so "no object" reads as background.
 */
static void vkr_harness_capture_uint_preview(const VkrCaptureItemResult *item,
                                             uint32_t component,
                                             uint32_t value_mask,
                                             uint8_t *rgba) {
  const uint64_t texel_stride =
      item->format == VKR_TEXTURE_FORMAT_R32G32_UINT ? 8u : 4u;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *row = vkr_harness_capture_row(item, y);
    uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const uint32_t source_id =
          vkr_harness_capture_read_u32(row + (uint64_t)x * texel_stride +
                                       (uint64_t)component * 4u) &
          value_mask;
      uint32_t id = source_id;
      id ^= id >> 16;
      id *= 0x7feb352du;
      id ^= id >> 15;
      id *= 0x846ca68bu;
      id ^= id >> 16;
      target[x * 4u + 0u] = source_id ? (uint8_t)id : 0u;
      target[x * 4u + 1u] = source_id ? (uint8_t)(id >> 8) : 0u;
      target[x * 4u + 2u] = source_id ? (uint8_t)(id >> 16) : 0u;
      target[x * 4u + 3u] = 255u;
    }
  }
}

/**
 * Writes the tightly packed little-endian 32-bit payload that comparison
 * actually reads. Packed R16G16 normals and identifiers keep their exact bits;
 * depth becomes float bits. `D16_UNORM` and `R16_SFLOAT` are widened here so
 * one canonical encoding covers every depth source.
 */
static void vkr_harness_capture_canonical_u32(const VkrCaptureItemResult *item,
                                              uint32_t component,
                                              uint32_t value_mask,
                                              uint8_t *tight) {
  const bool8_t depth = item->value_kind == VKR_CAPTURE_VALUE_DEPTH;
  const uint64_t texel_stride =
      item->format == VKR_TEXTURE_FORMAT_R32G32_UINT ? 8u : 4u;
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = tight + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      uint32_t canonical;
      if (depth && (item->format == VKR_TEXTURE_FORMAT_D16_UNORM ||
                    item->format == VKR_TEXTURE_FORMAT_R16_SFLOAT)) {
        const float32_t value = vkr_harness_capture_depth_at(item, source, x);
        MemCopy(&canonical, &value, sizeof(canonical));
      } else {
        canonical =
            vkr_harness_capture_read_u32(source + (uint64_t)x * texel_stride +
                                         (uint64_t)component * 4u) &
            value_mask;
      }
      vkr_harness_capture_write_u32_le(target + (uint64_t)x * 4u, canonical);
    }
  }
}

/** Writes a tight top-left IEEE-754 binary16 payload with a fixed byte order.
 */
static void
vkr_harness_capture_canonical_rgba16f(const VkrCaptureItemResult *item,
                                      uint8_t *tight) {
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = tight + (uint64_t)y * item->width * 8u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const uint8_t *source_texel = source + (uint64_t)x * 8u;
      uint8_t *target_texel = target + (uint64_t)x * 8u;
      for (uint32_t component = 0; component < 4u; ++component) {
        vkr_harness_capture_write_u16_le(
            target_texel + component * 2u,
            vkr_harness_capture_read_u16(source_texel + component * 2u));
      }
    }
  }
}

/** Writes a tight top-left IEEE-754 binary16 RG payload with a fixed byte
 * order. The DoF CoC has no display color transform. */
static void
vkr_harness_capture_canonical_rg16f(const VkrCaptureItemResult *item,
                                    uint8_t *tight) {
  for (uint32_t y = 0; y < item->height; ++y) {
    const uint8_t *source = vkr_harness_capture_row(item, y);
    uint8_t *target = tight + (uint64_t)y * item->width * 4u;
    for (uint32_t x = 0; x < item->width; ++x) {
      const uint8_t *source_texel = source + (uint64_t)x * 4u;
      uint8_t *target_texel = target + (uint64_t)x * 4u;
      for (uint32_t component = 0; component < 2u; ++component) {
        vkr_harness_capture_write_u16_le(
            target_texel + component * 2u,
            vkr_harness_capture_read_u16(source_texel + component * 2u));
      }
    }
  }
}

static bool8_t
vkr_harness_capture_encoding_is_png(const char *canonical_encoding) {
  return string_equals(canonical_encoding, "RGBA8_SRGB_PNG") ||
         string_equals(canonical_encoding, "RGBA8_UNORM") ||
         string_equals(canonical_encoding, "RGBA8_UNORM_PNG");
}

static bool8_t
vkr_harness_capture_encoding_is_rgba16f(const char *canonical_encoding) {
  return string_equals(canonical_encoding, "RGBA16_FLOAT_LE");
}

static bool8_t
vkr_harness_capture_encoding_is_rg16f(const char *canonical_encoding) {
  return string_equals(canonical_encoding, "RG16_FLOAT_LE");
}

/**
 * Emits the sidecar describing how one canonical payload was produced. Written
 * through the shared JSON writer so escaping, bounds, and atomic publication
 * are the writer's problem rather than a format string's.
 */
static bool8_t vkr_harness_capture_write_metadata(
    const char *path, const VkrHarnessCaptureResult *capture,
    const VkrCaptureItemResult *item, float32_t preview_min,
    float32_t preview_max) {
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(
          &file, string8_create_from_cstr((const uint8_t *)path,
                                          string_length(path)))) {
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  const bool8_t depth = string_equals(capture->value_kind, "depth");
  const bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version",
                                VKR_HARNESS_SCHEMA_VERSION) &&
      vkr_harness_json_emit_string(writer, "channel", capture->channel) &&
      vkr_harness_json_emit_u64(writer, "capture_version",
                                capture->capture_version) &&
      vkr_harness_json_emit_string(writer, "producer_resource",
                                   capture->producer_resource) &&
      vkr_harness_json_emit_string(writer, "value_kind", capture->value_kind) &&
      vkr_harness_json_emit_string(writer, "color_space",
                                   capture->color_space) &&
      vkr_harness_json_emit_string(writer, "source_format",
                                   capture->source_format) &&
      vkr_harness_json_emit_string(writer, "canonical_encoding",
                                   capture->canonical_encoding) &&
      vkr_harness_json_emit_string(writer, "origin", capture->origin) &&
      vkr_harness_json_emit_u64(writer, "width", capture->width) &&
      vkr_harness_json_emit_u64(writer, "height", capture->height) &&
      vkr_harness_json_emit_u64(writer, "source_row_pitch",
                                capture->source_row_pitch) &&
      vkr_harness_json_emit_u64(writer, "mip", capture->mip) &&
      vkr_harness_json_emit_u64(writer, "layer", capture->layer) &&
      vkr_harness_json_emit_f64(writer, "display_exposure",
                                item->display_exposure) &&
      vkr_harness_json_emit_u64(writer, "source_frame_index",
                                capture->source_frame_index) &&
      vkr_harness_json_emit_u64(writer, "submit_serial",
                                capture->submit_serial) &&
      (item->color_space != VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR ||
       (vkr_harness_json_emit_f64(writer, "display_headroom",
                                  item->display_output.headroom) &&
        vkr_harness_json_emit_f64(writer, "display_output_scale",
                                  item->display_output.output_scale))) &&
      vkr_harness_json_emit_string(writer, "data_path", capture->data_path) &&
      vkr_harness_json_emit_string(writer, "data_sha256",
                                   capture->data_sha256) &&
      vkr_harness_json_emit_string(writer, "preview_path",
                                   capture->preview_path) &&
      vkr_harness_json_emit_string(writer, "preview_sha256",
                                   capture->preview_sha256) &&
      vkr_harness_json_emit_name(writer, "preview_normalization") &&
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_string(
          writer, "mode",
          depth ? "observed_range"
          : item->color_space == VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR
              ? "display_white_sdr"
              : "identity") &&
      vkr_harness_json_emit_f64(writer, "min", (float64_t)preview_min) &&
      vkr_harness_json_emit_f64(writer, "max", (float64_t)preview_max) &&
      vkr_json_writer_end_object(writer) && vkr_json_writer_end_object(writer);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    return false_v;
  }
  return vkr_json_file_writer_commit(&file);
}

/** Fills the report row for one item, leaving only the digests to compute. */
static void vkr_harness_capture_describe(
    VkrHarnessCaptureResult *capture, const VkrCaptureItemResult *item,
    const VkrCaptureChannelDescription *channel,
    const VkrHarnessCaptureChannelDescription *logical_channel,
    const VkrCapturePollResult *poll, uint32_t checkpoint_frame,
    const char *stem, bool8_t color) {
  capture->checkpoint_frame = checkpoint_frame;
  /* The payload version belongs to the backend channel that produced it: a
     logical name only chooses renderer state, never the canonical encoding.
     Recording the backend's version is what invalidates a stored baseline when
     that encoding changes. */
  capture->capture_version = channel->version;
  string_format(capture->channel, sizeof(capture->channel), "%s",
                logical_channel->name);
  /* The backend reports which resource it actually copied; the catalog name is
     only a fallback for a producer that did not name itself. */
  string_format(capture->producer_resource, sizeof(capture->producer_resource),
                "%s",
                item->producer_resource[0] ? item->producer_resource
                                           : channel->source_name);
  string_format(capture->source_format, sizeof(capture->source_format), "%s",
                vkr_harness_capture_format_name(item->format));
  string_format(capture->canonical_encoding,
                sizeof(capture->canonical_encoding), "%s",
                channel->canonical_encoding);
  string_format(capture->value_kind, sizeof(capture->value_kind), "%s",
                vkr_harness_capture_value_name(item->value_kind));
  string_format(capture->color_space, sizeof(capture->color_space), "%s",
                vkr_harness_capture_color_space_name(item->color_space));
  string_format(capture->origin, sizeof(capture->origin), "top_left");
  capture->width = item->width;
  capture->height = item->height;
  capture->source_row_pitch = item->row_pitch;
  capture->mip = item->mip;
  capture->layer = item->layer;
  capture->source_frame_index = poll->source_frame_index;
  capture->submit_serial = poll->submit_serial;
  capture->comparison.outcome = VKR_HARNESS_COMPARISON_NOT_RUN;
  string_format(capture->comparison_status, sizeof(capture->comparison_status),
                "not_run");
  string_format(capture->data_path, sizeof(capture->data_path), "captures/%s%s",
                stem, color ? ".png" : ".raw");
  string_format(capture->preview_path, sizeof(capture->preview_path),
                "captures/%s.png", stem);
  string_format(capture->metadata_path, sizeof(capture->metadata_path),
                "captures/%s.json", stem);
}

bool8_t vkr_harness_capture_publish(
    const char *run_dir, uint32_t checkpoint_frame,
    const VkrCapturePollResult *poll, const char logical_channels[][64],
    uint32_t logical_channel_count, const VkrHarnessArenas *arenas,
    VkrHarnessReport *report, VkrHarnessError *error) {
  if (!run_dir || !poll || poll->status != VKR_CAPTURE_STATUS_READY ||
      !poll->items || !logical_channels ||
      logical_channel_count != poll->item_count || !arenas || !report ||
      !report->captures) {
    return false_v;
  }
  char capture_dir[VKR_HARNESS_PATH_MAX];
  string_format(capture_dir, sizeof(capture_dir), "%s/captures", run_dir);
  FilePath directory = vkr_harness_file_path(capture_dir);
  if (!file_create_directory(&directory)) {
    vkr_harness_error_set(error, "capture.directory", "captures",
                          "Unable to create '%s'", capture_dir);
    return false_v;
  }

  for (uint32_t i = 0; i < poll->item_count; ++i) {
    const VkrCaptureItemResult *item = &poll->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    const VkrHarnessCaptureChannelDescription *logical_channel =
        vkr_harness_capture_channel_description(logical_channels[i]);
    const uint32_t uint_component =
        string_equals(logical_channels[i], "visibility_primitives") ||
                string_equals(logical_channels[i],
                              "transmission_visibility_primitives")
            ? 1u
            : 0u;
    const uint32_t uint_mask = uint_component == 1u ? 0x7fffffffu : UINT32_MAX;
    if (report->capture_count >= report->capture_capacity || !channel ||
        !logical_channel || !item->data) {
      vkr_harness_error_set(error, "capture.publish", "captures",
                            "Capture item %u cannot be published", i);
      return false_v;
    }
    VkrCaptureChannelDescription resolved_channel = *channel;
    if (item->color_space == VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR) {
      resolved_channel.canonical_encoding = "RGBA16_FLOAT_LE";
      resolved_channel.color_space = VKR_CAPTURE_COLOR_SPACE_EXTENDED_LINEAR;
      resolved_channel.version = 2u;
    }
    channel = &resolved_channel;
    /* Extended display captures retain their native FP16 values. */
    const bool8_t png_canonical =
        vkr_harness_capture_encoding_is_png(channel->canonical_encoding);
    const bool8_t rgba16f_canonical =
        vkr_harness_capture_encoding_is_rgba16f(channel->canonical_encoding);
    const bool8_t rg16f_canonical =
        vkr_harness_capture_encoding_is_rg16f(channel->canonical_encoding);
    if (rgba16f_canonical &&
        item->format != VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT) {
      vkr_harness_error_set(error, "capture.encoding", logical_channel->name,
                            "RGBA16_FLOAT_LE requires R16G16B16A16_SFLOAT");
      return false_v;
    }
    if (rg16f_canonical && item->format != VKR_TEXTURE_FORMAT_R16G16_SFLOAT) {
      vkr_harness_error_set(error, "capture.encoding", logical_channel->name,
                            "RG16_FLOAT_LE requires R16G16_SFLOAT");
      return false_v;
    }
    char stem[160];
    char png_path[VKR_HARNESS_PATH_MAX];
    char data_path[VKR_HARNESS_PATH_MAX];
    char metadata_path[VKR_HARNESS_PATH_MAX];
    string_format(stem, sizeof(stem), "frame_%06u_%s", checkpoint_frame,
                  logical_channel->name);
    string_format(png_path, sizeof(png_path), "%s/%s.png", capture_dir, stem);
    string_format(metadata_path, sizeof(metadata_path), "%s/%s.json",
                  capture_dir, stem);
    string_format(data_path, sizeof(data_path),
                  png_canonical ? "%s/%s.png" : "%s/%s.raw", capture_dir, stem);

    Scratch scratch = scratch_create(arenas->transient);
    const uint64_t preview_bytes = (uint64_t)item->width * item->height * 4u;
    const uint64_t canonical_bytes =
        (uint64_t)item->width * item->height * (rgba16f_canonical ? 8u : 4u);
    uint8_t *rgba =
        arena_alloc(arenas->transient, preview_bytes, ARENA_MEMORY_TAG_ARRAY);
    uint8_t *tight = png_canonical
                         ? rgba
                         : arena_alloc(arenas->transient, canonical_bytes,
                                       ARENA_MEMORY_TAG_ARRAY);
    float32_t preview_min = 0.0f;
    float32_t preview_max = 1.0f;
    bool8_t ok = rgba != NULL && tight != NULL;
    if (ok) {
      if (rg16f_canonical) {
        /* Preview preserves channel values as linear R/G; raw payload remains
         * the comparison source and never receives a color transform. */
        for (uint32_t y = 0; y < item->height; ++y) {
          const uint8_t *source = vkr_harness_capture_row(item, y);
          uint8_t *target = rgba + (uint64_t)y * item->width * 4u;
          for (uint32_t x = 0; x < item->width; ++x) {
            const uint8_t *texel = source + (uint64_t)x * 4u;
            const float32_t r = vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel));
            const float32_t g = vkr_harness_capture_half_to_float(
                vkr_harness_capture_read_u16(texel + 2u));
            target[x * 4u] = (uint8_t)(Clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            target[x * 4u + 1u] =
                (uint8_t)(Clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            target[x * 4u + 2u] = 0u;
            target[x * 4u + 3u] = 255u;
          }
        }
      } else if (item->value_kind == VKR_CAPTURE_VALUE_COLOR) {
        vkr_harness_capture_color_rgba(item, rgba);
      } else if (item->value_kind == VKR_CAPTURE_VALUE_DEPTH) {
        vkr_harness_capture_depth_preview(item, rgba, &preview_min,
                                          &preview_max);
      } else {
        vkr_harness_capture_uint_preview(item, uint_component, uint_mask, rgba);
      }
    }

    VkrHarnessCaptureResult *capture = &report->captures[report->capture_count];
    if (ok && !png_canonical) {
      if (rgba16f_canonical) {
        vkr_harness_capture_canonical_rgba16f(item, tight);
      } else if (rg16f_canonical) {
        vkr_harness_capture_canonical_rg16f(item, tight);
      } else {
        vkr_harness_capture_canonical_u32(item, uint_component, uint_mask,
                                          tight);
      }
      ok = vkr_harness_atomic_write(data_path, tight, canonical_bytes, error);
    }
    ok = ok && vkr_harness_capture_png_write(png_path, rgba, item->width,
                                             item->height, arenas, error);
    if (ok) {
      vkr_harness_capture_describe(capture, item, channel, logical_channel,
                                   poll, checkpoint_frame, stem, png_canonical);
      /* Digests of the payloads must exist before the sidecar quotes them. */
      ok = vkr_harness_sha256_file(data_path, capture->data_sha256) &&
           vkr_harness_sha256_file(png_path, capture->preview_sha256) &&
           vkr_harness_capture_write_metadata(metadata_path, capture, item,
                                              preview_min, preview_max) &&
           vkr_harness_sha256_file(metadata_path, capture->metadata_sha256) &&
           vkr_harness_report_add_artifact(
               report, png_canonical ? "capture.color" : "capture.raw",
               capture->data_path,
               png_canonical ? "image/png" : "application/octet-stream",
               data_path) &&
           (png_canonical ||
            vkr_harness_report_add_artifact(report, "capture.preview",
                                            capture->preview_path, "image/png",
                                            png_path)) &&
           vkr_harness_report_add_artifact(report, "capture.metadata",
                                           capture->metadata_path,
                                           "application/json", metadata_path);
    }
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    if (!ok) {
      /* Digest, sidecar, and artifact failures carry no reason of their own. */
      if (error && error->code[0] == '\0') {
        vkr_harness_error_set(error, "capture.publish", "captures",
                              "Unable to publish '%s'", stem);
      }
      return false_v;
    }
    report->capture_count++;
  }
  return true_v;
}

bool8_t vkr_harness_capture_summary_write(const char *path,
                                          const VkrHarnessReport *report,
                                          Arena *transient,
                                          VkrHarnessError *error) {
  if (!path || !report || !transient ||
      (report->capture_count && !report->captures) ||
      (report->artifact_count && !report->artifacts) ||
      report->capture_count > VKR_HARNESS_MAX_CAPTURE_RESULTS ||
      report->artifact_count > VKR_HARNESS_MAX_ARTIFACTS) {
    return false_v;
  }
  const uint64_t capture_bytes =
      (uint64_t)report->capture_count * sizeof(VkrHarnessCaptureResult);
  const uint64_t artifact_bytes =
      (uint64_t)report->artifact_count * sizeof(VkrHarnessArtifact);
  const uint64_t size = sizeof(VkrHarnessCaptureSummaryHeaderV14) +
                        capture_bytes + artifact_bytes;
  Scratch scratch = scratch_create(transient);
  uint8_t *bytes = arena_alloc(transient, size, ARENA_MEMORY_TAG_ARRAY);
  if (!bytes) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemZero(bytes, size);
  VkrHarnessCaptureSummaryHeaderV14 *header =
      (VkrHarnessCaptureSummaryHeaderV14 *)bytes;
  MemCopy(header->magic, s_capture_summary_magic, sizeof(header->magic));
  header->version = 14u;
  header->capture_count = report->capture_count;
  header->artifact_count = report->artifact_count;
  header->tool = (uint32_t)report->tool;
  header->exit_code = (uint32_t)report->exit_code;
  header->authoritative = report->authoritative;
  header->profile_compatible = report->profile_compatible;
  string_format(header->status, sizeof(header->status), "%s", report->status);
  string_format(header->case_id, sizeof(header->case_id), "%s",
                report->case_manifest.id);
  string_format(header->case_manifest_sha256,
                sizeof(header->case_manifest_sha256), "%s",
                report->case_manifest.manifest_sha256);
  string_format(header->profile_id, sizeof(header->profile_id), "%s",
                report->profile.id);
  string_format(header->profile_manifest_sha256,
                sizeof(header->profile_manifest_sha256), "%s",
                report->profile.manifest_sha256);
  string_format(header->environment_fingerprint,
                sizeof(header->environment_fingerprint), "%s",
                report->environment_fingerprint);
  string_format(header->workload_fingerprint,
                sizeof(header->workload_fingerprint), "%s",
                report->workload_fingerprint);
  string_format(header->policy_fingerprint, sizeof(header->policy_fingerprint),
                "%s", report->policy_fingerprint);
  header->case_manifest = report->case_manifest;
  header->profile = report->profile;
  header->provenance = report->provenance;
  if (capture_bytes) {
    MemCopy(bytes + sizeof(*header), report->captures, capture_bytes);
  }
  if (artifact_bytes) {
    MemCopy(bytes + sizeof(*header) + capture_bytes, report->artifacts,
            artifact_bytes);
  }
  const bool8_t ok = vkr_harness_atomic_write(path, bytes, size, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

bool8_t
vkr_harness_capture_summary_read(const char *path, Arena *arena,
                                 VkrHarnessCaptureSummary *out_summary) {
  if (!path || !arena || !out_summary) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_harness_read_file(path, arena, &bytes, &size) ||
      size < offsetof(VkrHarnessCaptureSummaryHeaderV2, case_manifest)) {
    return false_v;
  }
  const VkrHarnessCaptureSummaryHeaderV2 *common =
      (const VkrHarnessCaptureSummaryHeaderV2 *)bytes;
  if (MemCompare(common->magic, s_capture_summary_magic,
                 sizeof(common->magic)) != 0 ||
      (common->version != 2u && common->version != 3u &&
       common->version != 4u && common->version != 5u &&
       common->version != 6u && common->version != 7u &&
       common->version != 8u && common->version != 9u &&
       common->version != 10u && common->version != 11u &&
       common->version != 12u && common->version != 13u &&
       common->version != 14u) ||
      common->tool > VKR_HARNESS_TOOL_COMPARE ||
      common->exit_code > VKR_HARNESS_EXIT_ERROR ||
      common->capture_count > VKR_HARNESS_MAX_CAPTURE_RESULTS ||
      common->artifact_count > VKR_HARNESS_MAX_ARTIFACTS) {
    return false_v;
  }
  const uint64_t header_size =
      common->version == 2u    ? sizeof(VkrHarnessCaptureSummaryHeaderV2)
      : common->version == 3u  ? sizeof(VkrHarnessCaptureSummaryHeaderV3)
      : common->version == 4u  ? sizeof(VkrHarnessCaptureSummaryHeaderV4)
      : common->version == 5u  ? sizeof(VkrHarnessCaptureSummaryHeaderV5)
      : common->version == 6u  ? sizeof(VkrHarnessCaptureSummaryHeaderV6)
      : common->version == 7u  ? sizeof(VkrHarnessCaptureSummaryHeaderV7)
      : common->version == 8u  ? sizeof(VkrHarnessCaptureSummaryHeaderV8)
      : common->version == 9u  ? sizeof(VkrHarnessCaptureSummaryHeaderV9)
      : common->version == 10u ? sizeof(VkrHarnessCaptureSummaryHeaderV10)
      : common->version == 11u ? sizeof(VkrHarnessCaptureSummaryHeaderV11)
      : common->version == 12u ? sizeof(VkrHarnessCaptureSummaryHeaderV12)
      : common->version == 13u ? sizeof(VkrHarnessCaptureSummaryHeaderV13)
                               : sizeof(VkrHarnessCaptureSummaryHeaderV14);
  const uint64_t capture_bytes =
      (uint64_t)common->capture_count * sizeof(VkrHarnessCaptureResult);
  const uint64_t artifact_bytes =
      (uint64_t)common->artifact_count * sizeof(VkrHarnessArtifact);
  if (size != header_size + capture_bytes + artifact_bytes) {
    return false_v;
  }
  out_summary->captures =
      (const VkrHarnessCaptureResult *)(bytes + header_size);
  out_summary->tool = (VkrHarnessTool)common->tool;
  out_summary->exit_code = (VkrHarnessExitCode)common->exit_code;
  out_summary->authoritative = common->authoritative;
  out_summary->profile_compatible = common->profile_compatible;
  string_format(out_summary->status, sizeof(out_summary->status), "%s",
                common->status);
  string_format(out_summary->case_id, sizeof(out_summary->case_id), "%s",
                common->case_id);
  string_format(out_summary->case_manifest_sha256,
                sizeof(out_summary->case_manifest_sha256), "%s",
                common->case_manifest_sha256);
  string_format(out_summary->profile_id, sizeof(out_summary->profile_id), "%s",
                common->profile_id);
  string_format(out_summary->profile_manifest_sha256,
                sizeof(out_summary->profile_manifest_sha256), "%s",
                common->profile_manifest_sha256);
  string_format(out_summary->environment_fingerprint,
                sizeof(out_summary->environment_fingerprint), "%s",
                common->environment_fingerprint);
  string_format(out_summary->workload_fingerprint,
                sizeof(out_summary->workload_fingerprint), "%s",
                common->workload_fingerprint);
  string_format(out_summary->policy_fingerprint,
                sizeof(out_summary->policy_fingerprint), "%s",
                common->policy_fingerprint);
  if (common->version == 2u) {
    vkr_harness_case_from_v3(&common->case_manifest,
                             &out_summary->case_manifest);
    vkr_harness_profile_from_v2(&common->profile, &out_summary->profile);
    out_summary->provenance = common->provenance;
  } else if (common->version == 3u) {
    const VkrHarnessCaptureSummaryHeaderV3 *header =
        (const VkrHarnessCaptureSummaryHeaderV3 *)bytes;
    vkr_harness_case_from_v3(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 4u) {
    const VkrHarnessCaptureSummaryHeaderV4 *header =
        (const VkrHarnessCaptureSummaryHeaderV4 *)bytes;
    vkr_harness_case_from_v4(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 5u) {
    const VkrHarnessCaptureSummaryHeaderV5 *header =
        (const VkrHarnessCaptureSummaryHeaderV5 *)bytes;
    vkr_harness_case_from_v5(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 6u) {
    const VkrHarnessCaptureSummaryHeaderV6 *header =
        (const VkrHarnessCaptureSummaryHeaderV6 *)bytes;
    vkr_harness_case_from_v6(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 7u) {
    const VkrHarnessCaptureSummaryHeaderV7 *header =
        (const VkrHarnessCaptureSummaryHeaderV7 *)bytes;
    vkr_harness_case_from_v7(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 8u) {
    const VkrHarnessCaptureSummaryHeaderV8 *header =
        (const VkrHarnessCaptureSummaryHeaderV8 *)bytes;
    vkr_harness_case_from_v8(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 9u) {
    const VkrHarnessCaptureSummaryHeaderV9 *header =
        (const VkrHarnessCaptureSummaryHeaderV9 *)bytes;
    vkr_harness_case_from_v9(&header->case_manifest,
                             &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 10u) {
    const VkrHarnessCaptureSummaryHeaderV10 *header =
        (const VkrHarnessCaptureSummaryHeaderV10 *)bytes;
    vkr_harness_case_from_v10(&header->case_manifest,
                              &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 11u) {
    const VkrHarnessCaptureSummaryHeaderV11 *header =
        (const VkrHarnessCaptureSummaryHeaderV11 *)bytes;
    vkr_harness_case_from_v11(&header->case_manifest,
                              &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 12u) {
    const VkrHarnessCaptureSummaryHeaderV12 *header =
        (const VkrHarnessCaptureSummaryHeaderV12 *)bytes;
    vkr_harness_case_from_v12(&header->case_manifest,
                              &out_summary->case_manifest);
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else if (common->version == 13u) {
    const VkrHarnessCaptureSummaryHeaderV13 *header =
        (const VkrHarnessCaptureSummaryHeaderV13 *)bytes;
    MemZero(&out_summary->case_manifest, sizeof(out_summary->case_manifest));
    MemCopy(&out_summary->case_manifest, &header->case_manifest,
            sizeof(header->case_manifest));
    out_summary->case_manifest.asset_context = VKR_HARNESS_ASSET_CONTEXT_LEGACY;
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  } else {
    const VkrHarnessCaptureSummaryHeaderV14 *header =
        (const VkrHarnessCaptureSummaryHeaderV14 *)bytes;
    out_summary->case_manifest = header->case_manifest;
    out_summary->profile = header->profile;
    out_summary->provenance = header->provenance;
  }
  if (common->version <= 10u)
    string_copy(out_summary->case_manifest.renderer.display_output, "sdr");
  if (common->version <= 12u) {
    out_summary->case_manifest.renderer.motion_blur_enabled = false_v;
    out_summary->case_manifest.renderer.motion_blur_shutter_angle = 180.0f;
  }
  out_summary->capture_count = common->capture_count;
  out_summary->artifacts =
      (const VkrHarnessArtifact *)(bytes + header_size + capture_bytes);
  out_summary->artifact_count = common->artifact_count;
  return true_v;
}

#pragma once

#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_json_writer.h"
#include "core/vkr_metrics.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/vkr_bloom.h"
#include "renderer/vkr_gtao.h"
#include "renderer/vkr_renderer.h"

#define VKR_HARNESS_SCHEMA_VERSION 1u
/**
 * Bumped to 2 when warmup stopped advancing the camera. Version 1 evaluated the
 * authored path from frame 0, so warmup consumed the head of every script and
 * the measured window began mid-path. No version-1 measurement compares to a
 * version-2 one; the value is part of the workload fingerprint so they cannot.
 */
#define VKR_HARNESS_CAMERA_SCRIPT_VERSION 2u
#define VKR_HARNESS_PATH_MAX 1024u
/**
 * Paths recorded inside a report are relative to its run root, never absolute.
 * Sizing them separately keeps a report's artifact and capture tables one order
 * of magnitude smaller than the filesystem path limit would make them.
 */
#define VKR_HARNESS_RELATIVE_PATH_MAX 256u
/** `"sha256:"` + 64 lowercase hex digits + terminator. */
#define VKR_HARNESS_DIGEST_MAX 72u
#define VKR_HARNESS_ID_MAX 96u
#define VKR_HARNESS_TEXT_MAX 256u
#define VKR_HARNESS_MAX_CAMERA_KEYS 64u
#define VKR_HARNESS_MAX_CAPTURES 32u
#define VKR_HARNESS_MAX_CAPTURE_CHANNELS 16u
#define VKR_HARNESS_MAX_REPLAYS                                                \
  (VKR_HARNESS_MAX_CAPTURES * (VKR_HARNESS_MAX_CAPTURE_CHANNELS + 1u))
#define VKR_HARNESS_MAX_ASSERTIONS 64u
#define VKR_HARNESS_MAX_REQUIRED_METRICS 64u
#define VKR_HARNESS_MAX_RUNS 32u
#define VKR_HARNESS_MAX_AUTHORITY_REASONS 32u
/**
 * Upper bounds on the arena-backed report tables. Each writer requests the
 * capacity its command can actually reach; these only bound what a report or a
 * child summary file is allowed to claim.
 */
#define VKR_HARNESS_MAX_CAPTURE_RESULTS                                        \
  (VKR_HARNESS_MAX_CAPTURES * VKR_HARNESS_MAX_CAPTURE_CHANNELS)
/** Every published capture contributes canonical data, preview, and metadata.
 */
#define VKR_HARNESS_ARTIFACTS_PER_CAPTURE 3u
#define VKR_HARNESS_MAX_ARTIFACTS                                              \
  ((VKR_HARNESS_MAX_CAPTURE_RESULTS * VKR_HARNESS_ARTIFACTS_PER_CAPTURE) +     \
   VKR_HARNESS_MAX_CAPTURES + VKR_HARNESS_MAX_RUNS + 32u)
#define VKR_HARNESS_MAX_EVENTS 4096u
#define VKR_HARNESS_MAX_FINGERPRINT_FIELDS 256u
#define VKR_HARNESS_MAX_SCENE_ASSETS 2048u
#define VKR_HARNESS_FLY_LOOKUP_SUBDIVISIONS 32u
#define VKR_HARNESS_FLY_LOOKUP_MAX                                             \
  (((VKR_HARNESS_MAX_CAMERA_KEYS - 1u) *                                       \
    VKR_HARNESS_FLY_LOOKUP_SUBDIVISIONS) +                                     \
   1u)

/** Per-frame validity of one pass row in the raw harness sample stream. */
#define VKR_HARNESS_PASS_FLAG_CPU_VALID 0x1u
#define VKR_HARNESS_PASS_FLAG_GPU_VALID 0x2u
#define VKR_HARNESS_PASS_FLAG_CULLED 0x4u
#define VKR_HARNESS_PASS_FLAG_DISABLED 0x8u
#define VKR_HARNESS_PASS_FLAG_OMITTED 0x10u
#define VKR_HARNESS_PASS_FLAG_GPU_UNSUPPORTED_SCOPE 0x20u

typedef enum VkrHarnessExitCode {
  VKR_HARNESS_EXIT_PASS = 0,
  VKR_HARNESS_EXIT_FAIL = 1,
  VKR_HARNESS_EXIT_INVALID = 2,
  VKR_HARNESS_EXIT_UNAVAILABLE = 3,
  VKR_HARNESS_EXIT_MISSING_BASELINE = 4,
  VKR_HARNESS_EXIT_ERROR = 5,
} VkrHarnessExitCode;

typedef enum VkrHarnessTool {
  VKR_HARNESS_TOOL_PROFILE = 0,
  VKR_HARNESS_TOOL_SNAPSHOT,
  VKR_HARNESS_TOOL_AUTOTEST,
  VKR_HARNESS_TOOL_COMPARE,
} VkrHarnessTool;

typedef enum VkrHarnessBootProfile {
  VKR_HARNESS_BOOT_FULL = 0,
  VKR_HARNESS_BOOT_AUTOMATION,
} VkrHarnessBootProfile;

typedef enum VkrHarnessTarget {
  VKR_HARNESS_TARGET_WINDOWED_VISIBLE = 0,
  VKR_HARNESS_TARGET_WINDOWED_HIDDEN,
  VKR_HARNESS_TARGET_OFFSCREEN,
} VkrHarnessTarget;

typedef enum VkrHarnessPresentMode {
  VKR_HARNESS_PRESENT_IMMEDIATE = 0,
  VKR_HARNESS_PRESENT_FIFO,
  VKR_HARNESS_PRESENT_NONE,
  VKR_HARNESS_PRESENT_MAILBOX,
  /**
   * No repetition reported an actual presentation configuration. Appended last
   * so persisted sample headers keep their existing numeric values.
   */
  VKR_HARNESS_PRESENT_UNKNOWN,
} VkrHarnessPresentMode;

typedef enum VkrHarnessCacheMode {
  VKR_HARNESS_CACHE_ISOLATED_COLD = 0,
  VKR_HARNESS_CACHE_ISOLATED_WARM,
  VKR_HARNESS_CACHE_SHARED,
} VkrHarnessCacheMode;

typedef enum VkrHarnessCameraMode {
  VKR_HARNESS_CAMERA_STATIC = 0,
  VKR_HARNESS_CAMERA_KEYFRAMES,
  VKR_HARNESS_CAMERA_ORBIT,
  VKR_HARNESS_CAMERA_FLYTHROUGH,
} VkrHarnessCameraMode;

typedef enum VkrHarnessCameraInterpolation {
  VKR_HARNESS_CAMERA_INTERPOLATION_LINEAR = 0,
  VKR_HARNESS_CAMERA_INTERPOLATION_CATMULL_ROM,
} VkrHarnessCameraInterpolation;

typedef enum VkrHarnessSpeed {
  VKR_HARNESS_SPEED_SLOW = 0,
  VKR_HARNESS_SPEED_MEDIUM,
  VKR_HARNESS_SPEED_FAST,
} VkrHarnessSpeed;

typedef enum VkrHarnessAssertionOperator {
  VKR_HARNESS_ASSERT_MAX = 0,
  VKR_HARNESS_ASSERT_MIN,
  VKR_HARNESS_ASSERT_EQUALS,
} VkrHarnessAssertionOperator;

typedef enum VkrHarnessStatisticKind {
  VKR_HARNESS_STAT_MEAN = 0,
  VKR_HARNESS_STAT_P50,
  VKR_HARNESS_STAT_P95,
  VKR_HARNESS_STAT_MIN,
  VKR_HARNESS_STAT_MAX,
  VKR_HARNESS_STAT_STDDEV,
  VKR_HARNESS_STAT_TOTAL,

  VKR_HARNESS_STAT_COUNT,
} VkrHarnessStatisticKind;

/**
 * An assertion whose evidence is missing or partially invalid is neither a
 * pass nor a renderer regression; it makes the run incomplete.
 */
typedef enum VkrHarnessAssertionOutcome {
  VKR_HARNESS_ASSERTION_PASS = 0,
  VKR_HARNESS_ASSERTION_FAIL,
  VKR_HARNESS_ASSERTION_INCOMPLETE,
} VkrHarnessAssertionOutcome;

typedef struct VkrHarnessError {
  char code[64];
  char field[128];
  char message[VKR_HARNESS_TEXT_MAX];
} VkrHarnessError;

/**
 * The two lifetimes a harness process has.
 *
 * Everything a run produces — sample arrays, catalogs, statistics, the report
 * tables — lives until that process publishes its report, so it belongs in one
 * bump allocation released at exit. Intermediate buffers belong in a `Scratch`
 * on `transient` and are always released on both the success and failure path.
 *
 * These are raw arenas rather than tagged `VkrAllocator` allocations on
 * purpose: a child publishes `memory.cpu.*` from the process-global allocator
 * counters, and harness bookkeeping must not appear in the renderer memory it
 * reports.
 */
typedef struct VkrHarnessArenas {
  Arena *persistent;
  Arena *transient;
} VkrHarnessArenas;

typedef struct VkrHarnessCameraKey {
  float64_t time_seconds;
  Vec3 position;
  float32_t yaw_degrees;
  float32_t pitch_degrees;
} VkrHarnessCameraKey;

typedef struct VkrHarnessCameraPose {
  Vec3 position;
  float32_t yaw_degrees;
  float32_t pitch_degrees;
} VkrHarnessCameraPose;

typedef struct VkrHarnessCamera {
  VkrHarnessCameraMode mode;
  VkrHarnessCameraInterpolation interpolation;
  VkrHarnessSpeed speed;
  float32_t vertical_fov_degrees;
  float32_t near_plane;
  float32_t far_plane;
  VkrHarnessCameraPose static_pose;
  VkrHarnessCameraKey keys[VKR_HARNESS_MAX_CAMERA_KEYS];
  uint32_t key_count;
  Vec3 orbit_center;
  float32_t orbit_radius;
  float32_t orbit_height;
  float32_t orbit_revolutions;
  float64_t orbit_duration_seconds;
  float32_t orbit_start_angle_degrees;
  float32_t fly_lookup_lengths[VKR_HARNESS_FLY_LOOKUP_MAX];
  float32_t fly_lookup_parameters[VKR_HARNESS_FLY_LOOKUP_MAX];
  uint32_t fly_lookup_count;
  float32_t fly_total_length;
} VkrHarnessCamera;

typedef struct VkrHarnessRendererConfig {
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
      "do not clamp". */
  uint32_t ibl_probe_limit;
} VkrHarnessRendererConfig;

typedef struct VkrHarnessCompareConfig {
  float64_t max_pixel_delta;
  float64_t max_mean_absolute_error;
  float64_t max_failed_pixel_ratio;
  bool8_t emit_diff;
} VkrHarnessCompareConfig;

typedef struct VkrHarnessCapture {
  uint32_t at_frame;
  char channels[VKR_HARNESS_MAX_CAPTURE_CHANNELS][64];
  uint32_t channel_count;
  VkrHarnessCompareConfig compare;
} VkrHarnessCapture;

/**
 * One logically-named capture output. Several logical channels can read the
 * same backend resource under different renderer state — `normals` and `unlit`
 * are both `final_color` — so the logical name, not the backend channel,
 * identifies a row for baseline comparison. Channels that agree on
 * `replay_mode` are produced by one replay; each distinct mode costs a process.
 */
typedef struct VkrHarnessCaptureChannelDescription {
  const char *name;
  const char *direct_channel;
  const char *replay_mode;
  uint32_t render_mode;
  uint32_t shadow_debug_mode;
} VkrHarnessCaptureChannelDescription;

typedef struct VkrHarnessCaptureReplay {
  uint32_t capture_index;
  char mode[32];
  char logical_channels[VKR_HARNESS_MAX_CAPTURE_CHANNELS][64];
  char direct_channels[VKR_HARNESS_MAX_CAPTURE_CHANNELS][64];
  uint32_t channel_count;
  uint32_t render_mode;
  uint32_t shadow_debug_mode;
} VkrHarnessCaptureReplay;

typedef struct VkrHarnessAssertion {
  char metric[128];
  VkrHarnessStatisticKind statistic;
  VkrHarnessAssertionOperator operation;
  float64_t limit;
  float64_t tolerance;
} VkrHarnessAssertion;

typedef struct VkrHarnessCase {
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
} VkrHarnessCase;

typedef struct VkrHarnessProfile {
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
  bool8_t submission_gpu_timing;
  bool8_t event_subjects;
  uint32_t minimum_repetitions;
  uint32_t warmup_stability_window;
  char warmup_stability_metric[128];
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
} VkrHarnessProfile;

typedef struct VkrHarnessFingerprintField {
  char name[96];
  char value[VKR_HARNESS_TEXT_MAX];
} VkrHarnessFingerprintField;

typedef struct VkrHarnessSceneAsset {
  char path[VKR_HARNESS_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
  uint64_t size;
} VkrHarnessSceneAsset;

typedef struct VkrHarnessSceneManifest {
  VkrHarnessSceneAsset *assets;
  uint32_t asset_count;
  char scene[VKR_HARNESS_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
} VkrHarnessSceneManifest;

typedef struct VkrHarnessStatistics {
  uint64_t sample_count;
  uint64_t invalid_count;
  float64_t mean;
  float64_t p50;
  float64_t p95;
  float64_t min;
  float64_t max;
  float64_t stddev;
  float64_t total;
} VkrHarnessStatistics;

typedef struct VkrHarnessMetricResult {
  char name[128];
  char unit[24];
  VkrHarnessStatistics statistics;
} VkrHarnessMetricResult;

typedef struct VkrHarnessPassResult {
  char name[128];
  VkrHarnessStatistics cpu_ms;
  VkrHarnessStatistics gpu_ms;
  uint64_t culled_count;
  uint64_t disabled_count;
  uint64_t omitted_count;
  uint64_t gpu_unsupported_scope_count;
} VkrHarnessPassResult;

typedef struct VkrHarnessProvenance {
  char started_at[40];
  char ended_at[40];
  char git_sha[48];
  bool8_t dirty;
  char build_type[32];
  char compiler[128];
  char os[128];
  char cpu[128];
  char gpu[128];
  uint32_t gpu_vendor_id;
  uint32_t gpu_device_id;
  char driver[128];
  char power_mode[32];
  char thermal_state_start[32];
  char thermal_state_end[32];
  int32_t process_priority;
  char binary_sha256[VKR_HARNESS_DIGEST_MAX];
  char color_format[32];
  char depth_format[32];
  char color_space[32];
  char world_renderer[32];
  VkrHarnessTarget actual_target;
  VkrHarnessPresentMode actual_present;
  uint32_t actual_target_image_count;
  uint32_t actual_target_width;
  uint32_t actual_target_height;
} VkrHarnessProvenance;

typedef struct VkrHarnessRunReference {
  uint32_t index;
  char status[24];
  char report[VKR_HARNESS_RELATIVE_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
} VkrHarnessRunReference;

typedef struct VkrHarnessArtifact {
  char role[96];
  char path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char media_type[64];
  char sha256[VKR_HARNESS_DIGEST_MAX];
  char status[24];
} VkrHarnessArtifact;

typedef enum VkrHarnessComparisonOutcome {
  VKR_HARNESS_COMPARISON_NOT_RUN = 0,
  VKR_HARNESS_COMPARISON_PASS,
  VKR_HARNESS_COMPARISON_FAIL,
  VKR_HARNESS_COMPARISON_INCOMPATIBLE,
} VkrHarnessComparisonOutcome;

typedef struct VkrHarnessComparisonResult {
  VkrHarnessComparisonOutcome outcome;
  float64_t mean_absolute_error;
  float64_t max_absolute_error;
  uint64_t failing_value_count;
  uint64_t failing_pixel_count;
  uint64_t value_count;
  uint64_t pixel_count;
  float64_t failed_pixel_ratio;
} VkrHarnessComparisonResult;

typedef struct VkrHarnessCaptureResult {
  uint32_t checkpoint_frame;
  uint32_t capture_version;
  char channel[64];
  char producer_resource[64];
  char source_format[32];
  char canonical_encoding[32];
  char value_kind[24];
  char color_space[24];
  char origin[24];
  uint32_t width;
  uint32_t height;
  uint64_t source_row_pitch;
  uint32_t mip;
  uint32_t layer;
  uint64_t source_frame_index;
  uint64_t submit_serial;
  char data_path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char data_sha256[VKR_HARNESS_DIGEST_MAX];
  char preview_path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char preview_sha256[VKR_HARNESS_DIGEST_MAX];
  char metadata_path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char metadata_sha256[VKR_HARNESS_DIGEST_MAX];
  char comparison_status[24];
  char baseline_data_path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char baseline_data_sha256[VKR_HARNESS_DIGEST_MAX];
  char diff_path[VKR_HARNESS_RELATIVE_PATH_MAX];
  char diff_sha256[VKR_HARNESS_DIGEST_MAX];
  VkrHarnessCompareConfig thresholds;
  VkrHarnessComparisonResult comparison;
} VkrHarnessCaptureResult;

typedef struct VkrHarnessEvent {
  char source[128];
  char subject[VKR_METRIC_EVENT_SUBJECT_MAX + 1u];
  uint64_t start_ns;
  uint64_t duration_ns;
  uint64_t bytes;
  uint32_t thread_id;
  uint32_t repetition;
  bool8_t success;
  bool8_t subject_truncated;
} VkrHarnessEvent;

typedef struct VkrHarnessReport {
  VkrHarnessTool tool;
  char run_id[64];
  char status[24];
  VkrHarnessExitCode exit_code;
  bool8_t authoritative;
  char authority_reasons[VKR_HARNESS_MAX_AUTHORITY_REASONS][64];
  uint32_t authority_reason_count;
  VkrHarnessCase case_manifest;
  VkrHarnessProfile profile;
  bool8_t profile_compatible;
  char incompatibility_reasons[32][64];
  uint32_t incompatibility_reason_count;
  VkrHarnessProvenance provenance;
  char environment_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char workload_fingerprint[VKR_HARNESS_DIGEST_MAX];
  char policy_fingerprint[VKR_HARNESS_DIGEST_MAX];
  VkrSubsystemMask subsystem_mask;
  uint32_t requested_repetitions;
  uint32_t completed_repetitions;
  bool8_t warmup_stable;
  bool8_t gpu_lane_lock_acquired;
  VkrHarnessRunReference runs[VKR_HARNESS_MAX_RUNS];
  uint32_t run_count;
  VkrHarnessRunReference *auxiliary_runs;
  uint32_t auxiliary_run_count;
  uint32_t auxiliary_run_capacity;
  VkrHarnessMetricResult *metrics;
  uint32_t metric_count;
  VkrHarnessPassResult *passes;
  uint32_t pass_count;
  uint64_t events_dropped;
  uint64_t event_subjects_truncated;
  VkrHarnessEvent *events;
  uint32_t event_count;
  /* Arena-backed like `metrics`, `passes`, and `events`: a snapshot parent
     merges hundreds of child rows, and inlining them would put megabytes on the
     stack of every writer. Sized once by
     `vkr_harness_report_init_storage()`. */
  VkrHarnessCaptureResult *captures;
  uint32_t capture_count;
  uint32_t capture_capacity;
  VkrHarnessArtifact *artifacts;
  uint32_t artifact_count;
  uint32_t artifact_capacity;
} VkrHarnessReport;

void vkr_harness_error_clear(VkrHarnessError *error);
void vkr_harness_error_set(VkrHarnessError *error, const char *code,
                           const char *field, const char *format, ...);
void vkr_harness_stdout(const char *format, ...);
void vkr_harness_stderr(const char *format, ...);

/** A borrowed, non-allocating FilePath view over null-terminated path text. */
FilePath vkr_harness_file_path(const char *path);

/** Reads a file into a raw harness arena without allocator-metric pollution. */
bool8_t vkr_harness_read_file(const char *path, Arena *arena,
                              uint8_t **out_data, uint64_t *out_size);

bool8_t vkr_harness_case_parse(const char *json, uint64_t json_length,
                               const char *manifest_path,
                               VkrHarnessCase *out_case,
                               VkrHarnessError *out_error);
bool8_t vkr_harness_profile_parse(const char *json, uint64_t json_length,
                                  const char *manifest_path,
                                  VkrHarnessProfile *out_profile,
                                  VkrHarnessError *out_error);
bool8_t vkr_harness_case_load(const char *repository_root,
                              const char *relative_manifest_path,
                              VkrHarnessCase *out_case,
                              VkrHarnessError *out_error);

const VkrHarnessCaptureChannelDescription *
vkr_harness_capture_channel_description(const char *name);
bool8_t vkr_harness_capture_replays_build(const VkrHarnessCase *case_manifest,
                                          VkrHarnessCaptureReplay *out_replays,
                                          uint32_t capacity,
                                          uint32_t *out_count,
                                          VkrHarnessError *out_error);
bool8_t vkr_harness_capture_replay_find(const VkrHarnessCase *case_manifest,
                                        uint32_t capture_index,
                                        const char *mode,
                                        VkrHarnessCaptureReplay *out_replay,
                                        VkrHarnessError *out_error);
bool8_t vkr_harness_profile_load(const char *repository_root,
                                 const char *relative_manifest_path,
                                 VkrHarnessProfile *out_profile,
                                 VkrHarnessError *out_error);

bool8_t vkr_harness_camera_prepare(VkrHarnessCamera *camera,
                                   VkrHarnessError *out_error);
bool8_t vkr_harness_camera_evaluate(const VkrHarnessCamera *camera,
                                    float64_t authored_time_seconds,
                                    VkrHarnessCameraPose *out_pose);
/**
 * @brief Authored camera time for one case frame index.
 *
 * Warmup holds the authored start pose, so the measured window begins at
 * authored time zero and covers the head of the script rather than its tail.
 * Advancing the camera through warmup made the drift check in
 * vkr_harness_warmup_stable() measure the cost gradient of a moving view
 * instead of settling, which no moving-camera case could pass.
 *
 * `frame_index` counts warmup and measured frames together from zero.
 */
float64_t vkr_harness_camera_script_time(uint32_t frame_index,
                                         uint32_t warmup_frames,
                                         float64_t fixed_delta_seconds);
float64_t vkr_harness_speed_multiplier(VkrHarnessSpeed speed);

bool8_t vkr_harness_path_is_safe_relative(const char *path);
/** Resolves an existing path, following symlinks, bounded to the harness max.
 */
bool8_t vkr_harness_realpath(const char *path,
                             char out_path[VKR_HARNESS_PATH_MAX]);
bool8_t vkr_harness_resolve_existing_path(const char *root,
                                          const char *relative_path,
                                          char out_path[VKR_HARNESS_PATH_MAX],
                                          VkrHarnessError *out_error);
bool8_t vkr_harness_resolve_output_path(const char *root,
                                        const char *relative_path,
                                        char out_path[VKR_HARNESS_PATH_MAX],
                                        VkrHarnessError *out_error);
bool8_t vkr_harness_existing_path_is_below(const char *root, const char *path);
/**
 * Writes everything before `path`'s last separator. Fails on a path with no
 * separator rather than silently yielding the working directory.
 */
bool8_t vkr_harness_path_parent(const char *path,
                                char out_directory[VKR_HARNESS_PATH_MAX]);
/**
 * Accepts either a run directory or the `report.json` inside it, so every
 * command that names a prior run resolves the same directory. In-place.
 */
void vkr_harness_path_to_run_root(char *path);

/** Incremental SHA-256 so a hash input never needs a contiguous buffer. */
typedef struct VkrHarnessSha256 {
  uint32_t state[8];
  uint64_t bit_length;
  uint8_t block[64];
  uint32_t block_length;
} VkrHarnessSha256;

void vkr_harness_sha256_begin(VkrHarnessSha256 *hash);
void vkr_harness_sha256_update(VkrHarnessSha256 *hash, const void *data,
                               uint64_t length);
void vkr_harness_sha256_end(VkrHarnessSha256 *hash,
                            char out_digest[VKR_HARNESS_DIGEST_MAX]);
void vkr_harness_sha256_bytes(const void *data, uint64_t length,
                              char out_digest[VKR_HARNESS_DIGEST_MAX]);
bool8_t vkr_harness_sha256_file(const char *path,
                                char out_digest[VKR_HARNESS_DIGEST_MAX]);
bool8_t vkr_harness_sha256_file_sized(const char *path,
                                      char out_digest[VKR_HARNESS_DIGEST_MAX],
                                      uint64_t *out_size);
bool8_t vkr_harness_fingerprint(const VkrHarnessFingerprintField *fields,
                                uint32_t field_count,
                                char out_digest[VKR_HARNESS_DIGEST_MAX],
                                VkrHarnessError *out_error);
bool8_t vkr_harness_case_fingerprints(
    const char *repo_root, VkrHarnessTool tool,
    const VkrHarnessCase *case_manifest, const VkrHarnessProfile *profile,
    VkrSubsystemMask subsystem_mask,
    const VkrHarnessFingerprintField *environment_fields,
    uint32_t environment_field_count,
    char out_environment[VKR_HARNESS_DIGEST_MAX],
    char out_workload[VKR_HARNESS_DIGEST_MAX],
    char out_policy[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *out_error);
bool8_t vkr_harness_case_fingerprints_with_scene_digest(
    VkrHarnessTool tool, const VkrHarnessCase *case_manifest,
    const VkrHarnessProfile *profile, VkrSubsystemMask subsystem_mask,
    const VkrHarnessFingerprintField *environment_fields,
    uint32_t environment_field_count, const char *scene_content_digest,
    char out_environment[VKR_HARNESS_DIGEST_MAX],
    char out_workload[VKR_HARNESS_DIGEST_MAX],
    char out_policy[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *out_error);

bool8_t vkr_harness_scene_manifest_build(const char *repo_root,
                                         const char *scene, Arena *arena,
                                         VkrHarnessSceneManifest *out_manifest,
                                         VkrHarnessError *out_error);
bool8_t
vkr_harness_scene_manifest_write(const char *path,
                                 const VkrHarnessSceneManifest *manifest,
                                 VkrHarnessError *out_error);
bool8_t vkr_harness_scene_manifest_verify_file(const char *path,
                                               const char *expected_digest);
bool8_t vkr_harness_scene_manifest_publish(const char *repo_root,
                                           const char *scene,
                                           const char *run_root, Arena *arena,
                                           VkrHarnessReport *report,
                                           VkrHarnessError *out_error);

/** Resolves the case's boot profile and optional workload requirements. */
bool8_t vkr_harness_subsystem_plan(VkrHarnessTool tool,
                                   const VkrHarnessCase *case_manifest,
                                   VkrSubsystemPlan *out_plan,
                                   VkrHarnessError *out_error);

/** Resolves an optional case pin against the caller's environment request. */
bool8_t
vkr_harness_renderer_backend_resolve(const VkrHarnessRendererConfig *renderer,
                                     const char *environment_request,
                                     VkrRendererBackendType *out_backend);

/**
 * @return NULL when the case's workload can answer the profile's policy,
 *         otherwise a stable reason. A mismatch means the wrong instrument for
 *         the experiment, so it resolves to
 *         `VKR_HARNESS_EXIT_MISSING_BASELINE`.
 */
const char *
vkr_harness_case_profile_mismatch(const VkrHarnessCase *case_manifest,
                                  const VkrHarnessProfile *profile);

/** "0x" + 16 hexadecimal digits + terminator. */
#define VKR_HARNESS_SUBSYSTEM_MASK_MAX 19u

/**
 * The one producer of the canonical mask text. The report schema pins
 * `^0x[0-9a-f]{16}$` and the workload fingerprint hashes the same string, so a
 * second spelling would silently split comparison identity.
 */
void vkr_harness_format_subsystem_mask(
    char out_text[VKR_HARNESS_SUBSYSTEM_MASK_MAX], VkrSubsystemMask mask);

/**
 * @param sort_scratch Caller-owned buffer of at least `sample_count` values.
 *        Percentiles need a sorted copy, and the caller sizes that buffer once
 *        rather than paying an allocation per metric.
 */
bool8_t vkr_harness_statistics_compute(const float64_t *samples,
                                       uint64_t sample_count,
                                       uint64_t invalid_count,
                                       float64_t *sort_scratch,
                                       VkrHarnessStatistics *out_statistics);

/**
 * GPU timing is complete only when each measured pass/frame row is explicitly
 * culled, disabled, omitted by graph construction, or has matching valid
 * CPU/GPU samples.
 */
bool8_t vkr_harness_gpu_pass_samples_complete(const uint8_t *flags,
                                              uint64_t sample_count);

VkrHarnessComparisonResult vkr_harness_compare_rgba8(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba);
VkrHarnessComparisonResult vkr_harness_compare_f32_le(
    const uint8_t *actual, const uint8_t *baseline, uint64_t pixel_count,
    const VkrHarnessCompareConfig *config, uint8_t *diff_rgba);
VkrHarnessComparisonResult vkr_harness_compare_u32_le(const uint8_t *actual,
                                                      const uint8_t *baseline,
                                                      uint64_t pixel_count,
                                                      uint8_t *diff_rgba);
const char *
vkr_harness_comparison_outcome_name(VkrHarnessComparisonOutcome outcome);

const char *vkr_harness_tool_name(VkrHarnessTool tool);
/** The report `status` string every exit code is published under. */
const char *vkr_harness_exit_code_name(VkrHarnessExitCode exit_code);
const char *vkr_harness_target_name(VkrHarnessTarget target);
const char *vkr_harness_present_name(VkrHarnessPresentMode present);
const char *vkr_harness_cache_name(VkrHarnessCacheMode cache);
const char *vkr_harness_boot_name(VkrHarnessBootProfile boot);
const char *vkr_harness_statistic_name(VkrHarnessStatisticKind statistic);
const char *vkr_harness_operator_name(VkrHarnessAssertionOperator operation);
bool8_t vkr_harness_statistic_from_name(const char *name,
                                        VkrHarnessStatisticKind *out_statistic);

float64_t vkr_harness_statistic_value(const VkrHarnessStatistics *statistics,
                                      VkrHarnessStatisticKind statistic);
const VkrHarnessMetricResult *
vkr_harness_report_find_metric(const VkrHarnessReport *report,
                               const char *name);
/**
 * Single verdict source for one assertion. The runner's exit code and the
 * per-assertion status written into the report must never disagree, so both
 * read this.
 */
VkrHarnessAssertionOutcome
vkr_harness_assertion_evaluate(const VkrHarnessReport *report,
                               const VkrHarnessAssertion *assertion,
                               float64_t *out_actual);
const char *
vkr_harness_assertion_outcome_name(VkrHarnessAssertionOutcome outcome);

bool8_t vkr_harness_make_directories(const char *path,
                                     VkrHarnessError *out_error);
bool8_t vkr_harness_atomic_write(const char *path, const void *data,
                                 uint64_t length, VkrHarnessError *out_error);
bool8_t vkr_harness_generate_run_id(char out_run_id[64]);
/**
 * Creates a fresh `<artifact_root>/<run id>` directory, retrying on the
 * millisecond collision that two runs started in the same tick would produce.
 * Exclusive creation is the claim: no two runs can ever share a directory.
 */
bool8_t vkr_harness_create_run_root(const char *artifact_root,
                                    char out_run_id[64],
                                    char out_run_root[VKR_HARNESS_PATH_MAX]);
bool8_t vkr_harness_timestamp_utc(char out_timestamp[40]);
bool8_t vkr_harness_report_write(const char *path,
                                 const VkrHarnessReport *report,
                                 VkrHarnessError *out_error);
bool8_t vkr_harness_summary_csv_write(const char *path,
                                      const VkrHarnessReport *report,
                                      Arena *transient,
                                      VkrHarnessError *out_error);

/**
 * Sizes the report's arena-backed capture and artifact tables.
 *
 * Must be called before the first `vkr_harness_report_add_artifact()` or
 * capture publication; both refuse to write past the requested capacity rather
 * than silently truncating evidence. Capacities are clamped to
 * `VKR_HARNESS_MAX_CAPTURE_RESULTS` and `VKR_HARNESS_MAX_ARTIFACTS`.
 */
bool8_t vkr_harness_report_init_storage(VkrHarnessReport *report, Arena *arena,
                                        uint32_t capture_capacity,
                                        uint32_t artifact_capacity);
bool8_t vkr_harness_report_init_auxiliary_runs(VkrHarnessReport *report,
                                               Arena *arena, uint32_t capacity);

/**
 * Fills a run reference's three comparison fingerprints from the report it
 * points at.
 *
 * A published report routinely holds several times the JSON parser's fixed
 * token budget, so this scans the serialized text rather than parsing it. The
 * scan is bounded to the top-level `comparison` object, which is why a nested
 * run reference cannot answer in the report's place.
 */
bool8_t
vkr_harness_report_read_fingerprints(const char *report_path, Arena *transient,
                                     VkrHarnessRunReference *out_reference);

/** Records why the run may not be used as evidence; always clears authority. */
void vkr_harness_report_add_authority_reason(VkrHarnessReport *report,
                                             const char *reason);
/** Records a case/profile mismatch; also clears authority. */
void vkr_harness_report_add_incompatibility(VkrHarnessReport *report,
                                            const char *reason);
/** Digests an existing file and records it under a repository-safe path. */
bool8_t vkr_harness_report_add_artifact(VkrHarnessReport *report,
                                        const char *role,
                                        const char *relative_path,
                                        const char *media_type,
                                        const char *absolute_path);
void vkr_harness_report_set_status(VkrHarnessReport *report, const char *status,
                                   VkrHarnessExitCode exit_code);
/** Terminal "evidence is missing" outcome: status, exit code, and reason. */
void vkr_harness_report_mark_incomplete(VkrHarnessReport *report,
                                        const char *reason);
/** Terminal "the environment is not the requested one" outcome. */
void vkr_harness_report_mark_unavailable(VkrHarnessReport *report,
                                         const char *reason);

/**
 * Named-member shorthand over `core/vkr_json_writer.h` for the C strings the
 * harness stores. Every document the harness emits goes through these, so no
 * writer hand-builds JSON into a fixed buffer.
 */
bool8_t vkr_harness_json_emit_name(VkrJsonWriter *writer, const char *name);
bool8_t vkr_harness_json_emit_string(VkrJsonWriter *writer, const char *name,
                                     const char *value);
bool8_t vkr_harness_json_emit_u64(VkrJsonWriter *writer, const char *name,
                                  uint64_t value);
bool8_t vkr_harness_json_emit_i64(VkrJsonWriter *writer, const char *name,
                                  int64_t value);
bool8_t vkr_harness_json_emit_f64(VkrJsonWriter *writer, const char *name,
                                  float64_t value);
bool8_t vkr_harness_json_emit_bool(VkrJsonWriter *writer, const char *name,
                                   bool8_t value);

#pragma once

#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_metrics.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/vkr_renderer.h"

#define VKR_HARNESS_SCHEMA_VERSION 1u
#define VKR_HARNESS_CAMERA_SCRIPT_VERSION 1u
#define VKR_HARNESS_PATH_MAX 1024u
/** `"sha256:"` + 64 lowercase hex digits + terminator. */
#define VKR_HARNESS_DIGEST_MAX 72u
#define VKR_HARNESS_ID_MAX 96u
#define VKR_HARNESS_TEXT_MAX 256u
#define VKR_HARNESS_MAX_CAMERA_KEYS 64u
#define VKR_HARNESS_MAX_CAPTURES 32u
#define VKR_HARNESS_MAX_CAPTURE_CHANNELS 16u
#define VKR_HARNESS_MAX_ASSERTIONS 64u
#define VKR_HARNESS_MAX_REQUIRED_METRICS 64u
#define VKR_HARNESS_MAX_RUNS 32u
#define VKR_HARNESS_MAX_AUTHORITY_REASONS 32u
#define VKR_HARNESS_MAX_ARTIFACTS 160u
#define VKR_HARNESS_MAX_EVENTS 4096u
#define VKR_HARNESS_MAX_FINGERPRINT_FIELDS 256u
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
  char shadow_preset[32];
  uint32_t shadow_cascades;
  char render_mode[16];
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
} VkrHarnessProfile;

typedef struct VkrHarnessFingerprintField {
  char name[96];
  char value[VKR_HARNESS_TEXT_MAX];
} VkrHarnessFingerprintField;

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
  char color_space[32];
  VkrHarnessPresentMode actual_present;
  uint32_t actual_target_image_count;
} VkrHarnessProvenance;

typedef struct VkrHarnessRunReference {
  uint32_t index;
  char status[24];
  char report[VKR_HARNESS_PATH_MAX];
  char sha256[VKR_HARNESS_DIGEST_MAX];
} VkrHarnessRunReference;

typedef struct VkrHarnessArtifact {
  char role[96];
  char path[VKR_HARNESS_PATH_MAX];
  char media_type[64];
  char sha256[VKR_HARNESS_DIGEST_MAX];
  char status[24];
} VkrHarnessArtifact;

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
  VkrHarnessMetricResult *metrics;
  uint32_t metric_count;
  VkrHarnessPassResult *passes;
  uint32_t pass_count;
  uint64_t events_dropped;
  uint64_t event_subjects_truncated;
  VkrHarnessEvent *events;
  uint32_t event_count;
  VkrHarnessArtifact artifacts[VKR_HARNESS_MAX_ARTIFACTS];
  uint32_t artifact_count;
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
bool8_t vkr_harness_profile_load(const char *repository_root,
                                 const char *relative_manifest_path,
                                 VkrHarnessProfile *out_profile,
                                 VkrHarnessError *out_error);

bool8_t vkr_harness_camera_prepare(VkrHarnessCamera *camera,
                                   VkrHarnessError *out_error);
bool8_t vkr_harness_camera_evaluate(const VkrHarnessCamera *camera,
                                    float64_t authored_time_seconds,
                                    VkrHarnessCameraPose *out_pose);
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
bool8_t vkr_harness_fingerprint(const VkrHarnessFingerprintField *fields,
                                uint32_t field_count,
                                char out_digest[VKR_HARNESS_DIGEST_MAX],
                                VkrHarnessError *out_error);
bool8_t vkr_harness_case_fingerprints(
    VkrHarnessTool tool, const VkrHarnessCase *case_manifest,
    const VkrHarnessProfile *profile, VkrSubsystemMask subsystem_mask,
    const VkrHarnessFingerprintField *environment_fields,
    uint32_t environment_field_count,
    char out_environment[VKR_HARNESS_DIGEST_MAX],
    char out_workload[VKR_HARNESS_DIGEST_MAX],
    char out_policy[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *out_error);

/** Resolves the case's boot profile and optional workload requirements. */
bool8_t vkr_harness_subsystem_plan(VkrHarnessTool tool,
                                   const VkrHarnessCase *case_manifest,
                                   VkrSubsystemPlan *out_plan,
                                   VkrHarnessError *out_error);

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
 * culled/disabled or has matching valid CPU/GPU samples.
 */
bool8_t vkr_harness_gpu_pass_samples_complete(const uint8_t *flags,
                                              uint64_t sample_count);

const char *vkr_harness_tool_name(VkrHarnessTool tool);
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
bool8_t vkr_harness_timestamp_utc(char out_timestamp[40]);
bool8_t vkr_harness_report_write(const char *path,
                                 const VkrHarnessReport *report,
                                 VkrHarnessError *out_error);
bool8_t vkr_harness_summary_csv_write(const char *path,
                                      const VkrHarnessReport *report,
                                      Arena *transient,
                                      VkrHarnessError *out_error);

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

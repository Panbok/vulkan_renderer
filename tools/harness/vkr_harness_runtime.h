/**
 * @file vkr_harness_runtime.h
 * @brief Internal seam between the `vkr_harness` parent and its child
 *        repetition processes.
 *
 * The parent orchestrates isolated child processes and aggregates their
 * evidence; only a child opens a renderer target and produces samples.
 * Everything they must agree on — the raw sample file layout, provenance
 * collection, and the current support boundary — lives here rather than in
 * either side.
 */
#pragma once

#include "vkr_harness.h"

/** Bumped with VKR_HARNESS_SCHEMA_VERSION; a parent rejects any other magic. */
#define VKR_HARNESS_SAMPLE_MAGIC "VKRSMP1"

/** Repository-relative root every `profile` run directory is created under. */
#define VKR_HARNESS_ARTIFACT_ROOT "build/_artifacts/profile"

/** `VkrHarnessSampleFileHeader.flags`. */
#define VKR_HARNESS_SAMPLE_FLAG_WARMUP_STABLE 0x1u
#define VKR_HARNESS_SAMPLE_FLAG_CHILD_FAILED 0x2u

/**
 * Fixed-layout header of `runs/<n>/samples.bin`. Written and read by one build
 * of one binary within a single parent invocation, so the payload is raw
 * host-endian records rather than a portable encoding.
 */
typedef struct VkrHarnessSampleFileHeader {
  char magic[8];
  uint32_t schema_version;
  uint32_t metric_count;
  uint32_t pass_count;
  uint32_t event_count;
  uint64_t events_dropped;
  uint64_t event_subjects_truncated;
  uint64_t snapshot_publications_dropped;
  uint64_t subsystem_mask;
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t actual_present;
  uint32_t actual_target;
  uint32_t actual_image_count;
  uint32_t actual_width;
  uint32_t actual_height;
  uint32_t actual_render_width;
  uint32_t actual_render_height;
  uint32_t gpu_vendor_id;
  uint32_t gpu_device_id;
  uint32_t flags;
  char gpu[128];
  char driver[128];
  char color_format[32];
  char depth_format[32];
  char color_space[32];
  char world_renderer[32];
} VkrHarnessSampleFileHeader;

typedef struct VkrHarnessSampleMetric {
  char name[128];
  char unit[24];
} VkrHarnessSampleMetric;

typedef struct VkrHarnessSamplePass {
  char name[128];
} VkrHarnessSamplePass;

typedef struct VkrHarnessSampleEvent {
  char source[128];
  char subject[VKR_METRIC_EVENT_SUBJECT_MAX + 1u];
  uint64_t start_ns;
  uint64_t duration_ns;
  uint64_t bytes;
  uint32_t thread_id;
  uint32_t status;
  bool8_t subject_truncated;
} VkrHarnessSampleEvent;

/**
 * One repetition's raw evidence. All arrays are indexed
 * `frame * <count> + <index>` over `warmup_frames + measure_frames` frames.
 * On read, every pointer aliases the persistent arena block holding the file
 * bytes and stays valid until that arena is destroyed.
 */
typedef struct VkrHarnessSampleSet {
  VkrHarnessSampleFileHeader header;
  const VkrHarnessSampleMetric *metrics;
  const float64_t *values;
  const uint8_t *availability;
  const VkrHarnessSamplePass *passes;
  const float64_t *pass_cpu_ms;
  const float64_t *pass_gpu_ms;
  const uint8_t *pass_flags;
  const VkrHarnessSampleEvent *events;
} VkrHarnessSampleSet;

/**
 * Returns whether a metric is produced from the current packet/frame rather
 * than from an asynchronously completed GPU result. Pure submission-timing
 * profiles use this subset for repetition determinism because their
 * completion-owned draw telemetry is not source-frame-indexed.
 */
bool8_t vkr_harness_metric_is_current_frame_work(const char *name);

bool8_t vkr_harness_samples_write(const char *path,
                                  const VkrHarnessSampleFileHeader *header,
                                  const VkrHarnessSampleSet *samples,
                                  Arena *transient, VkrHarnessError *out_error);
/**
 * Rejects any file whose magic, schema version, frame counts, or total length
 * disagrees with what `case_manifest` requires, so a truncated or stale child
 * artifact can never be aggregated. The result borrows `persistent`.
 */
bool8_t vkr_harness_samples_read(const char *path,
                                 const VkrHarnessCase *case_manifest,
                                 Arena *persistent,
                                 VkrHarnessSampleSet *out_samples);

/**
 * Statistics over the measured window only; warmup frames are skipped. Samples
 * that are not VKR_METRIC_AVAILABILITY_VALID are counted as invalid rather
 * than folded in as zeros.
 *
 * Results outlive the call and come from `arenas->persistent`; the sort and
 * gather buffers are scoped to `arenas->transient`.
 */
bool8_t vkr_harness_compute_metric_results(
    const VkrHarnessArenas *arenas, uint32_t warmup_frames,
    uint32_t measure_frames, uint32_t metric_count,
    const VkrHarnessSampleMetric *catalog, const float64_t *values,
    const uint8_t *availability, VkrHarnessMetricResult **out_metrics,
    VkrHarnessError *out_error);

bool8_t vkr_harness_compute_pass_results(
    const VkrHarnessArenas *arenas, uint32_t warmup_frames,
    uint32_t measure_frames, uint32_t pass_count,
    const VkrHarnessSamplePass *catalog, const float64_t *cpu_ms,
    const float64_t *gpu_ms, const uint8_t *flags,
    VkrHarnessPassResult **out_passes, VkrHarnessError *out_error);

/** Build type, compiler, OS/CPU, power/thermal, and process priority. */
void vkr_harness_provenance_system(VkrHarnessProvenance *provenance);
/**
 * Writes a provenance string, substituting "unknown" for an empty source.
 * Report consumers distinguish "not observed" from a value; they cannot
 * distinguish it from an empty string.
 */
void vkr_harness_provenance_set_text(char *field, uint64_t capacity,
                                     const char *value);
/** The above plus git HEAD, worktree cleanliness, and the binary digest. */
void vkr_harness_provenance_collect(const char *executable,
                                    const char *repo_root,
                                    VkrHarnessProvenance *provenance);
#define VKR_HARNESS_ENVIRONMENT_FIELD_COUNT 14u

/**
 * Environment fingerprint inputs. Parent and child must project the same
 * fields or their comparison identities diverge for one observation.
 *
 * @return the number of fields written, always
 *         VKR_HARNESS_ENVIRONMENT_FIELD_COUNT.
 */
uint32_t vkr_harness_environment_fields(
    const VkrHarnessProvenance *provenance, bool8_t exclusive_gpu_lane,
    VkrHarnessFingerprintField fields[VKR_HARNESS_ENVIRONMENT_FIELD_COUNT]);

int vkr_harness_child_run(const char *executable, const char *repo_root,
                          const char *case_path, const char *profile_path,
                          const char *run_dir, bool8_t prewarm,
                          int32_t capture_index, const char *replay_mode,
                          const char *scene_content_digest);
int vkr_harness_profile_run(const char *executable, const char *repo_root,
                            const char *case_path, const char *profile_path,
                            const char *artifact_root_override);
int vkr_harness_snapshot_run(const char *executable, const char *repo_root,
                             const char *case_path, const char *profile_path,
                             const char *artifact_root_override,
                             bool8_t cross_backend);
int vkr_harness_autotest_run(const char *executable, const char *repo_root,
                             const char *case_path, const char *profile_path);
bool8_t vkr_harness_capture_publish(
    const char *run_dir, uint32_t checkpoint_frame,
    const VkrCapturePollResult *poll, const char logical_channels[][64],
    uint32_t logical_channel_count, const VkrHarnessArenas *arenas,
    VkrHarnessReport *report, VkrHarnessError *error);
bool8_t vkr_harness_capture_png_write(const char *path, const uint8_t *rgba,
                                      uint32_t width, uint32_t height,
                                      const VkrHarnessArenas *arenas,
                                      VkrHarnessError *error);
VkrHarnessExitCode vkr_harness_compare_capture_sets(
    const char *actual_root, const char *baseline_root,
    VkrHarnessCaptureResult *actual, uint32_t actual_count,
    const VkrHarnessCaptureResult *baseline, uint32_t baseline_count,
    const VkrHarnessArenas *arenas, VkrHarnessError *error);
/**
 * Registers every diff image a comparison produced. Diffs are written beside
 * the captures they explain, so `run_root` is the report's own run root.
 */
void vkr_harness_compare_publish_diffs(VkrHarnessReport *report,
                                       const char *run_root);
int vkr_harness_baseline_propose(const char *repo_root, const char *from_run,
                                 const char *actor, const char *reason);
int vkr_harness_baseline_accept(const char *repo_root, const char *plan_path,
                                const char *confirmation);
int vkr_harness_compare_run(const char *repo_root, const char *run_path,
                            bool8_t cross_backend);

typedef struct VkrHarnessCaptureSummary {
  VkrHarnessTool tool;
  char status[24];
  VkrHarnessExitCode exit_code;
  bool8_t authoritative;
  bool8_t profile_compatible;
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
  const VkrHarnessCaptureResult *captures;
  uint32_t capture_count;
  const VkrHarnessArtifact *artifacts;
  uint32_t artifact_count;
} VkrHarnessCaptureSummary;

/**
 * Returns NULL when an accepted baseline may be compared with a snapshot.
 * Cross-backend mode drops only environment identity. It still requires the
 * same workload and policy, a backend-neutral case, and a different recorded
 * environment.
 */
const char *
vkr_harness_baseline_incompatibility(const VkrHarnessCaptureSummary *actual,
                                     const VkrHarnessCaptureSummary *baseline,
                                     bool8_t cross_backend);

bool8_t vkr_harness_baseline_current(const char *repo_root,
                                     const char *profile_id,
                                     const char *case_id, Arena *arena,
                                     char generation_root[VKR_HARNESS_PATH_MAX],
                                     VkrHarnessCaptureSummary *summary,
                                     VkrHarnessError *error);

/** Compact parent/child transport for snapshot metadata, not a public artifact.
 */
bool8_t vkr_harness_capture_summary_write(const char *path,
                                          const VkrHarnessReport *report,
                                          Arena *transient,
                                          VkrHarnessError *error);
bool8_t vkr_harness_capture_summary_read(const char *path, Arena *arena,
                                         VkrHarnessCaptureSummary *out_summary);

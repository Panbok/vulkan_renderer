/**
 * @file vkr_harness_runtime.h
 * @brief Internal seam between the `vkr_harness` parent and its child
 *        repetition processes.
 *
 * The parent orchestrates isolated child processes and aggregates their
 * evidence; only a child links the renderer and produces samples. Everything
 * they must agree on — the raw sample file layout, provenance collection, and
 * the Phase-2 support boundary — lives here rather than in either side.
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

/** Per-frame validity of one pass row; see vkr_harness_child_sample(). */
#define VKR_HARNESS_PASS_FLAG_CPU_VALID 0x1u
#define VKR_HARNESS_PASS_FLAG_GPU_VALID 0x2u
#define VKR_HARNESS_PASS_FLAG_CULLED 0x4u
#define VKR_HARNESS_PASS_FLAG_DISABLED 0x8u

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
  uint32_t warmup_frames;
  uint32_t measure_frames;
  uint32_t actual_present;
  uint32_t actual_image_count;
  uint32_t gpu_vendor_id;
  uint32_t gpu_device_id;
  uint32_t flags;
  char gpu[128];
  char driver[128];
  char color_format[32];
  char color_space[32];
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

/** Reads a whole file into `arena`; the caller controls its lifetime. */
bool8_t vkr_harness_read_file(const char *path, Arena *arena,
                              uint8_t **out_data, uint64_t *out_length);

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
#define VKR_HARNESS_ENVIRONMENT_FIELD_COUNT 12u

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

/**
 * @return NULL when Phase 2 can run this case/profile pair, otherwise a stable
 *         reason string.
 */
const char *vkr_harness_phase2_unsupported(const VkrHarnessCase *case_manifest,
                                           const VkrHarnessProfile *profile);

int vkr_harness_child_run(const char *executable, const char *repo_root,
                          const char *case_path, const char *profile_path,
                          const char *run_dir, bool8_t prewarm);
int vkr_harness_profile_run(const char *executable, const char *repo_root,
                            const char *case_path, const char *profile_path);

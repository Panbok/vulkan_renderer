#pragma once

#include "defines.h"
#include "renderer/vkr_temporal.h"

/** Default manual camera exposure for HDR scene presentation. */
#define VKR_DEFAULT_EXPOSURE 0.30f

/**
 * @brief Which exposure the frame applies.
 *
 * Manual is the pre-version-20 behaviour: one linear multiplier applied before
 * the tonemap curve. Automatic meters the scene on the GPU and derives the
 * multiplier from log-luminance percentiles; `manual_exposure` remains the
 * fallback when an invalid chain has no accepted luminance.
 */
typedef enum VkrExposureMode {
  VKR_EXPOSURE_MODE_MANUAL = 0u,
  VKR_EXPOSURE_MODE_AUTOMATIC = 1u,
  VKR_EXPOSURE_MODE_COUNT,
} VkrExposureMode;

/**
 * @brief Exposure-only discontinuities.
 *
 * Bits 0-7 are `VkrTemporalResetReason` and mean exactly what they mean there.
 * Exposure and temporal reconstruction share one definition of a scene
 * discontinuity, so the reasons are shared rather than re-derived; deriving
 * camera-cut geometry twice is how the two definitions would drift apart.
 * Exposure deliberately ignores `VKR_TEMPORAL_RESET_PROJECTION_CHANGE`: a lens
 * change does not change how bright the scene is.
 */
typedef enum VkrExposureResetReason {
  VKR_EXPOSURE_RESET_MODE_CHANGE = 1u << 8u,
} VkrExposureResetReason;

/** Temporal reasons that also invalidate an exposure adaptation chain. */
#define VKR_EXPOSURE_RESET_TEMPORAL_MASK                                       \
  ((uint32_t)(VKR_TEMPORAL_RESET_FIRST_FRAME | VKR_TEMPORAL_RESET_FRAME_GAP |  \
              VKR_TEMPORAL_RESET_EXTENT_CHANGE |                               \
              VKR_TEMPORAL_RESET_SCENE_CHANGE |                                \
              VKR_TEMPORAL_RESET_CAMERA_CUT | VKR_TEMPORAL_RESET_EXPLICIT))

/** Longest adaptation step a single frame may take, in seconds. */
#define VKR_EXPOSURE_MAX_DELTA_SECONDS 0.25f

/**
 * Histogram bin count.
 *
 * Compile-time rather than authored: the metering kernels reduce through a
 * group-shared bin array, whose size must be a constant in both Slang and MSL.
 * One constant is shared by the CPU reference, both shaders, and the graph
 * buffer size, so a mismatch is a compile error rather than a silent misread.
 */
#define VKR_EXPOSURE_HISTOGRAM_BIN_COUNT 256u

/**
 * @brief Cold metering configuration.
 *
 * Normalized once at renderer initialization and never per frame. Every value
 * the GPU metering passes trust is proven here, so the histogram and resolve
 * shaders contain no range, ordering, or finiteness guard.
 *
 * Luminance bounds are log2 of scene-linear luminance. `low_percentile` and
 * `high_percentile` clip the resolved histogram before the weighted log-average
 * so a few hot texels or a large letterbox region cannot own the result.
 */
typedef struct VkrExposureMeteringConfig {
  /**
   * Histogram bin count. Normalizes to VKR_EXPOSURE_HISTOGRAM_BIN_COUNT; it is
   * recorded here because the metering contract is meaningless without it, not
   * because it is selectable.
   */
  uint32_t histogram_bin_count;
  float32_t min_log_luminance;
  float32_t max_log_luminance;
  /** Retained fraction bounds, `0 <= low < high <= 1`. */
  float32_t low_percentile;
  float32_t high_percentile;
  /** Scene-linear luminance that maps to the middle of the tonemap curve. */
  float32_t middle_gray;
  /** Clamp applied to the resolved target EV before adaptation. */
  float32_t min_ev;
  float32_t max_ev;
  /**
   * Maximum EV change per second, named for the direction the displayed
   * image
   * moves: `brighten` applies when the target exposure is above the
   * adapted
   * exposure.
   */
  float32_t brighten_rate_per_second;
  float32_t darken_rate_per_second;
  /** Scene-linear luminance below which a texel is treated as background. */
  float32_t min_luminance;
} VkrExposureMeteringConfig;

/**
 * @brief Committed exposure history description.
 *
 * The adapted exposure itself is deliberately absent. It is a GPU value that is
 * never read back for the tonemap pass, so the CPU only tracks whether the
 * chain the GPU is accumulating is still the one this frame belongs to. Every
 * other discontinuity already arrives through `temporal_reset_reasons`.
 */
typedef struct VkrExposureState {
  uint32_t mode;
  bool8_t valid;
} VkrExposureState;

/** Per-frame exposure inputs assembled at the frontend boundary. */
typedef struct VkrExposureFrameInput {
  uint32_t mode;
  /** Validated linear multiplier; manual output and automatic fallback. */
  float32_t manual_exposure;
  float32_t compensation_ev;
  float64_t delta_time;
  /** Reset bits already derived for this frame by `vkr_temporal_prepare()`. */
  uint32_t temporal_reset_reasons;
  uint32_t explicit_reset_reasons;
} VkrExposureFrameInput;

/**
 * @brief Renderer-owned exposure state lowered with the packet.
 *
 * `manual` is what manual mode applies verbatim. Automatic mode reads the
 * newest completed adapted exposure on the GPU and never returns it to the CPU;
 * `manual` is the fallback when `history_valid` is false and no texels meter.
 */
typedef struct VkrExposureFrame {
  uint32_t mode;
  float32_t manual;
  float32_t compensation_ev;
  /** Adaptation step, clamped to [0, VKR_EXPOSURE_MAX_DELTA_SECONDS]. */
  float32_t delta_seconds;
  uint32_t reset_reasons;
  bool8_t history_valid;
} VkrExposureFrame;

/**
 * @brief Metering constants lowered to both metering kernels.
 *
 * Mirrors `VkrExposureMetering` in
 * `shaders/shared/exposure_kernel.slangh`. Derived quantities the kernels would
 * otherwise recompute per invocation are precomputed here, at the cold
 * boundary, because they are frame-uniform.
 */
typedef struct VkrExposureGpuMetering {
  float32_t min_log_luminance;
  float32_t log_luminance_range;
  float32_t inverse_log_luminance_range;
  float32_t min_luminance;
  float32_t low_percentile;
  float32_t high_percentile;
  /** log2 of the configured middle gray; the resolve subtracts, never divides.
   */
  float32_t log_middle_gray;
  float32_t min_ev;
  float32_t max_ev;
  float32_t brighten_rate_per_second;
  float32_t darken_rate_per_second;
  float32_t compensation_ev;
  float32_t delta_seconds;
  /** log2 of the validated manual multiplier; invalid-chain fallback. */
  float32_t manual_ev;
  uint32_t bin_count;
  uint32_t history_valid;
} VkrExposureGpuMetering;

_Static_assert(sizeof(VkrExposureGpuMetering) == 64,
               "Exposure metering ABI must remain 64 bytes");

/**
 * @brief One published adaptation record.
 *
 * `exposure_multiplier` is the only field the tonemap reads. The rest exist so
 * the histogram window, the resolved target, and the reset that produced them
 * are diagnosable without re-deriving them from a capture.
 */
typedef struct VkrExposureGpuState {
  float32_t exposure_multiplier;
  float32_t adapted_ev;
  float32_t target_ev;
  float32_t average_log_luminance;
  /** Retained count window edges after the percentile clip, in bin units. */
  float32_t retained_low_bin;
  float32_t retained_high_bin;
  uint32_t accepted_texel_count;
  uint32_t reset_reasons;
} VkrExposureGpuState;

_Static_assert(sizeof(VkrExposureGpuState) == 32,
               "Exposure state ABI must remain 32 bytes");

/** Bounded histogram written by the accumulate kernel. */
typedef struct VkrExposureGpuHistogram {
  uint32_t bins[VKR_EXPOSURE_HISTOGRAM_BIN_COUNT];
} VkrExposureGpuHistogram;

/** Latest completed automatic-exposure decision and its source histogram. */
typedef struct VkrExposureDebugSample {
  VkrExposureGpuState state;
  VkrExposureGpuHistogram histogram;
  uint64_t source_frame_index;
  uint64_t source_submit_value;
  bool8_t valid;
} VkrExposureDebugSample;

/** Lowers a normalized cold record plus this frame's inputs for the kernels. */
VkrExposureGpuMetering
vkr_exposure_gpu_metering(const VkrExposureMeteringConfig *config,
                          const VkrExposureFrame *frame);

/**
 * @brief CPU reference for the shared metering kernel.
 *
 * `shaders/shared/exposure_kernel.slangh` performs these exact steps. This
 * mirror exists so the arithmetic has deterministic CPU coverage; the project
 * has no GPU compute-kernel test harness, so a divergence here is the only
 * signal available before a capture.
 */

/** Returns whether a scene-linear luminance is metered at all. */
bool8_t vkr_exposure_luminance_accepted(const VkrExposureGpuMetering *metering,
                                        float32_t luminance);

/** Bin index for an accepted luminance. Out-of-range luminance clamps. */
uint32_t vkr_exposure_bin_index(const VkrExposureGpuMetering *metering,
                                float32_t luminance);

/** log2 luminance at the center of a bin. */
float32_t vkr_exposure_bin_log_luminance(const VkrExposureGpuMetering *metering,
                                         uint32_t bin);

/** Reduces one filled histogram into a published adaptation record. */
VkrExposureGpuState
vkr_exposure_resolve(const VkrExposureGpuMetering *metering,
                     const VkrExposureGpuHistogram *histogram,
                     const VkrExposureGpuState *previous);

/** Production metering defaults, already normalized. */
VkrExposureMeteringConfig vkr_exposure_metering_config_default(void);

/**
 * @brief Clamps an authored metering record into the supported ranges.
 *
 * Cold boundary for every metering value. A zeroed record normalizes to the
 * production defaults, so a caller may leave the block unset.
 */
VkrExposureMeteringConfig
vkr_exposure_metering_config_normalize(const VkrExposureMeteringConfig *config);

/** Builds frame-local exposure state without committing it. */
VkrExposureFrame vkr_exposure_prepare(const VkrExposureState *state,
                                      const VkrExposureFrameInput *input);

/** Commits one successfully submitted frame as the next history source. */
void vkr_exposure_commit(VkrExposureState *state,
                         const VkrExposureFrameInput *input);

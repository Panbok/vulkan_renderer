/**
 * @file vkr_shadow_system.h
 * @brief Cascaded shadow mapping (directional light) system.
 *
 * Owns per-cascade matrices. Produces per-frame
 * data that the world shader consumes to sample shadows.
 */
#pragma once

#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;
struct VkrCamera;

#define VKR_SHADOW_CASCADE_COUNT_MAX 8
#define VKR_SHADOW_MAP_SIZE_DEFAULT 4096

/**
 * @brief One cascade's fitted light-space volume, before projection.
 *
 * This is the quantity fit hysteresis operates on and the quantity Phase 3
 * reuse would have to compare, so it is a named value rather than locals inside
 * the matrix builder. Center and extent are light-space XY; the extent is
 * square, so one value covers both axes. `min_z`/`max_z` are the light-space
 * depth interval before near/far conversion, and their span is what receiver
 * bias must be divided by to be expressed in world units.
 */
typedef struct VkrShadowFit {
  float32_t center_x;
  float32_t center_y;
  float32_t extent;
  float32_t min_z;
  float32_t max_z;
  float32_t world_units_per_texel;
} VkrShadowFit;

/**
 * @brief Previous-frame fits and the configuration they were computed for.
 *
 * Hysteresis may only compare against a fit produced under identical framing
 * rules. Every field beside `cascades` exists to answer "is the stored fit
 * still meaningful", and a mismatch on any of them discards the whole history
 * rather than silently blending fits from two different configurations.
 *
 * `enable_generation` identifies a continuous enabled interval. Disabling
 * advances it, so re-enabling cannot resume from a fit computed before the gap.
 */
typedef struct VkrShadowFitHistory {
  VkrShadowFit cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  Vec3 light_direction;
  uint32_t cascade_count;
  uint32_t shadow_map_size;
  uint32_t projection_convention;
  uint64_t enable_generation;
  bool8_t valid;
} VkrShadowFitHistory;

/**
 * @brief Per-cascade data updated each frame.
 *
 * view_projection is valid only after vkr_shadow_system_update() for the
 * current frame. split_far is a view-space distance (positive along forward).
 *
 * `world_units_per_texel`, `light_space_origin`, and `light_space_depth_span`
 * are deliberately retained while no shader consumes them: Phase 3 guard-band
 * math needs the first, and Phase 7 needs all three to express receiver bias in
 * texels rather than in raw normalized depth. They are system-side state, not
 * packet ABI, so keeping them costs nothing at the seam.
 */
typedef struct VkrCascadeData {
  Mat4 view_projection;
  float32_t split_far;
  float32_t world_units_per_texel;
  Vec2 light_space_origin; // Light-space grid origin in right/up basis.
  /** max_z - min_z of the fitted light-space interval, in world units. */
  float32_t light_space_depth_span;
  Vec3 bounds_center;
  float32_t bounds_radius;
} VkrCascadeData;

/**
 * @brief Axis-aligned bounding box for shadow scene bounds.
 *
 * When set (use_scene_bounds = true), the shadow system clips this caster AABB
 * against each cascade's final light-space XY footprint and extends only that
 * cascade's depth range to the intersecting volume.
 *
 * If use_scene_bounds is false, the system falls back to extending the camera
 * frustum along the light direction by z_extension_factor * radius.
 */
typedef struct VkrShadowSceneBounds {
  Vec3 min;
  Vec3 max;
  bool8_t use_scene_bounds;
} VkrShadowSceneBounds;

#define VKR_SHADOW_SCENE_BOUNDS_DEFAULT                                        \
  ((VkrShadowSceneBounds){                                                     \
      .min = {-20.0f, -20.0f, -20.0f},                                         \
      .max = {20.0f, 20.0f, 20.0f},                                            \
      .use_scene_bounds = false_v,                                             \
  })

/**
 * @brief Shadow system configuration.
 *
 * Every field is classified as ACTIVE or RESERVED. A field is ACTIVE only when
 * a shipped consumer reads the value it produces; "the shadow system computes
 * it each frame" is not a consumer. RESERVED fields stay here so external
 * configuration does not churn when their consumer ships, but they are kept out
 * of `VkrShadowFrameData` until then, so the per-frame structure cannot imply a
 * capability the renderer does not have.
 *
 * ACTIVE
 * | Field | Consumer |
 * |---|---|
 * | cascade_count | split/matrix loop; packet cascade count |
 * | shadow_map_size | texel size and snapping in the cascade fit |
 * | cascade_split_lambda | vkr_shadow_compute_cascade_splits() |
 * | max_shadow_distance | far-split clamp in the same function |
 * | cascade_guard_band_texels[_per] | XY extent expansion in the cascade fit |
 * | z_extension_factor, cascade_z_extension_factor_per | light-space Z range |
 * | use_constant_cascade_size | selects sphere-radius extent over corner AABB |
 * | stabilize_cascades | extent quantization and texel snapping |
 * | anchor_snap_texels | light-view anchor snap |
 * | scene_bounds | vkr_shadow_fit_relevant_caster_z(), off by default |
 * | depth_bias_constant_factor, _slope_factor, _clamp | raster depth bias, via
 *   VkrShadowConfigOverride, on both selected implementations |
 *
 * RESERVED for the receiver-quality phase. Computed nowhere and consumed
 * nowhere today; the shipped receiver takes one nearest tap and one global
 * constant (`vkr_packet_shadow_bias`). Do not add these to the packet until the
 * shader that reads them ships.
 * | Field | Blocked on |
 * |---|---|
 * | shadow_bias, normal_bias, shadow_slope_bias | receiver bias block |
 * | pcf_radius | PCF kernel |
 * | cascade_blend_range | cascade cross-fade |
 * | shadow_distance_fade_range | max-distance fade |
 *
 * Removed as obsolete rather than reserved: `shadow_bias_texel_scale`,
 * `shadow_slope_bias_texel_scale`, the three `shadow_uv_*_scale_per` arrays,
 * `foliage_alpha_cutoff_bias`, `foliage_alpha_dither`, and
 * `debug_show_cascades`. The receiver-quality design defines no consumer for
 * them. Keeping them would preserve controls the renderer cannot honor.
 */
typedef struct VkrShadowConfig {
  uint32_t cascade_count;
  uint32_t shadow_map_size;
  float32_t cascade_split_lambda;
  float32_t max_shadow_distance;
  float32_t cascade_guard_band_texels;
  float32_t cascade_guard_band_texels_per[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t z_extension_factor;
  float32_t cascade_z_extension_factor_per[VKR_SHADOW_CASCADE_COUNT_MAX];
  /* Raster depth bias, in backend depth-bias units. Distinct from receiver
     bias: this one is applied while rendering the shadow map. */
  float32_t depth_bias_constant_factor;
  float32_t depth_bias_clamp;
  float32_t depth_bias_slope_factor;
  /* RESERVED below this line. See the table above. */
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t shadow_slope_bias;
  float32_t pcf_radius;
  float32_t shadow_distance_fade_range;
  float32_t cascade_blend_range;
  /* ACTIVE again. */
  bool8_t use_constant_cascade_size;
  float32_t anchor_snap_texels;
  bool8_t stabilize_cascades;
  VkrShadowSceneBounds scene_bounds;
} VkrShadowConfig;

/**
 * @brief High-quality CSM preset (recommended on modern GPUs).
 *
 * Four cascades at 2048. The reserved receiver-quality values below are carried
 * for the phase that consumes them; the shipped receiver takes one nearest tap
 * with a global constant bias and reads none of them.
 *
 * `cascade_split_lambda` stays at 0.80 deliberately. Lowering it trades near
 * texel density for far density and is a named quality experiment with its own
 * captures, not a cleanup.
 *
 * The 1.25/1.75 raster-bias factors transcribe the values Vulkan previously
 * hardcoded while this preset claimed zero. Vulkan output therefore keeps its
 * prior bias, and Metal now consumes the same packet controls.
 */
#define VKR_SHADOW_CONFIG_HIGH                                                 \
  ((VkrShadowConfig){                                                          \
      .cascade_count = 4,                                                      \
      .shadow_map_size = 2048,                                                 \
      .cascade_split_lambda = 0.80f,                                           \
      .max_shadow_distance = 200.0f,                                           \
      .cascade_guard_band_texels = 32.0f,                                      \
      .cascade_guard_band_texels_per = {16.0f, 24.0f, 32.0f, 48.0f},           \
      .z_extension_factor = 4.0f,                                              \
      .cascade_z_extension_factor_per = {2.0f, 3.0f, 4.0f, 5.0f},              \
      .depth_bias_constant_factor = 1.25f,                                     \
      .depth_bias_clamp = 0.0f,                                                \
      .depth_bias_slope_factor = 1.75f,                                        \
      .shadow_bias = 0.00001f,                                                 \
      .normal_bias = 0.001f,                                                   \
      .shadow_slope_bias = 0.0002f,                                            \
      .pcf_radius = 1.6f,                                                      \
      .shadow_distance_fade_range = 1.0f,                                      \
      .use_constant_cascade_size = true_v,                                     \
      .cascade_blend_range = 1.0f,                                             \
      .anchor_snap_texels = 16.0f,                                             \
      .stabilize_cascades = true_v,                                            \
      .scene_bounds = VKR_SHADOW_SCENE_BOUNDS_DEFAULT,                         \
  })

/**
 * @brief Balanced CSM preset (better performance/memory footprint).
 *
 * Its `depth_bias_*` values change rendered output for the first time here.
 * Vulkan previously hardcoded 1.25/1.75 for every preset and ignored these, and
 * Metal applied no raster bias at all; both now apply the 1.50/2.00 this preset
 * has always declared.
 */
#define VKR_SHADOW_CONFIG_BALANCED                                             \
  ((VkrShadowConfig){                                                          \
      .cascade_count = 3,                                                      \
      .shadow_map_size = 2048,                                                 \
      .cascade_split_lambda = 0.75f,                                           \
      .max_shadow_distance = 120.0f,                                           \
      .cascade_guard_band_texels = 128.0f,                                     \
      .cascade_guard_band_texels_per = {0},                                    \
      .z_extension_factor = 5.0f,                                              \
      .cascade_z_extension_factor_per = {0},                                   \
      .depth_bias_constant_factor = 1.50f,                                     \
      .depth_bias_clamp = 0.0f,                                                \
      .depth_bias_slope_factor = 2.00f,                                        \
      .shadow_bias = 0.001f,                                                   \
      .normal_bias = 0.01f,                                                    \
      .shadow_slope_bias = 0.001f,                                             \
      .pcf_radius = 2.0f,                                                      \
      .shadow_distance_fade_range = 10.0f,                                     \
      .use_constant_cascade_size = true_v,                                     \
      .cascade_blend_range = 8.0f,                                             \
      .anchor_snap_texels = 8.0f,                                              \
      .stabilize_cascades = true_v,                                            \
      .scene_bounds = VKR_SHADOW_SCENE_BOUNDS_DEFAULT,                         \
  })

/**
 * @brief Project-wide default.
 */
#define VKR_SHADOW_CONFIG_DEFAULT VKR_SHADOW_CONFIG_HIGH

static INLINE uint32_t
vkr_shadow_config_get_max_map_size(const VkrShadowConfig *config) {
  if (!config) {
    return VKR_SHADOW_MAP_SIZE_DEFAULT;
  }
  uint32_t size = config->shadow_map_size;
  if (size == 0) {
    size = VKR_SHADOW_MAP_SIZE_DEFAULT;
  }
  return size;
}

/**
 * @brief Converts fitted view-space bounds into the shader's right/up grid.
 *
 * The light view's X axis is the negation of the right basis reconstructed by
 * `shadow_light_space_xy`; Y has the same sign as the reconstructed up basis.
 * Keeping that sign relation here makes the PCF rotation cell stable under
 * light-view translation.
 */
Vec2 vkr_shadow_light_space_origin_from_view(const Mat4 *light_view,
                                             float32_t left, float32_t bottom);

/**
 * Fits the light-space Z interval of the scene AABB portion intersecting the
 * supplied cascade XY rectangle. Returns false when the volumes do not overlap.
 */
bool8_t vkr_shadow_fit_relevant_caster_z(
    const Mat4 *light_view, const VkrShadowSceneBounds *scene_bounds,
    float32_t left, float32_t right, float32_t bottom, float32_t top,
    float32_t *out_min_z, float32_t *out_max_z);

/**
 * @brief CPU-side frame data for the shadow packet payload.
 *
 * Contains exactly what the current packet path consumes and nothing else. Each
 * field below has a named reader in `application_draw_frame()`, which lowers
 * them into `VkrShadowPassPayload`. Quality values the receiver does not read
 * live in `VkrShadowConfig` as RESERVED and deliberately do not appear here: a
 * per-frame field that no shader reads is a claim the renderer cannot honour.
 *
 * `enabled` is the validity bit for the rest of the structure. The application
 * reads it instead of reaching back into the lighting system, so this data has
 * one activation authority.
 */
typedef struct VkrShadowFrameData {
  bool8_t enabled;
  uint32_t cascade_count;
  float32_t split_far[VKR_SHADOW_CASCADE_COUNT_MAX];
  Mat4 view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowFrameData;

/**
 * @brief Shadow system state.
 *
 * GPU resources are owned by the selected packet renderer.
 */
typedef struct VkrShadowSystem {
  VkrShadowConfig config;
  VkrCascadeData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t cascade_splits[VKR_SHADOW_CASCADE_COUNT_MAX + 1];

  Vec3 light_direction;
  bool8_t light_enabled;

  /** Previous-frame fits, for the stabilization deadbands. */
  VkrShadowFitHistory fit_history;
  /** Bumped when enabled shadows are disabled; invalidates fit_history. */
  uint64_t enable_generation;

  bool8_t initialized;
} VkrShadowSystem;

/**
 * @brief Discards the stored fit history.
 *
 * Call on scene replacement and target recreation. Stamped fit inputs are
 * checked during update; this function covers lifecycle events that leave
 * those stamps identical while making the stored fit meaningless anyway.
 */
void vkr_shadow_system_invalidate_fit_history(VkrShadowSystem *system);

/**
 * @brief Rounds a light-space extent up to a whole texel multiple.
 *
 * The unquantized extent varies continuously with camera orientation, so the
 * texel size derived from it does too. The quantizer uses a stable
 * power-of-two bracket rather than deriving its step from the value being
 * rounded, which would be a no-op. Growth is bounded by one bracket quantum.
 * `extent` must be positive and `shadow_map_size` nonzero; initialization and
 * cascade fitting establish both preconditions before this hot-path helper is
 * called. Exposed for tests.
 */
float32_t vkr_shadow_quantize_extent_up(float32_t extent,
                                        uint32_t shadow_map_size);

/**
 * @brief Applies center, extent, and depth deadbands against the previous fit.
 *
 * Returns the fit to render with. Both pointers are required, and `previous`
 * must be a fit produced under the same configuration; the caller owns that
 * check.
 *
 * The three rules are independent, and each one exists to stop a specific
 * shimmer source:
 *  - a center that moved less than one texel keeps the previous snapped center;
 *    the stabilization path reserves one extra guard texel for this shift;
 *  - an extent that shrank by less than two texels keeps the previous extent,
 *    so the cascade does not breathe as the frustum slice rotates; growth is
 *    always taken, because a too-small extent clips casters;
 *  - a depth interval that shrank by less than two texels on either side keeps
 *    the previous bound, for the same reason in Z.
 *
 * Extent and depth shrink are deadbanded while growth is not, so those bounds
 * never become smaller than the raw fit. Center hysteresis is safe for receiver
 * coverage because the stabilized fit adds one guard texel before snapping.
 */
VkrShadowFit vkr_shadow_apply_fit_hysteresis(const VkrShadowFit *previous,
                                             const VkrShadowFit *raw);

/**
 * @brief Initialize shadow system resources and pipeline.
 *
 * Normalizes the packet-facing cascade configuration.
 */
bool8_t vkr_shadow_system_init(VkrShadowSystem *system,
                               struct s_RendererFrontend *rf,
                               const VkrShadowConfig *config);

/**
 * @brief Destroy shadow system resources.
 *
 * Clears CPU-side shadow state.
 */
void vkr_shadow_system_shutdown(VkrShadowSystem *system,
                                struct s_RendererFrontend *rf);

/**
 * @brief Recompute cascade splits and light-space matrices for this frame.
 *
 * light_enabled gates whether valid data is produced (disabled => identity).
 */
void vkr_shadow_system_update(VkrShadowSystem *system,
                              const struct VkrCamera *camera,
                              bool8_t light_enabled, Vec3 light_direction);

/**
 * @brief Fill frame data for shader upload and sampler binding.
 */
void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data);

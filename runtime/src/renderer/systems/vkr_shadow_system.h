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
#include "vkr_frame_input.h"
#include "vkr_renderer.h"

struct VkrCamera;
struct VkrLocalShadowPassPayload;
struct VkrPointLight;

#include "vkr_shadow.h"

struct VkrWorldPassPayload;

/**
 * @brief One cascade's fitted light-space volume, before projection.
 *
 * This is the quantity fit hysteresis operates on and the quantity Phase 3
 * reuse would have to compare, so it is a named value rather than locals inside
 * the matrix builder. Center and extent are light-space XY; the extent is
 * square, so one value covers both axes. `min_z`/`max_z` are the light-space
 * depth interval before near/far conversion; the receiver divides its
 * texel-denominated bias by the clamped span derived from them.
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
  /** Light direction each cascade's fit was framed with. */
  Vec3 light_directions[VKR_SHADOW_CASCADE_COUNT_MAX];
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
 * `world_units_per_texel` drives guard-band math; together with
 * `light_space_origin` and `light_space_depth_span` it is also what lets the
 * receiver express bias in texels rather than in raw normalized depth. These
 * are the *raw* fit's values. A cascade reused from retained contents publishes
 * its committed rendered fit instead, so the receiver never pairs a rendered
 * matrix with a raw fit's divisors.
 */
typedef struct VkrCascadeData {
  Mat4 view_projection;
  Mat4 light_view;
  VkrShadowFit fit;
  VkrShadowFit previous_fit;
  bool8_t previous_fit_valid;
  float32_t split_far;
  float32_t world_units_per_texel;
  Vec2 light_space_origin; // Light-space grid origin in right/up basis.
  /** Clamped orthographic far-minus-near span, in world units. */
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
 * @brief Measured world-space bounds of the scene's shadow casters.
 *
 * A Z-only input to cascade fitting (spec section 6.6). It replaces the blind
 * `z_extension_factor * radius` guess with the depth interval real casters
 * actually occupy, which matters because receiver bias is applied in normalized
 * depth: a Z range six times wider than the geometry needs makes the same
 * normalized bias mean six times more world distance.
 *
 * Deliberately *not* routed through `VkrShadowSceneBounds.use_scene_bounds`.
 * That flag also moves light-view anchoring and the XY fit; this input must
 * change the Z interval and nothing else.
 *
 * `valid` is false for an empty or unmeasurable scene, in which case fitting
 * falls back to `z_extension_factor`.
 */
typedef struct VkrShadowCasterDepthBounds {
  Vec3 min;
  Vec3 max;
  bool8_t valid;
} VkrShadowCasterDepthBounds;

/**
 * @brief Shadow system configuration.
 *
 * Every field is ACTIVE: a shipped consumer reads the value it produces.
 * "The shadow system computes it each frame" is not a consumer, and a field
 * without one belongs in neither this struct nor `VkrShadowFrameData`.
 *
 * | Field | Consumer |
 * |---|---|
 * | local_shadow_face_budget, local_shadow_map_size,
 *   local_shadow_fade_distance, local_shadow_full_filter_all |
 *   vkr_local_shadow_cache_resolve() |
 * | cascade_count | split/matrix loop; packet cascade count |
 * | shadow_map_size | texel size and snapping in the cascade fit |
 * | cascade_split_lambda | vkr_shadow_compute_cascade_splits() |
 * | max_shadow_distance | far-split clamp in the same function |
 * | cascade_guard_band_texels[_per] | XY extent expansion in the cascade fit |
 * | z_extension_factor, cascade_z_extension_factor_per | light-space Z range |
 * | use_constant_cascade_size | selects sphere-radius extent over corner AABB |
 * | stabilize_cascades | extent quantization and texel snapping |
 * | anchor_snap_texels | light-view anchor snap |
 * | light_direction_tolerance_degrees | per-cascade retained light direction
 *   in vkr_shadow_system_update() |
 * | scene_bounds | vkr_shadow_fit_relevant_caster_z(), off by default |
 * | depth_bias_constant_factor, _slope_factor, _clamp | raster depth bias, via
 *   VkrShadowConfigOverride, on both selected implementations |
 * | receiver_bias_texels, receiver_slope_bias_texels, normal_offset_texels,
 *   pcf_radius_texels, pcf_sample_count, pcf_uniform_early_out,
 *   cascade_blend_fraction,
 *   shadow_distance_fade_range | lowered into VkrShadowReceiverPacketData and
 *   read by packet_directional_shadow() and
 *   vkr_metal_packet_directional_shadow_sample() |
 * | reuse_guard_band_texels, reuse_depth_guard_fraction,
 *   reuse_predictive_max_texels, reuse_dynamic_scan_budget,
 *   reuse_proactive_refresh_budget |
 *   vkr_shadow_guarded_fit() and the bounded dynamic overlap scan |
 *
 * Removed as obsolete rather than reserved: `shadow_bias_texel_scale`,
 * `shadow_slope_bias_texel_scale`, the three `shadow_uv_*_scale_per` arrays,
 * `foliage_alpha_cutoff_bias`, `foliage_alpha_dither`, and
 * `debug_show_cascades`. The receiver-quality design defines no consumer for
 * them. Keeping them would preserve controls the renderer cannot honor.
 */
typedef struct VkrShadowConfig {
  uint32_t local_shadow_face_budget;
  /** Largest local shadow face; a light's faces follow its range. */
  uint32_t local_shadow_map_size;
  /** Camera distance in metres at which a light's local shadow has faded out,
   * over the last VKR_LOCAL_SHADOW_FADE_BAND_METRES; lights past it are not
   * filtered. Bounds the shadowed lights a pixel filters. */
  float32_t local_shadow_fade_distance;
  /** Screen-space contact shadows for the full-filter local lights. Ultra
   * only: the march costs about 1.1 ms of Shadow.LocalMask in the Bistro
   * street view on the M1 Pro, mostly through the occupancy it takes. */
  bool8_t local_shadow_contact;
  /** Full-filter local lights widen their penumbra by their authored source
   * radius (the Soft shadows setting); false keeps the fixed filter. */
  bool8_t local_shadow_soft;
  /** Every shown local light takes the full filter, not only the most
   * important few. The tiled pipeline sets it every frame: its dynamic-light
   * limit bounds the shadowed lights, so none shows the single tap's stepped
   * edge (ADR-087). The desktop pipeline leaves it false (ADR-019). */
  bool8_t local_shadow_full_filter_all;
  /** Cascades from VKR_SHADOW_EVSM_FIRST_CASCADE also keep filtered
   * exponential variance moments that deferred lighting samples instead of
   * PCF (the Filtered far shadows setting). On in High and Ultra, off in
   * Balanced; the tiled pipeline clears it every frame (ADR-087). */
  bool8_t far_cascade_evsm;
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
  /* Receiver quality. Biases are in shadow-map texels, so one value means the
     same world distance in every cascade; the receiver divides through each
     cascade's own texel size and fitted depth span. */
  float32_t receiver_bias_texels;
  float32_t receiver_slope_bias_texels;
  float32_t normal_offset_texels;
  float32_t pcf_radius_texels;
  /** Must pass vkr_shadow_pcf_sample_count_supported(). */
  uint32_t pcf_sample_count;
  /** Allows the nine-tap uniform-region probe for kernels of 16+ taps. */
  bool8_t pcf_uniform_early_out;
  /** Fraction of each cascade's span spent fading into the next cascade. */
  float32_t cascade_blend_fraction;
  /** World distance over which shadows fade out before max_shadow_distance. */
  float32_t shadow_distance_fade_range;
  /** Phase 3B retained-cache expansion and bounded dynamic overlap scan. */
  float32_t reuse_guard_band_texels;
  float32_t reuse_depth_guard_fraction;
  float32_t reuse_predictive_max_texels;
  uint32_t reuse_dynamic_scan_budget;
  /** Max still-reusable cascades refreshed per frame. Zero disables P5. */
  uint32_t reuse_proactive_refresh_budget;
  bool8_t sdsm_enabled;
  uint32_t sdsm_max_source_lag_frames;
  float32_t sdsm_temporal_blend;
  float32_t sdsm_max_contraction_fraction;
  bool8_t use_constant_cascade_size;
  float32_t anchor_snap_texels;
  /** While the light keeps moving, cascade 0 keeps its fitted direction until
      the light turns past this angle; each farther cascade doubles it. A light
      that stops is adopted exactly once VKR_SHADOW_LIGHT_SETTLE_UPDATES pass.
      Zero adopts every change.
      Requires stabilize_cascades. */
  float32_t light_direction_tolerance_degrees;
  bool8_t stabilize_cascades;
  VkrShadowSceneBounds scene_bounds;
} VkrShadowConfig;

/* A 24-minute day turns the sun about 0.004 degrees per 60 Hz frame, so
 * cascade 0 refreshes about every six frames and cascade 3 about every fifty.
 * A 15 m caster's shadow moves under 7 mm between cascade 0 refreshes. */
#define VKR_SHADOW_LIGHT_DIRECTION_TOLERANCE_DEGREES_DEFAULT 0.025f
/* A light unchanged for this many updates has stopped and is adopted exactly.
 * More than one tolerates a simulation that turns the sun on every second to
 * fourth rendered frame without settling between its steps. */
#define VKR_SHADOW_LIGHT_SETTLE_UPDATES 4u

/* Local-shadow faces per preset: ten point lights for Ultra, five for High,
 * two for Balanced. Each shadowed light adds its PCF to every pixel in its
 * range, so the budget, not the 64-face capacity, bounds that cost; lights
 * past the two most important take a single filtered tap. */
#define VKR_LOCAL_SHADOW_FACE_BUDGET_ULTRA 60u
/* Camera distance at which local shadows have faded out, and the width of the
 * fade before it. Lights stay resident past it, so the distance bounds only
 * filtering; distant lights cover few pixels and take the single tap. */
#define VKR_LOCAL_SHADOW_FADE_DISTANCE_DEFAULT 120.0f
#define VKR_LOCAL_SHADOW_FADE_BAND_METRES 5.0f
#define VKR_LOCAL_SHADOW_FACE_BUDGET_HIGH 30u
#define VKR_LOCAL_SHADOW_FACE_BUDGET_BALANCED 12u

/**
 * @brief High-quality CSM preset (recommended on modern GPUs).
 *
 * Four cascades at VKR_SHADOW_MAP_SIZE_DEFAULT (2048).
 *
 * `cascade_split_lambda` stays at 0.80 deliberately. Lowering it trades near
 * texel density for far density and is a named quality experiment with its own
 * captures, not a cleanup.
 *
 * The 1.25/1.75 raster-bias factors transcribe the values Vulkan previously
 * hardcoded while this preset claimed zero. Vulkan output therefore keeps its
 * prior bias, and Metal now consumes the same packet controls.
 *
 * The receiver values are the design's starting points, not tuned defaults.
 * Tap count, radius, blend fraction, and every bias are independent sweep
 * variables; changing more than one at a time makes a capture or timing result
 * uninterpretable.
 *
 * SDSM remains opt-in; the rewrite specification records the measured Metal
 * gate that rejected it as a default.
 *
 * Cascades 2 and 3 take far-cascade EVSM (ADR-041) on the desktop pipeline.
 */
#define VKR_SHADOW_CONFIG_HIGH                                                 \
  ((VkrShadowConfig){                                                          \
      .local_shadow_face_budget = VKR_LOCAL_SHADOW_FACE_BUDGET_HIGH,           \
      .local_shadow_map_size = VKR_LOCAL_SHADOW_MAP_SIZE_DEFAULT,              \
      .local_shadow_fade_distance = VKR_LOCAL_SHADOW_FADE_DISTANCE_DEFAULT,    \
      .local_shadow_soft = true_v,                                             \
      .far_cascade_evsm = true_v,                                              \
      .cascade_count = 4,                                                      \
      .shadow_map_size = VKR_SHADOW_MAP_SIZE_DEFAULT,                          \
      .cascade_split_lambda = 0.80f,                                           \
      .max_shadow_distance = 200.0f,                                           \
      .cascade_guard_band_texels = 32.0f,                                      \
      .cascade_guard_band_texels_per = {16.0f, 24.0f, 32.0f, 48.0f},           \
      .z_extension_factor = 4.0f,                                              \
      .cascade_z_extension_factor_per = {2.0f, 3.0f, 4.0f, 5.0f},              \
      .depth_bias_constant_factor = 1.25f,                                     \
      .depth_bias_clamp = 0.0f,                                                \
      .depth_bias_slope_factor = 1.75f,                                        \
      .receiver_bias_texels = 1.0f,                                            \
      .receiver_slope_bias_texels = 2.0f,                                      \
      .normal_offset_texels = 1.0f,                                            \
      .pcf_radius_texels = 1.5f,                                               \
      .pcf_sample_count = 16u,                                                 \
      .pcf_uniform_early_out = true_v,                                         \
      .cascade_blend_fraction = 0.08f,                                         \
      .shadow_distance_fade_range = 20.0f,                                     \
      .use_constant_cascade_size = true_v,                                     \
      .reuse_guard_band_texels = 128.0f,                                       \
      .reuse_depth_guard_fraction = 0.0625f,                                   \
      .reuse_predictive_max_texels = 64.0f,                                    \
      .reuse_dynamic_scan_budget = VKR_SHADOW_DYNAMIC_SCAN_BUDGET_DEFAULT,     \
      .reuse_proactive_refresh_budget = 0u,                                    \
      .sdsm_enabled = false_v,                                                 \
      .sdsm_max_source_lag_frames = 4u,                                        \
      .sdsm_temporal_blend = 0.85f,                                            \
      .sdsm_max_contraction_fraction = 0.10f,                                  \
      .anchor_snap_texels = 16.0f,                                             \
      .light_direction_tolerance_degrees =                                     \
          VKR_SHADOW_LIGHT_DIRECTION_TOLERANCE_DEGREES_DEFAULT,                \
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
      .local_shadow_face_budget = VKR_LOCAL_SHADOW_FACE_BUDGET_BALANCED,       \
      .local_shadow_map_size = 512u,                                           \
      .local_shadow_fade_distance = VKR_LOCAL_SHADOW_FADE_DISTANCE_DEFAULT,    \
      .local_shadow_soft = true_v,                                             \
      .cascade_count = 3,                                                      \
      .shadow_map_size = VKR_SHADOW_MAP_SIZE_DEFAULT,                          \
      .cascade_split_lambda = 0.75f,                                           \
      .max_shadow_distance = 120.0f,                                           \
      .cascade_guard_band_texels = 128.0f,                                     \
      .cascade_guard_band_texels_per = {0},                                    \
      .z_extension_factor = 5.0f,                                              \
      .cascade_z_extension_factor_per = {0},                                   \
      .depth_bias_constant_factor = 1.50f,                                     \
      .depth_bias_clamp = 0.0f,                                                \
      .depth_bias_slope_factor = 2.00f,                                        \
      .receiver_bias_texels = 1.0f,                                            \
      .receiver_slope_bias_texels = 2.0f,                                      \
      .normal_offset_texels = 1.0f,                                            \
      .pcf_radius_texels = 1.5f,                                               \
      .pcf_sample_count = 9u,                                                  \
      .pcf_uniform_early_out = true_v,                                         \
      .cascade_blend_fraction = 0.08f,                                         \
      .shadow_distance_fade_range = 12.0f,                                     \
      .use_constant_cascade_size = true_v,                                     \
      .anchor_snap_texels = 8.0f,                                              \
      .light_direction_tolerance_degrees =                                     \
          VKR_SHADOW_LIGHT_DIRECTION_TOLERANCE_DEGREES_DEFAULT,                \
      .stabilize_cascades = true_v,                                            \
      .scene_bounds = VKR_SHADOW_SCENE_BOUNDS_DEFAULT,                         \
  })

/**
 * @brief Project-wide default.
 */
#define VKR_SHADOW_CONFIG_DEFAULT VKR_SHADOW_CONFIG_HIGH

/** High with the Ultra local-shadow face budget, for GPUs measured to afford
 * the extra per-pixel filtering. */
static inline VkrShadowConfig vkr_shadow_config_ultra(void) {
  VkrShadowConfig config = VKR_SHADOW_CONFIG_HIGH;
  config.local_shadow_face_budget = VKR_LOCAL_SHADOW_FACE_BUDGET_ULTRA;
  config.local_shadow_contact = true_v;
  return config;
}

vkr_internal INLINE uint32_t
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
 * @brief CPU-side frame data for the shadow packet payload.
 *
 * Contains exactly what the current packet path consumes and nothing else. Each
 * field below has a named reader in `application_draw_frame()`, which lowers
 * them into `VkrShadowPassPayload`.
 *
 * `enabled` is the validity bit for the rest of the structure. The application
 * reads it instead of reaching back into the lighting system, so this data has
 * one activation authority.
 *
 * The four receiver arrays are the resolved per-cascade description of the
 * matrix published in `view_projection`. For a cascade reused from retained
 * contents they come from the committed rendered fit, never from the current
 * raw fit; pairing a reused layer with a raw fit's texel size or depth span
 * would misconvert every texel-denominated bias for that cascade.
 */
typedef struct VkrShadowFrameData {
  bool8_t enabled;
  uint32_t cascade_count;
  uint32_t cascade_render_mask;
  float32_t split_near[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t split_far[VKR_SHADOW_CASCADE_COUNT_MAX];
  Mat4 view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t world_units_per_texel[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t light_space_depth_span[VKR_SHADOW_CASCADE_COUNT_MAX];
  Vec2 light_space_origin[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t rendered[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t reused[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t correctness_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t proactive_refreshed[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t dynamic_candidates_tested[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t dynamic_forced[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrShadowSdsmStatus sdsm_status;
  uint32_t sdsm_source_lag;
  uint32_t sdsm_occupied_count;
  float32_t sdsm_linear_near;
  float32_t sdsm_linear_far;
} VkrShadowFrameData;

typedef struct VkrShadowCascadeHistory {
  bool8_t static_only_contents;
  VkrShadowFit rendered_fit;
  Mat4 rendered_light_view;
  Mat4 rendered_view_projection;
  uint64_t static_generation;
  uint64_t publication_generation;
  uint64_t caster_bounds_generation;
  uint64_t bias_signature;
  uint64_t light_signature;
  uint64_t resource_generation;
  uint64_t last_submit_value;
} VkrShadowCascadeHistory;

typedef struct VkrShadowPendingHistory {
  VkrShadowCascadeHistory cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t image_index;
  uint32_t cascade_mask;
  bool8_t active;
} VkrShadowPendingHistory;

/** Camera state that drives local-shadow priority and the fade-in. */
typedef struct VkrLocalShadowCamera {
  Mat4 view;
  /** Faces the camera cannot see take no dynamic square; an all-zero
   * projection sees every face. */
  Mat4 projection;
  Vec3 position;
  /** Seconds since the previous resolve; bounds each strength step. */
  float32_t delta_seconds;
  /** Frame index of this resolve, on the counter that stamps
   * VkrLocalLightContributionSample::source_frame_index. */
  uint64_t frame_index;
} VkrLocalShadowCamera;

/** Committed depth content of one cached face. */
typedef struct VkrLocalShadowFaceHistory {
  /** Projection and atlas square the content was drawn with. */
  VkrLocalShadowView view;
  uint64_t static_generation;
  uint64_t publication_generation;
  uint64_t resource_generation;
  uint64_t transmission_resource_generations
      [VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT];
  /** Nonzero once a successful submission committed the content. */
  uint64_t last_submit_value;
  /** Transmission layer + 1 drawn with the face; zero without one. */
  uint32_t transmission_layer;
  /** The content holds no dynamic caster: it was drawn with the static
   * casters only, or with every caster while none dynamic could reach it,
   * and no publication was pending. Dynamic squares are never committed. */
  bool8_t static_only_contents;
} VkrLocalShadowFaceHistory;

/* Lights the local-shadow cache holds: the scene table's and the baked
   lamps a frame shadows with its moving casters. */
#define VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX                                       \
  (VKR_MAX_SCENE_POINT_LIGHTS + VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX)

/** One resident light of the local-shadow cache. */
typedef struct VkrLocalShadowCacheLight {
  uint32_t render_id;
  uint32_t light_kind;
  uint32_t face_count;
  /** Side in texels of every face, fixed by the light's range. */
  uint32_t face_size;
  /** Each face's atlas square: cell x | cell y << 8 | layer << 16, in
   * VKR_LOCAL_SHADOW_FACE_SIZE_MIN cells. */
  uint32_t face_cells[6];
  /** Each face's transmission layer + 1; zero without one. A light holds
   * layers for all its faces or none. */
  uint32_t transmission_layers[6];
  /** Fill strength in [0, 1]; rises from zero once every face is valid. */
  float32_t strength;
  /** Camera-distance fade in [0, 1]; receivers see strength times it. */
  float32_t distance_fade;
  /** The light takes one filtered tap and no contact shadows. */
  bool8_t reduced;
  VkrLocalShadowFaceHistory faces[6];
} VkrLocalShadowCacheLight;

/**
 * CPU state of the persistent local-shadow cache. Every shadow-casting light
 * of the scene table is resident with fixed atlas squares in one atlas that
 * all frames in flight share; a face draws when its content is invalid or
 * stale, in priority order, within the face budget per frame. The view table
 * a frame publishes is rebuilt from it each frame.
 */
typedef struct VkrLocalShadowCache {
  VkrLocalShadowCacheLight lights[VKR_LOCAL_SHADOW_LIGHT_COUNT_MAX];
  uint32_t light_count;
  uint32_t atlas_layer_count;
  uint32_t face_budget;
  /** Transmission layers drawn since the arrays were created; every layer
   * below it holds drawn content. */
  uint32_t transmission_layer_count;
  uint64_t transmission_resource_generation;
  /** Camera of the previous resolve, for camera-cut detection. */
  Mat4 camera_view;
  Vec3 camera_position;
  /** Feedback from earlier frames describes a view the last snap left. */
  uint64_t feedback_after_frame;
  bool8_t valid;
} VkrLocalShadowCache;

typedef struct VkrLocalShadowPendingFace {
  uint32_t light;
  uint32_t face;
  VkrLocalShadowFaceHistory history;
} VkrLocalShadowPendingFace;

/** Faces drawn and atlas layers cleared by the frame in flight, committed to
 * the cache only after its submission succeeds. */
typedef struct VkrLocalShadowPendingHistory {
  VkrLocalShadowPendingFace faces[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  uint32_t face_count;
  uint32_t cleared_layer_mask;
  uint32_t transmission_layer_count;
  bool8_t active;
} VkrLocalShadowPendingHistory;

/**
 * @brief Shadow system state.
 *
 * GPU resources are owned by the selected packet renderer.
 */
typedef struct VkrShadowSystem {
  VkrShadowConfig config;
  VkrCascadeData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t cascade_splits[VKR_SHADOW_CASCADE_COUNT_MAX + 1];

  /** Latest input light direction. */
  Vec3 light_direction;
  /** Consecutive updates with an unchanged direction, saturating at
      VKR_SHADOW_LIGHT_SETTLE_UPDATES, where the light counts as stopped. */
  uint32_t light_unchanged_updates;
  /** Direction each cascade is fitted with this frame (see
      light_direction_tolerance_degrees). */
  Vec3 cascade_light_directions[VKR_SHADOW_CASCADE_COUNT_MAX];
  bool8_t light_enabled;

  /** Previous-frame fits, for the stabilization deadbands. */
  VkrShadowFitHistory fit_history;
  /** Bumped when enabled shadows are disabled; invalidates fit_history. */
  uint64_t enable_generation;

  /** Submitted per-image contents also supply the newest compatible fit.
   * Other physical images converge by rendering that descriptor before reuse;
   * selecting its CPU metadata never proves another image's depth contents. */
  VkrShadowCascadeHistory cascade_history[VKR_SHADOW_TARGET_IMAGE_COUNT_MAX]
                                         [VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrShadowPendingHistory pending_history;

  /** Resident local-light faces shared by every frame in flight. */
  VkrLocalShadowCache local_cache;
  /** Holds the next cache state while a resolve reads the current one. */
  VkrLocalShadowCache local_cache_scratch;
  VkrLocalShadowPendingHistory pending_local_history;
  /** Newest measured light contribution; invalid on backends without it. */
  VkrLocalLightContributionSample light_contribution;
  /** Diagnostic: rank by distance even when contribution is measured. */
  bool8_t light_contribution_ranking_disabled;
  /** Render ids of the baked lamps the last resolve selected; they keep
   * their place against stronger lamps by the incumbent bonus. */
  uint32_t baked_lamp_render_ids[VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX];
  uint32_t baked_lamp_selected_count;

  VkrShadowDepthRangeSample pending_sdsm_sample;
  float32_t sdsm_linear_near;
  float32_t sdsm_linear_far;
  uint64_t sdsm_current_frame_index;
  uint64_t sdsm_current_scene_generation;
  uint64_t sdsm_source_frame_index;
  uint64_t sdsm_source_scene_generation;
  uint64_t sdsm_source_projection_generation;
  uint64_t sdsm_submit_value;
  uint32_t sdsm_source_lag;
  uint32_t sdsm_occupied_count;
  VkrShadowSdsmStatus sdsm_status;
  bool8_t sdsm_range_valid;

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

void vkr_shadow_system_set_depth_range_sample(
    VkrShadowSystem *system, const VkrShadowDepthRangeSample *sample,
    uint64_t current_frame_index, uint64_t current_scene_generation);

/**
 * Keeps the newest valid light-contribution sample for local-shadow selection.
 * Older, repeated or invalid samples are ignored.
 */
void vkr_shadow_system_set_light_contribution_sample(
    VkrShadowSystem *system, const VkrLocalLightContributionSample *sample);

/**
 * Resolves the local-shadow cache for the frame: keeps every shadow-casting
 * light resident, schedules the faces whose content is invalid or stale in
 * priority order within the face budget, and fills the frame's payload. The
 * drawn faces commit with vkr_shadow_system_commit_frame().
 *
 * `baked_lamps` are the static lamps the desktop pipeline lights through
 * its lightmap (ADR-104), or NULL. The resolve shadows
 * the moving casters of at most VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX of
 * them: those whose light is strongest at a moving caster that meets their
 * range on a face the camera may see.
 */
void vkr_shadow_system_resolve_local_shadows(
    VkrShadowSystem *system, VkrRetainedLocalShadowToken retained_token,
    const struct VkrWorldPassPayload *candidates,
    const struct VkrPointLight *lights, uint32_t light_count,
    const struct VkrPointLight *baked_lamps, uint32_t baked_lamp_count,
    const VkrLocalShadowCamera *camera,
    struct VkrLocalShadowPassPayload *out_payload);

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
                               const VkrShadowConfig *config);

/**
 * @brief Destroy shadow system resources.
 *
 * Clears CPU-side shadow state.
 */
void vkr_shadow_system_shutdown(VkrShadowSystem *system);

/**
 * @brief Recompute cascade splits and light-space matrices for this frame.
 *
 * light_enabled gates whether valid data is produced (disabled => identity).
 */
void vkr_shadow_system_update(VkrShadowSystem *system,
                              const struct VkrCamera *camera,
                              bool8_t light_enabled, Vec3 light_direction,
                              const VkrShadowCasterDepthBounds *caster_bounds);

/**
 * @brief Fill frame data for shader upload and sampler binding.
 */
void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data);

/** Resolves retained reuse after prepare selected the physical target image. */
void vkr_shadow_system_resolve_frame(
    VkrShadowSystem *system, uint32_t image_index,
    VkrRetainedShadowToken retained_token,
    const struct VkrWorldPassPayload *candidates,
    VkrTextureFormat shadow_depth_format, VkrShadowFrameData *out_data);

/** Publishes staged rendered fits only after packet submission succeeds. */
void vkr_shadow_system_commit_frame(VkrShadowSystem *system,
                                    uint64_t submit_value);
void vkr_shadow_system_discard_frame(VkrShadowSystem *system);

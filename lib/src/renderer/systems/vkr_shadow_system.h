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
 * @brief Per-cascade data updated each frame.
 *
 * view_projection is valid only after vkr_shadow_system_update() for the
 * current frame. split_far is a view-space distance (positive along forward).
 */
typedef struct VkrCascadeData {
  Mat4 view_projection;
  float32_t split_far;
  float32_t world_units_per_texel;
  Vec2 light_space_origin; // Light-space grid origin in right/up basis.
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
 * cascade_count is clamped to [1, VKR_SHADOW_CASCADE_COUNT_MAX].
 * shadow_map_size is the resolution used for all cascades.
 * max_shadow_distance clamps the far split to avoid wasting resolution.
 * cascade_guard_band_texels expands each cascade's XY bounds (in texels) to
 * reduce shadow pop-in from casters just outside the view frustum and from
 * stabilization snapping. Higher values trade resolution for coverage.
 * use_constant_cascade_size forces each cascade's XY bounds to a size derived
 * from the slice's bounding sphere radius (rather than the light-space AABB of
 * the slice corners). This reduces shimmering caused by cascade extents
 * "breathing" as the camera rotates relative to the light.
 * cascade_blend_range is a view-space distance (in the same units as the
 * camera clip planes) over which the shader cross-fades between cascades near
 * split planes. Use 0 to disable blending.
 * shadow_distance_fade_range is a view-space distance used to fade out shadow
 * strength near the farthest split to avoid hard cutoffs. Use 0 to disable.
 * anchor_snap_texels snaps the shadow anchor in light space to a coarse grid
 * (in texels of cascade 0) to reduce long-range drift as the camera moves.
 * z_extension_factor extends the light-space depth range to capture shadow
 * casters outside the camera frustum. Value is multiplied by the cascade's
 * bounding sphere radius. Only used if scene_bounds.use_scene_bounds is false.
 *
 * depth_bias_* are Vulkan rasterization depth-bias parameters applied when
 * rendering the shadow map (receiver-side bias is controlled by shadow_bias /
 * normal_bias/slope_bias in the world shader).
 * shadow_bias_texel_scale and shadow_slope_bias_texel_scale add per-cascade
 * bias based on world-units-per-texel (0 disables).
 * foliage_alpha_cutoff_bias adds a small amount to alpha_cutoff for foliage
 * materials during shadow map rendering to reduce cutout flicker.
 * foliage_alpha_dither enables a world-space dither for foliage cutout in the
 * shadow pass. This is shadow-only and does not affect the main material pass.
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
  float32_t depth_bias_constant_factor;
  float32_t depth_bias_clamp;
  float32_t depth_bias_slope_factor;
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t shadow_slope_bias;
  float32_t shadow_bias_texel_scale;
  float32_t shadow_slope_bias_texel_scale;
  float32_t pcf_radius;
  float32_t shadow_uv_margin_scale_per[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_uv_soft_margin_scale_per[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_uv_kernel_margin_scale_per[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_distance_fade_range;
  float32_t foliage_alpha_cutoff_bias;
  bool8_t foliage_alpha_dither;
  bool8_t use_constant_cascade_size;
  float32_t cascade_blend_range;
  float32_t anchor_snap_texels;
  bool8_t stabilize_cascades;
  bool8_t debug_show_cascades;
  VkrShadowSceneBounds scene_bounds;
} VkrShadowConfig;

/**
 * @brief High-quality CSM preset (recommended on modern GPUs).
 *
 * Uses 4 cascades and a 2048 shadow map to minimize aliasing. The world shader
 * uses Poisson PCF and scales the effective radius per cascade based on
 * world-units-per-texel to keep softness roughly consistent in world space.
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
      .depth_bias_constant_factor = 0.0f,                                      \
      .depth_bias_clamp = 0.0f,                                                \
      .depth_bias_slope_factor = 0.0f,                                         \
      .shadow_bias = 0.00001f,                                                 \
      .normal_bias = 0.001f,                                                   \
      .shadow_slope_bias = 0.0002f,                                            \
      .shadow_bias_texel_scale = 0.0002f,                                      \
      .shadow_slope_bias_texel_scale = 0.0001f,                                \
      .pcf_radius = 1.6f,                                                      \
      .shadow_uv_margin_scale_per = {0.75f, 1.0f, 1.25f, 1.5f},                \
      .shadow_uv_soft_margin_scale_per = {1.0f, 1.25f, 1.5f, 2.0f},            \
      .shadow_uv_kernel_margin_scale_per = {1.0f, 1.1f, 1.15f, 1.2f},          \
      .shadow_distance_fade_range = 1.0f,                                      \
      .foliage_alpha_cutoff_bias = 0.02f,                                      \
      .foliage_alpha_dither = true_v,                                          \
      .use_constant_cascade_size = true_v,                                     \
      .cascade_blend_range = 1.0f,                                             \
      .anchor_snap_texels = 16.0f,                                             \
      .stabilize_cascades = true_v,                                            \
      .debug_show_cascades = false_v,                                          \
      .scene_bounds = VKR_SHADOW_SCENE_BOUNDS_DEFAULT,                         \
  })

/**
 * @brief Balanced CSM preset (better performance/memory footprint).
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
      .shadow_bias_texel_scale = 0.001f,                                       \
      .shadow_slope_bias_texel_scale = 0.001f,                                 \
      .pcf_radius = 2.0f,                                                      \
      .shadow_uv_margin_scale_per = {0},                                       \
      .shadow_uv_soft_margin_scale_per = {0},                                  \
      .shadow_uv_kernel_margin_scale_per = {0},                                \
      .shadow_distance_fade_range = 10.0f,                                     \
      .foliage_alpha_cutoff_bias = 0.05f,                                      \
      .foliage_alpha_dither = true_v,                                          \
      .use_constant_cascade_size = true_v,                                     \
      .cascade_blend_range = 8.0f,                                             \
      .anchor_snap_texels = 8.0f,                                              \
      .stabilize_cascades = true_v,                                            \
      .debug_show_cascades = false_v,                                          \
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
 * @brief CPU-side frame data to upload to the world shader.
 *
 * The selected renderer owns the shadow image; this structure contains only
 * backend-neutral packet inputs.
 */
typedef struct VkrShadowFrameData {
  bool8_t enabled;
  uint32_t cascade_count;
  float32_t shadow_map_inv_size[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t pcf_radius;
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t shadow_slope_bias;
  float32_t shadow_bias_texel_scale;
  float32_t shadow_slope_bias_texel_scale;
  float32_t shadow_uv_margin_scale[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_uv_soft_margin_scale[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_uv_kernel_margin_scale[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t shadow_distance_fade_range;
  float32_t cascade_blend_range;
  bool8_t debug_show_cascades;

  float32_t split_far[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t world_units_per_texel[VKR_SHADOW_CASCADE_COUNT_MAX];
  Vec2 light_space_origin[VKR_SHADOW_CASCADE_COUNT_MAX];
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

  bool8_t initialized;
} VkrShadowSystem;

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

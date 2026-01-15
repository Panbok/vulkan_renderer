/**
 * @file vkr_shadow_system.h
 * @brief Cascaded shadow mapping (directional light) system.
 *
 * Owns shadow map render targets and per-cascade matrices. Produces per-frame
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

#define VKR_SHADOW_CASCADE_COUNT_MAX 4
#define VKR_SHADOW_MAP_SIZE_DEFAULT 1024

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
  Vec3 bounds_center;
  float32_t bounds_radius;
} VkrCascadeData;

/**
 * @brief Axis-aligned bounding box for shadow scene bounds.
 *
 * When set (use_scene_bounds = true), the shadow system extends each cascade's
 * light-space depth range to include this entire bounding box. This ensures all
 * potential shadow casters are included in shadow maps regardless of camera
 * position, eliminating shadow "pop-in" when moving.
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
      .min = {-100.0f, -100.0f, -100.0f},                                      \
      .max = {100.0f, 100.0f, 100.0f},                                         \
      .use_scene_bounds = true_v,                                              \
  })

/**
 * @brief Shadow system configuration.
 *
 * cascade_count is clamped to [1, VKR_SHADOW_CASCADE_COUNT_MAX].
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
 * z_extension_factor extends the light-space depth range to capture shadow
 * casters outside the camera frustum. Value is multiplied by the cascade's
 * bounding sphere radius. Only used if scene_bounds.use_scene_bounds is false.
 */
typedef struct VkrShadowConfig {
  uint32_t cascade_count;
  uint32_t shadow_map_size;
  float32_t cascade_split_lambda;
  float32_t max_shadow_distance;
  float32_t cascade_guard_band_texels;
  float32_t z_extension_factor;
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t pcf_radius;
  bool8_t use_constant_cascade_size;
  float32_t cascade_blend_range;
  bool8_t stabilize_cascades;
  bool8_t debug_show_cascades;
  VkrShadowSceneBounds scene_bounds;
} VkrShadowConfig;

#define VKR_SHADOW_CONFIG_DEFAULT                                              \
  ((VkrShadowConfig){                                                          \
      .cascade_count = 3,                                                      \
      .shadow_map_size = VKR_SHADOW_MAP_SIZE_DEFAULT,                          \
      .cascade_split_lambda = 0.75f,                                           \
      .max_shadow_distance = 120.0f,                                           \
      .cascade_guard_band_texels = 128.0f,                                     \
      .z_extension_factor = 5.0f,                                              \
      .shadow_bias = 0.001f,                                                   \
      .normal_bias = 0.01f,                                                    \
      .pcf_radius = 1.0f,                                                      \
      .use_constant_cascade_size = true_v,                                     \
      .cascade_blend_range = 30.0f,                                            \
      .stabilize_cascades = true_v,                                            \
      .debug_show_cascades = false_v,                                          \
      .scene_bounds = VKR_SHADOW_SCENE_BOUNDS_DEFAULT,                         \
  })

/**
 * @brief Per-frame shadow resources (one per swapchain image).
 */
typedef struct VkrShadowFrameResources {
  VkrTextureOpaqueHandle shadow_maps[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrRenderTargetHandle shadow_targets[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowFrameResources;

/**
 * @brief CPU-side frame data to upload to the world shader.
 *
 * shadow_maps is indexed by cascade and is specific to the swapchain image.
 */
typedef struct VkrShadowFrameData {
  bool8_t enabled;
  uint32_t cascade_count;
  float32_t shadow_map_inv_size;
  float32_t pcf_radius;
  float32_t shadow_bias;
  float32_t normal_bias;
  float32_t cascade_blend_range;
  bool8_t debug_show_cascades;

  float32_t split_far[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t world_units_per_texel[VKR_SHADOW_CASCADE_COUNT_MAX];
  Mat4 view_projection[VKR_SHADOW_CASCADE_COUNT_MAX];

  VkrTextureOpaqueHandle shadow_maps[VKR_SHADOW_CASCADE_COUNT_MAX];
} VkrShadowFrameData;

/**
 * @brief Shadow system state.
 *
 * frames is allocated with the renderer allocator and must be freed on
 * shutdown.
 */
typedef struct VkrShadowSystem {
  VkrShadowConfig config;
  VkrCascadeData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t cascade_splits[VKR_SHADOW_CASCADE_COUNT_MAX + 1];

  uint32_t frame_resource_count;
  VkrShadowFrameResources *frames;

  VkrRenderPassHandle shadow_renderpass;
  bool8_t owns_renderpass;
  VkrShaderConfig shader_config;
  VkrPipelineHandle shadow_pipeline;
  Vec3 light_direction;
  bool8_t light_enabled;

  bool8_t initialized;
} VkrShadowSystem;

/**
 * @brief Initialize shadow system resources and pipeline.
 *
 * Creates a named shadow renderpass if needed, allocates per-frame depth
 * textures and render targets, and builds the shadow pipeline from shadercfg.
 */
bool8_t vkr_shadow_system_init(VkrShadowSystem *system,
                               struct s_RendererFrontend *rf,
                               const VkrShadowConfig *config);

/**
 * @brief Destroy shadow system resources.
 *
 * Releases render targets, textures, pipeline, and renderpass if owned.
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
 * @brief Get the render target for the given frame/cascade.
 */
VkrRenderTargetHandle
vkr_shadow_system_get_render_target(const VkrShadowSystem *system,
                                    uint32_t frame_index,
                                    uint32_t cascade_index);

/**
 * @brief Fill frame data for shader upload and sampler binding.
 */
void vkr_shadow_system_get_frame_data(const VkrShadowSystem *system,
                                      uint32_t frame_index,
                                      VkrShadowFrameData *out_data);

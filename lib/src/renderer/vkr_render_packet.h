#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_transform.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_instance_buffer.h"
#include "renderer/vkr_renderer.h"

/** Version constant for VkrRenderPacket.packet_version validation. */
#define VKR_RENDER_PACKET_VERSION 2u

/**
 * @brief Alias for mesh handles used by stateless draw items.
 *
 * Generation rules:
 * - generation != 0: mesh instance handle (managed by mesh manager)
 * - generation == 0: mesh slot handle (non-instanced mesh index + 1)
 */
typedef VkrMeshInstanceHandle VkrMeshHandle;

/**
 * @brief Frame-level metadata provided by the application.
 *
 * window_width/height must match the swapchain dimensions from
 * vkr_renderer_prepare_frame(). viewport_width/height of 0 means "use window
 * dimensions". frame_index is app-defined and not used for buffering.
 */
typedef struct VkrFrameInfo {
  uint32_t frame_index;
  float64_t delta_time;
  uint32_t window_width;
  uint32_t window_height;
  uint32_t viewport_width;
  uint32_t viewport_height;
  bool8_t editor_enabled;
} VkrFrameInfo;

/**
 * @brief Global camera and lighting data for the frame.
 *
 * These values are consumed by shaders and remain valid only for the submit.
 */
typedef struct VkrFrameGlobals {
  Mat4 view;
  Mat4 projection;
  Vec3 view_position;
  Vec4 ambient_color;
  uint32_t render_mode;
} VkrFrameGlobals;

/**
 * @brief Draw item referencing cached resources and instance data ranges.
 *
 * first_instance indexes into the payload's instance array and must satisfy
 * (first_instance + instance_count) <= payload->instance_count.
 */
typedef struct VkrDrawItem {
  VkrMeshHandle mesh;
  uint32_t submesh_index;
  VkrMaterialHandle material;
  uint32_t instance_count;
  uint32_t first_instance;
  uint64_t sort_key;
  VkrPipelineHandle pipeline_override;
} VkrDrawItem;

/**
 * @brief Payload for the world pass (opaque + transparent draw lists).
 */
typedef struct VkrWorldPassPayload {
  const VkrDrawItem *opaque_draws;
  uint32_t opaque_draw_count;
  const VkrDrawItem *transparent_draws;
  uint32_t transparent_draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrWorldPassPayload;

/**
 * @brief Optional overrides for shadow depth bias settings.
 */
typedef struct VkrShadowConfigOverride {
  float32_t depth_bias_constant;
  float32_t depth_bias_slope;
  float32_t depth_bias_clamp;
} VkrShadowConfigOverride;

/**
 * @brief Payload for the shadow pass across cascades.
 *
 * cascade_count must be in [1, VKR_SHADOW_CASCADE_COUNT_MAX].
 */
typedef struct VkrShadowPassPayload {
  uint32_t cascade_count;
  Mat4 light_view_proj[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t split_depths[VKR_SHADOW_CASCADE_COUNT_MAX];
  const VkrDrawItem *opaque_draws;
  uint32_t opaque_draw_count;
  const VkrDrawItem *alpha_draws;
  uint32_t alpha_draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
  const VkrShadowConfigOverride *config_override;
} VkrShadowPassPayload;

/**
 * @brief Payload for the UI pass.
 */
typedef struct VkrUiPassPayload {
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrUiPassPayload;

/**
 * @brief Payload for the skybox pass.
 */
typedef struct VkrSkyboxPassPayload {
  VkrTextureHandle cubemap;
  VkrMaterialHandle material;
} VkrSkyboxPassPayload;

/**
 * @brief Payload for the editor pass.
 */
typedef struct VkrEditorPassPayload {
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrEditorPassPayload;

/**
 * @brief Payload for the picking pass (request-driven).
 *
 * pending=false skips the pass entirely.
 */
typedef struct VkrPickingPassPayload {
  bool8_t pending;
  uint32_t x;
  uint32_t y;
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrPickingPassPayload;

/**
 * @brief Per-text slot update applied during submit.
 *
 * content/transform are optional; NULL means "no change".
 */
typedef struct VkrTextUpdate {
  uint32_t text_id;
  String8 content;
  const VkrTransform *transform;
} VkrTextUpdate;

/**
 * @brief Text update payload for world and UI text systems.
 */
typedef struct VkrTextUpdatesPayload {
  const VkrTextUpdate *world_text_updates;
  uint32_t world_text_update_count;
  const VkrTextUpdate *ui_text_updates;
  uint32_t ui_text_update_count;
} VkrTextUpdatesPayload;

/**
 * @brief Optional GPU debug and telemetry requests for the frame.
 */
typedef struct VkrGpuDebugPayload {
  bool8_t enable_timing;
  bool8_t capture_pass_timestamps;
} VkrGpuDebugPayload;

/**
 * @brief Render packet consumed by the stateless renderer frontend.
 *
 * All pointers are app-owned and must remain valid until submit returns.
 * Non-NULL pass payloads enable their corresponding render-graph passes.
 */
typedef struct VkrRenderPacket {
  uint32_t packet_version;
  VkrFrameInfo frame;
  VkrFrameGlobals globals;
  const VkrWorldPassPayload *world;
  const VkrShadowPassPayload *shadow;
  const VkrSkyboxPassPayload *skybox;
  const VkrUiPassPayload *ui;
  const VkrEditorPassPayload *editor;
  const VkrPickingPassPayload *picking;
  const VkrTextUpdatesPayload *text_updates;
  const VkrGpuDebugPayload *debug;
} VkrRenderPacket;

/**
 * @brief Validation error detail for packet submission.
 *
 * field_path/message pointers remain valid until submit returns.
 */
typedef struct VkrValidationError {
  VkrRendererError code;
  const char *field_path;
  const char *message;
} VkrValidationError;

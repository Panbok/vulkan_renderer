#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_transform.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_shadow_system.h"
#include "renderer/vkr_bloom.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_exposure.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gtao.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_renderer.h"
#include "renderer/vkr_temporal.h"

/** Version constant for VkrRenderPacket.packet_version validation. */
#define VKR_RENDER_PACKET_VERSION 23u

#define VKR_FRAME_IBL_PROBE_MAX 16u
#define VKR_PREPARED_TEXT_DRAW_MAX 64u

/** Frame-local reflection probe descriptor lowered by the selected renderer. */
typedef struct VkrFrameIblProbe {
  /** Published L2 diffuse coefficient slot (ADR-038). Resolve it from the
      probe's source cubemap with vkr_renderer_ibl_sh_slot(); slot 0 is the
      valid black sentinel. Must be less than VKR_SH_SLOT_CAPACITY. */
  uint32_t sh_slot;
  VkrTextureHandle prefilter;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t weight;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  bool8_t box_projection_enabled;
} VkrFrameIblProbe;

/** Packet-facing alias for generation-safe scene mesh-instance identity. */
typedef VkrMeshInstanceHandle VkrMeshHandle;

#define VKR_MESH_HANDLE_INVALID VKR_MESH_INSTANCE_HANDLE_INVALID

/**
 * @brief Frame-level metadata provided by the application.
 *
 * window_width/height must match the swapchain dimensions from
 * vkr_renderer_prepare_frame(). viewport_width/height of 0 means "use window
 * dimensions". frame_index is app-defined, but temporal history requires each
 * successfully submitted frame to increment it by one; gaps reset history.
 */
typedef struct VkrFrameInfo {
  uint32_t frame_index;
  float64_t delta_time;
  uint32_t window_width;
  uint32_t window_height;
  uint32_t viewport_width;
  uint32_t viewport_height;
  bool8_t editor_enabled;
  uint64_t scene_generation;
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
  /**
   * Exposure controls, versioned as of packet 20. The pre-20 single `exposure`
   * multiplier could not also carry a logarithmic bias, so the linear and
   * logarithmic controls are separate fields and the mode selects which path
   * consumes them. `manual_exposure` keeps the old field's meaning and default.
   */
  uint32_t exposure_mode;
  float32_t manual_exposure;
  /** Additive EV bias. Automatic mode only; manual mode ignores it. */
  float32_t exposure_compensation_ev;
  uint32_t render_mode;
  /**
   * Bloom controls, added in packet 21. A zeroed block disables bloom, so a
   * updated caller that leaves the new fields zeroed keeps byte-identical
   * output: no bloom resource and no bloom pass is instantiated for the frame.
   *
   * The threshold is scene-linear, so exposure changes how strong the result
   * looks without changing which scene values entered the chain. Chain length,
   * firefly ceiling, and filter selection are cold configuration rather than
   * packet fields; they describe the resource contract, not art direction.
   */
  bool8_t bloom_enabled;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  /**
   * GTAO controls, added in packet 22. A zeroed block disables the dedicated
   * current-frame depth pyramid and AO passes. Radius is expressed in positive
   * view-space units; power shapes the final ambient visibility.
   */
  bool8_t gtao_enabled;
  float32_t gtao_radius;
  float32_t gtao_power;
  /**
   * Renderer-owned temporal state. Callers leave this zeroed; the frontend
   * derives it after validation and commits it only after successful submit.
   */
  VkrTemporalFrame temporal;
  /** Renderer-owned exposure state, derived and committed the same way. */
  VkrExposureFrame exposure;
  /** Renderer-owned bloom state, normalized from the fields above. */
  VkrBloomFrame bloom;
  /** Renderer-owned GTAO state, normalized from the fields above. */
  VkrGtaoFrame gtao;
} VkrFrameGlobals;

/** Backend-neutral frame lighting controls consumed by world shading. */
typedef struct VkrFrameLighting {
  bool8_t directional_enabled;
  Vec3 directional_direction;
  Vec3 directional_color;
  float32_t directional_intensity;
  bool8_t ibl_enabled;
  /** Logical cubemap used to derive global IBL; independent of the visible
   * skybox so retained backends do not have to infer lighting from a pass. */
  VkrTextureHandle ibl_source;
  float32_t ibl_intensity;
  float32_t ibl_diffuse_intensity;
  float32_t ibl_specular_intensity;
  /** Borrowed, frame-local scene light table and its conservative lookup. */
  const VkrPointLight *point_lights;
  uint32_t point_light_count;
  const VkrPointLightGrid *point_light_grid;
  const VkrFrameIblProbe *ibl_probes;
  uint32_t ibl_probe_count;
} VkrFrameLighting;

/**
 * @brief Draw item referencing cached resources and instance data ranges.
 *
 * first_instance indexes into the payload's instance array and must satisfy
 * (first_instance + instance_count) <= payload->instance_count.
 */
typedef struct VkrDrawItem {
  VkrMeshHandle mesh;
  /** Shared GPU geometry identity. GPU-driven renderers resolve this handle so

   * * repeated scene instances do not duplicate vertex/index allocations. */
  VkrGeometryHandle geometry;
  uint32_t submesh_index;
  VkrMaterialHandle material;
  uint32_t instance_count;
  uint32_t first_instance;
  uint64_t sort_key;
} VkrDrawItem;

/** The candidate has a conservative local-space bounding sphere. */
#define VKR_WORLD_DRAW_CANDIDATE_BOUNDS_VALID 0x1u
/** The candidate participates in the camera opaque/cutout view. */
#define VKR_WORLD_DRAW_CANDIDATE_CAMERA_OPAQUE 0x2u
/** The candidate participates in shadow-cascade views. */
#define VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER 0x4u

/**
 * @brief Unculled multi-view draw source row borrowed for one packet
 * submission.
 *
 * GPU culling consumes one source row per original instance x submesh pair.
 */
typedef struct VkrWorldDrawCandidate {
  VkrMeshHandle mesh;
  VkrGeometryHandle geometry;
  uint32_t submesh_index;
  VkrMaterialHandle material;
  VkrInstanceDataGPU instance;
  Vec4 local_bounding_sphere;
  uint32_t state_bucket;
  uint32_t flags;
} VkrWorldDrawCandidate;

/**
 * @brief Prepared retained-text geometry borrowed for one packet submission.
 *
 * Text shaping and atlas selection are frontend work. Backends only lower the
 * already-shaped indexed geometry and the logical atlas handle. revision is
 * incremented whenever the borrowed geometry changes so a backend may cache a
 * private copy without comparing content.
 */
typedef struct VkrPreparedTextDraw {
  const VkrTextVertex *vertices;
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  uint32_t max_index;
  VkrTextureHandle atlas;
  Mat4 model;
  float32_t screen_px_range;
  uint32_t font_mode;
  uint32_t object_id;
  uint32_t revision;
} VkrPreparedTextDraw;

/**
 * @brief Payload for GPU-driven world stages and retained ordinary blend.
 */
typedef struct VkrWorldPassPayload {
  const VkrWorldDrawCandidate *gpu_candidates;
  uint32_t gpu_candidate_count;
  /** Rows in gpu_candidates eligible for the camera opaque/cutout view. */
  uint32_t gpu_camera_opaque_candidate_count;
  /** Rows in gpu_candidates eligible for shadow-cascade views. */
  uint32_t gpu_shadow_candidate_count;
  /** Independent unculled transmissive stream consumed by Metal P12. */
  const VkrWorldDrawCandidate *transmission_gpu_candidates;
  uint32_t transmission_gpu_candidate_count;
  /** Camera-culled, back-to-front ordinary blend draws. */
  const VkrDrawItem *transparent_draws;
  uint32_t transparent_draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
  const VkrPreparedTextDraw *text_draws;
  uint32_t text_draw_count;

  /**
   * Shadow-caster mobility partition and the generations that describe it.
   *
   * Cascade reuse needs three things this block provides: which generations the
   * captured contents correspond to, a bounded set of dynamic casters to test
   * for overlap, and whether any candidate was omitted at the publication
   * boundary. Without them a reuse decision would be made on faith.
   *
   * The partition is a *view* of `gpu_candidates`, not a second copy: static
   * candidates occupy `[0, static_candidate_count)` and dynamic candidates
   * `[static_candidate_count, gpu_shadow_candidate_count)`. One stream keeps
   * the GPU classify path free of a per-candidate mobility branch.
   *
   * `publication_pending` is true when any candidate was dropped this frame
   * because its geometry or material had not published yet. A cascade cannot be
   * reused while it is set: the missing caster might belong inside the volume.
   */
  uint32_t static_candidate_count;
  uint64_t static_generation;
  uint64_t dynamic_generation;
  /** Selected-backend geometry/material resolvability generation. */
  uint64_t publication_generation;
  uint64_t caster_bounds_generation;
  bool8_t publication_pending;
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
 * @brief One cascade's receiver-facing description.
 *
 * These values must describe the matrix actually published for this cascade.
 * A cascade reused from retained contents publishes the fit it was *rendered*
 * with, so its texel size, origin, and depth span come from that committed fit
 * rather than from the current frame's raw fit.
 *
 * `light_view_projection` maps world space to the cascade's light clip space.
 * `split_near_far_texel_depth` is (near, far, world units per texel, fitted
 * light-space depth span). Near and far are view-space distances along forward
 * and bound the slice used for cascade selection and cross-fade. The depth span
 * is the divisor that converts a texel-denominated receiver bias into the
 * normalized orthographic depth the shadow map stores.
 * `origin_inv_size_pad` is (light-space origin x, y, 1 / shadow map size, 0).
 * The origin is in the receiver's reconstructed right/up basis, which is what
 * makes the rotated kernel's cell hash stable under light-view translation.
 */
typedef struct VkrShadowCascadePacketData {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth;
  Vec4 origin_inv_size_pad;
} VkrShadowCascadePacketData;

/**
 * @brief Receiver filter and bias state shared by every cascade.
 *
 * Every bias here is denominated in shadow-map texels, not in normalized depth.
 * The receiver converts through the owning cascade's `world_units_per_texel`
 * and `light_space_depth_span`, so one configured value means the same world
 * distance in cascade 0 and cascade 3 even though their fitted Z ranges differ
 * by orders of magnitude. Raster depth bias is a separate control in backend
 * units and stays in `VkrShadowConfigOverride`.
 */
typedef struct VkrShadowReceiverPacketData {
  float32_t receiver_bias_texels;
  float32_t slope_bias_texels;
  /** Texel count converted to a world-space normal offset before projection. */
  float32_t normal_offset_texels;
  float32_t pcf_radius_texels;
  /** Must pass vkr_shadow_pcf_sample_count_supported() at the cold boundary. */
  uint32_t pcf_sample_count;
  /** Zero or one. Enables the nine-tap uniform-region probe at 16+ taps. */
  bool32_t pcf_uniform_early_out;
  /** Fraction of a cascade's span spent cross-fading into the next one. */
  float32_t cascade_blend_fraction;
  /** View distances over which shadow strength falls to zero. */
  float32_t fade_start;
  float32_t fade_end;
} VkrShadowReceiverPacketData;

/**
 * @brief Payload for the shadow pass across cascades.
 *
 * cascade_count must be in [1, VKR_SHADOW_CASCADE_COUNT_MAX].
 */
typedef struct VkrShadowPassPayload {
  uint32_t cascade_count;
  bool8_t sdsm_enabled;
  /** Bit i is set only when cascade i must execute its graph pass. */
  uint32_t cascade_render_mask;
  VkrShadowCascadePacketData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrShadowReceiverPacketData receiver;
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
  const VkrPreparedTextDraw *text_draws;
  uint32_t text_draw_count;
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
  bool8_t capture_submission_timing;
  /** 0=off, 1=cascade index, 2=shadow factor, 3=sampled map depth. */
  uint32_t shadow_debug_mode;
  /** Optional batch reserved and copied by this frame. Borrowed for submit. */
  const VkrCaptureBatchRequest *capture;
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
  const VkrFrameLighting *lighting;
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

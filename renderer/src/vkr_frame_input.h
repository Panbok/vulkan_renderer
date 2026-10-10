#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_transform.h"
#include "vkr_atmosphere.h"
#include "vkr_bloom.h"
#include "vkr_buffer.h"
#include "vkr_decal.h"
#include "vkr_dof.h"
#include "vkr_exposure.h"
#include "vkr_fog.h"
#include "vkr_froxel_fog.h"
#include "vkr_gpu_abi.h"
#include "vkr_gtao.h"
#include "vkr_ibl_math.h"
#include "vkr_lighting.h"
#include "vkr_motion_blur.h"
#include "vkr_render_resources.h"
#include "vkr_renderer.h"
#include "vkr_shadow.h"
#include "vkr_subsurface.h"
#include "vkr_temporal.h"
#include "vkr_ui_draw_types.h"

/** Version constant for VkrFrameInput.version validation. */
#define VKR_FRAME_INPUT_VERSION 53u

#define VKR_FRAME_IBL_PROBE_MAX 16u

/** Cold-boundary limits for the frame-local retained UI stream. */
#define VKR_UI_VERTEX_CAPACITY 65536u
#define VKR_UI_INDEX_CAPACITY 98304u
#define VKR_UI_BATCH_CAPACITY 4096u

/** Frame-local reflection probe descriptor lowered by the selected renderer. */
typedef struct VkrFrameIblProbe {
  /** Published L2 diffuse coefficient slot (ADR-038). Resolve it from the
      probe's source cubemap with vkr_render_assets_ibl_sh_slot(); slot 0 is the
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
 * vkr_renderer_begin_frame(). viewport_width/height of 0 means "use window
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

/** Presentation transform applied after scene-linear exposure. */
typedef enum VkrDisplayTransform {
  VKR_DISPLAY_TRANSFORM_AGX = 0,
  VKR_DISPLAY_TRANSFORM_ACES_FITTED = 1,
  VKR_DISPLAY_TRANSFORM_COUNT,
} VkrDisplayTransform;

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
  /** The EV range automatic exposure settles in, as look volumes set it
      (packet 53, ADR-097); equal values keep the renderer's metering
      range. */
  float32_t exposure_min_ev;
  float32_t exposure_max_ev;
  /** Presentation transform selected for the fullscreen tonemap pass. */
  uint32_t display_transform;
  /** White balance offsets normalized to [-1, 1]. */
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  /** Restrained display grading after metering and before display transform. */
  float32_t color_contrast;
  float32_t color_saturation;
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
  /** Presentation-only sharpness in [0, 1]. Zero bypasses sharpening. */
  float32_t image_sharpness;
  /** Half-resolution opaque SSR with independent reflection history. */
  bool8_t ssr_enabled;
  bool8_t ssgi_enabled;
  VkrFogSettings fog;
  /** Opt-in local participating medium; supersedes analytic fog when enabled.
   */
  VkrFroxelFogSettings froxel_fog;
  /** Optional post-reconstruction lens blur; a zeroed block disables it. */
  bool8_t dof_enabled;
  float32_t dof_focus_distance;
  float32_t dof_f_stop;
  bool8_t motion_blur_enabled;
  /** Centred exposure angle in [0,360] degrees; zero also bypasses blur. */
  float32_t motion_blur_shutter_angle;
} VkrFrameGlobals;

/** Backend-neutral frame lighting controls consumed by world shading. */
typedef struct VkrFrameLighting {
  bool8_t directional_enabled;
  Vec3 directional_direction;
  Vec3 directional_color;
  float32_t directional_intensity;
  bool8_t ibl_enabled;
  /** Logical cubemap used to derive global IBL; independent of the visible
   * sky so retained backends do not have to infer lighting from a pass. */
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
  VkrDiffuseVolumeBinding diffuse_volume;
  /** Sampled only by the tiled pipeline (ADR-087, ADR-088). */
  VkrLightmapBinding lightmap;
  VkrSubsurfaceBinding subsurface;
  const VkrRectangleLight *rectangle_lights;
  uint32_t rectangle_light_count;
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
/** The candidate participates in directional and local shadow views. */
#define VKR_WORLD_DRAW_CANDIDATE_SHADOW_CASTER 0x4u
/** Local shadows route this caster through ordered refractive transmission. */
#define VKR_WORLD_DRAW_CANDIDATE_SHADOW_TRANSMISSION 0x8u

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
  Vec2 unit_range;
  uint32_t font_mode;
  uint32_t object_id;
  uint32_t revision;
} VkrPreparedTextDraw;

/**
 * @brief One frame-local UI geometry stream split into ordered scissor batches.
 *
 * All arrays are borrowed until packet submission returns. Vertices use the
 * shared 32-byte UI/text GPU record, while batches retain Y-down attachment
 * scissors for direct lowering by both native APIs.
 */
typedef struct VkrPreparedUiDrawList {
  const VkrUiVertex *vertices;
  uint32_t vertex_count;
  const uint32_t *indices;
  uint32_t index_count;
  const VkrUiDrawBatch *batches;
  uint32_t batch_count;
} VkrPreparedUiDrawList;

#define VKR_WORLD_MATERIAL_FEATURE_CLEARCOAT 0x1u
#define VKR_WORLD_MATERIAL_FEATURE_SHEEN 0x2u
#define VKR_WORLD_MATERIAL_FEATURE_ANISOTROPY 0x4u
#define VKR_WORLD_MATERIAL_FEATURE_ALL 0x7u

/** Immutable source arrays and current pose borrowed through render_frame.
 * Vertices use the complete geometry-local order. Unreferenced vertices may
 * carry joints from other skins; kernels must guard joint_count before reads.
 * A matching temporal identity and discontinuity is required for motion reuse.
 */
typedef struct VkrSkinningInput {
  VkrGeometryHandle geometry;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  uint64_t pose_generation;
  uint64_t discontinuity;
  const VkrVertex3d *vertices;
  const VkrSkinningInfluence *influences;
  const Mat4 *palette;
  uint32_t vertex_count;
  uint32_t joint_count;
} VkrSkinningInput;

/**
 * One static-world change: the static generation it produced and the
 * world-space box it may have altered, or anywhere when not `bounded`. One
 * generation may list several boxes.
 */
typedef struct VkrStaticChange {
  uint64_t generation;
  Vec3 min;
  Vec3 max;
  bool8_t bounded;
} VkrStaticChange;

/**
 * @brief Payload for GPU-driven world stages and retained ordinary blend.
 */
typedef struct VkrWorldPassPayload {
  const VkrSkinningInput *skinning;
  uint32_t skinning_count;
  /** Conservative aggregate over opaque/cutout source materials. Unknown
   * aggregates retain all planes for packet producers that do not extract it.
   */
  uint32_t opaque_material_features;
  bool8_t opaque_material_features_valid;
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
  /** Those of transparent_draws whose transmissive material has a volume
   * (thickness > 0) or a rough surface (VkrMaterial::rough). The tiled
   * pipeline copies the scene for their refraction only when this is nonzero
   * (ADR-087). */
  uint32_t refractive_draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
  const VkrPreparedTextDraw *text_draws;
  uint32_t text_draw_count;
  /** Projected decals in compositing order, the last on top, and their
   * world-space lookup; only the tiled pipeline draws them (ADR-092). */
  const VkrDecal *decals;
  uint32_t decal_count;
  const VkrDecalGrid *decal_grid;

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
  /**
   * The static changes of every generation after `static_change_floor`,
   * oldest first. Retained shadows drawn at a static generation not below the
   * floor stay valid where no later change reaches; NULL means unknown, and
   * every static change then reaches everything.
   */
  const VkrStaticChange *static_changes;
  uint32_t static_change_count;
  uint64_t static_change_floor;
  uint64_t dynamic_generation;
  /** Selected-backend geometry/material resolvability generation. */
  uint64_t publication_generation;
  /**
   * Advances once a publication that can change an admitted caster's shadow
   * has completed: a texture, sampler or material. A geometry publication
   * cannot, because a mesh draws only after its geometry has published and
   * its arrival is a topology change, so streamed or rebuilt geometry leaves
   * retained shadows valid. Zero follows `publication_generation`.
   */
  uint64_t caster_publication_generation;
  uint64_t caster_bounds_generation;
  bool8_t publication_pending;
} VkrWorldPassPayload;

/**
 * Whether static changes after generation `since` may reach the world-space
 * sphere at `center` of `radius`: the list does not cover them, one is
 * unbounded, or one's box meets the sphere.
 */
bool8_t vkr_world_static_changes_reach(const VkrWorldPassPayload *world,
                                       uint64_t since, Vec3 center,
                                       float32_t radius);

/**
 * The publication generation that drawn content follows: completed texture,
 * sampler and material publications, or every publication when the frame
 * does not say. A geometry upload cannot change what draws until its mesh
 * joins the frame, which moves the static generation.
 */
uint64_t
vkr_world_content_publication_generation(const VkrWorldPassPayload *world);

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
 * `origin_inv_size_sun` is (light-space origin x, y, 1 / shadow map size,
 * tangent of half the sun angular diameter). A zero tangent retains PCF.
 * The origin is in the receiver's reconstructed right/up basis, which is what
 * makes the rotated kernel's cell hash stable under light-view translation.
 */
typedef struct VkrShadowCascadePacketData {
  Mat4 light_view_projection;
  Vec4 split_near_far_texel_depth;
  Vec4 origin_inv_size_sun;
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

/** One baked lamp whose moving-caster shadows a frame subtracts from its
 * baked light (ADR-104). The lamp is not in the frame's
 * light table: its light shades only through the lightmap. Its composite
 * views name each face's dynamic square where a moving caster reaches the
 * face and its static square otherwise; its static views name the static
 * squares. Both are blocks of the lamp's face count among the receiver
 * views, so receivers take the moving casters' visibility from the
 * difference of the two. */
/* Receiver views of the scene's lights and two blocks of six per baked
   lamp, then the companion views. */
#define VKR_LOCAL_SHADOW_VIEW_CAPACITY                                         \
  (VKR_LOCAL_SHADOW_FACE_COUNT_MAX +                                           \
   12u * VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX +                               \
   VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX)

typedef struct VkrLocalShadowBakedLamp {
  VkrPointLight light;
  uint32_t composite_first_view;
  uint32_t static_first_view;
} VkrLocalShadowBakedLamp;

/** Fixed application-owned frame payload, borrowed until render returns.
 * First-view entries encode index+1; zero means the light is unshadowed this
 * frame. Views are the resident cache faces whose content is valid after this
 * submission; they need not keep their indices between frames.
 *
 * A face's static square holds the static casters only, or every caster
 * when the slot that drew it was not static-only. A face that a dynamic
 * caster may reach and the camera may see takes a dynamic square for the
 * frame: a copy of its static square plus the dynamic casters. Its view then
 * names the dynamic square, and a companion view after `view_count` names
 * the static square. */
typedef struct VkrLocalShadowPassPayload {
  uint32_t view_count;
  /** Companion views at `view_count` and after: the static squares of the
   * faces whose views name dynamic squares. Receivers never sample them;
   * render slots draw and copy them. */
  uint32_t companion_view_count;
  uint32_t map_size;
  /** Render slots drawn at most per frame and transmission array layers. */
  uint32_t face_budget;
  /** Layers of the shared atlas; a different count recreates it. */
  uint32_t atlas_layer_count;
  /** The trailing atlas layers that hold dynamic squares, and nothing else;
   * zero while the world has no dynamic caster. */
  uint32_t dynamic_layer_count;
  /** The last this many render slots draw dynamic squares:
   * Shadow.Local.Copy fills each from the static square of companion view
   * `dynamic_source_views[slot]`, then the slot draws only the dynamic
   * casters over it, without a clear. They draw no transmission. */
  uint32_t dynamic_render_count;
  /** Render slots before the dynamic ones that draw only the static casters;
   * the others draw every caster. */
  uint64_t static_render_mask;
  /** Faces that wanted a dynamic square this frame; those past
   * VKR_LOCAL_SHADOW_DYNAMIC_FACE_COUNT_MAX show their static square. */
  uint32_t dynamic_faces_wanted;
  /** Baked lamps whose moving-caster shadows this frame shows, and the baked
   * lamps whose range held a moving caster in view (the selection's
   * candidates). */
  uint32_t baked_lamp_count;
  uint32_t baked_lamp_candidates;
  VkrLocalShadowBakedLamp baked_lamps[VKR_LOCAL_SHADOW_BAKED_LAMP_COUNT_MAX];
  /** Atlas layers without retained contents, cleared whole before any face
   * draws. Faces of a cleared layer that are not drawn this submission are
   * not views. */
  uint32_t atlas_clear_mask;
  /** Faces drawn this submission. Render slot i draws view render_views[i],
   * a companion view for the static square of a face that shows a dynamic
   * one, with opaque culling view i; slots below `transmission_render_count`
   * also draw the view's transmission layer with transmission culling view
   * i. */
  uint32_t render_count;
  uint32_t transmission_render_count;
  /** Transmission layers receivers may sample; every layer below it has been
   * drawn. Zero without refractive casters. */
  uint32_t transmission_layer_count;
  /** Whether refractive casters exist, so opaque faces exclude them. */
  bool8_t refractive_casters;
  /** Shadow.LocalMask applies contact shadows to the full-filter lights. */
  bool8_t contact_shadows;
  uint32_t render_views[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  /** Render slots, all below `transmission_render_count`, whose face keeps
   * the opaque content it already holds and draws only its transmission
   * layer. */
  uint64_t retained_opaque_mask;
  uint32_t light_first_view[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t dynamic_source_views[VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX];
  VkrLocalShadowView views[VKR_LOCAL_SHADOW_VIEW_CAPACITY];
} VkrLocalShadowPassPayload;

/** First render slot that draws a dynamic square; `render_count` without
 * one. */
static inline uint32_t vkr_local_shadow_dynamic_render_first(
    const VkrLocalShadowPassPayload *payload) {
  return payload->render_count - payload->dynamic_render_count;
}

/** Atlas layer of the face render slot `slot` draws. */
static inline uint32_t
vkr_local_shadow_render_atlas_layer(const VkrLocalShadowPassPayload *payload,
                                    uint32_t slot) {
  return (uint32_t)payload->views[payload->render_views[slot]].atlas_rect.w;
}

/** Whether the atlas layer of render slot `slot` is cleared whole this
 * submission, so the face needs no clear of its own square. */
static inline bool8_t
vkr_local_shadow_render_layer_cleared(const VkrLocalShadowPassPayload *payload,
                                      uint32_t slot) {
  return (payload->atlas_clear_mask &
          (UINT32_C(1) << vkr_local_shadow_render_atlas_layer(payload,
                                                              slot))) != 0u;
}

/** Transmission layer of the face render slot `slot` draws; valid only for
 * slots below `transmission_render_count`. */
static inline uint32_t vkr_local_shadow_render_transmission_layer(
    const VkrLocalShadowPassPayload *payload, uint32_t slot) {
  return (uint32_t)payload->views[payload->render_views[slot]].shadow_params.y -
         1u;
}

/** Bits of the transmission layers drawn this submission. */
static inline uint64_t vkr_local_shadow_render_transmission_mask(
    const VkrLocalShadowPassPayload *payload) {
  uint64_t mask = 0u;
  for (uint32_t slot = 0u; payload && slot < payload->transmission_render_count;
       ++slot)
    mask |= vkr_local_shadow_view_bits(
        vkr_local_shadow_render_transmission_layer(payload, slot), 1u);
  return mask;
}

/** Bits of the atlas layers that faces draw into this submission. */
static inline uint64_t
vkr_local_shadow_render_layer_mask(const VkrLocalShadowPassPayload *payload) {
  uint64_t mask = 0u;
  for (uint32_t slot = 0u; payload && slot < payload->render_count; ++slot)
    mask |= vkr_local_shadow_view_bits(
        vkr_local_shadow_render_atlas_layer(payload, slot), 1u);
  return mask;
}

/**
 * @brief Payload for the shadow pass across cascades.
 *
 * cascade_count must be in [1, VKR_SHADOW_CASCADE_COUNT_MAX].
 */
typedef struct VkrShadowPassPayload {
  uint32_t cascade_count;
  bool8_t sdsm_enabled;
  /** Cascades from VKR_SHADOW_EVSM_FIRST_CASCADE keep filtered exponential
   * variance moments that deferred lighting samples instead of PCF; requires
   * cascade_count above that cascade. */
  bool8_t evsm_enabled;
  /** Bit i is set only when cascade i must execute its graph pass, which
   * draws every caster into the cascade's shadow-map layer. */
  uint32_t cascade_render_mask;
  /** The world has dynamic casters: the static cascade image exists and a
   * cascade may be composed from its static layer and a dynamic draw. The
   * three masks below are zero without it. */
  bool8_t static_active;
  /** Cascades whose static layer draws the static casters this frame. */
  uint32_t static_render_mask;
  /** Cascades whose static layer is copied into their shadow-map layer;
   * disjoint from cascade_render_mask and includes static_render_mask. */
  uint32_t copy_mask;
  /** Copied cascades that then draw the dynamic casters over the copy. */
  uint32_t dynamic_render_mask;
  VkrShadowCascadePacketData cascades[VKR_SHADOW_CASCADE_COUNT_MAX];
  VkrShadowReceiverPacketData receiver;
  const VkrShadowConfigOverride *config_override;
} VkrShadowPassPayload;

/**
 * @brief Payload for the UI pass.
 */
typedef struct VkrUiPassPayload {
  VkrPreparedUiDrawList draw_list;
} VkrUiPassPayload;

/** Upper bound of a uniform sky radiance, the largest finite RGBA16F value. */
#define VKR_SKY_CONSTANT_RADIANCE_MAX 65504.0f

/**
 * @brief Visible sky and camera-dependent atmosphere for the frame.
 *
 * An enabled `atmosphere` is the published generation's settings, and the two
 * lookup textures are the ones that generation baked (ADR-058). The renderer
 * derives the sky-view lookup, aerial perspective and the visible sun disc
 * from them every frame. A disabled atmosphere shows `constant_radiance`, the
 * uniform radiance of a constant environment source.
 *
 * `clouds` publishes with the atmosphere and requires it. The caller advances
 * `cloud_wind_offset_m` by the wind every frame and wraps each component into
 * [0, VKR_CLOUD_WIND_PERIOD_M) (ADR-074).
 */
typedef struct VkrSkyPassPayload {
  VkrAtmosphereSettings atmosphere;
  VkrTextureHandle transmittance;
  VkrTextureHandle multiple_scattering;
  Vec3 constant_radiance;
  VkrCloudSettings clouds;
  Vec2 cloud_wind_offset_m;
} VkrSkyPassPayload;

/**
 * @brief Payload for the editor pass.
 */
/** One unlit, always-on-top editor handle. Draw order also owns pick priority.
    Geometry is published asset storage; records are borrowed through render. */
typedef struct VkrEditorOverlayDraw {
  VkrGeometryHandle geometry;
  uint32_t submesh_index;
  Mat4 model;
  Vec4 color;
  uint32_t object_id;
} VkrEditorOverlayDraw;

#define VKR_EDITOR_OVERLAY_DRAW_MAX 16u
/** Selection outline mask draws: the selected entity and its descendants.
    A larger selection outlines only its first draws. */
#define VKR_EDITOR_SELECTION_DRAW_MAX 1024u

/** Plane of the editor ground grid. */
typedef enum VkrEditorGridPlane {
  /** y = 0, seen from perspective, top and bottom views. */
  VKR_EDITOR_GRID_PLANE_XZ = 0,
  /** x = 0, seen from the left and right views. */
  VKR_EDITOR_GRID_PLANE_ZY,
  /** z = 0, seen from the front and back views. */
  VKR_EDITOR_GRID_PLANE_XY,
  VKR_EDITOR_GRID_PLANE_COUNT,
} VkrEditorGridPlane;

/** Ground grid through the world origin, drawn over the Scene image before
    the selection outline and handles (ADR-027). Lines anti-alias from their
    screen footprint and step by ten as the camera zooms. */
typedef struct VkrEditorGridPayload {
  bool8_t enabled;
  /** Draw over nearer opaque geometry instead of hiding behind it. */
  bool8_t through_geometry;
  VkrEditorGridPlane plane;
  /** Finest cell in world units, > 0. */
  float32_t cell_size;
  /** Lines fade out from `fade_start` to `fade_end` world units from the
      camera; both zero keep every distance opaque. */
  float32_t fade_start;
  float32_t fade_end;
  /** Where the plane lies along its normal: world Y of the XZ plane, so the
      grid can lie on a floor modelled above the origin, X of the ZY plane
      and Z of the XY plane, the depth a side view is framed on. */
  float32_t height;
} VkrEditorGridPayload;

typedef struct VkrEditorPassPayload {
  /** Scene destination in Y-down swapchain pixels: (x, y, width, height).
      Components are finite integral values validated at packet submission. */
  Vec4 image_rect_px;
  /** Keep the last submitted Scene image while the editor UI continues. */
  bool8_t scene_rendering_stopped;
  /** Blur only the retained Scene composite; UI is drawn afterward. */
  bool8_t scene_backdrop_blur;
  const VkrEditorOverlayDraw *overlay_draws;
  uint32_t overlay_draw_count;
  /** Submeshes whose mask edge is outlined over the Scene image, drawn with
      the overlay camera and geometry path. Records reuse the overlay layout;
      `color` and `object_id` are ignored. Borrowed through render. */
  const VkrEditorOverlayDraw *selection_draws;
  uint32_t selection_draw_count;
  /** Outline color, linear RGB with alpha as coverage. */
  Vec4 selection_color;
  /** Outline reach in Scene-image pixels, 1..8. */
  uint32_t selection_width_px;
  VkrEditorGridPayload grid;
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
  /** Echoed by readback; zero is reserved for capture-only requests. */
  uint64_t request_id;
} VkrPickingPassPayload;

/**
 * @brief Optional GPU debug and telemetry requests for the frame.
 */
typedef struct VkrGpuDebugPayload {
  bool8_t enable_timing;
  bool8_t capture_pass_timestamps;
  bool8_t capture_submission_timing;
  /** Enables the graph-declared fifth transmission peel for focused evidence.
   */
  bool8_t transmission_depth_diagnostic_enabled;
  /** 0=off, 1=cascade index, 2=shadow factor, 3=sampled map depth. */
  uint32_t shadow_debug_mode;
  /** Optional batch reserved and copied by this frame. Borrowed for submit. */
  const VkrCaptureBatchRequest *capture;
} VkrGpuDebugPayload;

/**
 * @brief Caller-owned rendering input for one acquired frame.
 *
 * All pointers are caller-owned and must remain valid until render returns.
 * Streams are authoritative: rendering does not extract UI/text or mutate
 * assets. Non-NULL pass payloads enable their corresponding render-graph
 * passes.
 */
typedef struct VkrAnimationPreviewDraw {
  VkrGeometryHandle geometry;
  uint32_t submesh_index;
  uint32_t skinning_index;
  Mat4 model;
} VkrAnimationPreviewDraw;

/** Independent, fixed 512-square editor view. No main-view history is reused.
 */
typedef struct VkrAnimationPreviewInput {
  Mat4 view_projection;
  const VkrAnimationPreviewDraw *draws;
  uint32_t draw_count;
} VkrAnimationPreviewInput;

typedef struct VkrFrameInput {
  const VkrAnimationPreviewInput *animation_preview;
  uint32_t version;
  VkrFrameInfo frame;
  VkrFrameGlobals globals;
  const VkrFrameLighting *lighting;
  const VkrWorldPassPayload *world;
  const VkrShadowPassPayload *shadow;
  const VkrLocalShadowPassPayload *local_shadow;
  const VkrSkyPassPayload *sky;
  const VkrUiPassPayload *ui;
  const VkrEditorPassPayload *editor;
  const VkrPickingPassPayload *picking;
  const VkrGpuDebugPayload *debug;
} VkrFrameInput;

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

/** Validate caller-owned data before allocating or recording frame work. */
VkrRendererError vkr_frame_input_validate(const VkrFrameInput *packet,
                                          VkrValidationError *out_error);

/** CPU metadata committed only with the history buffer's successful submit. */
typedef struct VkrSkinningHistoryRecord {
  VkrGeometryHandle geometry;
  uint32_t temporal_index;
  uint32_t temporal_generation;
  uint32_t vertex_count;
  uint64_t discontinuity;
  uint64_t offset_bytes;
} VkrSkinningHistoryRecord;

typedef struct VkrSkinningHistory {
  uint64_t producer_submit;
  uint32_t count;
  VkrSkinningHistoryRecord records[VKR_SKINNING_BINDING_CAPACITY];
} VkrSkinningHistory;

bool8_t vkr_skinning_history_prepare(const VkrWorldPassPayload *world,
                                     VkrSkinningHistory *history);
uint64_t vkr_skinning_history_find(const VkrSkinningHistory *history,
                                   const VkrSkinningInput *input);

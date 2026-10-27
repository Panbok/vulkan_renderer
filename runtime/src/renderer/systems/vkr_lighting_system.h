#pragma once

#include "defines.h"
#include "math/vec.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_gpu_abi.h"

#include "vkr_lighting.h"

/** One light's state across frames of a ranked point-light selection: the
 * scene table's (ADR-019) or the tiled pipeline's dynamic-light limit
 * (ADR-087), matched by render id. */
typedef struct VkrPointLightLimitEntry {
  uint32_t render_id;
  /** Intensity scale in [0, 1]: rises toward one while the light is selected
   * and falls toward zero once it is not, so lights fade instead of popping. */
  float32_t weight;
  /** The light was among the selected lights; it ranks with the incumbent
   * bonus. */
  bool8_t selected;
  /** The light held a shadow; it ranks for shadows with the bonus. */
  bool8_t shadowed;
  /** The light was in the table's head; it ranks for the head with the
   * bonus. */
  bool8_t head;
} VkrPointLightLimitEntry;

/** Lights a selection's state records: the selected lights and the drawn
 * lights still fading out, at most two tables' worth. */
#define VKR_POINT_LIGHT_SELECTION_ENTRY_MAX (2u * VKR_MAX_SCENE_POINT_LIGHTS)

/** A ranked selection's state from its previous frame. */
typedef struct VkrPointLightSelection {
  /** Identified lights that were selected or drawn, sorted by render id. */
  VkrPointLightLimitEntry entries[VKR_POINT_LIGHT_SELECTION_ENTRY_MAX];
  uint32_t entry_count;
  Vec3 camera;
  /** Lights competed for the selection: one offered was not drawn or one
   * drawn was still fading. Only then does a newly selected light fade in;
   * otherwise it is new content and shows at once. */
  bool8_t contended;
  bool8_t valid;
} VkrPointLightSelection;

/** Rank of a point light for a camera: the camera's distance to the light's
 * range, zero inside it and for an unbounded light, then its distance to the
 * light's centre. An incumbent ranks as if the camera were
 * VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS times closer. */
typedef struct VkrPointLightRankKey {
  float32_t range_distance;
  float32_t center_distance;
} VkrPointLightRankKey;

/** Point lights the scene syncs keep while they rank: a table's worth of the
 * best and the lights that still show from the previous frame. */
#define VKR_POINT_LIGHT_CANDIDATE_MAX (2u * VKR_MAX_SCENE_POINT_LIGHTS)

/** What a ranked order compares of one light, copied so the library sort
 * needs no context: its key, its stable identity and its index. */
typedef struct VkrPointLightSortRecord {
  VkrPointLightRankKey key;
  /** Render id, zero sorting last, then position. */
  uint32_t stable_id;
  Vec3 position;
  uint32_t index;
} VkrPointLightSortRecord;

/** Scratch of one ranked selection over at most
 * VKR_POINT_LIGHT_CANDIDATE_MAX lights, each array by light index except
 * `order` and `records`. Too large for a stack frame at these limits. */
typedef struct VkrPointLightRanking {
  VkrPointLightLimitEntry previous[VKR_POINT_LIGHT_CANDIDATE_MAX];
  bool8_t identified[VKR_POINT_LIGHT_CANDIDATE_MAX];
  VkrPointLightRankKey keys[VKR_POINT_LIGHT_CANDIDATE_MAX];
  uint32_t order[VKR_POINT_LIGHT_CANDIDATE_MAX];
  bool8_t selected[VKR_POINT_LIGHT_CANDIDATE_MAX];
  /** The light is in the table's head. */
  bool8_t head[VKR_POINT_LIGHT_CANDIDATE_MAX];
  /** Intensity scale; zero for a light not drawn. */
  float32_t weights[VKR_POINT_LIGHT_CANDIDATE_MAX];
  VkrPointLightSortRecord records[VKR_POINT_LIGHT_CANDIDATE_MAX];
} VkrPointLightRanking;

/** The frame's point-light candidates, gathered by the scene syncs in
 * bounded storage however many lights the scenes hold. */
typedef struct VkrPointLightCandidates {
  VkrPointLight lights[VKR_POINT_LIGHT_CANDIDATE_MAX];
  VkrPointLightRankKey keys[VKR_POINT_LIGHT_CANDIDATE_MAX];
  /** The light was drawn in the previous frame, so it keeps a candidate
   * place while it fades out, whatever its rank. */
  bool8_t showing[VKR_POINT_LIGHT_CANDIDATE_MAX];
  uint32_t count;
  /** Candidate indices of the best-ranked lights offered so far, best
   * first. */
  uint32_t best[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t best_count;
  /** Lights offered this frame, kept or not. */
  uint32_t offered;
  Vec3 camera;
  /** Fade step of this frame in [0, 1]; one snaps. */
  float32_t step;
  /** A first frame, a camera cut or an unusable step: no fade state is
   * followed and every selected light shows at once. */
  bool8_t snap;
} VkrPointLightCandidates;

/**
 * @brief Lighting system for managing lighting data and applying to shaders.
 *
 * This system caches lighting data from the ECS and applies it to the shader
 * uniforms. It also tracks dirty state to avoid unnecessary updates.
 */
typedef struct VkrLightingSystem {
  // Cached GPU-ready data (updated from ECS each frame)
  struct {
    bool8_t enabled;
    Vec3 direction; // world space
    Vec3 color;
    float32_t intensity;
    float32_t sun_angular_diameter_degrees;
  } directional;

  /* The scene table: the point lights nearest the camera, each scaled by
     its selection fade (ADR-019). Its head, the first
     min(count, VKR_POINT_LIGHT_GRID_LIGHT_MAX) rows, holds the best ranked;
     only head rows cast shadows. Head and rest are each in render id order,
     so a light changes row only when it enters or leaves the head. */
  VkrPointLight point_lights[VKR_MAX_SCENE_POINT_LIGHTS];
  /* Each table light's intensity before its fades, which sizes its local
     shadow: a light fading out keeps its shadow's size while it shows. */
  float32_t point_light_unfaded_intensity[VKR_MAX_SCENE_POINT_LIGHTS];
  uint32_t point_light_count;
  uint32_t point_light_dropped_count;
  /* Static lights light only through baked data, the lightmaps and diffuse
     volumes, so the tables keep the dynamic lights (the tiled pipeline,
     ADR-087 and ADR-088). Read by the scene syncs. */
  bool8_t static_lights_baked;
  /* The shadow-casting static lamps the table leaves out for being baked:
     the desktop pipeline shadows its moving casters under the nearest of
     them (ADR-104). Past VKR_POINT_LIGHT_GRID_LIGHT_MAX, the shadowed head's
     width, the syncs keep those nearest the camera. */
  VkrPointLight baked_lamps[VKR_POINT_LIGHT_GRID_LIGHT_MAX];
  VkrPointLightRankKey baked_lamp_keys[VKR_POINT_LIGHT_GRID_LIGHT_MAX];
  uint32_t baked_lamp_count;
  /* Built only for the tiled pipeline, which reads it
     (vkr_lighting_system_build_point_light_grid); zero otherwise. */
  VkrPointLightGrid point_light_grid;

  VkrRectangleLight rectangle_lights[VKR_MAX_SCENE_RECTANGLE_LIGHTS];
  uint32_t rectangle_light_count;

  /* The frame's ranked point lights, which the table is selected from. */
  VkrPointLightCandidates point_light_candidates;
  /* The table selection's state from the previous frame, which this frame
     ranks and fades from, and this frame's, which the next frame's sync
     adopts; an appended scene selects again from the same state. */
  VkrPointLightSelection point_light_selection;
  VkrPointLightSelection point_light_selection_next;

  /* State of vkr_lighting_system_limit_point_lights from its last call. The
     scene syncs leave it alone. */
  VkrPointLightSelection point_light_limit;
  /* Scratch of the selection running now. */
  VkrPointLightRanking point_light_ranking;

  /* Leaves out of the frame the scene lights that light nothing the camera
     sees, as rooms decide (vkr_scene_rooms_light_hidden); NULL keeps every
     light. Set by the runtime before the scene syncs. */
  bool8_t (*point_light_hidden)(const void *context, Vec3 position,
                                float32_t range);
  const void *point_light_hidden_context;

  // Dirty tracking
  bool8_t dirty;
} VkrLightingSystem;

/**
 * @brief Initializes the lighting system.
 * @param system The lighting system to initialize.
 * @return true_v if the lighting system was initialized successfully, false_v
 * otherwise.
 */
bool8_t vkr_lighting_system_init(VkrLightingSystem *system);

/**
 * @brief Shuts down the lighting system.
 * @param system The lighting system to shut down.
 */
void vkr_lighting_system_shutdown(VkrLightingSystem *system);

/**
 * @brief Syncs a frame's lights from the ECS.
 * The directional light comes from the sun that vkr_scene_sync_sun resolved.
 * The point lights begin the frame's selection for `camera_position` (see
 * vkr_lighting_system_begin_point_lights) and end it, so the table is
 * complete when the sync returns.
 * @param system The lighting system to sync from the ECS.
 * @param scene The scene to sync from.
 * @param camera_position Camera the table keeps the nearest lights of.
 * @param delta_seconds Seconds since the previous sync, for the fades.
 */
void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                         const VkrScene *scene,
                                         Vec3 camera_position,
                                         float32_t delta_seconds);

/** Append an additive scene's point and rectangle lights after a sync; its
 * directional light and sun do not replace the rendered scene's (ADR-076).
 * Its point lights rank with the synced ones for the same camera. */
void vkr_lighting_system_append_scene(VkrLightingSystem *system,
                                      const VkrScene *scene);

/** A light that a ranked selection selected or shadowed keeps its place
 * until a competitor is this factor nearer (ADR-019, ADR-087). */
#define VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS 1.15f
/** Seconds for a light a selection selects or drops to fade in or out, as
 * long as a local shadow takes to fade in. */
#define VKR_POINT_LIGHT_LIMIT_FADE_SECONDS 0.25f

/** Begins a frame's point-light selection for `camera_position`: adopts the
 * previous frame's selection state and forgets the candidates. The first
 * frame, a camera cut (the TAA rule, position only) and an invalid
 * `delta_seconds` snap: every selected light shows at once. */
void vkr_lighting_system_begin_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds);

/** Offers one point light to the frame's selection. It stays a candidate
 * while it ranks among the VKR_MAX_SCENE_POINT_LIGHTS best offered so far,
 * or while it still shows from the previous frame, so the candidates stay
 * bounded however many lights are offered. */
void vkr_lighting_system_offer_point_light(VkrLightingSystem *system,
                                           const VkrPointLight *light);

/** Selects the scene table from the candidates. The
 * VKR_MAX_SCENE_POINT_LIGHTS nearest the camera by the distance to their
 * range are selected, ties by the distance to their centre and then by
 * render id; a light selected in the previous frame ranks with the incumbent
 * bonus. While lights compete for the table, a newly selected light fades in
 * over VKR_POINT_LIGHT_LIMIT_FADE_SECONDS once a slot is free and a light no
 * longer selected fades out in its slot, keeping its shadow; without
 * competition every offered light shows at full intensity, as new content
 * does. The VKR_POINT_LIGHT_GRID_LIGHT_MAX drawn lights ranked best, a light
 * in the previous frame's head with the incumbent bonus, form the head;
 * lights past it lose their shadow. Head and rest each keep render id order.
 * Calling it again in the same frame selects again from the same previous
 * state. Leaves the grid alone. */
void vkr_lighting_system_select_point_lights(VkrLightingSystem *system);

/** A white Lambertian surface lit head-on by a directional light of
 * irradiance E shows E * exposure / pi. Below this fraction of display white,
 * a sixteenth of an 8-bit step, the light is invisible (ADR-081). */
#define VKR_LIGHTING_KEY_LIGHT_VISIBLE_MIN (1.0f / 4096.0f)

/** Lights with the frame's atmosphere key light, the sun or the moon
 * (ADR-081): `toward_light` points from the observer toward it, `irradiance`
 * is its light attenuated to the observer and the diameter sizes the shadow
 * penumbra. With a known `display_exposure`, a light too dim to show is
 * disabled, so neither it nor its cascaded shadows render; zero keeps it. */
void vkr_lighting_system_apply_atmosphere_light(
    VkrLightingSystem *system, Vec3 toward_light, Vec3 irradiance,
    float32_t angular_diameter_degrees, float32_t display_exposure);

/** The tiled pipeline's bounded set of dynamic lights (ADR-087). Selects
 * the `light_max` point lights nearest `camera_position` by the distance to
 * their range, ties by the distance to their centre and then by table order,
 * and lets the `shadow_max` nearest selected casters keep their shadows. A
 * light selected or shadowed by the previous call ranks as if the camera
 * were VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS times closer, so membership
 * does not flip while distances cross.
 *
 * A light with a unique nonzero render id fades: its intensity scale rises
 * by `delta_seconds` / VKR_POINT_LIGHT_LIMIT_FADE_SECONDS per call while
 * selected and falls as fast once not. A fading-out light keeps its table
 * slot and loses its shadow, which the local shadow cache fades out; a
 * newly selected light takes a slot once one is free, so at most
 * `light_max` lights draw. The first call, a camera cut and an invalid
 * `delta_seconds` snap every selected light to full intensity. Keeps the
 * table order of the drawn lights; the caller rebuilds the grid. */
void vkr_lighting_system_limit_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds,
                                            uint32_t light_max,
                                            uint32_t shadow_max);

/** Rebuilds the conservative world-space lookup from the point_lights rows
 * below VKR_POINT_LIGHT_GRID_LIGHT_MAX. Only the tiled pipeline reads it;
 * call it after the table's last change of the frame. */
void vkr_lighting_system_build_point_light_grid(VkrLightingSystem *system);

/** Returns the global-plus-cell mask used for a world-space fragment. */
VkrPointLightMask
vkr_lighting_system_point_light_mask_at(const VkrLightingSystem *system,
                                        Vec3 world_position);

/** Tests whether one scene-table index is present in a mask. */
bool8_t
vkr_lighting_system_point_light_mask_contains(const VkrPointLightMask *mask,
                                              uint32_t light_index);

/**
 * @brief Applies the lighting system to the shader uniforms.
 * @param system The lighting system to apply to the shader uniforms.
 */

/**
 * @brief Marks the lighting system as dirty.
 * @param system The lighting system to mark as dirty.
 */
void vkr_lighting_system_mark_dirty(VkrLightingSystem *system);

/**
 * @brief Checks if any updates are pending.
 * @param system The lighting system to check if any updates are pending.
 * @return true_v if any updates are pending, false_v otherwise.
 */
bool8_t vkr_lighting_system_is_dirty(const VkrLightingSystem *system);

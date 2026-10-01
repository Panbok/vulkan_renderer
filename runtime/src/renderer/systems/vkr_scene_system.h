/**
 * @file vkr_scene_system.h
 * @brief Scene system for managing ECS-based scenes with renderer integration.
 *
 * The scene system provides:
 * - Entity/component management via VkrWorld (ECS)
 * - Transform hierarchy with topological sorting for parent-before-child
 * updates
 * - Dirty tracking for efficient render bridge sync
 * - Mesh ownership tracking for cleanup
 * - Picking result to entity mapping
 */
#pragma once
#include "vkr_atmosphere.h"
#include "vkr_fog.h"
#include "vkr_froxel_fog.h"
#include "vkr_lighting.h"

#include "containers/str.h"
#include "core/vkr_entity.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_scene_simulation.h"

// Forward declarations
struct VkrRenderAssets;
struct VkrMeshLoadDesc;
typedef struct SceneChildIndexSlot SceneChildIndexSlot;

// ============================================================================
// Error Types
// ============================================================================

typedef enum VkrSceneError {
  VKR_SCENE_ERROR_NONE = 0,
  VKR_SCENE_ERROR_ALLOC_FAILED,
  VKR_SCENE_ERROR_WORLD_INIT_FAILED,
  VKR_SCENE_ERROR_COMPONENT_REGISTRATION_FAILED,
  VKR_SCENE_ERROR_ENTITY_LIMIT_REACHED,
  VKR_SCENE_ERROR_INVALID_ENTITY,
  VKR_SCENE_ERROR_MESH_LOAD_FAILED,
  VKR_SCENE_ERROR_FILE_NOT_FOUND,
  VKR_SCENE_ERROR_FILE_READ_FAILED,
  VKR_SCENE_ERROR_PARSE_FAILED,
  VKR_SCENE_ERROR_UNSUPPORTED_VERSION,
  VKR_SCENE_ERROR_COMPONENT_ADD_FAILED,
} VkrSceneError;

// ============================================================================
// Component Types
// ============================================================================

/**
 * @brief Name component for entities.
 * The name string is owned by the scene allocator.
 */
typedef struct SceneName {
  String8 name;
} SceneName;

// Transform dirty flags
#define SCENE_TRANSFORM_DIRTY_LOCAL 0x01 // TRS changed, recompute local matrix
#define SCENE_TRANSFORM_DIRTY_WORLD 0x02 // World matrix needs recompute
#define SCENE_TRANSFORM_DIRTY_HIERARCHY                                        \
  0x04 // Parent link changed, rebuild topo order
#define SCENE_TRANSFORM_WORLD_UPDATED                                          \
  0x08 // World matrix was updated this frame (for child propagation)

/**
 * @brief Transform component with TRS, cached matrices, and hierarchy support.
 *
 * Dirty flag semantics:
 * - DIRTY_LOCAL: Set by transform setters. Cleared after local matrix
 * recompute.
 * - DIRTY_WORLD: Set when local or parent changes. Cleared after world matrix
 * recompute.
 * - DIRTY_HIERARCHY: Set when parent link changes. Triggers topo order rebuild.
 * - WORLD_UPDATED: Set when world matrix updated this frame. Used for deferred
 *   dirty propagation to children during topo traversal. Cleared in Pass 1.
 */
typedef struct SceneTransform {
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;

  VkrEntityId parent; // VKR_ENTITY_ID_INVALID means root
  Mat4 local;         // Cached local matrix (TRS composition)
  Mat4 world;         // Cached world matrix (parent.world * local)

  bool8_t matrix_authored;
  bool8_t trs_editable;
  uint8_t flags; // Bitmask of SCENE_TRANSFORM_DIRTY_* flags
} SceneTransform;

/* Transient presentation override; authored transforms remain unchanged. */
typedef struct SceneEvaluatedTransform {
  Mat4 world;
} SceneEvaluatedTransform;

/* Stable within a source revision; names and ECS entity slots are not keys. */
typedef struct SceneSourceIdentity {
  uint32_t scene_entity_index;
  uint32_t gltf_node_index;
  uint32_t gltf_mesh_index;
  uint32_t gltf_camera_index;
  uint32_t gltf_skin_index;
  uint32_t gltf_light_index;
  uint64_t source_fingerprint;
} SceneSourceIdentity;

/**
 * Document-stable entity id (ADR-076): the entity's UUID in its scene
 * document, as 16 bytes in text order. Overlays bind document entities
 * through it, so reordering a document keeps its edits attached.
 */
typedef struct VkrSceneDocumentId {
  uint8_t bytes[16];
} VkrSceneDocumentId;

/**
 * @brief Mesh renderer component linking entity to mesh manager slot.
 */
typedef struct SceneMeshRenderer {
  VkrMeshInstanceHandle instance; // Handle to mesh instance
} SceneMeshRenderer;

/**
 * @brief Visibility component for controlling render visibility.
 */
typedef struct SceneVisibility {
  bool8_t visible;        // If false, entity is not rendered
  bool8_t inherit_parent; // If true, effective = parent.visible && this.visible
} SceneVisibility;

/**
 * @brief Persistent render id for picking and editor selection.
 *
 * The id is stable for the entity lifetime and is never reused.
 * Scene picking encodes object_id = render_id + 1 (kind 0, 0 = background).
 * Top bits are reserved for picking kind tags, so the render id range is
 * limited to keep object_id encodings unambiguous.
 */
typedef struct SceneRenderId {
  uint32_t id;
} SceneRenderId;

// ============================================================================
// Text3D and Shape Components
// ============================================================================

/**
 * @brief Shape types for scene primitive shapes.
 */
typedef enum SceneShapeType {
  SCENE_SHAPE_TYPE_CUBE = 0,
  SCENE_SHAPE_TYPE_COUNT,
} SceneShapeType;

/**
 * @brief 3D text component tied to world-resources text instances.
 *
 * text_index is the world text id (currently entity index).
 * World resources own GPU instances; the scene stores ids/size metadata.
 * world_width/world_height capture the base plane size for gizmo pivot math.
 */
typedef struct SceneText3D {
  uint32_t text_index;    // World text id (currently entity index)
  bool8_t dirty;          // True if text content changed, needs re-render
  float32_t world_width;  // Base width in world units (before entity scale)
  float32_t world_height; // Base height in world units (before entity scale)
} SceneText3D;

/**
 * @brief Primitive shape component rendered via mesh manager.
 *
 * The geometry is generated on load and tracked as a scene-owned mesh.
 */
typedef struct SceneShape {
  SceneShapeType type;
  Vec3 dimensions;     // Width, height, depth (for cube)
  Vec4 color;          // RGBA color
  uint32_t mesh_index; // Index into mesh manager (generated geometry)
} SceneShape;

/**
 * Where a game spawns its player once simulation runs (ADR-079), a typed
 * component. The entity's world transform is the spawn pose; the scene
 * resolves its first enabled, visible start, then the root World's.
 */
typedef struct ScenePlayerStart {
  bool8_t enabled;
} ScenePlayerStart;

/**
 * Authored shape values (ADR-076), a typed component. Setting it rebuilds
 * the entity's generated geometry and mesh; removing it releases them.
 */
typedef struct SceneShapeSettings {
  SceneShapeType type;
  Vec3 dimensions;
  Vec4 color; /* RGBA; used when no material is named. */
  char material_name[64];
  char material_path[256];
} SceneShapeSettings;

/**
 * Authored 3D text values (ADR-076), a typed component. Setting it replaces
 * the entity's world text slot, keeping its font and texture size.
 */
typedef struct SceneTextSettings {
  char content[256];
  float32_t font_size;
  Vec4 color;
} SceneTextSettings;

/**
 * Authored playback of an animated mesh (ADR-076), a typed component the
 * animation binding owns. Setting it reconciles the entity's player; the clip
 * must exist in the bound bank. `clip_name` mirrors the selected clip.
 */
typedef struct SceneAnimationSettings {
  uint32_t clip;
  float32_t rate;
  bool8_t loop;
  bool8_t playing;
  char clip_name[64];
} SceneAnimationSettings;

/**
 * Read-only facts about an entity's mesh renderer for Details (ADR-076),
 * described by `vkr_scene_mesh_info_type` and filled from the live instance
 * and its shared asset. Not a component; nothing stores or edits it.
 */
typedef struct SceneMeshInfo {
  char asset[256];
  char material[128]; /* First submesh's material, then "(+N)" others. */
  uint32_t submeshes;
  uint32_t vertices;
  uint32_t triangles;
  float32_t radius;  /* Local bounding sphere. */
  uint32_t state;    /* VkrMeshLoadingState. */
  uint32_t mobility; /* VkrShadowCasterMobility. */
} SceneMeshInfo;

// ============================================================================
// Light Components
// ============================================================================

/**
 * @brief Directional light component.
 *
 * World direction is computed as: quat_rotate(transform.rotation,
 * direction_local). If entity has no transform, direction_local is used
 * directly.
 */
typedef struct SceneDirectionalLight {
  Vec3 color;           // Linear RGB
  float32_t intensity;  // Light intensity multiplier
  Vec3 direction_local; // Local-space direction (default: {0, -1, 0})
  /** Apparent solar-disc diameter in degrees; zero keeps a hard PCF edge. */
  float32_t sun_angular_diameter_degrees;
  /** Colour temperature in [1000, 40000] kelvin whose unit-luminance
      blackbody tints `color`, or zero for `color` alone. */
  float32_t temperature_kelvin;
  bool8_t enabled; // Whether this light is active
  /** Drives an enabled atmosphere's sun (ADR-058). Scene-authored lights
      default to true; lights imported from glTF default to false. */
  bool8_t atmosphere_sun;
  /** Drives an enabled atmosphere's moon, its second light (ADR-081), and
      takes precedence: a moon light is never the sun. Defaults to false. */
  bool8_t atmosphere_moon;
} SceneDirectionalLight;

/** The enabled directional light the renderer lights with: the lowest render
 * id, else the first found. An enabled atmosphere considers only atmosphere
 * sun lights. `direction` is the world direction along incoming light and
 * `color` includes the temperature tint. */
typedef struct VkrSceneSunLight {
  Vec3 direction;
  Vec3 color;
  float32_t intensity;
  float32_t sun_angular_diameter_degrees;
} VkrSceneSunLight;

/** The frame's direct light: the sun while it lights the observer, else the
 * moon (ADR-081), with its irradiance attenuated to the observer altitude. */
typedef struct VkrSceneKeyLight {
  /** Unit direction from the observer toward the light. */
  Vec3 toward;
  Vec3 irradiance;
  float32_t angular_diameter_degrees;
  bool8_t moon;
} VkrSceneKeyLight;

/** The scene's sun or moon light, resolved once per frame by
 * vkr_scene_sync_sun. `tint_kelvin` and `tint` memoize the temperature tint,
 * which integrates the blackbody spectrum. */
typedef struct VkrSceneSun {
  VkrSceneSunLight light;
  /* The light that is the sun and its container: this scene or the World it
   * falls back to. Other directional lights are inactive. */
  VkrEntityId entity;
  const struct VkrScene *owner;
  bool8_t found;
  float32_t tint_kelvin;
  Vec3 tint;
} VkrSceneSun;

/**
 * @brief Point light component.
 *
 * Position is derived from entity's SceneTransform.world translation.
 * Attenuation follows the formula: 1 / (constant + linear*d + quadratic*d^2)
 */
typedef struct ScenePointLight {
  Vec3 color;          // Linear RGB
  float32_t intensity; // Light intensity multiplier
  float32_t constant;  // Attenuation constant term (usually 1.0)
  float32_t linear;    // Attenuation linear term
  float32_t quadratic; // Attenuation quadratic term
  float32_t range;     // Zero means unbounded.
  Vec3 direction_local;
  float32_t inner_cone_angle;
  float32_t outer_cone_angle;
  VkrPointLightKind kind;
  bool8_t enabled;      // Whether this light is active
  bool8_t casts_shadow; // Requires a finite positive range.
} ScenePointLight;

/** One-sided rectangular emitter. Entity translation is its center; rotation
 * maps local +X/+Y to its axes; entity scale is ignored; local -Z emits. */
typedef struct SceneRectangleLight {
  Vec3 color;
  float32_t radiance;
  Vec2 size;
  bool8_t enabled;
} SceneRectangleLight;

/**
 * @brief Runtime status of the published global environment tuple.
 *
 * `READY` means the source, prefilter and coefficient slot are valid.
 * `FAILED` keeps rendering alive without a global environment.
 */
typedef enum VkrSceneEnvironmentBakeState {
  VKR_SCENE_ENV_BAKE_STATE_NONE = 0,
  VKR_SCENE_ENV_BAKE_STATE_READY,
  VKR_SCENE_ENV_BAKE_STATE_FAILED,
} VkrSceneEnvironmentBakeState;

/** Largest constant environment radiance: the largest finite RGBA16F value. */
#define VKR_SCENE_ENVIRONMENT_CONSTANT_MAX 65504.0f

/** Producer of the global sky/IBL tuple. The atmosphere is the only sky
    (ADR-058); a constant source serves isolated fixtures and previews. */
typedef enum VkrSceneEnvironmentSourceKind {
  VKR_SCENE_ENV_SOURCE_NONE = 0,
  VKR_SCENE_ENV_SOURCE_CONSTANT,
  VKR_SCENE_ENV_SOURCE_ATMOSPHERE,
} VkrSceneEnvironmentSourceKind;

/**
 * @brief Runtime status of reflection probe bake products.
 *
 * `PENDING` means a source cubemap is available and convolution bake has not
 * completed yet.
 * `READY` means the coefficient slot and prefilter cubemap are selectable.
 * `FAILED` keeps rendering alive by falling back to scene/global IBL maps.
 */
typedef enum VkrSceneReflectionProbeBakeState {
  VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_NONE = 0,
  VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING,
  VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY,
  VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED,
} VkrSceneReflectionProbeBakeState;

#define VKR_SCENE_REFLECTION_PROBE_MAX 16u

/**
 * @brief Scene-owned local reflection probe for box-projected IBL.
 *
 * The probe volume is an axis-aligned world-space box:
 * `center +/- extents`. `blend_distance` expands influence outside the box
 * with linear falloff.
 *
 * Ownership:
 * - `source_cubemap` and `prefilter_cubemap` are scene handles released by
 *   scene reset/shutdown paths. The diffuse response is L2 coefficients owned
 *   by the renderer's slot pool, not a scene-owned cubemap (ADR-038).
 * - Prepared targets exist only until bake recording completes. Their backend
 *   objects retire after the submit serial that protects the recorded work.
 */
typedef struct VkrSceneReflectionProbe {
  bool8_t enabled;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  /** Authored L2 deringing exponent (ADR-038). Zero is the identity
      window. Validated at scene load; never interpreted during lighting. */
  float32_t sh_deringing;
  uint32_t source_mip_count;
  bool8_t uses_scene_environment_source;
  VkrTextureHandle source_cubemap;
  VkrTextureHandle prefilter_cubemap;
  VkrSceneReflectionProbeBakeState bake_state;
} VkrSceneReflectionProbe;

/**
 * @brief Scene-owned global environment: authored sky-light controls and the
 * published source/prefilter/SH tuple.
 *
 * Ownership:
 * - `source_cubemap` and `prefilter_cubemap` are retained by the scene and
 *   released by scene shutdown/reload paths. The diffuse response is L2
 *   coefficients owned by the renderer's slot pool (ADR-038).
 * - Bake products are scene-owned cubemaps generated at runtime and released
 *   with normal texture-system handle symmetry.
 * - Prepared targets exist only until bake recording completes. Their backend
 *   objects retire after the submit serial that protects the recorded work.
 *
 * The authored controls apply to whichever source publishes the tuple. An
 * atmosphere revision replaces only the tuple, so the controls survive it.
 * `enabled` gates global IBL lighting; an atmosphere sky stays visible when
 * it is false.
 */
typedef struct VkrSceneEnvironment {
  bool8_t enabled;
  VkrSceneEnvironmentSourceKind source_kind;
  /** Scene-linear radiance of a constant source, in
      [0, VKR_SCENE_ENVIRONMENT_CONSTANT_MAX]. */
  Vec3 constant_radiance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  /** Authored L2 deringing exponent (ADR-038). Zero is the identity
      window. Validated at scene load; never interpreted during lighting. */
  float32_t sh_deringing;

  VkrTextureHandle source_cubemap;
  VkrTextureHandle prefilter_cubemap;
  /** Lookup textures an atmosphere generation bakes beside its source. The
      camera-dependent sky reads them every frame (ADR-058). */
  VkrTextureHandle atmosphere_transmittance;
  VkrTextureHandle atmosphere_multiple_scattering;

  uint32_t source_face_size;
  uint32_t source_mip_count;

  VkrSceneEnvironmentBakeState bake_state;
} VkrSceneEnvironment;

/** Candidate output stays private until every native bake product is ready. */
typedef enum VkrSceneAtmosphereBakeState {
  VKR_SCENE_ATMOSPHERE_BAKE_STATE_NONE = 0,
  VKR_SCENE_ATMOSPHERE_BAKE_STATE_PENDING,
  VKR_SCENE_ATMOSPHERE_BAKE_STATE_READY,
  VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED,
} VkrSceneAtmosphereBakeState;

/**
 * Scene-owned atmosphere publication state. `environment` remains the sole
 * frame-visible sky/IBL tuple. A single retired tuple preserves ownership when
 * its cold release cannot complete; it must drain before another replacement.
 * Only its texture handles are meaningful. The cloud layer is part of each
 * revision and publishes with it (ADR-074).
 */
typedef struct VkrSceneAtmosphere {
  /** The authored settings. `live_settings` replaces their sun with the
      scene's sun light every frame; the frame draws the published medium lit
      by that sun, while `requested_settings` follows it into the revision
      bake at most every VKR_SCENE_SUN_REFRESH_SECONDS (ADR-058). */
  VkrAtmosphereSettings authored_settings;
  VkrAtmosphereSettings live_settings;
  VkrAtmosphereSettings requested_settings;
  VkrAtmosphereSettings candidate_settings;
  VkrAtmosphereSettings active_settings;
  VkrCloudSettings requested_clouds;
  VkrCloudSettings candidate_clouds;
  VkrCloudSettings active_clouds;
  /** Seconds since the sun last requested a revision. */
  float64_t sun_refresh_elapsed;
  /** The frame's key light, memoized by its prepared parameters. */
  VkrAtmosphereGpuParams frame_params;
  VkrSceneKeyLight frame_key_light;
  VkrSceneEnvironment retired_environment;
  VkrTextureHandle candidate_source_cubemap;
  VkrTextureHandle candidate_prefilter_cubemap;
  VkrTextureHandle candidate_transmittance;
  VkrTextureHandle candidate_multiple_scattering;
  float32_t requested_sh_deringing;
  uint64_t requested_revision;
  uint64_t candidate_revision;
  uint64_t active_revision;
  VkrSceneAtmosphereBakeState bake_state;
} VkrSceneAtmosphere;

// ============================================================================
// World Components (ADR-076)
// ============================================================================

/* Authored world settings live on entities as components. Runtime products
 * (cubemaps, lookup textures, bake state, texture bindings) stay in the scene
 * fields above and are derived from the resolved component. */

/** Authored global environment controls. `source_kind`, `constant_radiance`
 * and `sh_deringing` select baked products and apply on the next load. */
typedef struct SceneEnvironmentSettings {
  bool8_t enabled;
  VkrSceneEnvironmentSourceKind source_kind;
  Vec3 constant_radiance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  float32_t sh_deringing;
} SceneEnvironmentSettings;

/** Authored post-processing: exposure, white balance, grading, bloom, depth
 * of field, motion blur and ambient occlusion shape. Machine quality gates
 * (whether bloom or AO run at all) stay in the graphics preferences. */
typedef struct ScenePostProcess {
  uint32_t exposure_mode; /* VkrExposureMode. */
  float32_t manual_exposure;
  float32_t exposure_compensation_ev;
  float32_t white_balance_temperature;
  float32_t white_balance_tint;
  float32_t contrast;
  float32_t saturation;
  float32_t sharpness;
  float32_t bloom_threshold;
  float32_t bloom_knee;
  float32_t bloom_intensity;
  float32_t dof_focus_distance;
  float32_t dof_f_stop;
  float32_t motion_blur_shutter_angle;
  float32_t gtao_radius;
  float32_t gtao_power;
} ScenePostProcess;

/** Authored local reflection probe. `slot` indexes the runtime products in
 * `VkrScene.reflection_probes`, assigned at load; the cubemap reference
 * applies on the next load. */
typedef struct SceneReflectionProbeSettings {
  bool8_t enabled;
  Vec3 center;
  Vec3 extents;
  float32_t blend_distance;
  float32_t intensity;
  float32_t diffuse_intensity;
  float32_t specular_intensity;
  float32_t sh_deringing;
  char cubemap[256];
  uint32_t slot;
} SceneReflectionProbeSettings;

/** Baked diffuse irradiance volume; `path` applies on the next load. */
typedef struct SceneDiffuseVolumeSettings {
  bool8_t enabled;
  char path[256];
} SceneDiffuseVolumeSettings;

/** Subsurface scattering profiles loaded with the scene. */
typedef struct SceneSubsurfaceSettings {
  bool8_t enabled;
  uint32_t profile_count;
} SceneSubsurfaceSettings;

/** Resolved world state, recomputed when world components change: each
 * singleton's effective value, or its default when no instance exists. */
/** World physics settings (ADR-076): gravity for every body and character
 * in the physics set, in m/s^2. */
typedef struct ScenePhysicsSettings {
  Vec3 gravity;
} ScenePhysicsSettings;

/** World animation settings (ADR-076): scales the clock of every animation
 * player in the active scene. */
typedef struct SceneAnimationWorldSettings {
  float32_t time_scale;
} SceneAnimationWorldSettings;

typedef struct VkrSceneWorldState {
  uint64_t revision;
  SceneEnvironmentSettings environment;
  VkrAtmosphereSettings atmosphere;
  VkrCloudSettings clouds;
  VkrFogSettings fog;
  /** Includes the density boxes gathered from their components. */
  VkrFroxelFogSettings froxel_fog;
  ScenePostProcess post_process;
  SceneDiffuseVolumeSettings diffuse_volume;
  SceneSubsurfaceSettings subsurface;
  ScenePhysicsSettings physics_settings;
  SceneAnimationWorldSettings animation_settings;
  bool8_t has_environment;
  bool8_t has_atmosphere;
  bool8_t has_clouds;
  bool8_t has_post_process;
  /** Entities whose singletons won resolution; invalid when none. */
  VkrEntityId environment_entity;
  VkrEntityId atmosphere_entity;
  VkrEntityId clouds_entity;
  VkrEntityId fog_entity;
  VkrEntityId froxel_fog_entity;
  VkrEntityId post_process_entity;
  VkrEntityId diffuse_volume_entity;
  VkrEntityId subsurface_entity;
  VkrEntityId physics_settings_entity;
  VkrEntityId animation_settings_entity;
  uint32_t probe_count;
  SceneReflectionProbeSettings probes[VKR_SCENE_REFLECTION_PROBE_MAX];
} VkrSceneWorldState;

/** Entity-id world field of the root World container (ADR-076). Scenes the
 * resource system loads use world 0; additive scenes use small ids. */
#define VKR_SCENE_WORLD_ROOT_ID 0x7FFFu
/** Additive scene containers use world ids 1..VKR_SCENE_ADDITIVE_MAX. */
#define VKR_SCENE_ADDITIVE_MAX 6u
/** Picking render ids per container: container c owns
 * [c * RANGE, (c + 1) * RANGE); the primary scene keeps range 0. */
#define VKR_SCENE_RENDER_ID_RANGE (1u << 22)
/** The root World's picking range follows the additive containers'. */
#define VKR_SCENE_WORLD_RENDER_ID_BASE                                         \
  ((VKR_SCENE_ADDITIVE_MAX + 1u) * VKR_SCENE_RENDER_ID_RANGE)

/** Largest number of descriptor-typed component types a scene registers. */
#define VKR_SCENE_TYPE_MAX 64u

typedef struct VkrSceneComponentType {
  const struct VkrTypeDesc *type;
  VkrComponentTypeId id;
} VkrSceneComponentType;

// ============================================================================
// Scene Type
// ============================================================================

/**
 * @brief Scene containing ECS world and renderer integration state.
 */
typedef struct s_VkrSceneAnimation VkrSceneAnimation;
typedef struct s_VkrScenePhysics VkrScenePhysics;
typedef struct s_VkrScenePhysicsSet VkrScenePhysicsSet;
typedef struct VkrSceneCollisionLayers VkrSceneCollisionLayers;

/** Scene-level settings (ADR-076), saved in the scene's edit overlay. */
typedef struct VkrSceneSettings {
  /** The root World's objects apply where the scene has none of its own;
      off scopes the scene to its own objects. */
  bool8_t inherit_world;
} VkrSceneSettings;

/* Geometry kinds the editor's Show menu can leave undrawn. */
#define VKR_SCENE_SHOW_HIDE_STATIC_MESHES (1u << 0)
#define VKR_SCENE_SHOW_HIDE_ANIMATED_MESHES (1u << 1)
#define VKR_SCENE_SHOW_HIDE_SHAPES (1u << 2)
#define VKR_SCENE_SHOW_HIDE_ALL                                                \
  (VKR_SCENE_SHOW_HIDE_STATIC_MESHES | VKR_SCENE_SHOW_HIDE_ANIMATED_MESHES |   \
   VKR_SCENE_SHOW_HIDE_SHAPES)

typedef struct VkrScene {
  VkrScenePhysics *physics;
  /** Scenes sharing one native physics world (ADR-076), or NULL for a
      private world. */
  VkrScenePhysicsSet *physics_set;
  VkrSceneSimulation simulation;
  VkrSceneCollisionLayers *collision_layers;
  VkrSceneCollisionLayers *collision_layers_pending;
  uint64_t collision_layers_revision;
  char physics_asset_root[1024];
  bool8_t physics_paused;
  bool8_t physics_disabled;
  VkrComponentTypeId comp_physics_body;
  VkrComponentTypeId comp_physics_collider;
  VkrSceneAnimation *animations; // Independently owned per-wrapper pose state.
  /** Runtime-spawned models (vkr_scene_model.h), newest first. */
  struct VkrSceneModel *models;
  VkrWorld *world;                // ECS storage (authoritative scene state)
  VkrAllocator *alloc;            // Scene-owned allocator
  struct VkrRenderAssets *assets; // Borrowed owner of published scene assets
  uint64_t structure_revision;
  uint16_t world_id; // Copied into entity IDs

  // Component type IDs (cached after registration)
  /** Document ids by document entity index, or NULL when the document has
      none. Scene-owned and freed at shutdown; the loader fills them. */
  VkrSceneDocumentId *document_ids;
  uint32_t document_id_count;
  VkrComponentTypeId comp_source_identity;
  VkrComponentTypeId comp_name;
  VkrComponentTypeId comp_transform;
  VkrComponentTypeId comp_evaluated_transform;
  VkrComponentTypeId comp_mesh_renderer;
  VkrComponentTypeId comp_visibility;
  VkrComponentTypeId comp_render_id;
  VkrComponentTypeId comp_text3d;
  VkrComponentTypeId comp_shape;
  VkrComponentTypeId comp_directional_light;
  VkrComponentTypeId comp_point_light;
  VkrComponentTypeId comp_rectangle_light;
  uint32_t rectangle_light_count;

  // Compiled queries for efficient per-frame iteration
  VkrQueryCompiled query_transforms;  // Entities with SceneTransform
  VkrQueryCompiled query_renderables; // (SceneTransform, SceneMeshRenderer)
  VkrQueryCompiled
      query_directional_light;         // Entities with SceneDirectionalLight
  VkrQueryCompiled query_point_lights; // (SceneTransform, ScenePointLight)
  VkrQueryCompiled query_rectangle_lights;
  VkrQueryCompiled query_shapes; // (SceneTransform, SceneShape)
  bool8_t queries_valid;         // False until first compile

  // Transform hierarchy support
  VkrEntityId *topo_order; // Topologically sorted entity IDs (full IDs, not
                           // just indices)
  uint32_t topo_count;     // Number of entities in topo_order
  uint32_t topo_capacity;  // Allocated size
  bool8_t
      hierarchy_dirty; // Set when parent links change; triggers topo rebuild

  // Parent -> children index for transform hierarchy queries.
  // Stored as a slot array keyed by parent entity index with a generation
  // guard.
  SceneChildIndexSlot *child_index_slots;
  uint32_t child_index_capacity; // Slot count (>= world->dir.capacity)
  bool8_t child_index_valid;     // False until rebuilt or incrementally updated

  // Owned mesh indices (mesh-slot path; used by shapes)
  uint32_t *owned_meshes;
  uint32_t owned_mesh_count;
  uint32_t owned_mesh_capacity;

  // Owned mesh instances (for cleanup on scene destroy - new instance system)
  VkrMeshInstanceHandle *owned_instances;
  uint32_t owned_instance_count;
  uint32_t owned_instance_capacity;

  // Render dirty tracking (entities needing sync to mesh manager)
  VkrEntityId *render_dirty_entities;
  uint32_t render_dirty_count;
  uint32_t render_dirty_capacity;
  bool8_t render_full_sync_needed; // Set on scene load or dirty overflow
  /** Editor Show filter (VKR_SCENE_SHOW_HIDE_*): geometry kinds the viewport
      neither draws nor picks, without editing the scene. Zero shows all. */
  uint32_t editor_hidden_kinds;

  uint32_t next_render_id; // Monotonic render id allocator (0 reserved)
  /** Offset of this container's picking range; local render ids stay small
      and index the render bridge (ADR-076). */
  uint32_t render_id_base;

  /** Runtime products of the resolved world components: published sky/IBL
      tuple and bake state, with its live controls lowered from the resolved
      environment component. */
  VkrSceneEnvironment environment;
  VkrSceneAtmosphere atmosphere;
  VkrSceneSun sun;
  /** The atmosphere's moon light; never found without an enabled
      atmosphere. */
  VkrSceneSun moon;
  /** Descriptor-typed component types registered with the ECS (ADR-076). */
  VkrSceneComponentType types[VKR_SCENE_TYPE_MAX];
  uint32_t type_count;
  /** Bumped by every world component change; resolution compares it. */
  uint64_t world_revision;
  /** Root World consulted after this scene's own singletons, or NULL; the
      runtime sets it each frame. Resolution notices its revision. */
  const struct VkrScene *world_fallback;
  /** Root World that owns World-only types whether or not the scene
      inherits it, or NULL. */
  const struct VkrScene *world_root;
  uint64_t world_fallback_revision;
  /** Effective world settings, resolved from components once per change. */
  VkrSceneWorldState world_state;
  VkrSceneSettings settings;
  /** Scene-owned baked diffuse-volume texture and lattice mapping. */
  VkrDiffuseVolumeBinding diffuse_volume;
  VkrSubsurfaceBinding subsurface;
  VkrSceneReflectionProbe reflection_probes[VKR_SCENE_REFLECTION_PROBE_MAX];
  uint32_t reflection_probe_count;
} VkrScene;

// ============================================================================
// Scene Lifecycle
// ============================================================================

/**
 * @brief Initialize a scene.
 * @param scene Scene to initialize (caller-allocated)
 * @param alloc Allocator for scene data
 * @param world_id World ID embedded in entity IDs (use 0 for single-scene apps)
 * @param initial_entity_capacity Initial entity capacity hint
 * @param out_error Optional error output
 * @return true on success
 */
bool8_t vkr_scene_init(VkrScene *scene, VkrAllocator *alloc, uint16_t world_id,
                       uint32_t initial_entity_capacity,
                       VkrSceneError *out_error);

/**
 * @brief Shutdown a scene and release all resources.
 * @param scene Scene to shutdown
 * @param assets Optional published asset owner. The caller must prove
 * GPU idle at the caller before teardown; drain retirement before another load.
 */
void vkr_scene_shutdown(VkrScene *scene, struct VkrRenderAssets *assets);

// ============================================================================
// Typed Components (ADR-076)
// ============================================================================

/** ECS component id of a registered type, or VKR_COMPONENT_TYPE_INVALID. */
VkrComponentTypeId vkr_scene_type_id(const VkrScene *scene,
                                     const struct VkrTypeDesc *type);
/** The entity's component of `type`, or NULL. Valid until a structural
    change to the world. */
const void *vkr_scene_get_typed(const VkrScene *scene, VkrEntityId entity,
                                const struct VkrTypeDesc *type);
/** Validate and store a component, adding it when absent. World component
    changes bump `world_revision`. */
bool8_t vkr_scene_set_typed(VkrScene *scene, VkrEntityId entity,
                            const struct VkrTypeDesc *type, const void *value);
bool8_t vkr_scene_remove_typed(VkrScene *scene, VkrEntityId entity,
                               const struct VkrTypeDesc *type);
/** Adds world types registered after this scene initialized, such as a
    script module loaded later (ADR-079). Call outside queries and ticks.
    False when the scene's type table is full. */
bool8_t vkr_scene_sync_world_types(VkrScene *scene);
/** Writes up to `capacity` live entities carrying `type` in entity index
    order and returns how many carry it, which may exceed `capacity`. */
uint32_t vkr_scene_find_typed(const VkrScene *scene,
                              const struct VkrTypeDesc *type,
                              VkrEntityId *out_entities, uint32_t capacity);
/** World matrix of the resolved Player Start: the scene's first enabled,
    visible one, then the root World's. False when neither has one. */
bool8_t vkr_scene_player_start(const VkrScene *scene, Mat4 *out_world);
/** Create a named root entity carrying one component; used for world
    components that have no placed transform. Returns invalid on failure. */
VkrEntityId vkr_scene_create_typed_entity(VkrScene *scene, String8 name,
                                          const struct VkrTypeDesc *type,
                                          const void *value);
/** Recompute `world_state` when world components changed and lower it into
    the runtime products: environment controls, atmosphere revision requests
    and reflection probe controls. Cheap when nothing changed; call once per
    frame and after loading. Returns true when the state changed. */
bool8_t vkr_scene_resolve_world(VkrScene *scene);
/** Gravity of the resolved physics settings, or the default before the
 * scene's first resolution. */
Vec3 vkr_scene_gravity(const VkrScene *scene);
/** Animation clock scale of the resolved World animation settings. */
float32_t vkr_scene_animation_time_scale(const VkrScene *scene);
/** Whether the scene may hold the type: World-only types need the root. */
bool8_t vkr_scene_type_allowed(const VkrScene *scene,
                               const struct VkrTypeDesc *type);
/** Mesh renderer facts of an entity; false when it has no live instance. */
bool8_t vkr_scene_mesh_info(const VkrScene *scene, VkrEntityId entity,
                            SceneMeshInfo *out);
/** Set the root World consulted after this scene's own singletons; ignored
    while the scene's settings do not inherit the World, except for World-only
    types, which always resolve from it. */
void vkr_scene_set_world_fallback(VkrScene *scene, const VkrScene *root);
/** True when `entity`'s singleton of `type` is the one resolution selected. */
bool8_t vkr_scene_singleton_active(const VkrScene *scene, VkrEntityId entity,
                                   const struct VkrTypeDesc *type);

/** Releases the scene's diffuse-volume texture and disables volume sampling. */
void vkr_scene_reset_diffuse_volume(VkrScene *scene,
                                    struct VkrRenderAssets *assets);

/** Queues an atmosphere revision. GPU work begins at the cold world seam.
    `clouds` publishes with the revision and requires an enabled atmosphere.
    `sh_deringing` is the global environment's authored window (ADR-038). */
bool8_t vkr_scene_request_atmosphere(VkrScene *scene,
                                     const VkrAtmosphereSettings *settings,
                                     const VkrCloudSettings *clouds,
                                     float32_t sh_deringing);

/** The sky light follows a moving sun with at most one revision request per
    this many seconds; the drawn sun and direct light follow every frame. */
#define VKR_SCENE_SUN_REFRESH_SECONDS 0.25

/** Resolves `scene->sun` and `scene->moon` and makes an enabled atmosphere
    follow them: each light's world direction, tinted colour times intensity
    and disc diameter replace the authored sun or moon in `live_settings`.
    Without an atmosphere sun light the sky has no sun, and without a moon
    light no moon. A sun or moon that differs from the requested
    revision queues a new one immediately while nothing is published or the
    latest request has not started baking, otherwise once
    VKR_SCENE_SUN_REFRESH_SECONDS have passed since the last. Call once per
    frame, with its duration, before the atmosphere preparation and the
    lighting system read them (ADR-058). */
void vkr_scene_sync_sun(VkrScene *scene, float64_t delta_seconds);

/** The frame's atmosphere: the published medium, which the published lookups
    baked, lit by the scene's current sun. Meaningful while a revision is
    published. */
VkrAtmosphereSettings
vkr_scene_atmosphere_frame_settings(const VkrScene *scene);

/** The frame's key light, memoized by the frame's prepared parameters. */
VkrSceneKeyLight vkr_scene_atmosphere_frame_key_light(VkrScene *scene);

/**
 * @brief Update scene transforms and prepare for renderer sync.
 * Call once per frame before syncing the scene to the renderer.
 * @param scene Scene to update
 * @param dt Delta time (currently unused, reserved for future animation)
 */
void vkr_scene_update(VkrScene *scene, float64_t dt);

/* Propagate authored/evaluated transforms without advancing simulation time. */
void vkr_scene_update_transforms(VkrScene *scene);

// ============================================================================
// Scene Runtime Handle API (preferred for renderer/resource integration)
// ============================================================================

/**
 * @brief Creates a runtime scene handle with an internal render bridge.
 *
 * Intended for use by `VkrResourceSystem` loaders and other higher-level
 * systems that want a single handle for update/sync/picking.
 *
 * @param alloc Allocator for the runtime handle and scene-owned data.
 * @param world_id World ID embedded in entity IDs (use 0 for single-scene
 * apps).
 * @param initial_entity_capacity Initial ECS entity capacity hint.
 * @param initial_picking_capacity Initial render-id-to-entity mapping capacity.
 * @param out_error Optional error output.
 * @return A valid handle on success, or `VKR_SCENE_HANDLE_INVALID` on failure.
 */
VkrSceneHandle vkr_scene_handle_create(VkrAllocator *alloc, uint16_t world_id,
                                       uint32_t initial_entity_capacity,
                                       uint32_t initial_picking_capacity,
                                       VkrSceneError *out_error);

/**
 * @brief Destroys a runtime scene handle and releases owned renderer resources.
 *
 * @param handle Scene handle to destroy.
 * @param assets Optional published asset owner (required for owned mesh
 * cleanup).
 */
void vkr_scene_handle_destroy(VkrSceneHandle handle,
                              struct VkrRenderAssets *assets);

/**
 * @brief Gets the underlying scene pointer from a runtime handle.
 * @param handle Scene handle.
 * @return Scene pointer, or NULL if handle is invalid.
 */
VkrScene *vkr_scene_handle_get_scene(VkrSceneHandle handle);

/**
 * @brief Updates scene transforms/dirty tracking for a runtime handle.
 * @param handle Scene handle.
 * @param dt Delta time (currently unused).
 */
void vkr_scene_handle_update(VkrSceneHandle handle, float64_t dt);

/**
 * @brief Incrementally syncs dirty entities from scene to renderer.
 * @param handle Scene handle.
 * @param assets Published asset owner.
 */
void vkr_scene_handle_sync(VkrSceneHandle handle,
                           struct VkrRenderAssets *assets);

/**
 * @brief Full sync of all renderables (use after scene load).
 * @param handle Scene handle.
 * @param assets Published asset owner.
 */
void vkr_scene_handle_full_sync(VkrSceneHandle handle,
                                struct VkrRenderAssets *assets);

/**
 * @brief Convenience helper: update + incremental sync.
 * @param handle Scene handle.
 * @param assets Published asset owner.
 * @param dt Delta time.
 */
void vkr_scene_handle_update_and_sync(VkrSceneHandle handle,
                                      struct VkrRenderAssets *assets,
                                      float64_t dt);

/**
 * @brief Map picking object_id to entity for a runtime handle.
 * @param handle Scene handle.
 * @param object_id Picking result (scene kind, render_id + 1, or 0 for
 * background).
 * @return Entity ID, or VKR_ENTITY_ID_INVALID if not found.
 */
VkrEntityId vkr_scene_handle_entity_from_picking_id(VkrSceneHandle handle,
                                                    uint32_t object_id);

// ============================================================================
// Entity Management
// ============================================================================

/**
 * @brief Create a new entity in the scene.
 * @param scene Scene to create entity in
 * @param out_error Optional error output
 * @return Entity ID, or VKR_ENTITY_ID_INVALID on failure
 */
VkrEntityId vkr_scene_create_entity(VkrScene *scene, VkrSceneError *out_error);

/**
 * @brief Destroy an entity and remove it from the scene.
 * @param scene Scene containing the entity
 * @param entity Entity to destroy
 */
void vkr_scene_destroy_entity(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Check if an entity is alive.
 * @param scene Scene to check in
 * @param entity Entity to check
 * @return true if entity exists and is alive
 */
bool8_t vkr_scene_entity_alive(const VkrScene *scene, VkrEntityId entity);

// ============================================================================
// Component Helpers
// ============================================================================

/**
 * @brief Set entity name (copies string into scene allocator).
 * @param scene Scene containing the entity
 * @param entity Entity to name
 * @param name Name string (copied)
 * @return true on success
 */
bool8_t vkr_scene_set_name(VkrScene *scene, VkrEntityId entity, String8 name);

/**
 * @brief Get entity name.
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Name string, or empty string if no name component
 */
String8 vkr_scene_get_name(const VkrScene *scene, VkrEntityId entity);

/**
 * Borrowed direct children of `parent` from the hierarchy index, valid until
 * the hierarchy next changes. Empty (NULL, count 0) when `parent` has no
 * children or the index is not built.
 */
const VkrEntityId *vkr_scene_get_children(const VkrScene *scene,
                                          VkrEntityId parent,
                                          uint32_t *out_count);

/**
 * @brief Add or update transform component.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param position World position
 * @param rotation Orientation quaternion
 * @param scale Scale factors
 * @return true on success
 */
bool8_t vkr_scene_set_transform(VkrScene *scene, VkrEntityId entity,
                                Vec3 position, VkrQuat rotation, Vec3 scale);

/**
 * @brief Get transform component (mutable).
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Transform pointer, or NULL if no transform component
 */
SceneTransform *vkr_scene_get_transform(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Set entity position (auto-marks dirty).
 */
void vkr_scene_set_position(VkrScene *scene, VkrEntityId entity, Vec3 position);

/**
 * @brief Set entity rotation (auto-marks dirty).
 */
void vkr_scene_set_rotation(VkrScene *scene, VkrEntityId entity,
                            VkrQuat rotation);

/**
 * @brief Set entity scale (auto-marks dirty).
 */
void vkr_scene_set_scale(VkrScene *scene, VkrEntityId entity, Vec3 scale);

/**
 * @brief Set entity parent (auto-marks hierarchy dirty).
 * @param scene Scene containing the entity
 * @param entity Entity to reparent
 * @param parent New parent entity (VKR_ENTITY_ID_INVALID for root)
 */
void vkr_scene_set_parent(VkrScene *scene, VkrEntityId entity,
                          VkrEntityId parent);

/**
 * @brief Add mesh renderer component and ensure a render id for picking.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param instance Handle to mesh instance
 * @return true on success
 */
bool8_t vkr_scene_set_mesh_renderer(VkrScene *scene, VkrEntityId entity,
                                    VkrMeshInstanceHandle instance);

/**
 * @brief Ensure entity has a render id (assigned if missing).
 * @param scene Scene containing the entity
 * @param entity Entity to ensure render id for
 * @param out_render_id Optional output for the assigned render id
 * @return true on success, false if the render id space is exhausted
 */
bool8_t vkr_scene_ensure_render_id(VkrScene *scene, VkrEntityId entity,
                                   uint32_t *out_render_id);

/**
 * @brief Get entity render id.
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Render id or 0 if missing
 */
uint32_t vkr_scene_get_render_id(const VkrScene *scene, VkrEntityId entity);

/**
 * @brief Set visibility component.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param visible Visibility state
 * @param inherit_parent Whether to inherit parent visibility
 */
void vkr_scene_set_visibility(VkrScene *scene, VkrEntityId entity,
                              bool8_t visible, bool8_t inherit_parent);

/** Effective visibility through inheriting parents. Hidden entities do not
    render, and hidden lights and world objects take no effect. */
bool8_t vkr_scene_entity_visible(const VkrScene *scene, VkrEntityId entity);

/** Set the editor Show filter; a change resyncs every renderable. */
void vkr_scene_set_editor_hidden_kinds(VkrScene *scene, uint32_t hidden_kinds);

// ============================================================================
// Light Components
// ============================================================================

/**
 * @brief Add a point light component and ensure render id for picking.
 * @param scene Scene containing the entity.
 * @param entity Entity to add point light to (must have SceneTransform).
 * @param light Point light configuration.
 * @return true on success.
 */
bool8_t vkr_scene_set_point_light(VkrScene *scene, VkrEntityId entity,
                                  const ScenePointLight *light);

/**
 * @brief Get point light component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks point light.
 */
ScenePointLight *vkr_scene_get_point_light(VkrScene *scene, VkrEntityId entity);

bool8_t vkr_scene_set_rectangle_light(VkrScene *scene, VkrEntityId entity,
                                      const SceneRectangleLight *light);
SceneRectangleLight *vkr_scene_get_rectangle_light(VkrScene *scene,
                                                   VkrEntityId entity);

/**
 * @brief Add a directional light component.
 * @param scene Scene containing the entity.
 * @param entity Entity to add directional light to.
 * @param light Directional light configuration.
 * @return true on success.
 */
bool8_t vkr_scene_set_directional_light(VkrScene *scene, VkrEntityId entity,
                                        const SceneDirectionalLight *light);

/**
 * @brief Get directional light component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks directional light.
 */
SceneDirectionalLight *vkr_scene_get_directional_light(VkrScene *scene,
                                                       VkrEntityId entity);

// ============================================================================
// Mesh Ownership
// ============================================================================

/**
 * @brief Track a mesh instance as scene-owned.
 * Scene will destroy this instance on shutdown.
 * @param scene Scene to own the instance.
 * @param instance Instance handle to track.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_track_instance(VkrScene *scene,
                                 VkrMeshInstanceHandle instance,
                                 VkrSceneError *out_error);

/**
 * @brief Stop owning a tracked mesh instance, for a caller that destroys it
 * itself.
 * @return true when the scene tracked the instance.
 */
bool8_t vkr_scene_untrack_instance(VkrScene *scene,
                                   VkrMeshInstanceHandle instance);

// ============================================================================
// Text3D Component
// ============================================================================

/**
 * @brief Configuration for adding a text3d component to an entity.
 */
typedef struct VkrSceneText3DConfig {
  String8 text;            // Text content (passed through to world resources)
  VkrFontHandle font;      // Font handle (or invalid for default)
  float32_t font_size;     // Text-target pixels per em (0 = font default)
  Vec4 color;              // Text color RGBA
  uint32_t texture_width;  // Texture width (0 = auto)
  uint32_t texture_height; // Texture height (0 = auto)
  float32_t uv_inset_px;   // Half-texel inset to avoid bleeding (0 = default)
} VkrSceneText3DConfig;

#define VKR_SCENE_TEXT3D_CONFIG_DEFAULT                                        \
  (VkrSceneText3DConfig){.text = {0},                                          \
                         .font = VKR_FONT_HANDLE_INVALID,                      \
                         .font_size = 32.0f,                                   \
                         .color = {1.0f, 1.0f, 1.0f, 1.0f},                    \
                         .texture_width = 512,                                 \
                         .texture_height = 128,                                \
                         .uv_inset_px = 0.5f}

/**
 * @brief Add a text3d component to an entity.
 *
 * Sends a world-resources create request and links the entity to that text id.
 * World resources own GPU resources; the scene stores text metadata.
 *
 * @param scene Scene containing the entity.
 * @param entity Entity to add text3d component to.
 * @param config Text configuration.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_set_text3d(VkrScene *scene, VkrEntityId entity,
                             const VkrSceneText3DConfig *config,
                             VkrSceneError *out_error);

/**
 * @brief Get the SceneText3D component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks text3d.
 */
SceneText3D *vkr_scene_get_text3d(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Update text content for a text3d entity.
 *
 * Marks the text as dirty for re-rendering on next sync.
 *
 * @param scene Scene containing the entity.
 * @param entity Entity with text3d component.
 * @param text New text content (passed to world resources).
 * @return true on success.
 */
bool8_t vkr_scene_update_text3d(VkrScene *scene, VkrEntityId entity,
                                String8 text);

// ============================================================================
// Shape Component
// ============================================================================

/**
 * @brief Configuration for adding a shape component to an entity.
 */
typedef struct VkrSceneShapeConfig {
  SceneShapeType type;   // Shape type (cube only for now)
  Vec3 dimensions;       // Width, height, depth
  Vec4 color;            // RGBA color
  String8 material_name; // Material name for acquire (matches .mt name=)
  String8 material_path; // Material file path for loading
} VkrSceneShapeConfig;

#define VKR_SCENE_SHAPE_CONFIG_DEFAULT                                         \
  (VkrSceneShapeConfig) {                                                      \
    .type = SCENE_SHAPE_TYPE_CUBE, .dimensions = {1.0f, 1.0f, 1.0f},           \
    .color = {1.0f, 1.0f, 1.0f, 1.0f}, .material_name = {0},                   \
    .material_path = {0}                                                       \
  }

/**
 * @brief Add a shape component to an entity.
 *
 * Creates geometry via geometry system and adds it to mesh manager.
 * The mesh is tracked as scene-owned.
 *
 * @param scene Scene containing the entity.
 * @param assets Published asset owner.
 * @param entity Entity to add shape to.
 * @param config Shape configuration.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_set_shape(VkrScene *scene, struct VkrRenderAssets *assets,
                            VkrEntityId entity,
                            const VkrSceneShapeConfig *config,
                            VkrSceneError *out_error);

// ============================================================================
// Entity Lookup
// ============================================================================

/**
 * @brief Find an entity by name.
 * @param scene Scene to search.
 * @param name Entity name to find.
 * @return Entity ID, or VKR_ENTITY_ID_INVALID if not found.
 */
VkrEntityId vkr_scene_find_entity_by_name(const VkrScene *scene, String8 name);

/* Exact imported matrix is retained until an explicit representable TRS edit.
 */
bool8_t vkr_scene_set_local_matrix(VkrScene *scene, VkrEntityId entity,
                                   Mat4 local);

/* Publish an evaluated world pose; NULL removes the transient override. */
bool8_t vkr_scene_set_evaluated_transform(VkrScene *scene, VkrEntityId entity,
                                          const Mat4 *world);
bool8_t vkr_scene_set_source_identity(VkrScene *scene, VkrEntityId entity,
                                      const SceneSourceIdentity *identity);

/** Parse a canonical 36-character UUID in either case. */
bool8_t vkr_scene_document_id_parse(String8 text, VkrSceneDocumentId *out);
/** Lowercase canonical text with a terminator. */
void vkr_scene_document_id_format(const VkrSceneDocumentId *id, char out[37]);
/** Scene-owned storage for `count` document ids, replacing any previous
    ids; NULL on allocation failure. */
VkrSceneDocumentId *vkr_scene_document_ids_reserve(VkrScene *scene,
                                                   uint32_t count);

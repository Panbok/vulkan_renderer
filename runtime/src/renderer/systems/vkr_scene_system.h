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

#include "assets/vkr_diffuse_volume.h"
#include "assets/vkr_light_layers.h"
#include "assets/vkr_lightmap_set.h"
#include "containers/str.h"
#include "core/vkr_entity.h"
#include "core/vkr_type_desc.h"
#include "level/vkr_blockout.h"
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

#define SCENE_DECAL_MATERIAL_CAPACITY 256u

/**
 * A `decal` component (ADR-092): the entity's world transform places a box
 * whose inside is [-0.5, 0.5] on each axis. The decal projects along the
 * box's -Y onto the surfaces facing its +Y and lays its material's base
 * colour over theirs before lighting. Only the tiled pipeline draws decals.
 */
typedef struct SceneDecal {
  /** A .mt material file; empty uses VKR_SCENE_DECAL_DEFAULT_MATERIAL. */
  char material[SCENE_DECAL_MATERIAL_CAPACITY];
  float32_t opacity;
  /** Radians between a surface's normal and the box's +Y at which the decal
   * starts to fade and at which it is gone. */
  float32_t fade_angle_start;
  float32_t fade_angle_end;
  /** Share of the box's half depth over which it fades toward its near and
   * far faces. */
  float32_t depth_fade;
  /** Overlapping decals draw in ascending order, the highest on top; equal
   * orders keep the order their components were added in. */
  int32_t sort_order;
  bool8_t enabled;
} SceneDecal;

/* How a brush takes part in the level (ADR-084): solid renders and collides,
 * visual only renders, clip only collides, and trigger is a sensor volume. Clip
 * and trigger brushes draw only while the editor edits the scene. */
typedef enum SceneBrushRole {
  SCENE_BRUSH_ROLE_SOLID = 0,
  SCENE_BRUSH_ROLE_VISUAL,
  SCENE_BRUSH_ROLE_CLIP,
  SCENE_BRUSH_ROLE_TRIGGER,
  SCENE_BRUSH_ROLE_COUNT,
} SceneBrushRole;

/* A convex brush, a typed component. Its faces are direct children that
 * carry SceneBrushFace; any change to the brush, a face, the set of faces
 * or the brush's transform rebuilds its mesh and collision. */
typedef struct SceneBrushSettings {
  SceneBrushRole role;
} SceneBrushSettings;

#define SCENE_BRUSH_MATERIAL_CAPACITY 256u

/* One face of a brush: the plane `dot(normal, p) = distance` in the brush's
 * local space bounds the solid, which lies on the side the normal points
 * away from. */
typedef struct SceneBrushFace {
  Vec3 normal;
  float32_t distance;
  /* Material file; empty uses the dev grid material. */
  char material[SCENE_BRUSH_MATERIAL_CAPACITY];
  /* Repeats added after scaling, meters per repeat and rotation (radians)
     of the projected texture. */
  Vec2 uv_offset;
  Vec2 uv_scale;
  float32_t uv_rotation;
  /* Project in world space, so neighbouring brushes line up; false
     projects in brush space, so the texture moves with the brush. */
  bool8_t uv_world;
} SceneBrushFace;

/* A heightfield terrain (ADR-084), centred on its entity, which must not be
 * rotated or scaled. Its samples live in `heightfield`, a file relative to the
 * scene's asset root (vkr_heightfield.h) that saving the scene writes; the
 * scene builds its tile meshes and one height field collider. Each layer
 * names a material its weights blend in; `texture_size` is the metres one
 * texture repeat covers. */
#define SCENE_TERRAIN_PATH_CAPACITY 256u
#define SCENE_TERRAIN_MATERIAL_CAPACITY 128u

typedef struct SceneTerrain {
  char heightfield[SCENE_TERRAIN_PATH_CAPACITY];
  char layer0[SCENE_TERRAIN_MATERIAL_CAPACITY];
  char layer1[SCENE_TERRAIN_MATERIAL_CAPACITY];
  char layer2[SCENE_TERRAIN_MATERIAL_CAPACITY];
  char layer3[SCENE_TERRAIN_MATERIAL_CAPACITY];
  float32_t texture_size;
  /* Metres around a streaming source a streamed terrain draws full
     detail; farther ground draws its overview. */
  float32_t stream_radius;
} SceneTerrain;

/* World partition (ADR-086). A scene with a `world_partition` component
 * streams its editor-created root entities by square cells of the ground
 * plane around its streaming sources; `always_loaded` keeps an entity in the
 * scene's persistent layer instead. Sizes are metres. */
typedef struct SceneWorldPartition {
  float32_t cell_size;
  /* Cells within this distance of a source load. */
  float32_t load_radius;
  /* Unloaded cells within this distance draw their proxies. */
  float32_t proxy_radius;
  /* Cells loaded at once; the farthest beyond it unload first. */
  uint32_t cell_budget;
} SceneWorldPartition;

typedef struct SceneAlwaysLoaded {
  bool8_t enabled;
} SceneAlwaysLoaded;

/* An entity the editor never snaps (ADR-084): the magnet, Snap and parenting
   in the Outliner leave it where it is placed. */
typedef struct SceneFreePlacement {
  bool8_t enabled;
} SceneFreePlacement;

/* Population (ADR-084). A `spline`
 * curve passes through its `spline_point` child entities in `order`, in the
 * spline entity's space. A `spline_mesh` on a spline repeats a cooked mesh
 * along it; a `scatter` places seeded instances of one in its entity's box,
 * dropped onto the first surface below. Instances are runtime data rebuilt
 * from these rules; documents store only the rules. */
#define SCENE_POPULATION_MESH_CAPACITY 256u

typedef struct SceneSpline {
  bool8_t closed;
} SceneSpline;

typedef struct SceneSplinePoint {
  float32_t order;
} SceneSplinePoint;

typedef struct SceneSplineMesh {
  /* Cooked .vkb and the source mesh in it, counting from 0. */
  char mesh[SCENE_POPULATION_MESH_CAPACITY];
  uint32_t mesh_index;
  /* Metres between copies along the curve. */
  float32_t spacing;
  Vec3 scale;
  /* Offset from the curve in each copy's frame: x right, y up, z along. */
  Vec3 offset;
  /* Turn of the mesh within each copy's frame, degrees about X, Y, Z. */
  Vec3 rotation;
  /* Copies tilt with the curve's slope instead of staying upright. */
  bool8_t follow_slope;
} SceneSplineMesh;

typedef struct SceneScatter {
  char mesh[SCENE_POPULATION_MESH_CAPACITY];
  uint32_t mesh_index;
  uint32_t count;
  uint32_t seed;
  /* Half sizes of the box, centred on the entity, that copies land in. */
  Vec3 extents;
  float32_t scale_min;
  float32_t scale_max;
  /* Turn of the mesh within each copy's frame, degrees about X, Y, Z. */
  Vec3 rotation;
  /* Copies lean to the surface's normal instead of standing upright. */
  bool8_t align_to_surface;
  bool8_t random_yaw;
} SceneScatter;

/* Entity IO (ADR-084): engine components whose outputs connections wire to
 * other entities' inputs. The script host's router runs them during a
 * session; their runtime state lives there, not in the component. */
#define SCENE_IO_NAME_CAPACITY 64u

/* Reports what enters and leaves the entity's sensor: a trigger brush or a
 * sensor collider. `filter` names a component the other entity must carry,
 * such as the player's; empty takes any. */
typedef struct SceneTrigger {
  bool8_t enabled;
  /* Disable after the first entry, as Source's trigger_once. */
  bool8_t once;
  char filter[SCENE_IO_NAME_CAPACITY];
} SceneTrigger;

/* Passes `trigger` on as `on_trigger` while enabled. */
typedef struct SceneRelay {
  bool8_t enabled;
} SceneRelay;

/* Pressed by `press`, as the player's use key sends it, as Source's
 * func_button: fires `on_pressed` with the activator, then takes no press
 * for `wait` seconds, or with a negative wait for the rest of the session;
 * while `locked` a press fires `on_refused` instead. */
typedef struct SceneButton {
  float32_t wait;
  bool8_t locked;
} SceneButton;

/* Fires `on_timer` every `interval` seconds of simulation while running. */
typedef struct SceneTimer {
  float32_t interval;
  bool8_t start_running;
  /* Stop after one firing. */
  bool8_t once;
} SceneTimer;

/* An integer that `add`, `subtract` and `set` change within [min, max],
 * firing `on_changed`, then `on_max` or `on_min` when it reaches a bound. */
typedef struct SceneCounter {
  int32_t start;
  int32_t min;
  int32_t max;
} SceneCounter;

/* Moves the entity and everything under it between its authored pose and
 * an open pose `distance` metres along `direction` (in the entity's own
 * space), as Source's func_door and func_movelinear do, or with a nonzero
 * `angle` one turned that many degrees about `axis` through `pivot`, as
 * func_door_rotating does; with `spin` it turns on while open, as a fan.
 * It moves only while the game plays, through an evaluated pose; the saved
 * transform never changes. Solid and clip brushes under it collide where it
 * moves. */
typedef struct SceneMover {
  Vec3 direction;
  /* Metres to the open pose; zero takes the size of what it moves along
     `direction`, less `lip`. */
  float32_t distance;
  /* What stays in view of an automatic distance, as Source's lip. */
  float32_t lip;
  /* Metres per second. */
  float32_t speed;
  /* Seconds it stays open before it closes by itself; negative stays open. */
  float32_t wait;
  bool8_t start_open;
  /* Moves back and forth without inputs, resting `wait` at each end. */
  bool8_t loop;
  /* Refuses `open`, `toggle` and `set_position` until unlocked. */
  bool8_t locked;
  /* Degrees to the open pose about `axis` through `pivot`, both in its own
     space; zero slides along `direction` instead. `speed` is then degrees
     per second. */
  float32_t angle;
  Vec3 axis;
  Vec3 pivot;
  /* Turns about `axis` without end while open and stops where it is when
     closed; `angle` does not apply. */
  bool8_t spin;
} SceneMover;

/* One connection, a child entity of its source: when the source fires
 * `output`, `target` receives `input` after `delay` seconds. `value`, when
 * set, replaces the output's value as text the input's kind parses. A
 * nonzero `limit` stops it after that many firings per session. Output and
 * input names may be prefixed with their component, as `trigger.on_enter`. */
typedef struct SceneIoConnection {
  char output[SCENE_IO_NAME_CAPACITY];
  VkrEntityRef target;
  char input[SCENE_IO_NAME_CAPACITY];
  char value[SCENE_IO_NAME_CAPACITY];
  float32_t delay;
  uint32_t limit;
} SceneIoConnection;

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
#define VKR_SCENE_TEXT_FONT_COUNT 4u

typedef struct SceneTextSettings {
  char content[256]; /* UTF-8; '\n' starts a line. */
  /* Index into the engine text fonts (vkr_scene_text_font_name). */
  uint32_t font;
  /* Em height in meters at unit scale; the slot's texture box is one meter
     wide, so this is font pixels per texture width. */
  float32_t size;
  Vec4 color;
  uint32_t align; /* VKR_TEXT_ALIGN_LEFT, _CENTER or _RIGHT, per line. */
  float32_t line_spacing;   /* Multiple of the font's line height. */
  float32_t letter_spacing; /* Extra advance per glyph in ems. */
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

/* How a light reaches the tiled pipeline (ADR-088): a static light is baked
   into its light group's lightmap layer and adds nothing at runtime; a
   dynamic light is never baked and lights at runtime. The desktop pipeline
   lights both at runtime. Zero is static, so a zero-initialized light is. */
typedef enum VkrLightMobility {
  VKR_LIGHT_MOBILITY_STATIC = 0,
  VKR_LIGHT_MOBILITY_DYNAMIC = 1,
} VkrLightMobility;

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
  /* Emitter radius in metres, finite in [0, VKR_POINT_LIGHT_SOURCE_RADIUS_MAX];
   * zero keeps the fixed shadow filter. */
  float32_t source_radius;
  VkrPointLightKind kind;
  bool8_t enabled;      // Whether this light is active
  bool8_t casts_shadow; // Requires a finite positive range.
  VkrLightMobility mobility;
  /* A light group name (vkr_light_group_name_valid); empty is the
     default group. */
  char light_group[VKR_LIGHT_GROUP_NAME_BYTES];
  /* Runtime: the group's slot in its scene's light groups, which setting the
     light assigns; VKR_SCENE_LIGHT_GROUP_NONE for a dynamic light. */
  uint8_t light_group_slot;
} ScenePointLight;

/** One-sided rectangular emitter. Entity translation is its center; rotation
 * maps local +X/+Y to its axes; entity scale is ignored; local -Z emits. */
typedef struct SceneRectangleLight {
  Vec3 color;
  float32_t radiance;
  Vec2 size;
  bool8_t enabled;
  VkrLightMobility mobility;
  /* A light group name (vkr_light_group_name_valid); empty is the
     default group. */
  char light_group[VKR_LIGHT_GROUP_NAME_BYTES];
  /* Runtime: as ScenePointLight's. */
  uint8_t light_group_slot;
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

/** Bytes of a time of day's night group list: four group names, commas and
 * the terminator. */
#define SCENE_TIME_OF_DAY_NIGHT_GROUPS_CAPACITY 128u

/** World time of day (ADR-090): the sun and moon turn about the atmosphere's
 * celestial pole as the sky does, their authored directions taken as noon
 * (`hour` 12). While the scene's simulation runs, the hour advances 24 hours
 * per `day_minutes` real minutes; zero keeps it. `night_groups` names,
 * separated by commas, the light groups that light only while the sun is
 * down. */
typedef struct SceneTimeOfDay {
  bool8_t enabled;
  float32_t hour;
  float32_t day_minutes;
  char night_groups[SCENE_TIME_OF_DAY_NIGHT_GROUPS_CAPACITY];
} SceneTimeOfDay;

/** Light groups a scene's lights name (ADR-090), registered as lights are
 * set; a light keeps its group's slot. Each frame a group's factor is its
 * intensity, times the night fade when the time of day lists it as a night
 * group; lights scale their output by it. */
#define VKR_SCENE_LIGHT_GROUP_MAX 16u
#define VKR_SCENE_LIGHT_GROUP_NONE 0xFFu

typedef struct VkrSceneLightGroups {
  char names[VKR_SCENE_LIGHT_GROUP_MAX][VKR_LIGHT_GROUP_NAME_BYTES];
  /* Set by scripts and commands, one until then; a simulation reset restores
     one. */
  float32_t intensities[VKR_SCENE_LIGHT_GROUP_MAX];
  float32_t factors[VKR_SCENE_LIGHT_GROUP_MAX];
  uint32_t count;
} VkrSceneLightGroups;

/** A diffuse volume's light layers (ADR-054, ADR-090): each probe's SH per
 * layer and the region texel of its row, kept so the scene can recompose the
 * published texture as the sun turns and light group factors change.
 * Heap-owned by the scene; `probe_sh` and `probe_regions` are malloc'd. */
typedef struct VkrSceneDiffuseVolumeLayers {
  VkrLightLayer layers[VKR_DIFFUSE_VOLUME_MAX_LAYERS];
  uint32_t layer_count;
  uint32_t probe_count;
  /** probe_count * layer_count SH, probe-major. */
  VkrShL2Packed *probe_sh;
  /** Texel 7 of each probe's row: probe region, lower-corner cell region. */
  float32_t *probe_regions;
  /** Weights of the published texture. */
  float32_t weights[VKR_DIFFUSE_VOLUME_MAX_LAYERS];
  float64_t since_compose;
  uint32_t version;
  /** Texture name stem: the volume's path. */
  char name[256];
} VkrSceneDiffuseVolumeLayers;

/** Lightmap instances one scene binds: slots travel as exact floats in the
    prepared instance row (VkrPreparedInstanceGPU). */
#define VKR_SCENE_LIGHTMAP_MAX_INSTANCES (1u << 24)

/** A scene's lightmap set (ADR-088): the layers, each instance's matching
 * key and page rectangle, and the texture of every layer page. The scene
 * gives each matched mesh instance its lightmap slot (instance index + 1)
 * and weighs the layers each frame. Heap-owned by the scene; `instances`,
 * `rects` and `bound` are malloc'd. */
typedef struct VkrSceneLightmaps {
  VkrTextureHandle texture;
  uint32_t page_size;
  uint32_t page_count;
  uint32_t layer_count;
  VkrLightLayer layers[VKR_LIGHTMAP_SET_MAX_LAYERS];
  uint32_t instance_count;
  VkrLightmapInstance *instances;
  /** One per instance: the frame's rectangle table. */
  VkrLightmapRect *rects;
  /** Mesh instances and generated meshes the last binding gave a slot. */
  VkrMeshLightmapSlot *bound;
  uint32_t bound_count;
  /** Scene revisions the binding matched; a change rebinds. */
  uint64_t bound_structure_revision;
  uint64_t bound_mesh_revision;
  bool8_t binding_current;
} VkrSceneLightmaps;

/** The live time of day: the hour this frame, and a script-set hour that
 * advances from the tick it was set at until a simulation reset. */
typedef struct VkrSceneClock {
  /** In [0, 24); 12 without an enabled time of day. */
  float64_t hour;
  /** Night fade: zero while the sun stands 5 degrees or more above the
      horizon, one once it has set, and one without a sun. */
  float32_t night;
  bool8_t set;
  float64_t set_hour;
  uint64_t set_tick;
} VkrSceneClock;

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
  SceneTimeOfDay time_of_day;
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
  VkrEntityId time_of_day_entity;
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
/* Brush rebuild state (vkr_scene_brush.h). */
typedef struct s_VkrSceneBrushes VkrSceneBrushes;
struct VkrSubMeshDesc;
typedef struct s_VkrScenePhysicsSet VkrScenePhysicsSet;
typedef struct VkrSceneCollisionLayers VkrSceneCollisionLayers;

/** Scene-level settings (ADR-076), saved in the scene's edit overlay. */
typedef struct VkrSceneSettings {
  /** The root World's objects apply where the scene has none of its own;
      off scopes the scene to its own objects. */
  bool8_t inherit_world;
  /** Largest extent of a material texture loaded while this scene is open,
      a power of two from VKR_SCENE_TEXTURE_EXTENT_MIN to
      VKR_SCENE_TEXTURE_EXTENT_MAX, or zero for full resolution. The Graphics
      texture resolution still applies. */
  uint32_t texture_max_extent;
} VkrSceneSettings;

#define VKR_SCENE_TEXTURE_EXTENT_MIN 256u
#define VKR_SCENE_TEXTURE_EXTENT_MAX 16384u

/** Whether `extent` is a valid VkrSceneSettings.texture_max_extent. */
bool8_t vkr_scene_texture_extent_valid(uint32_t extent);

/* Geometry kinds the editor's Show menu can leave undrawn. */
#define VKR_SCENE_SHOW_HIDE_STATIC_MESHES (1u << 0)
#define VKR_SCENE_SHOW_HIDE_ANIMATED_MESHES (1u << 1)
#define VKR_SCENE_SHOW_HIDE_SHAPES (1u << 2)
#define VKR_SCENE_SHOW_HIDE_ALL                                                \
  (VKR_SCENE_SHOW_HIDE_STATIC_MESHES | VKR_SCENE_SHOW_HIDE_ANIMATED_MESHES |   \
   VKR_SCENE_SHOW_HIDE_SHAPES)

/* Streaming sources one scene follows. */
#define VKR_SCENE_STREAM_SOURCES_MAX 4u

struct VkrScene;

/** Called before an entity is destroyed, while its components exist. */
typedef void (*VkrSceneEntityObserver)(struct VkrScene *scene,
                                       VkrEntityId entity, void *context);

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
  VkrEntityRef *document_ids;
  uint32_t document_id_count;
  VkrComponentTypeId comp_source_identity;
  VkrComponentTypeId comp_name;
  VkrComponentTypeId comp_transform;
  VkrComponentTypeId comp_evaluated_transform;
  /** Tag of runtime-only entities that saving skips. */
  VkrComponentTypeId comp_transient;
  /** VkrEntityRef of an entity the editor created; document entities take
      theirs from `document_ids` (vkr_scene_entity_ref). */
  VkrComponentTypeId comp_entity_ref;
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
  /** Clip and trigger brushes draw only while this holds: the editor sets it
      while it edits the scene; games and Play leave it off. */
  bool8_t editor_volumes;
  /** Editor Hide (ADR-084): objects the viewport neither draws nor picks,
      with everything under them; with `editor_isolate`, the only objects it
      draws. Sorted by id, never saved or journaled. */
  VkrEntityId *editor_hidden;
  uint32_t editor_hidden_count;
  uint32_t editor_hidden_capacity;
  bool8_t editor_isolate;
  /** Brush rebuild state, created with the first brush. */
  VkrSceneBrushes *brushes;
  /** Terrain state, created with the first terrain (vkr_scene_terrain.h). */
  struct s_VkrSceneTerrains *terrains;
  /** Decal material bindings, created with the first decal
   * (vkr_scene_decal.h). */
  struct s_VkrSceneDecals *decals;
  /** Population state, created with the first spline mesh or scatter
      (vkr_scene_population.h). */
  struct s_VkrScenePopulation *population;
  /** World partition cells, created with the first one
      (vkr_scene_partition.h). */
  struct s_VkrScenePartition *partition;
  /** Points content streams around (world partition): the editor camera or
      each player, set by the host before each update. */
  Vec3 stream_sources[VKR_SCENE_STREAM_SOURCES_MAX];
  uint32_t stream_source_count;
  /** Origin rebase (ADR-086): document position = world position +
      `origin_offset`. Nonzero only during Play past the rebase distance;
      `origin_saved` holds each root's position from before the first shift,
      so restoring is exact. */
  Vec3 origin_offset;
  struct SceneOriginSaved *origin_saved;
  uint32_t origin_saved_count;

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
  /** Time of day and light groups (ADR-090); vkr_scene_sync_sun updates
      both. */
  VkrSceneClock clock;
  VkrSceneLightGroups light_groups;
  /** Descriptor-typed component types registered with the ECS (ADR-076). */
  VkrSceneComponentType types[VKR_SCENE_TYPE_MAX];
  uint32_t type_count;
  /** Bumped by every world component change; resolution compares it. */
  uint64_t world_revision;
  /** Told about each entity before vkr_scene_destroy_entity tears it down;
      the script host runs destroy hooks from it (ADR-079). */
  VkrSceneEntityObserver destroy_observer;
  void *destroy_observer_context;
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
  /** The volume's layers, or NULL; see VkrSceneDiffuseVolumeLayers. */
  VkrSceneDiffuseVolumeLayers *diffuse_volume_layers;
  /** The scene's lightmap set, or NULL; see VkrSceneLightmaps. */
  VkrSceneLightmaps *lightmaps;
  /** Advances whenever an entity gains a mesh renderer or a generated mesh,
      so lightmap binding sees meshes that attach after the set. */
  uint64_t mesh_revision;
  VkrSubsurfaceBinding subsurface;
  VkrSceneReflectionProbe reflection_probes[VKR_SCENE_REFLECTION_PROBE_MAX];
  uint32_t reflection_probe_count;
  /** Engine text fonts the scene's text uses past the default, one
      reference each until shutdown; a font whose load failed is marked and
      not retried. */
  VkrFontHandle text_fonts[VKR_SCENE_TEXT_FONT_COUNT];
  uint32_t text_font_failed;
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
/** Moves every component of `type` to its new layout after the type's
    registered descriptor changed from `previous` (ADR-079): a new ECS
    component of the new size replaces the old one on each entity, with its
    value converted by vkr_type_migrate. The old component id stays
    registered, unused. True when the scene never synced the type. Call
    outside queries and ticks. */
bool8_t vkr_scene_migrate_world_type(VkrScene *scene,
                                     const struct VkrTypeDesc *type,
                                     const struct VkrTypeDesc *previous);
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

/** Releases the scene's diffuse-volume texture and layers and disables
    volume sampling. */
void vkr_scene_reset_diffuse_volume(VkrScene *scene,
                                    struct VkrRenderAssets *assets);

/** The upload of a diffuse-volume texture: 8-by-probe-count RGBA32F rows of
    `layers` weighted by `weights` (one per layer), in malloc'd storage that
    vkr_texture_system_release_prepared_load frees. */
bool8_t vkr_scene_diffuse_volume_prepare_texture(
    const VkrSceneDiffuseVolumeLayers *layers, const float32_t *weights,
    struct VkrTexturePreparedLoad *out_prepared);

/** The weights of `layers` for the scene's current sun and light group
    factors: sun keys by vkr_light_layers_sun_weights, lamp groups by their
    group's factor (one for a group no light has named). */
void vkr_scene_light_layer_weights(const VkrScene *scene,
                                   const VkrLightLayer *layers,
                                   uint32_t layer_count,
                                   float32_t *out_weights);

/** Recomposes the volume texture when a layer weight has moved by more than
    one percent since the last composition, at most every
    VKR_SCENE_SUN_REFRESH_SECONDS, replacing the texture and releasing the
    previous one. Call once per frame after vkr_scene_sync_sun on the thread
    that finalizes textures. */
void vkr_scene_update_diffuse_volume(VkrScene *scene,
                                     struct VkrRenderAssets *assets,
                                     float64_t delta_seconds);

/** Clears the lightmap slots the scene gave, releases its lightmap texture
    and set, and disables lightmap sampling. */
void vkr_scene_reset_lightmaps(VkrScene *scene, struct VkrRenderAssets *assets);

/** Gives every mesh instance or generated mesh that a lightmap instance
    names its slot, after the set loads and whenever entities or meshes
    change since the last binding. A lightmap instance with a document id
    names the entity with that id, else the entity at its index in the scene
    document; its instance index names that entity's source node, or the
    entity itself for a model without nodes and a brush. Returns how many
    instances are bound. */
uint32_t vkr_scene_bind_lightmaps(VkrScene *scene);

/** The frame's lightmap binding: the texture, the rectangle table and the
    layers with nonzero weight for the current sun and light group factors.
    A zero binding without a set. */
void vkr_scene_lightmap_binding(const VkrScene *scene,
                                VkrLightmapBinding *out_binding);

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
    VKR_SCENE_SUN_REFRESH_SECONDS have passed since the last. With an
    enabled time of day the sun and moon first turn to the clock's hour.
    The clock's night fade and the scene's light group factors follow the
    resulting sun (ADR-090). Call once per frame, with its duration, before
    the atmosphere preparation and the lighting system read them
    (ADR-058). */
void vkr_scene_sync_sun(VkrScene *scene, float64_t delta_seconds);

/** Hours the sun turns from noon at `hour`, as radians about the celestial
    pole, right-handed: negative after noon, as the sky turns. */
float32_t vkr_scene_time_of_day_angle(float64_t hour);

/** Starts the clock at `hour` from the current simulation tick, until the
    simulation resets. False for a non-finite hour. */
bool8_t vkr_scene_set_time_of_day_hour(VkrScene *scene, float64_t hour);

/** Sets a light group's intensity, registering the group when no light has
    named it yet; zero switches its lights off. False for an invalid name, a
    negative or non-finite intensity, or a full registry. */
bool8_t vkr_scene_set_light_group_intensity(VkrScene *scene, const char *name,
                                            float32_t intensity);

/** The next name of a night group list from `*cursor`, with surrounding
    spaces trimmed and empty entries skipped; false at the end. The name
    borrows the list. */
bool8_t vkr_scene_night_group_next(const char *list, uint32_t *cursor,
                                   String8 *out_name);

/** Recomputes the scene's light group factors for a night fade and a time of
    day's night group list; vkr_scene_sync_sun calls it with the scene's
    own, and the runtime with the rendered scene's for additive scenes. */
void vkr_scene_update_light_groups(VkrScene *scene, float32_t night,
                                   const char *night_groups);

/** The output factor of a light in group `slot`: one for
    VKR_SCENE_LIGHT_GROUP_NONE. */
vkr_internal INLINE float32_t
vkr_scene_light_group_factor(const VkrScene *scene, uint8_t slot) {
  return slot < scene->light_groups.count ? scene->light_groups.factors[slot]
                                          : 1.0f;
}

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

/* Moves every root entity by -`shift` metres and adds it to
   `origin_offset`; the first shift remembers each root's position. The
   caller moves physics (vkr_scene_physics_shift) and cameras with it. */
bool8_t vkr_scene_shift_origin(VkrScene *scene, Vec3 shift);
/* Puts every root remembered at the first shift back where it was, moves
   roots made since by `origin_offset`, and returns the offset it undid. */
Vec3 vkr_scene_restore_origin(VkrScene *scene);

/* Replaces the scene's streaming sources with the first
   VKR_SCENE_STREAM_SOURCES_MAX of `sources`, in world space. */
void vkr_scene_set_stream_sources(VkrScene *scene, const Vec3 *sources,
                                  uint32_t count);

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
 *
 * The destroy observer runs first, while the entity's components and
 * children exist; it may destroy other entities, including this one.
 * @param scene Scene containing the entity
 * @param entity Entity to destroy
 */
void vkr_scene_destroy_entity(VkrScene *scene, VkrEntityId entity);

/** Reserves an entity ID, also inside a tick; see vkr_entity_reserve_entity.
 * The ID is not alive until vkr_scene_create_reserved_entity. */
VkrEntityId vkr_scene_reserve_entity(VkrScene *scene);

/** Creates the entity a reservation names. False in a tick, for an ID that is
 * not reserved, or when creation fails. */
bool8_t vkr_scene_create_reserved_entity(VkrScene *scene, VkrEntityId entity);

/** Returns an unused reservation. */
void vkr_scene_cancel_reserved_entity(VkrScene *scene, VkrEntityId entity);

/** Marks an entity as runtime-only: saving never writes it or its edits.
 * Scripts mark everything they spawn (ADR-079). */
bool8_t vkr_scene_set_transient(VkrScene *scene, VkrEntityId entity);
bool8_t vkr_scene_entity_transient(const VkrScene *scene, VkrEntityId entity);

/** Installs the scene's one destroy observer; false when another holds the
 * slot. `context` is borrowed until vkr_scene_unobserve_destroy. */
bool8_t vkr_scene_observe_destroy(VkrScene *scene,
                                  VkrSceneEntityObserver observer,
                                  void *context);

/** Removes the observer installed with `context`; false for another. */
bool8_t vkr_scene_unobserve_destroy(VkrScene *scene, void *context);

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
 * @brief Box around the loaded meshes and shapes of an entity and its
 * descendants, in the entity's local frame (before its own scale).
 * @return False when none of them has loaded geometry.
 */
bool8_t vkr_scene_entity_local_bounds(const VkrScene *scene, VkrEntityId entity,
                                      Vec3 *out_min, Vec3 *out_max);

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
/** Most objects the editor's hidden set holds. */
#define VKR_SCENE_EDITOR_HIDDEN_MAX 256u
/** Replace the editor's hidden objects, or with `isolate` the only ones
    drawn, by id in any container; an empty set draws everything. A change
    resyncs every renderable. */
bool8_t vkr_scene_set_editor_hidden(VkrScene *scene,
                                    const VkrEntityId *entities, uint32_t count,
                                    bool8_t isolate);
/** Whether the editor's hidden set leaves `entity` drawn: neither it nor an
    object above it is hidden, or with isolate one of them is isolated. */
bool8_t vkr_scene_editor_shown(const VkrScene *scene, VkrEntityId entity);

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
  String8 text;             // Text content (passed through to world resources)
  VkrFontHandle font;       // Font handle (or invalid for default)
  float32_t font_size;      // Text-target pixels per em (0 = font default)
  Vec4 color;               // Text color RGBA
  uint32_t texture_width;   // Texture width (0 = auto)
  uint32_t texture_height;  // Texture height (0 = auto)
  float32_t uv_inset_px;    // Half-texel inset to avoid bleeding (0 = default)
  uint32_t align;           // VkrTextAlign of each line in the block
  float32_t line_spacing;   // Line height multiplier (0 = 1)
  float32_t letter_spacing; // Extra pixels between glyphs
} VkrSceneText3DConfig;

#define VKR_SCENE_TEXT3D_CONFIG_DEFAULT                                        \
  (VkrSceneText3DConfig){.text = {0},                                          \
                         .font = VKR_FONT_HANDLE_INVALID,                      \
                         .font_size = 32.0f,                                   \
                         .color = {1.0f, 1.0f, 1.0f, 1.0f},                    \
                         .texture_width = 512,                                 \
                         .texture_height = 128,                                \
                         .uv_inset_px = 0.5f,                                  \
                         .align = 0u,                                          \
                         .line_spacing = 1.0f,                                 \
                         .letter_spacing = 0.0f}

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
 * @brief Entity-local point centered just above a text's glyphs, where an
 * editor marker sits; the box center while the text has no geometry.
 */
Vec3 vkr_scene_text_marker_local(const VkrScene *scene, VkrEntityId entity);

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

/* Attaches a generated mesh of `submesh_count` submeshes to `entity` as its
 * runtime shape: transform, visibility, picking and Show-filter sync follow
 * the shape path. The mesh acquires its own references; the caller keeps and
 * later releases its own. The entity must have no generated mesh. */
bool8_t vkr_scene_attach_generated_mesh(VkrScene *scene, VkrEntityId entity,
                                        const struct VkrSubMeshDesc *submeshes,
                                        uint32_t submesh_count,
                                        VkrSceneError *out_error);
/* Replaces `entity`'s generated mesh with these submeshes in place, or
 * attaches one when it has none. A drawn mesh's change reaches only the
 * world-space boxes `change_min`/`change_max` (`change_count` of them; zero
 * means the whole mesh), so retained shadows elsewhere stay valid
 * (vkr_mesh_manager_replace). Fails, keeping the old mesh, while a submesh
 * still waits for its publications. */
bool8_t vkr_scene_replace_generated_mesh(
    VkrScene *scene, VkrEntityId entity, const struct VkrSubMeshDesc *submeshes,
    uint32_t submesh_count, const Vec3 *change_min, const Vec3 *change_max,
    uint32_t change_count, VkrSceneError *out_error);
/* Releases `entity`'s generated mesh, if any. */
void vkr_scene_detach_generated_mesh(VkrScene *scene, VkrEntityId entity);
/* Whether clip and trigger brushes draw (VkrScene.editor_volumes). */
void vkr_scene_set_editor_volumes(VkrScene *scene, bool8_t visible);

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

/** The document-stable id of `entity`: the one the editor gave it, or its
    document's. False for an entity without one, such as a script spawn or a
    node inside an imported model. */
bool8_t vkr_scene_entity_ref(const VkrScene *scene, VkrEntityId entity,
                             VkrEntityRef *out);
/** Gives an entity the editor created its document-stable id. */
bool8_t vkr_scene_set_entity_ref(VkrScene *scene, VkrEntityId entity,
                                 const VkrEntityRef *id);
/** The alive entity whose document-stable id is `id`, or invalid; scans the
    scene, so callers resolve once per publication. */
VkrEntityId vkr_scene_find_entity_ref(const VkrScene *scene,
                                      const VkrEntityRef *id);
/** Loads the material file `path` for generated geometry, adding one
    reference the caller releases when `*out_owned`; the default material,
    not owned, when it does not load. */
VkrMaterialHandle vkr_scene_material_load(VkrScene *scene, const char *path,
                                          bool8_t *out_owned);

/** Whether `entity` is a part of its parent rather than an object of its
    own: a brush face or an IO connection. Lists hide parts, and deleting
    the parent deletes its parts with it. */
bool8_t vkr_scene_entity_is_part(const VkrScene *scene, VkrEntityId entity);

/** A random version 4 UUID for a new entity. */
void vkr_scene_entity_ref_generate(VkrEntityRef *out);

/** Scene-owned storage for `count` document ids, replacing any previous
    ids; NULL on allocation failure. */
VkrEntityRef *vkr_scene_document_ids_reserve(VkrScene *scene, uint32_t count);

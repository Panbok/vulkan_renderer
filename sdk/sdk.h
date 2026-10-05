/**
 * @file sdk.h
 * @brief The VKR script SDK: everything a C script module may use.
 *
 * A module is game code built with a project. It reaches the engine only
 * through the functions below, which forward through a private table the
 * host fills, so the same sources link statically into an executable or
 * build as a shared library that imports no engine symbols. Only the
 * foundation's value types and inline math come from engine headers.
 *
 * Programming model
 * -----------------
 * - A component is plain data users attach to entities and edit in Details
 *   (VKR_COMPONENT). A behavior runs hooks for every entity that carries its
 *   component (VKR_BEHAVIOR). A module groups components and behaviors and
 *   may add its own hooks over module data (VKR_MODULE).
 * - Every hook returns void and receives a VkrCtx. Report a failure with
 *   vkr_fail(); the host stops the session after the hook returns.
 *
 * Instances and lifetimes
 * -----------------------
 * A script instance is a module running on one container: the root World or
 * a scene. A container-scoped module (the default) runs one instance per
 * container, so a zone scene's instance ends when the zone unloads. A
 * World-scoped module runs one instance for the whole game.
 *
 * - Temp: vkr_temp_alloc() memory lasts until the current hook returns.
 * - Scoped: what a behavior hook acquires (entities, characters, models,
 *   runtime state, render poses) belongs to that entity's behavior and is
 *   released when the entity stops carrying the component, the entity dies
 *   or the instance ends. vkr_destroy() and the other release calls end it
 *   earlier.
 *
 * A behavior's `destroy` hook runs when its entity is destroyed during a
 * session, before anything is torn down: the component, children and scoped
 * resources still exist. `stop` follows, then the scope is released. Ending
 * Play or unloading a container runs `stop` only.
 *
 * On Windows each module links its own static C runtime, with its own heap,
 * errno and FILE table: free what a module allocates in that module, and
 * pass no FILE or allocation across the SDK.
 * - Persistent: module data and what module hooks acquire live as long as
 *   the instance. Module data is zeroed when the instance starts.
 *
 * The host releases leftover acquisitions in reverse order, so a hook that
 * fails part way leaves nothing behind. Handles are generational: a call
 * with a released entity fails instead of touching another one.
 *
 * A spawn ends at the latest with its scope, earlier with its owner entity or
 * after its lifetime (VkrSpawnDesc), or by vkr_destroy(). Saving never writes
 * what scripts spawn.
 *
 * Hooks run on the thread that owns the scene. In fixed_update and
 * late_fixed_update, structural calls (spawning, destroying, models,
 * characters, bodies, shapes, names, parents, visibility, render poses,
 * components and runtime state) are queued and apply in order right after
 * the tick. A spawn's handle is valid at once: it reads as alive, calls on it
 * are queued too, and its transform and components appear after the tick.
 */
#pragma once

#include "core/vkr_entity_ref.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_quat.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/** Modules built for another SDK version are refused. */
#define VKR_SDK_VERSION 7u

/** Component types or behaviors one module may declare. */
#define VKR_SDK_EXPORT_MAX 64u

// =============================================================================
// Handles and value types
// =============================================================================

/** The calling script instance. Pass it to every SDK call; do not keep it
 * past the hook that received it. */
typedef struct VkrCtx VkrCtx;

/** An entity in any loaded container. Zero is no entity. */
typedef struct VkrEntity {
  uint64_t id;
} VkrEntity;

#define VKR_ENTITY_NONE ((VkrEntity){0})

static inline bool8_t vkr_entity_valid(VkrEntity entity) {
  return entity.id != 0u;
}

static inline bool8_t vkr_entity_equal(VkrEntity a, VkrEntity b) {
  return a.id == b.id;
}

/** A loaded container: the root World or a scene. Zero is the calling
 * instance's own container. */
typedef struct VkrContainer {
  uint32_t id;
} VkrContainer;

#define VKR_CONTAINER_SELF ((VkrContainer){0})

typedef enum VkrScope {
  /** One instance per container; it ends when its container unloads. */
  VKR_SCOPE_CONTAINER = 0,
  /** One instance on the root World, for the whole game. */
  VKR_SCOPE_WORLD = 1,
} VkrScope;

typedef enum VkrLogLevel {
  VKR_LOG_ERROR = 1,
  VKR_LOG_WARN = 2,
  VKR_LOG_INFO = 3,
  VKR_LOG_DEBUG = 4,
} VkrLogLevel;

/** A local transform: translation, rotation and scale. A zero rotation
 * means identity and a zero scale means one, so `{.position = p}` places an
 * entity at `p`. */
typedef struct VkrTRS {
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
} VkrTRS;

typedef struct VkrSpawnDesc {
  /** Optional display name. */
  const char *name;
  /** Optional parent in the same container. */
  VkrEntity parent;
  VkrTRS transform;
  /** Target container; VKR_CONTAINER_SELF for the instance's own. */
  VkrContainer container;
  /** Optional owner in any container: the spawn is destroyed with it. */
  VkrEntity owner;
  /** Simulated seconds after which the spawn is destroyed; zero for none.
   * The clock stops while the simulation pauses. */
  float32_t lifetime;
} VkrSpawnDesc;

/** A coloured box primitive with the given size in metres. */
typedef struct VkrShapeDesc {
  Vec3 size;
  Vec4 color;
} VkrShapeDesc;

/** Where an entity's spawned model stands (`vkr_model_state`). */
typedef enum VkrModelState {
  VKR_MODEL_NONE = 0,
  /** Requested; its mesh and animation are loading. */
  VKR_MODEL_LOADING,
  /** Its nodes exist, and its animation plays when it has one. */
  VKR_MODEL_READY,
  /** A load failed; nothing of it remains. */
  VKR_MODEL_FAILED,
} VkrModelState;

/** Work running on a worker thread (`vkr_task_run`); zero is none. */
typedef struct VkrTask {
  uint64_t id;
} VkrTask;

/** A task's function: reads and writes only its own copy of the data. */
typedef void (*VkrTaskFn)(void *data);

/** Bytes of data one task may carry. */
#define VKR_TASK_DATA_MAX (64u * 1024u)

// =============================================================================
// Components
// =============================================================================

typedef enum VkrFieldKind {
  VKR_FIELD_KIND_BOOL = 0,
  VKR_FIELD_KIND_I32,
  VKR_FIELD_KIND_U32,
  VKR_FIELD_KIND_F32,
  /** Radians stored; Details shows degrees. */
  VKR_FIELD_KIND_ANGLE,
  VKR_FIELD_KIND_VEC2,
  VKR_FIELD_KIND_VEC3,
  VKR_FIELD_KIND_VEC4,
  VKR_FIELD_KIND_QUAT,
  VKR_FIELD_KIND_COLOR,
  VKR_FIELD_KIND_DIRECTION,
  /** An int32_t index into `names`. */
  VKR_FIELD_KIND_ENUM,
  /** A VkrEntityRef to an object of the entity's own container, picked in
   * Details; vkr_resolve() finds the entity during a session. */
  VKR_FIELD_KIND_ENTITY,
} VkrFieldKind;

typedef enum VkrFieldFlags {
  VKR_FIELD_FLAG_NONE = 0u,
  VKR_FIELD_FLAG_READ_ONLY = 1u << 0,
  VKR_FIELD_FLAG_HIDDEN = 1u << 1,
  /** Shown but never saved. */
  VKR_FIELD_FLAG_TRANSIENT = 1u << 2,
  VKR_FIELD_FLAG_SLIDER = 1u << 3,
} VkrFieldFlags;

/** One edited and saved member of a component. */
typedef struct VkrFieldDesc {
  /** Stable snake_case key in documents and Cmd paths. */
  const char *name;
  const char *label;
  const char *tooltip;
  /** Details sub-heading starting at this field; NULL continues. */
  const char *group;
  /** Display suffix such as "m/s"; NULL for none. */
  const char *unit;
  /** Shown instead of a zero value with a special meaning; NULL for none. */
  const char *zero_label;
  /** ENUM value names, NULL-terminated. */
  const char *const *names;
  uint32_t offset;
  VkrFieldKind kind;
  uint32_t flags; /**< VkrFieldFlags. */
  /** Inclusive bounds in stored units (degrees for ANGLE); used when
   * min < max. */
  float32_t min;
  float32_t max;
  /** Drag step in display units; zero uses a default. */
  float32_t step;
} VkrFieldDesc;

/** A component type. VKR_COMPONENT builds one; a hand-written descriptor
 * must live in writable static storage for the whole process. */
typedef struct VkrComponentDesc {
  /** Stable snake_case type name, unique across modules. */
  const char *name;
  const char *label;
  const VkrFieldDesc *fields;
  uint32_t field_count;
  uint32_t size;
  uint32_t align;
  /** Fills a value with its defaults; zero when NULL. */
  void (*defaults)(void *value);
  /** Host-owned; leave zero. */
  const void *host_binding;
} VkrComponentDesc;

// =============================================================================
// Input, simulation and physics values
// =============================================================================

typedef enum VkrInputKind {
  VKR_INPUT_KEY = 0,
  VKR_INPUT_BUTTON,
  /** Captured mouse motion. */
  VKR_INPUT_LOOK,
} VkrInputKind;

typedef enum VkrMouseButton {
  VKR_MOUSE_LEFT = 0,
  VKR_MOUSE_RIGHT,
  VKR_MOUSE_MIDDLE,
} VkrMouseButton;

/** Key codes. */
typedef enum VkrKey {
  VKR_KEY_BACKSPACE = 0x08,
  VKR_KEY_TAB = 0x09,
  VKR_KEY_ENTER = 0x0D,
  VKR_KEY_SHIFT = 0x10,
  VKR_KEY_CONTROL = 0x11,
  VKR_KEY_ESCAPE = 0x1B,
  VKR_KEY_SPACE = 0x20,
  VKR_KEY_PAGE_UP = 0x21,
  VKR_KEY_PAGE_DOWN = 0x22,
  VKR_KEY_END = 0x23,
  VKR_KEY_HOME = 0x24,
  VKR_KEY_LEFT = 0x25,
  VKR_KEY_UP = 0x26,
  VKR_KEY_RIGHT = 0x27,
  VKR_KEY_DOWN = 0x28,
  VKR_KEY_INSERT = 0x2D,
  VKR_KEY_DELETE = 0x2E,
  VKR_KEY_0 = 0x30,
  VKR_KEY_1,
  VKR_KEY_2,
  VKR_KEY_3,
  VKR_KEY_4,
  VKR_KEY_5,
  VKR_KEY_6,
  VKR_KEY_7,
  VKR_KEY_8,
  VKR_KEY_9,
  VKR_KEY_A = 0x41,
  VKR_KEY_B,
  VKR_KEY_C,
  VKR_KEY_D,
  VKR_KEY_E,
  VKR_KEY_F,
  VKR_KEY_G,
  VKR_KEY_H,
  VKR_KEY_I,
  VKR_KEY_J,
  VKR_KEY_K,
  VKR_KEY_L,
  VKR_KEY_M,
  VKR_KEY_N,
  VKR_KEY_O,
  VKR_KEY_P,
  VKR_KEY_Q,
  VKR_KEY_R,
  VKR_KEY_S,
  VKR_KEY_T,
  VKR_KEY_U,
  VKR_KEY_V,
  VKR_KEY_W,
  VKR_KEY_X,
  VKR_KEY_Y,
  VKR_KEY_Z,
  VKR_KEY_F1 = 0x70,
  VKR_KEY_F2,
  VKR_KEY_F3,
  VKR_KEY_F4,
  VKR_KEY_F5,
  VKR_KEY_F6,
  VKR_KEY_F7,
  VKR_KEY_F8,
  VKR_KEY_F9,
  VKR_KEY_F10,
  VKR_KEY_F11,
  VKR_KEY_F12,
  VKR_KEY_LSHIFT = 0xA0,
  VKR_KEY_RSHIFT = 0xA1,
  VKR_KEY_LCONTROL = 0xA2,
  VKR_KEY_RCONTROL = 0xA3,
  VKR_KEY_LALT = 0xA4,
  VKR_KEY_RALT = 0xA5,
  VKR_KEY_PLUS = 0xBB,
  VKR_KEY_COMMA = 0xBC,
  VKR_KEY_MINUS = 0xBD,
  VKR_KEY_PERIOD = 0xBE,
  VKR_KEY_SLASH = 0xBF,
} VkrKey;

/** One ordered input transition, delivered while the session runs. */
typedef struct VkrInputEvent {
  /** Monotonic seconds, comparable with vkr_time(). */
  float64_t time;
  VkrInputKind kind;
  /** VkrKey for KEY, VkrMouseButton for BUTTON. */
  uint32_t code;
  bool8_t pressed;
  /** LOOK motion; positive dy is upward. */
  float64_t dx;
  float64_t dy;
} VkrInputEvent;

typedef enum VkrMotion {
  VKR_MOTION_STATIC = 0,
  VKR_MOTION_KINEMATIC,
  VKR_MOTION_DYNAMIC,
} VkrMotion;

typedef enum VkrBodyShape {
  VKR_BODY_BOX = 0,
  VKR_BODY_SPHERE,
  VKR_BODY_CAPSULE,
} VkrBodyShape;

/** A rigid body with one collider at the entity's origin. Start from
 * vkr_body_default(). */
typedef struct VkrBodyDesc {
  VkrMotion motion;
  VkrBodyShape shape;
  Vec3 half_extent;
  float32_t radius;
  /** Capsule cylinder half height along Y. */
  float32_t half_height;
  float32_t mass;
  float32_t friction;
  float32_t restitution;
  float32_t gravity_factor;
  bool8_t sensor;
  uint16_t layer;
  uint16_t mask;
} VkrBodyDesc;

typedef struct VkrQueryFilter {
  /** Collision layers the query hits. */
  uint16_t mask;
  bool8_t include_sensors;
  const VkrEntity *ignored;
  uint32_t ignored_count;
} VkrQueryFilter;

typedef struct VkrRayHit {
  VkrEntity entity;
  VkrEntity collider;
  /** Fraction of the query displacement where it hit. */
  float32_t fraction;
  Vec3 position;
  Vec3 normal;
} VkrRayHit;

/** A capsule character. Start from vkr_character_default(). */
typedef struct VkrCharacterDesc {
  float32_t radius;
  float32_t half_height;
  float32_t max_slope_radians;
  float32_t step_up;
  float32_t step_down;
  float32_t mass;
  float32_t max_strength;
  uint16_t layer;
  uint16_t mask;
} VkrCharacterDesc;

typedef enum VkrGround {
  VKR_GROUND_ON_GROUND = 0,
  VKR_GROUND_STEEP,
  VKR_GROUND_UNSUPPORTED,
  VKR_GROUND_IN_AIR,
} VkrGround;

typedef struct VkrCharacterState {
  Vec3 foot;
  Vec3 velocity;
  Vec3 ground_velocity;
  Vec3 ground_normal;
  VkrEntity ground_entity;
  VkrGround ground;
  bool8_t crouched;
} VkrCharacterState;

typedef struct VkrCharacterMove {
  /** Desired velocity before gravity, including kept vertical speed. */
  Vec3 velocity;
  /** Added once as gravity * dt. */
  Vec3 gravity;
  float32_t dt;
  bool8_t crouch;
} VkrCharacterMove;

/** One weighted clip sample on a caller-owned clock. */
typedef struct VkrAnimSample {
  uint32_t clip;
  float64_t time;
  float32_t weight;
} VkrAnimSample;

// =============================================================================
// Entity IO
// =============================================================================

/** What an output carries or an input takes: nothing, or one value of a
 * field kind. VKR_IO_ENTITY carries a live entity. */
typedef enum VkrIoKind {
  VKR_IO_NONE = 0,
  VKR_IO_BOOL,
  VKR_IO_I32,
  VKR_IO_U32,
  VKR_IO_F32,
  VKR_IO_ANGLE,
  VKR_IO_VEC2,
  VKR_IO_VEC3,
  VKR_IO_VEC4,
  VKR_IO_QUAT,
  VKR_IO_COLOR,
  VKR_IO_DIRECTION,
  VKR_IO_ENUM,
  VKR_IO_ENTITY,
} VkrIoKind;

/** One value an output fired or an input received. */
typedef struct VkrIoValue {
  VkrIoKind kind;
  union {
    bool8_t boolean;
    int32_t i32;
    uint32_t u32;
    float32_t f32;
    /* VEC2 to QUAT, COLOR and DIRECTION. */
    Vec4 vector;
    VkrEntity entity;
  };
} VkrIoValue;

/** An input's handler: `component` is the receiving behavior's component. */
typedef void (*VkrIoInputFn)(VkrCtx *ctx, VkrEntity self, void *component,
                             const VkrIoValue *value);

/** One output (without handler) or input of a behavior. Lists end with a
 * NULL name; VKR_OUTPUTS and VKR_INPUTS build them. */
typedef struct VkrIoPortDesc {
  const char *name;
  const char *label;
  VkrIoKind kind;
  VkrIoInputFn handler;
} VkrIoPortDesc;

/** An input resolved by vkr_io_input(); zero is none. */
typedef struct VkrIoInput {
  uint32_t id;
} VkrIoInput;

/** Runtime-only per-entity data, never saved or edited. */
typedef struct VkrStateType {
  uint32_t id;
} VkrStateType;

#define VKR_CLIP_NONE UINT32_MAX
/** Samples one vkr_anim_blend() call may mix. */
#define VKR_ANIM_BLEND_MAX 32u

// =============================================================================
// Module and behavior descriptions (built by VKR_MODULE and VKR_BEHAVIOR)
// =============================================================================

typedef struct VkrBehaviorDesc {
  VkrComponentDesc *component;
  void (*start)(VkrCtx *ctx, VkrEntity self, void *component);
  void (*stop)(VkrCtx *ctx, VkrEntity self, void *component);
  /** The entity is being destroyed; everything it holds still exists. */
  void (*destroy)(VkrCtx *ctx, VkrEntity self, void *component);
  void (*update)(VkrCtx *ctx, VkrEntity self, void *component, float32_t dt);
  void (*late_update)(VkrCtx *ctx, VkrEntity self, void *component,
                      float32_t dt);
  void (*fixed_update)(VkrCtx *ctx, VkrEntity self, void *component);
  void (*late_fixed_update)(VkrCtx *ctx, VkrEntity self, void *component);
  /** Something entered or left a sensor on this entity, or this entity
   * entered or left a sensor: `other` is the other side. */
  void (*trigger_enter)(VkrCtx *ctx, VkrEntity self, void *component,
                        VkrEntity other);
  void (*trigger_exit)(VkrCtx *ctx, VkrEntity self, void *component,
                       VkrEntity other);
  /** Outputs its code fires and inputs connections may call. */
  const VkrIoPortDesc *outputs;
  const VkrIoPortDesc *inputs;
} VkrBehaviorDesc;

typedef struct VkrModuleDesc {
  uint32_t sdk_version;
  const char *name;
  VkrScope scope;
  uint32_t data_size;
  uint32_t data_align;
  /** Bump when the data struct changes so hot reload restarts instead of
   * running new code over old bytes. */
  uint32_t data_version;
  VkrComponentDesc *components[VKR_SDK_EXPORT_MAX];
  uint32_t component_count;
  const VkrBehaviorDesc *behaviors[VKR_SDK_EXPORT_MAX];
  uint32_t behavior_count;
  /** The instance started; acquire what the whole run needs. */
  void (*start)(VkrCtx *ctx, void *data);
  /** The instance ends; its acquisitions are released after this. */
  void (*stop)(VkrCtx *ctx, void *data);
  /** Every frame before the scene advances. */
  void (*update)(VkrCtx *ctx, void *data, float32_t dt);
  /** Every frame after the scene advanced: cameras, HUD, presentation. */
  void (*late_update)(VkrCtx *ctx, void *data, float32_t dt);
  /** Every fixed tick before physics. */
  void (*fixed_update)(VkrCtx *ctx, void *data);
  /** Every fixed tick after physics. */
  void (*late_fixed_update)(VkrCtx *ctx, void *data);
  /** Every ordered input transition while the session runs. */
  void (*input)(VkrCtx *ctx, void *data, const VkrInputEvent *event);
} VkrModuleDesc;

/** A module's one entry point, `vkr_module_<Name>`. Returns its description,
 * or NULL when `sdk_version` differs from the version it was built for. */
typedef const VkrModuleDesc *(*VkrModuleEntry)(uint32_t sdk_version);

/** A project library's entry, `vkr_project_modules`, which Bakery generates:
 * writes up to `capacity` module entries and returns how many it lists. */
typedef uint32_t (*VkrProjectEntry)(VkrModuleEntry *entries, uint32_t capacity);

// =============================================================================
// Private host table: call the vkr_* functions below instead
// =============================================================================

typedef struct VkrSdkTable {
  /* Diagnostics and control. */
  void (*log)(VkrCtx *ctx, VkrLogLevel level, const char *message);
  void (*fail)(VkrCtx *ctx, const char *message);
  const char *(*last_error)(VkrCtx *ctx);
  void (*disable)(VkrCtx *ctx);
  void *(*temp_alloc)(VkrCtx *ctx, uint64_t size, uint64_t align);
  bool8_t (*option)(VkrCtx *ctx, const char *name);

  /* Containers. */
  VkrContainer (*container_self)(VkrCtx *ctx);
  VkrContainer (*container_active)(VkrCtx *ctx);
  VkrContainer (*container_world)(VkrCtx *ctx);
  VkrContainer (*container_of)(VkrCtx *ctx, VkrEntity entity);

  /* Entities. */
  VkrEntity (*spawn)(VkrCtx *ctx, const VkrSpawnDesc *desc);
  void (*destroy)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*alive)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*set_name)(VkrCtx *ctx, VkrEntity entity, const char *name);
  bool8_t (*set_parent)(VkrCtx *ctx, VkrEntity entity, VkrEntity parent);
  bool8_t (*set_visible)(VkrCtx *ctx, VkrEntity entity, bool8_t visible);
  bool8_t (*visible)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*set_transform)(VkrCtx *ctx, VkrEntity entity,
                           const VkrTRS *transform);
  bool8_t (*world_matrix)(VkrCtx *ctx, VkrEntity entity, Mat4 *out);
  bool8_t (*set_render_pose)(VkrCtx *ctx, VkrEntity entity, const Mat4 *world);
  bool8_t (*has_visual)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*set_shape)(VkrCtx *ctx, VkrEntity entity,
                       const VkrShapeDesc *shape);
  bool8_t (*spawn_model)(VkrCtx *ctx, VkrEntity entity, const char *mesh,
                         const char *animation);
  void (*despawn_model)(VkrCtx *ctx, VkrEntity entity);
  VkrModelState (*model_state)(VkrCtx *ctx, VkrEntity entity);

  /* Components and runtime state. */
  const VkrComponentDesc *(*component_named)(VkrCtx *ctx, const char *name);
  const void *(*component_get)(VkrCtx *ctx, VkrEntity entity,
                               const VkrComponentDesc *type);
  void *(*component_get_mut)(VkrCtx *ctx, VkrEntity entity,
                             const VkrComponentDesc *type);
  bool8_t (*component_set)(VkrCtx *ctx, VkrEntity entity,
                           const VkrComponentDesc *type, const void *value);
  uint32_t (*component_find)(VkrCtx *ctx, VkrContainer container,
                             const VkrComponentDesc *type, VkrEntity *out,
                             uint32_t capacity);
  VkrStateType (*state_type)(VkrCtx *ctx, const char *name, uint32_t size,
                             uint32_t align);
  void *(*state_add)(VkrCtx *ctx, VkrEntity entity, VkrStateType type,
                     const void *value);
  void *(*state_get)(VkrCtx *ctx, VkrEntity entity, VkrStateType type);
  void (*state_remove)(VkrCtx *ctx, VkrEntity entity, VkrStateType type);
  bool8_t (*player_start)(VkrCtx *ctx, Mat4 *out_world);

  /* Clock and frame. */
  float64_t (*time)(VkrCtx *ctx);
  float64_t (*fixed_dt)(VkrCtx *ctx);
  uint64_t (*ticks)(VkrCtx *ctx);
  float64_t (*sim_time)(VkrCtx *ctx);
  float64_t (*sim_debt)(VkrCtx *ctx);
  bool8_t (*simulating)(VkrCtx *ctx);
  bool8_t (*paused)(VkrCtx *ctx);
  bool8_t (*playing)(VkrCtx *ctx);
  void (*set_time_step)(VkrCtx *ctx, float64_t dt);
  Vec3 (*gravity)(VkrCtx *ctx);
  bool8_t (*input_focused)(VkrCtx *ctx);
  bool8_t (*key_down)(VkrCtx *ctx, VkrKey key);
  bool8_t (*camera_available)(VkrCtx *ctx);
  void (*set_camera)(VkrCtx *ctx, Vec3 position, float32_t yaw_degrees,
                     float32_t pitch_degrees);
  void (*hud_text)(VkrCtx *ctx, const char *text);

  /* Physics. */
  VkrBodyDesc (*body_default)(VkrCtx *ctx);
  bool8_t (*set_body)(VkrCtx *ctx, VkrEntity entity, const VkrBodyDesc *body);
  bool8_t (*impulse)(VkrCtx *ctx, VkrEntity entity, Vec3 impulse,
                     const Vec3 *world_point);
  bool8_t (*raycast)(VkrCtx *ctx, Vec3 origin, Vec3 displacement,
                     const VkrQueryFilter *filter, VkrRayHit *hit);
  bool8_t (*sweep_sphere)(VkrCtx *ctx, Vec3 origin, Vec3 displacement,
                          float32_t radius, const VkrQueryFilter *filter,
                          VkrRayHit *hit);
  VkrCharacterDesc (*character_default)(VkrCtx *ctx);
  bool8_t (*character_create)(VkrCtx *ctx, VkrEntity entity,
                              const VkrCharacterDesc *desc,
                              const Vec3 *spawn_foot);
  void (*character_destroy)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*character_state)(VkrCtx *ctx, VkrEntity entity,
                             VkrCharacterState *out);
  bool8_t (*character_move)(VkrCtx *ctx, VkrEntity entity,
                            const VkrCharacterMove *move,
                            VkrCharacterState *out);

  /* Animation of an entity's animated model. */
  uint64_t (*anim_id)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*anim_has_graph)(VkrCtx *ctx, VkrEntity entity);
  uint32_t (*anim_clip_count)(VkrCtx *ctx, VkrEntity entity);
  uint32_t (*anim_clip_find)(VkrCtx *ctx, VkrEntity entity, const char *name);
  float32_t (*anim_clip_duration)(VkrCtx *ctx, VkrEntity entity, uint32_t clip);
  uint32_t (*anim_bone_count)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*anim_bone_pose)(VkrCtx *ctx, VkrEntity entity, uint32_t bone,
                            Mat4 *out);
  bool8_t (*anim_play)(VkrCtx *ctx, VkrEntity entity, uint32_t clip,
                       bool8_t loop);
  bool8_t (*anim_crossfade)(VkrCtx *ctx, VkrEntity entity, uint32_t clip,
                            bool8_t loop, float64_t duration);
  void (*anim_set_playing)(VkrCtx *ctx, VkrEntity entity, bool8_t playing);
  bool8_t (*anim_set_rate)(VkrCtx *ctx, VkrEntity entity, float64_t rate);
  float64_t (*anim_rate)(VkrCtx *ctx, VkrEntity entity);
  float64_t (*anim_time)(VkrCtx *ctx, VkrEntity entity);
  float64_t (*anim_duration)(VkrCtx *ctx, VkrEntity entity);
  bool8_t (*anim_blend)(VkrCtx *ctx, VkrEntity entity,
                        const VkrAnimSample *samples, uint32_t count,
                        bool8_t discontinuity);

  /* Tasks. */
  VkrTask (*task_run)(VkrCtx *ctx, VkrTaskFn fn, const void *data,
                      uint32_t size);
  bool8_t (*task_take)(VkrCtx *ctx, VkrTask task, void *out, uint32_t size,
                       bool8_t wait);

  /* Entity references. */
  VkrEntity (*resolve)(VkrCtx *ctx, VkrEntity owner, VkrEntityRef ref);

  /* Entity IO. */
  bool8_t (*io_fire)(VkrCtx *ctx, VkrEntity self, const VkrComponentDesc *type,
                     uint32_t output, const VkrIoValue *value);
  VkrIoInput (*io_input)(VkrCtx *ctx, const VkrComponentDesc *type,
                         const char *name);
  bool8_t (*io_send)(VkrCtx *ctx, VkrEntity target, VkrIoInput input,
                     const VkrIoValue *value);

  /* Time of day. */
  float64_t (*time_of_day)(VkrCtx *ctx);
  bool8_t (*set_time_of_day)(VkrCtx *ctx, float64_t hour);
  bool8_t (*set_light_group)(VkrCtx *ctx, const char *name,
                             float32_t intensity);
} VkrSdkTable;

/* The host's context starts with this member. */
struct VkrCtx {
  const VkrSdkTable *sdk;
};

// =============================================================================
// Diagnostics and control
// =============================================================================

#define VKR_SDK_MESSAGE_CAPACITY 512u

/** Logs one formatted message under the module's name. */
static inline void vkr_log(VkrCtx *ctx, VkrLogLevel level, const char *format,
                           ...) {
  char message[VKR_SDK_MESSAGE_CAPACITY];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  ctx->sdk->log(ctx, level, message);
}

/** Fails the current hook. The host reports "<module>: <message>" and stops
 * the session after the hook returns: a failed start releases what it
 * acquired, and a failed tick faults the simulation. */
static inline void vkr_fail(VkrCtx *ctx, const char *format, ...) {
  char message[VKR_SDK_MESSAGE_CAPACITY];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  ctx->sdk->fail(ctx, message);
}

/** Why the last failed SDK call failed; valid until the next call. */
static inline const char *vkr_last_error(VkrCtx *ctx) {
  return ctx->sdk->last_error(ctx);
}

/** This instance has nothing to do: it receives no further hooks until its
 * container starts again. Its acquisitions stay until the instance ends. */
static inline void vkr_disable(VkrCtx *ctx) { ctx->sdk->disable(ctx); }

/** Memory that lasts until the current hook returns; NULL when exhausted. */
static inline void *vkr_temp_alloc(VkrCtx *ctx, uint64_t size, uint64_t align) {
  return ctx->sdk->temp_alloc(ctx, size, align);
}

/** `count` zeroed values of `Type` lasting until the hook returns. */
#define vkr_temp(ctx, Type, count)                                             \
  ((Type *)vkr_temp_alloc((ctx), sizeof(Type) * (uint64_t)(count),             \
                          AlignOf(Type)))

/** A formatted string lasting until the hook returns; NULL when exhausted. */
static inline const char *vkr_format(VkrCtx *ctx, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(NULL, 0, format, args);
  va_end(args);
  char *text = length >= 0
                   ? (char *)vkr_temp_alloc(ctx, (uint64_t)length + 1u, 1u)
                   : NULL;
  if (text) {
    va_start(args, format);
    (void)vsnprintf(text, (size_t)length + 1u, format, args);
    va_end(args);
  }
  return text;
}

/** A launch option, such as "gameplay" for the sample training content. */
static inline bool8_t vkr_option(VkrCtx *ctx, const char *name) {
  return ctx->sdk->option(ctx, name);
}

// =============================================================================
// Containers
// =============================================================================

static inline VkrContainer vkr_container_self(VkrCtx *ctx) {
  return ctx->sdk->container_self(ctx);
}

/** The container being played: the primary scene, or the World when it
 * plays alone. */
static inline VkrContainer vkr_container_active(VkrCtx *ctx) {
  return ctx->sdk->container_active(ctx);
}

/** The root World, or the active container when no World is loaded. */
static inline VkrContainer vkr_container_world(VkrCtx *ctx) {
  return ctx->sdk->container_world(ctx);
}

static inline VkrContainer vkr_container_of(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->container_of(ctx, entity);
}

// =============================================================================
// Entities
// =============================================================================

/** Creates an entity owned by the calling scope; see the file comment. In a
 * fixed update the entity appears right after the tick. NONE on failure. */
static inline VkrEntity vkr_spawn(VkrCtx *ctx, const VkrSpawnDesc *desc) {
  return ctx->sdk->spawn(ctx, desc);
}

/** Destroys an entity and its children, running their destroy hooks; in a
 * fixed update, right after the tick. */
static inline void vkr_destroy(VkrCtx *ctx, VkrEntity entity) {
  ctx->sdk->destroy(ctx, entity);
}

static inline bool8_t vkr_alive(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->alive(ctx, entity);
}

static inline bool8_t vkr_set_name(VkrCtx *ctx, VkrEntity entity,
                                   const char *name) {
  return ctx->sdk->set_name(ctx, entity, name);
}

static inline bool8_t vkr_set_parent(VkrCtx *ctx, VkrEntity entity,
                                     VkrEntity parent) {
  return ctx->sdk->set_parent(ctx, entity, parent);
}

static inline bool8_t vkr_set_visible(VkrCtx *ctx, VkrEntity entity,
                                      bool8_t visible) {
  return ctx->sdk->set_visible(ctx, entity, visible);
}

/** Effective visibility through visible parents. */
static inline bool8_t vkr_visible(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->visible(ctx, entity);
}

static inline bool8_t vkr_set_transform(VkrCtx *ctx, VkrEntity entity,
                                        const VkrTRS *transform) {
  return ctx->sdk->set_transform(ctx, entity, transform);
}

/** The entity's world matrix, current with every earlier edit. */
static inline bool8_t vkr_world_matrix(VkrCtx *ctx, VkrEntity entity,
                                       Mat4 *out) {
  return ctx->sdk->world_matrix(ctx, entity, out);
}

static inline Vec3 vkr_world_position(VkrCtx *ctx, VkrEntity entity) {
  Mat4 world;
  return ctx->sdk->world_matrix(ctx, entity, &world)
             ? vec3_new(world.elements[12], world.elements[13],
                        world.elements[14])
             : vec3_zero();
}

/** Draws the entity at `world` instead of its transform or physics pose,
 * until called with NULL or the calling scope ends. The authored transform
 * stays unchanged. */
static inline bool8_t vkr_set_render_pose(VkrCtx *ctx, VkrEntity entity,
                                          const Mat4 *world) {
  return ctx->sdk->set_render_pose(ctx, entity, world);
}

/** The entity or a descendant renders a mesh or a shape. */
static inline bool8_t vkr_has_visual(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->has_visual(ctx, entity);
}

static inline bool8_t vkr_set_shape(VkrCtx *ctx, VkrEntity entity,
                                    const VkrShapeDesc *shape) {
  return ctx->sdk->set_shape(ctx, entity, shape);
}

/**
 * Spawns a cooked model (a .vkb, and an optional .vka of the same source)
 * under `entity`, which must carry no mesh. The files load on the engine's
 * workers; the model's nodes appear at a later frame start, when
 * `vkr_model_state` turns READY, and until then `entity` has no visual or
 * animation. Owned by the calling scope; releasing it while it loads cancels
 * the loads. False when the request cannot start.
 */
static inline bool8_t vkr_spawn_model(VkrCtx *ctx, VkrEntity entity,
                                      const char *mesh, const char *animation) {
  return ctx->sdk->spawn_model(ctx, entity, mesh, animation);
}

static inline void vkr_despawn_model(VkrCtx *ctx, VkrEntity entity) {
  ctx->sdk->despawn_model(ctx, entity);
}

/** Where `entity`'s spawned model stands; NONE for an entity without one,
 * including authored models. */
static inline VkrModelState vkr_model_state(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->model_state(ctx, entity);
}

// =============================================================================
// Components and runtime state
// =============================================================================

/** A component type registered by name: an engine type such as
 * "player_start", or another module's. NULL when unknown. Look it up once,
 * outside per-frame loops. */
static inline const VkrComponentDesc *vkr_component_named(VkrCtx *ctx,
                                                          const char *name) {
  return ctx->sdk->component_named(ctx, name);
}

/** The entity's component, or NULL. Valid until the hook returns. */
static inline const void *vkr_component_get(VkrCtx *ctx, VkrEntity entity,
                                            const VkrComponentDesc *type) {
  return ctx->sdk->component_get(ctx, entity, type);
}

/** Mutable access to the entity's component, or NULL. Writes change the
 * authored values. Valid until the hook returns. */
static inline void *vkr_component_get_mut(VkrCtx *ctx, VkrEntity entity,
                                          const VkrComponentDesc *type) {
  return ctx->sdk->component_get_mut(ctx, entity, type);
}

/** Stores `value`, adding the component when absent; NULL `value` stores
 * the type's defaults. */
static inline bool8_t vkr_component_set(VkrCtx *ctx, VkrEntity entity,
                                        const VkrComponentDesc *type,
                                        const void *value) {
  return ctx->sdk->component_set(ctx, entity, type, value);
}

/** Writes up to `capacity` entities of the instance's container carrying
 * `type`, in entity order, and returns how many carry it. */
static inline uint32_t vkr_find(VkrCtx *ctx, const VkrComponentDesc *type,
                                VkrEntity *out, uint32_t capacity) {
  return ctx->sdk->component_find(ctx, VKR_CONTAINER_SELF, type, out, capacity);
}

/** vkr_find() in another container. */
static inline uint32_t vkr_find_in(VkrCtx *ctx, VkrContainer container,
                                   const VkrComponentDesc *type, VkrEntity *out,
                                   uint32_t capacity) {
  return ctx->sdk->component_find(ctx, container, type, out, capacity);
}

/** The entity an ENTITY field of `owner` names, in `owner`'s container, or
 * none. It searches the container, so resolve once, in `start`. */
static inline VkrEntity vkr_resolve(VkrCtx *ctx, VkrEntity owner,
                                    VkrEntityRef ref) {
  return ctx->sdk->resolve(ctx, owner, ref);
}

/** Fires output `output` (VKR_OUTPUT_ID) of `self`'s component `type`:
 * every connection from it delivers to its target through the router after
 * the current tick, or at once outside one. `value` may be NULL. */
static inline bool8_t vkr_io_fire(VkrCtx *ctx, VkrEntity self,
                                  const VkrComponentDesc *type, uint32_t output,
                                  const VkrIoValue *value) {
  return ctx->sdk->io_fire(ctx, self, type, output, value);
}

/** Input `name` of component `type`, or of every entity (`show`, `hide`,
 * `destroy`) when `type` is NULL. Resolve once, in `start`. */
static inline VkrIoInput vkr_io_input(VkrCtx *ctx, const VkrComponentDesc *type,
                                      const char *name) {
  return ctx->sdk->io_input(ctx, type, name);
}

/** Calls `input` on `target` through the router, as a connection would, as
 * Source's ent_fire does. */
static inline bool8_t vkr_io_send(VkrCtx *ctx, VkrEntity target,
                                  VkrIoInput input, const VkrIoValue *value) {
  return ctx->sdk->io_send(ctx, target, input, value);
}

/** A runtime-only per-entity data type, registered once by name. */
static inline VkrStateType vkr_state_type(VkrCtx *ctx, const char *name,
                                          uint32_t size, uint32_t align) {
  return ctx->sdk->state_type(ctx, name, size, align);
}

#define VKR_STATE_TYPE(ctx, Type)                                              \
  vkr_state_type((ctx), #Type, (uint32_t)sizeof(Type), (uint32_t)AlignOf(Type))

/** Adds the entity's state, owned by the calling scope; NULL `value` zeroes
 * it. Returns the stored state, valid until the hook returns, or NULL. In a
 * fixed update it returns the queued value, which the tick's end stores. */
static inline void *vkr_state_add(VkrCtx *ctx, VkrEntity entity,
                                  VkrStateType type, const void *value) {
  return ctx->sdk->state_add(ctx, entity, type, value);
}

static inline void *vkr_state_get(VkrCtx *ctx, VkrEntity entity,
                                  VkrStateType type) {
  return ctx->sdk->state_get(ctx, entity, type);
}

static inline void vkr_state_remove(VkrCtx *ctx, VkrEntity entity,
                                    VkrStateType type) {
  ctx->sdk->state_remove(ctx, entity, type);
}

/** World matrix of the Player Start the active container resolves: its own
 * first enabled one, then the root World's. False when neither has one. */
static inline bool8_t vkr_player_start(VkrCtx *ctx, Mat4 *out_world) {
  return ctx->sdk->player_start(ctx, out_world);
}

// =============================================================================
// Clock and frame
// =============================================================================

/** Monotonic seconds. In update and late_update, the frame's time measured
 * after the input pump; input events carry their own. */
static inline float64_t vkr_time(VkrCtx *ctx) { return ctx->sdk->time(ctx); }

/** Seconds per fixed tick. */
static inline float64_t vkr_fixed_dt(VkrCtx *ctx) {
  return ctx->sdk->fixed_dt(ctx);
}

/** Fixed ticks completed since the simulation reset. */
static inline uint64_t vkr_ticks(VkrCtx *ctx) { return ctx->sdk->ticks(ctx); }

/** Simulated seconds completed since the reset. */
static inline float64_t vkr_sim_time(VkrCtx *ctx) {
  return ctx->sdk->sim_time(ctx);
}

/** Elapsed seconds not yet simulated, below one tick. */
static inline float64_t vkr_sim_debt(VkrCtx *ctx) {
  return ctx->sdk->sim_debt(ctx);
}

/** Running: not paused, disabled or faulted. */
static inline bool8_t vkr_simulating(VkrCtx *ctx) {
  return ctx->sdk->simulating(ctx);
}

static inline bool8_t vkr_paused(VkrCtx *ctx) { return ctx->sdk->paused(ctx); }

/** The transport plays this frame: Play rather than editing, paused or
 * stepping. */
static inline bool8_t vkr_playing(VkrCtx *ctx) {
  return ctx->sdk->playing(ctx);
}

/** How far presentation is between the last two ticks, in [0, 1]. */
static inline float32_t vkr_tick_alpha(VkrCtx *ctx) {
  if (vkr_paused(ctx)) {
    return 1.0f;
  }
  const float64_t alpha = vkr_sim_debt(ctx) / vkr_fixed_dt(ctx);
  return alpha < 1.0 ? (float32_t)alpha : 1.0f;
}

/** From update: advance the scene by `dt` this frame instead of the measured
 * frame time, as a module that owns the input clock does. */
static inline void vkr_set_time_step(VkrCtx *ctx, float64_t dt) {
  ctx->sdk->set_time_step(ctx, dt);
}

static inline Vec3 vkr_gravity(VkrCtx *ctx) { return ctx->sdk->gravity(ctx); }

/** The active scene's hour of day in [0, 24); 12 without a time of day. */
static inline float64_t vkr_time_of_day(VkrCtx *ctx) {
  return ctx->sdk->time_of_day(ctx);
}

/** Runs the time of day from `hour` until Play stops; the sun and moon turn
 * with it. */
static inline bool8_t vkr_set_time_of_day(VkrCtx *ctx, float64_t hour) {
  return ctx->sdk->set_time_of_day(ctx, hour);
}

/** Scales the static lights of light group `name` ("" is the default group)
 * in every container until Play stops; zero switches them off. A night group
 * also fades with the sun. */
static inline bool8_t vkr_set_light_group(VkrCtx *ctx, const char *name,
                                          float32_t intensity) {
  return ctx->sdk->set_light_group(ctx, name, intensity);
}

/** The game owns keyboard and mouse this frame. */
static inline bool8_t vkr_input_focused(VkrCtx *ctx) {
  return ctx->sdk->input_focused(ctx);
}

static inline bool8_t vkr_key_down(VkrCtx *ctx, VkrKey key) {
  return ctx->sdk->key_down(ctx, key);
}

/** The Scene shows its perspective camera, which scripts may drive. */
static inline bool8_t vkr_camera_available(VkrCtx *ctx) {
  return ctx->sdk->camera_available(ctx);
}

/** From late_update: drives the camera this frame. A later module wins. */
static inline void vkr_set_camera(VkrCtx *ctx, Vec3 position,
                                  float32_t yaw_degrees,
                                  float32_t pitch_degrees) {
  ctx->sdk->set_camera(ctx, position, yaw_degrees, pitch_degrees);
}

/** From late_update: adds formatted lines to this frame's overlay. */
static inline void vkr_hud(VkrCtx *ctx, const char *format, ...) {
  char text[VKR_SDK_MESSAGE_CAPACITY];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  ctx->sdk->hud_text(ctx, text);
}

// =============================================================================
// Physics
// =============================================================================

static inline VkrBodyDesc vkr_body_default(VkrCtx *ctx) {
  return ctx->sdk->body_default(ctx);
}

/** Gives the entity a rigid body, replacing any it had. */
static inline bool8_t vkr_set_body(VkrCtx *ctx, VkrEntity entity,
                                   const VkrBodyDesc *body) {
  return ctx->sdk->set_body(ctx, entity, body);
}

/** Applies an impulse to a dynamic body, at its centre for NULL `point`. */
static inline bool8_t vkr_impulse(VkrCtx *ctx, VkrEntity entity, Vec3 impulse,
                                  const Vec3 *world_point) {
  return ctx->sdk->impulse(ctx, entity, impulse, world_point);
}

/** True with `hit` when the segment from `origin` along `displacement` hits
 * a collider. */
static inline bool8_t vkr_raycast(VkrCtx *ctx, Vec3 origin, Vec3 displacement,
                                  const VkrQueryFilter *filter,
                                  VkrRayHit *hit) {
  return ctx->sdk->raycast(ctx, origin, displacement, filter, hit);
}

static inline bool8_t vkr_sweep_sphere(VkrCtx *ctx, Vec3 origin,
                                       Vec3 displacement, float32_t radius,
                                       const VkrQueryFilter *filter,
                                       VkrRayHit *hit) {
  return ctx->sdk->sweep_sphere(ctx, origin, displacement, radius, filter, hit);
}

static inline VkrCharacterDesc vkr_character_default(VkrCtx *ctx) {
  return ctx->sdk->character_default(ctx);
}

/** Gives the entity a character controller owned by the calling scope,
 * standing at `spawn_foot` or, for NULL, at the entity's position. */
static inline bool8_t vkr_character_create(VkrCtx *ctx, VkrEntity entity,
                                           const VkrCharacterDesc *desc,
                                           const Vec3 *spawn_foot) {
  return ctx->sdk->character_create(ctx, entity, desc, spawn_foot);
}

static inline void vkr_character_destroy(VkrCtx *ctx, VkrEntity entity) {
  ctx->sdk->character_destroy(ctx, entity);
}

static inline bool8_t vkr_character_state(VkrCtx *ctx, VkrEntity entity,
                                          VkrCharacterState *out) {
  return ctx->sdk->character_state(ctx, entity, out);
}

/** Moves the character one step; call from fixed_update. */
static inline bool8_t vkr_character_move(VkrCtx *ctx, VkrEntity entity,
                                         const VkrCharacterMove *move,
                                         VkrCharacterState *out) {
  return ctx->sdk->character_move(ctx, entity, move, out);
}

// =============================================================================
// Animation of an entity's animated model
// =============================================================================

/** Identity of the entity's animation playback: zero for none, and a new
 * value when its model is replaced. */
static inline uint64_t vkr_anim_id(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_id(ctx, entity);
}

/** An animation graph already drives the playback. */
static inline bool8_t vkr_anim_has_graph(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_has_graph(ctx, entity);
}

static inline uint32_t vkr_anim_clip_count(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_clip_count(ctx, entity);
}

/** Clip index by name, or VKR_CLIP_NONE. */
static inline uint32_t vkr_anim_clip_find(VkrCtx *ctx, VkrEntity entity,
                                          const char *name) {
  return ctx->sdk->anim_clip_find(ctx, entity, name);
}

static inline float32_t vkr_anim_clip_duration(VkrCtx *ctx, VkrEntity entity,
                                               uint32_t clip) {
  return ctx->sdk->anim_clip_duration(ctx, entity, clip);
}

/** Skeleton nodes of the animation. */
static inline uint32_t vkr_anim_bone_count(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_bone_count(ctx, entity);
}

/** A node's current pose in the model's space. */
static inline bool8_t vkr_anim_bone_pose(VkrCtx *ctx, VkrEntity entity,
                                         uint32_t bone, Mat4 *out) {
  return ctx->sdk->anim_bone_pose(ctx, entity, bone, out);
}

static inline bool8_t vkr_anim_play(VkrCtx *ctx, VkrEntity entity,
                                    uint32_t clip, bool8_t loop) {
  return ctx->sdk->anim_play(ctx, entity, clip, loop);
}

static inline bool8_t vkr_anim_crossfade(VkrCtx *ctx, VkrEntity entity,
                                         uint32_t clip, bool8_t loop,
                                         float64_t duration) {
  return ctx->sdk->anim_crossfade(ctx, entity, clip, loop, duration);
}

static inline void vkr_anim_set_playing(VkrCtx *ctx, VkrEntity entity,
                                        bool8_t playing) {
  ctx->sdk->anim_set_playing(ctx, entity, playing);
}

static inline bool8_t vkr_anim_set_rate(VkrCtx *ctx, VkrEntity entity,
                                        float64_t rate) {
  return ctx->sdk->anim_set_rate(ctx, entity, rate);
}

static inline float64_t vkr_anim_rate(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_rate(ctx, entity);
}

/** Time in the current clip. */
static inline float64_t vkr_anim_time(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_time(ctx, entity);
}

/** Duration of the current clip. */
static inline float64_t vkr_anim_duration(VkrCtx *ctx, VkrEntity entity) {
  return ctx->sdk->anim_duration(ctx, entity);
}

/** Poses the model as a weighted blend of clip samples. Pause the playback
 * first, or its own clock replaces the pose on the next advance. */
static inline bool8_t vkr_anim_blend(VkrCtx *ctx, VkrEntity entity,
                                     const VkrAnimSample *samples,
                                     uint32_t count, bool8_t discontinuity) {
  return ctx->sdk->anim_blend(ctx, entity, samples, count, discontinuity);
}

// =============================================================================
// Tasks
// =============================================================================

/**
 * Runs `fn` on a worker thread over its own copy of `size` bytes of `data`
 * (at most VKR_TASK_DATA_MAX). The function must not call the SDK and may
 * touch only that copy, plus memory nothing else changes until the task is
 * taken. The task belongs to the calling scope like a spawn: when the scope
 * ends, the host waits for it and drops its result. A zero task when it
 * cannot start.
 */
static inline VkrTask vkr_task_run(VkrCtx *ctx, VkrTaskFn fn, const void *data,
                                   uint32_t size) {
  return ctx->sdk->task_run(ctx, fn, data, size);
}

/** Once the task has finished, copies up to `size` bytes of its data into
 * `out` and forgets it. False while it runs, or for an unknown task. */
static inline bool8_t vkr_task_take(VkrCtx *ctx, VkrTask task, void *out,
                                    uint32_t size) {
  return ctx->sdk->task_take(ctx, task, out, size, false_v);
}

/** Waits for the task, running it here if no worker has started it, then
 * takes it. False for an unknown task. */
static inline bool8_t vkr_task_wait(VkrCtx *ctx, VkrTask task, void *out,
                                    uint32_t size) {
  return ctx->sdk->task_take(ctx, task, out, size, true_v);
}

// =============================================================================
// Authoring macros
// =============================================================================

/*
 * A component, its behavior and its module are declared once:
 *
 *   VKR_COMPONENT(Door, door, "Door",
 *                 VKR_FIELD(F32, speed, "Speed", 0.25f, .unit = "turns/s")
 *                 VKR_FIELD(BOOL, locked, "Locked", false_v))
 *
 *   static void door_update(VkrCtx *ctx, VkrEntity self, Door *door,
 *                           float32_t dt) {
 *   }
 *
 *   VKR_BEHAVIOR(door, .update = door_update)
 *
 *   VKR_MODULE(Door, VkrNoData, VKR_EXPORT_BEHAVIOR(door))
 *
 * VKR_FIELD(kind, name, label, default, options...) is one saved field:
 * `kind` is BOOL, I32, U32, F32, ANGLE, VEC2, VEC3, VEC4, QUAT, COLOR,
 * DIRECTION, ENUM (with `.names`) or ENTITY (default `(VkrEntityRef){0}`,
 * no object); options are VkrFieldDesc designators
 * such as `.unit`, `.min`, `.max`, `.step`, `.tooltip` or `.group`.
 *
 * VKR_COMPONENT(Type, name, label, FIELDS) declares `Type`, its descriptor
 * `name_type()`, `name_get(ctx, entity)` and `name_find(ctx, out, capacity)`.
 * A module split across files puts VKR_COMPONENT_DECLARE(Type, name, FIELDS)
 * in a shared header and VKR_COMPONENT_DEFINE(Type, name, label, FIELDS) in
 * one of its files.
 *
 * VKR_BEHAVIOR(name, hooks...) gives component `name` the hooks
 * `.start`, `.stop`, `.destroy`, `.update`, `.late_update`, `.fixed_update`
 * and `.late_fixed_update`, each taking `(VkrCtx *, VkrEntity self, Type *)`
 * and update hooks also the frame's `float32_t dt`; `.trigger_enter` and
 * `.trigger_exit` also take the other `VkrEntity`. `.outputs` and `.inputs`
 * name the lists VKR_OUTPUTS(name, ...) and VKR_INPUTS(name, ...) declare:
 *
 *   VKR_OUTPUTS(door, VKR_OUTPUT(opened, "On opened", NONE))
 *   static void door_open(VkrCtx *ctx, VkrEntity self, Door *door,
 *                         const VkrIoValue *value) {
 *     VKR_FIRE(ctx, self, door, opened, NULL);
 *   }
 *   VKR_INPUTS(door, VKR_INPUT(open, "Open", NONE, door_open))
 *   VKR_BEHAVIOR(door, .update = door_update, .outputs = door_outputs,
 *                .inputs = door_inputs)
 *
 * VKR_MODULE(Name, Data, EXPORTS, options...) defines the entry point.
 * EXPORTS is a sequence of VKR_EXPORT_COMPONENT(name) and
 * VKR_EXPORT_BEHAVIOR(name). Options are `.scope`, `.data_version` and the
 * module hooks, each taking `(VkrCtx *, Data *)` plus `dt` for updates and
 * the event for `.input`.
 */

/** Module data for a module that needs none. */
typedef struct VkrNoData {
  uint8_t unused;
} VkrNoData;

#if defined(_MSC_VER) && !defined(__clang__)
#define VKR_SDK_MAYBE_UNUSED
#else
#define VKR_SDK_MAYBE_UNUSED __attribute__((unused))
#endif

#if defined(_WIN32)
#define VKR_SDK_EXPORT __declspec(dllexport)
#else
#define VKR_SDK_EXPORT __attribute__((visibility("default")))
#endif

#define VKR_SDK_CTYPE_BOOL bool8_t
#define VKR_SDK_CTYPE_I32 int32_t
#define VKR_SDK_CTYPE_U32 uint32_t
#define VKR_SDK_CTYPE_F32 float32_t
#define VKR_SDK_CTYPE_ANGLE float32_t
#define VKR_SDK_CTYPE_VEC2 Vec2
#define VKR_SDK_CTYPE_VEC3 Vec3
#define VKR_SDK_CTYPE_VEC4 Vec4
#define VKR_SDK_CTYPE_QUAT VkrQuat
#define VKR_SDK_CTYPE_COLOR Vec3
#define VKR_SDK_CTYPE_DIRECTION Vec3
#define VKR_SDK_CTYPE_ENUM int32_t
#define VKR_SDK_CTYPE_ENTITY VkrEntityRef

/* A field list is a sequence of parenthesized tuples. */
#define VKR_FIELD(...) (__VA_ARGS__)

#define VKR_SDK_CAT_(a, b) VKR_SDK_CAT2_(a, b)
#define VKR_SDK_CAT2_(a, b) a##b

/* Each walk alternates two macros over a sequence so no macro expands inside
   itself; the last one left pastes into an empty `_END`. A walk's output is
   one macro argument, so it holds commas only inside parentheses. */
#define VKR_SDK_MEMBER_(kind_, name_, label_, value_, ...)                     \
  VKR_SDK_CTYPE_##kind_ name_;
#define VKR_SDK_MEMBERS_A_(...) VKR_SDK_MEMBER_(__VA_ARGS__) VKR_SDK_MEMBERS_B_
#define VKR_SDK_MEMBERS_B_(...) VKR_SDK_MEMBER_(__VA_ARGS__) VKR_SDK_MEMBERS_A_
#define VKR_SDK_MEMBERS_A__END
#define VKR_SDK_MEMBERS_B__END
#define VKR_SDK_MEMBERS_(fields) VKR_SDK_CAT_(VKR_SDK_MEMBERS_A_ fields, _END)

#define VKR_SDK_FIELD_(kind_, name_, label_, value_, ...)                      \
  if (count < VKR_SDK_FIELD_MAX) {                                             \
    fields[count++] = ((VkrFieldDesc){                                         \
        .name = #name_,                                                        \
        .label = label_,                                                       \
        .offset = (uint32_t)offsetof(VkrSdkComponentThis, name_),              \
        .kind = VKR_FIELD_KIND_##kind_,                                        \
        __VA_ARGS__});                                                         \
  }
#define VKR_SDK_FIELDS_A_(...) VKR_SDK_FIELD_(__VA_ARGS__) VKR_SDK_FIELDS_B_
#define VKR_SDK_FIELDS_B_(...) VKR_SDK_FIELD_(__VA_ARGS__) VKR_SDK_FIELDS_A_
#define VKR_SDK_FIELDS_A__END
#define VKR_SDK_FIELDS_B__END
#define VKR_SDK_FIELDS_(fields) VKR_SDK_CAT_(VKR_SDK_FIELDS_A_ fields, _END)

#define VKR_SDK_DEFAULT_(kind_, name_, label_, value_, ...)                    \
  component->name_ = (value_);
#define VKR_SDK_DEFAULTS_A_(...)                                               \
  VKR_SDK_DEFAULT_(__VA_ARGS__) VKR_SDK_DEFAULTS_B_
#define VKR_SDK_DEFAULTS_B_(...)                                               \
  VKR_SDK_DEFAULT_(__VA_ARGS__) VKR_SDK_DEFAULTS_A_
#define VKR_SDK_DEFAULTS_A__END
#define VKR_SDK_DEFAULTS_B__END
#define VKR_SDK_DEFAULTS_(fields) VKR_SDK_CAT_(VKR_SDK_DEFAULTS_A_ fields, _END)

/* Fields one component may declare. */
#define VKR_SDK_FIELD_MAX 64u

/* Declares a component's struct, descriptor function, accessors and hook
   types; include it from every file of the module that uses the type. The
   formatter would read the hook parameters as multiplications. */
// clang-format off
#define VKR_COMPONENT_DECLARE(Type_, id_, fields_)                             \
  typedef struct Type_ {                                                       \
    VKR_SDK_MEMBERS_(fields_)                                                  \
  } Type_;                                                                     \
  VkrComponentDesc *id_##_type(void);                                          \
  VKR_SDK_MAYBE_UNUSED static inline const Type_ *id_##_get(                   \
      VkrCtx *ctx, VkrEntity entity) {                                         \
    return (const Type_ *)vkr_component_get(ctx, entity, id_##_type());        \
  }                                                                            \
  VKR_SDK_MAYBE_UNUSED static inline uint32_t id_##_find(                      \
      VkrCtx *ctx, VkrEntity *out, uint32_t capacity) {                        \
    const uint32_t found = vkr_find(ctx, id_##_type(), out, capacity);         \
    return found < capacity ? found : capacity;                                \
  }                                                                            \
  typedef struct id_##_behavior_hooks {                                        \
    /* Lets the hook designators follow an initialized member. */              \
    uint8_t reserved;                                                          \
    void (*start)(VkrCtx *ctx, VkrEntity self, Type_ *component);              \
    void (*stop)(VkrCtx *ctx, VkrEntity self, Type_ *component);               \
    void (*destroy)(VkrCtx *ctx, VkrEntity self, Type_ *component);            \
    void (*update)(VkrCtx *ctx, VkrEntity self, Type_ *component,              \
                   float32_t dt);                                              \
    void (*late_update)(VkrCtx *ctx, VkrEntity self, Type_ *component,         \
                        float32_t dt);                                         \
    void (*fixed_update)(VkrCtx *ctx, VkrEntity self, Type_ *component);       \
    void (*late_fixed_update)(VkrCtx *ctx, VkrEntity self, Type_ *component);  \
    void (*trigger_enter)(VkrCtx *ctx, VkrEntity self, Type_ *component,       \
                          VkrEntity other);                                    \
    void (*trigger_exit)(VkrCtx *ctx, VkrEntity self, Type_ *component,        \
                         VkrEntity other);                                     \
    const VkrIoPortDesc *(*outputs)(void);                                     \
    const VkrIoPortDesc *(*inputs)(void);                                      \
  } id_##_behavior_hooks;                                                      \
  typedef Type_ id_##_component_type;
// clang-format on

/* Defines the descriptor in one file of the module; the same FIELDS give
   each field its label, default and options. */
#define VKR_COMPONENT_DEFINE(Type_, id_, label_, fields_)                      \
  static void id_##_defaults(void *value) {                                    \
    Type_ *component = value;                                                  \
    VKR_SDK_DEFAULTS_(fields_)                                                 \
  }                                                                            \
  /* Built on first use in writable static storage the host binds. */          \
  VkrComponentDesc *id_##_type(void) {                                         \
    typedef Type_ VkrSdkComponentThis;                                         \
    static VkrFieldDesc fields[VKR_SDK_FIELD_MAX];                             \
    static VkrComponentDesc type;                                              \
    if (!type.name) {                                                          \
      uint32_t count = 0u;                                                     \
      VKR_SDK_FIELDS_(fields_)                                                 \
      type = (VkrComponentDesc){                                               \
          .name = #id_,                                                        \
          .label = label_,                                                     \
          .fields = fields,                                                    \
          .field_count = count,                                                \
          .size = (uint32_t)sizeof(Type_),                                     \
          .align = (uint32_t)AlignOf(Type_),                                   \
          .defaults = id_##_defaults,                                          \
      };                                                                       \
    }                                                                          \
    return &type;                                                              \
  }

/* A component used by one file: VKR_COMPONENT_DECLARE plus DEFINE. */
#define VKR_COMPONENT(Type_, id_, label_, fields_)                             \
  VKR_COMPONENT_DECLARE(Type_, id_, fields_)                                   \
  VKR_COMPONENT_DEFINE(Type_, id_, label_, fields_)

/* Typed hooks reach the erased description through these trampolines, so no
   function is called through a pointer of another type. */
#define VKR_BEHAVIOR(id_, ...)                                                 \
  static const id_##_behavior_hooks id_##_hooks = {.reserved = 0u,             \
                                                   __VA_ARGS__};               \
  static void id_##_hook_start(VkrCtx *ctx, VkrEntity self, void *c) {         \
    id_##_hooks.start(ctx, self, (id_##_component_type *)c);                   \
  }                                                                            \
  static void id_##_hook_stop(VkrCtx *ctx, VkrEntity self, void *c) {          \
    id_##_hooks.stop(ctx, self, (id_##_component_type *)c);                    \
  }                                                                            \
  static void id_##_hook_destroy(VkrCtx *ctx, VkrEntity self, void *c) {       \
    id_##_hooks.destroy(ctx, self, (id_##_component_type *)c);                 \
  }                                                                            \
  static void id_##_hook_update(VkrCtx *ctx, VkrEntity self, void *c,          \
                                float32_t dt) {                                \
    id_##_hooks.update(ctx, self, (id_##_component_type *)c, dt);              \
  }                                                                            \
  static void id_##_hook_late_update(VkrCtx *ctx, VkrEntity self, void *c,     \
                                     float32_t dt) {                           \
    id_##_hooks.late_update(ctx, self, (id_##_component_type *)c, dt);         \
  }                                                                            \
  static void id_##_hook_fixed_update(VkrCtx *ctx, VkrEntity self, void *c) {  \
    id_##_hooks.fixed_update(ctx, self, (id_##_component_type *)c);            \
  }                                                                            \
  static void id_##_hook_late_fixed_update(VkrCtx *ctx, VkrEntity self,        \
                                           void *c) {                          \
    id_##_hooks.late_fixed_update(ctx, self, (id_##_component_type *)c);       \
  }                                                                            \
  static void id_##_hook_trigger_enter(VkrCtx *ctx, VkrEntity self, void *c,   \
                                       VkrEntity other) {                      \
    id_##_hooks.trigger_enter(ctx, self, (id_##_component_type *)c, other);    \
  }                                                                            \
  static void id_##_hook_trigger_exit(VkrCtx *ctx, VkrEntity self, void *c,    \
                                      VkrEntity other) {                       \
    id_##_hooks.trigger_exit(ctx, self, (id_##_component_type *)c, other);     \
  }                                                                            \
  static const VkrBehaviorDesc *id_##_behavior(void) {                         \
    static VkrBehaviorDesc behavior;                                           \
    behavior = (VkrBehaviorDesc){                                              \
        .component = id_##_type(),                                             \
        .start = id_##_hooks.start ? id_##_hook_start : NULL,                  \
        .stop = id_##_hooks.stop ? id_##_hook_stop : NULL,                     \
        .destroy = id_##_hooks.destroy ? id_##_hook_destroy : NULL,            \
        .update = id_##_hooks.update ? id_##_hook_update : NULL,               \
        .late_update =                                                         \
            id_##_hooks.late_update ? id_##_hook_late_update : NULL,           \
        .fixed_update =                                                        \
            id_##_hooks.fixed_update ? id_##_hook_fixed_update : NULL,         \
        .late_fixed_update = id_##_hooks.late_fixed_update                     \
                                 ? id_##_hook_late_fixed_update                \
                                 : NULL,                                       \
        .trigger_enter =                                                       \
            id_##_hooks.trigger_enter ? id_##_hook_trigger_enter : NULL,       \
        .trigger_exit =                                                        \
            id_##_hooks.trigger_exit ? id_##_hook_trigger_exit : NULL,         \
        .outputs = id_##_hooks.outputs ? id_##_hooks.outputs() : NULL,         \
        .inputs = id_##_hooks.inputs ? id_##_hooks.inputs() : NULL,            \
    };                                                                         \
    return &behavior;                                                          \
  }

/* An output or input list is a sequence of tuples: VKR_OUTPUT(name, label,
   KIND) and VKR_INPUT(name, label, KIND, handler), KIND being NONE or a
   VkrIoKind suffix such as F32 or ENTITY. */
#define VKR_OUTPUT(...) (__VA_ARGS__)
#define VKR_INPUT(...) (__VA_ARGS__)

#define VKR_SDK_OUTPUT_ID_(name_, label_, kind_) uint8_t name_;
#define VKR_SDK_OUTPUT_IDS_A_(...)                                             \
  VKR_SDK_OUTPUT_ID_(__VA_ARGS__) VKR_SDK_OUTPUT_IDS_B_
#define VKR_SDK_OUTPUT_IDS_B_(...)                                             \
  VKR_SDK_OUTPUT_ID_(__VA_ARGS__) VKR_SDK_OUTPUT_IDS_A_
#define VKR_SDK_OUTPUT_IDS_A__END
#define VKR_SDK_OUTPUT_IDS_B__END
#define VKR_SDK_OUTPUT_IDS_(ports)                                             \
  VKR_SDK_CAT_(VKR_SDK_OUTPUT_IDS_A_ ports, _END)

#define VKR_SDK_OUTPUT_DESC_(name_, label_, kind_)                             \
  if (count < VKR_SDK_EXPORT_MAX) {                                            \
    ports[count++] = ((VkrIoPortDesc){#name_, label_, VKR_IO_##kind_, NULL});  \
  }
#define VKR_SDK_OUTPUT_DESCS_A_(...)                                           \
  VKR_SDK_OUTPUT_DESC_(__VA_ARGS__) VKR_SDK_OUTPUT_DESCS_B_
#define VKR_SDK_OUTPUT_DESCS_B_(...)                                           \
  VKR_SDK_OUTPUT_DESC_(__VA_ARGS__) VKR_SDK_OUTPUT_DESCS_A_
#define VKR_SDK_OUTPUT_DESCS_A__END
#define VKR_SDK_OUTPUT_DESCS_B__END
#define VKR_SDK_OUTPUT_DESCS_(ports)                                           \
  VKR_SDK_CAT_(VKR_SDK_OUTPUT_DESCS_A_ ports, _END)

/* An input's handler takes `(VkrCtx *, VkrEntity self, Type *, const
   VkrIoValue *)`; its trampoline passes the erased component, which converts
   to `Type *` as an argument. One handler serves one input. */
#define VKR_SDK_INPUT_TRAMPOLINE_(name_, label_, kind_, handler_)              \
  static void vkr_sdk_input_##handler_(VkrCtx *ctx, VkrEntity self, void *c,   \
                                       const VkrIoValue *value) {              \
    handler_(ctx, self, c, value);                                             \
  }
#define VKR_SDK_INPUT_TRAMPOLINES_A_(...)                                      \
  VKR_SDK_INPUT_TRAMPOLINE_(__VA_ARGS__) VKR_SDK_INPUT_TRAMPOLINES_B_
#define VKR_SDK_INPUT_TRAMPOLINES_B_(...)                                      \
  VKR_SDK_INPUT_TRAMPOLINE_(__VA_ARGS__) VKR_SDK_INPUT_TRAMPOLINES_A_
#define VKR_SDK_INPUT_TRAMPOLINES_A__END
#define VKR_SDK_INPUT_TRAMPOLINES_B__END
#define VKR_SDK_INPUT_TRAMPOLINES_(ports)                                      \
  VKR_SDK_CAT_(VKR_SDK_INPUT_TRAMPOLINES_A_ ports, _END)

#define VKR_SDK_INPUT_DESC_(name_, label_, kind_, handler_)                    \
  if (count < VKR_SDK_EXPORT_MAX) {                                            \
    ports[count++] = ((VkrIoPortDesc){#name_, label_, VKR_IO_##kind_,          \
                                      vkr_sdk_input_##handler_});              \
  }
#define VKR_SDK_INPUT_DESCS_A_(...)                                            \
  VKR_SDK_INPUT_DESC_(__VA_ARGS__) VKR_SDK_INPUT_DESCS_B_
#define VKR_SDK_INPUT_DESCS_B_(...)                                            \
  VKR_SDK_INPUT_DESC_(__VA_ARGS__) VKR_SDK_INPUT_DESCS_A_
#define VKR_SDK_INPUT_DESCS_A__END
#define VKR_SDK_INPUT_DESCS_B__END
#define VKR_SDK_INPUT_DESCS_(ports)                                            \
  VKR_SDK_CAT_(VKR_SDK_INPUT_DESCS_A_ ports, _END)

/* Declares component `id_`'s outputs, listed by `id_##_outputs()`, and
   their indices; VKR_OUTPUT_ID(id_, name) is one index, checked when it
   compiles. Lists are built on first use, as field lists are. */
#define VKR_OUTPUTS(id_, ports_)                                               \
  typedef struct id_##_output_ids {                                            \
    VKR_SDK_OUTPUT_IDS_(ports_)                                                \
  } id_##_output_ids;                                                          \
  static const VkrIoPortDesc *id_##_outputs(void) {                            \
    static VkrIoPortDesc ports[VKR_SDK_EXPORT_MAX + 1u];                       \
    if (!ports[0].name) {                                                      \
      uint32_t count = 0u;                                                     \
      VKR_SDK_OUTPUT_DESCS_(ports_)                                            \
    }                                                                          \
    return ports;                                                              \
  }
#define VKR_OUTPUT_ID(id_, name_) ((uint32_t)offsetof(id_##_output_ids, name_))

/* Declares component `id_`'s inputs, listed by `id_##_inputs()`, after
   their handlers. */
#define VKR_INPUTS(id_, ports_)                                                \
  VKR_SDK_INPUT_TRAMPOLINES_(ports_)                                           \
  static const VkrIoPortDesc *id_##_inputs(void) {                             \
    static VkrIoPortDesc ports[VKR_SDK_EXPORT_MAX + 1u];                       \
    if (!ports[0].name) {                                                      \
      uint32_t count = 0u;                                                     \
      VKR_SDK_INPUT_DESCS_(ports_)                                             \
    }                                                                          \
    return ports;                                                              \
  }

/* vkr_io_fire for output `name_` of component `id_`. */
#define VKR_FIRE(ctx_, self_, id_, name_, value_)                              \
  vkr_io_fire((ctx_), (self_), id_##_type(), VKR_OUTPUT_ID(id_, name_),        \
              (value_))

/* An export list is a sequence of (kind, name) tuples. */
#define VKR_EXPORT_COMPONENT(id_) (COMPONENT, id_)
#define VKR_EXPORT_BEHAVIOR(id_) (BEHAVIOR, id_)

#define VKR_SDK_EXPORT_COMPONENT_(id_)                                         \
  if (desc.component_count < VKR_SDK_EXPORT_MAX) {                             \
    desc.components[desc.component_count] = id_##_type();                      \
  }                                                                            \
  ++desc.component_count;
#define VKR_SDK_EXPORT_BEHAVIOR_(id_)                                          \
  if (desc.behavior_count < VKR_SDK_EXPORT_MAX) {                              \
    desc.behaviors[desc.behavior_count] = id_##_behavior();                    \
  }                                                                            \
  ++desc.behavior_count;
#define VKR_SDK_EXPORT_(kind_, id_) VKR_SDK_EXPORT_##kind_##_(id_)
#define VKR_SDK_EXPORTS_A_(...) VKR_SDK_EXPORT_(__VA_ARGS__) VKR_SDK_EXPORTS_B_
#define VKR_SDK_EXPORTS_B_(...) VKR_SDK_EXPORT_(__VA_ARGS__) VKR_SDK_EXPORTS_A_
#define VKR_SDK_EXPORTS_A__END
#define VKR_SDK_EXPORTS_B__END
#define VKR_SDK_EXPORTS_(exports) VKR_SDK_CAT_(VKR_SDK_EXPORTS_A_ exports, _END)

// clang-format off
#define VKR_MODULE(Name_, Data_, exports_, ...)                                \
  typedef struct Name_##_module_options {                                      \
    /* Lets the option designators follow an initialized member. */            \
    uint8_t reserved;                                                          \
    VkrScope scope;                                                            \
    uint32_t data_version;                                                     \
    void (*start)(VkrCtx *ctx, Data_ *data);                                   \
    void (*stop)(VkrCtx *ctx, Data_ *data);                                    \
    void (*update)(VkrCtx *ctx, Data_ *data, float32_t dt);                    \
    void (*late_update)(VkrCtx *ctx, Data_ *data, float32_t dt);               \
    void (*fixed_update)(VkrCtx *ctx, Data_ *data);                            \
    void (*late_fixed_update)(VkrCtx *ctx, Data_ *data);                       \
    void (*input)(VkrCtx *ctx, Data_ *data, const VkrInputEvent *event);       \
  } Name_##_module_options;                                                    \
  static const Name_##_module_options Name_##_options = {.reserved = 0u,       \
                                                         __VA_ARGS__};         \
  static void Name_##_hook_start(VkrCtx *ctx, void *data) {                    \
    Name_##_options.start(ctx, (Data_ *)data);                                 \
  }                                                                            \
  static void Name_##_hook_stop(VkrCtx *ctx, void *data) {                     \
    Name_##_options.stop(ctx, (Data_ *)data);                                  \
  }                                                                            \
  static void Name_##_hook_update(VkrCtx *ctx, void *data, float32_t dt) {     \
    Name_##_options.update(ctx, (Data_ *)data, dt);                            \
  }                                                                            \
  static void Name_##_hook_late_update(VkrCtx *ctx, void *data,                \
                                       float32_t dt) {                         \
    Name_##_options.late_update(ctx, (Data_ *)data, dt);                       \
  }                                                                            \
  static void Name_##_hook_fixed_update(VkrCtx *ctx, void *data) {             \
    Name_##_options.fixed_update(ctx, (Data_ *)data);                          \
  }                                                                            \
  static void Name_##_hook_late_fixed_update(VkrCtx *ctx, void *data) {        \
    Name_##_options.late_fixed_update(ctx, (Data_ *)data);                     \
  }                                                                            \
  static void Name_##_hook_input(VkrCtx *ctx, void *data,                      \
                                 const VkrInputEvent *event) {                 \
    Name_##_options.input(ctx, (Data_ *)data, event);                          \
  }                                                                            \
  VKR_SDK_EXPORT const VkrModuleDesc *vkr_module_##Name_(                      \
      uint32_t sdk_version) {                                                  \
    if (sdk_version != VKR_SDK_VERSION) {                                      \
      return NULL;                                                             \
    }                                                                          \
    static VkrModuleDesc desc;                                                 \
    desc = (VkrModuleDesc){                                                    \
        .sdk_version = VKR_SDK_VERSION,                                        \
        .name = #Name_,                                                        \
        .scope = Name_##_options.scope,                                        \
        .data_size = (uint32_t)sizeof(Data_),                                  \
        .data_align = (uint32_t)AlignOf(Data_),                                \
        .data_version = Name_##_options.data_version,                          \
        .start = Name_##_options.start ? Name_##_hook_start : NULL,            \
        .stop = Name_##_options.stop ? Name_##_hook_stop : NULL,               \
        .update = Name_##_options.update ? Name_##_hook_update : NULL,         \
        .late_update =                                                         \
            Name_##_options.late_update ? Name_##_hook_late_update : NULL,     \
        .fixed_update =                                                        \
            Name_##_options.fixed_update ? Name_##_hook_fixed_update : NULL,   \
        .late_fixed_update = Name_##_options.late_fixed_update                 \
                                 ? Name_##_hook_late_fixed_update              \
                                 : NULL,                                       \
        .input = Name_##_options.input ? Name_##_hook_input : NULL,            \
    };                                                                         \
    VKR_SDK_EXPORTS_(exports_)                                                 \
    return &desc;                                                              \
  }
// clang-format on

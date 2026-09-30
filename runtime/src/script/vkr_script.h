/**
 * @file vkr_script.h
 * @brief Native C script module ABI (ADR-079).
 *
 * A script module is game code built with a project. It reaches the engine
 * only through the host-owned VkrScriptApi table, so the same module can be
 * linked statically into an executable or built as a shared library that
 * links no engine symbols. Runtime headers still supply its value types and
 * inline math. Scene, input and asset pointers are opaque to a module: it
 * passes them back to the table and never reads their fields.
 *
 * Behaviors a user attaches to entities are the module's component types:
 * plain-data VkrTypeDesc tables that the host copies and registers as scene
 * world types before any scene initializes. Documents, Details, Add component,
 * presets and Cmd paths then accept them like engine components.
 *
 * Hot reload replaces a shared library's code while its state stays. The
 * engine therefore keeps no module function pointer past one call except
 * those in the description, which the host reads again after every reload,
 * and a module keeps no pointer to its own code or constants in its state.
 * Superseded libraries stay loaded until the session stops.
 */
#pragma once

#include "animation/vkr_animation_player.h"
#include "core/input.h"
#include "core/logger.h"
#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_physics.h"

#define VKR_SCRIPT_ABI_VERSION 2u
/* Component types one module may declare; the host refuses more. */
#define VKR_SCRIPT_MODULE_TYPE_MAX 8u

struct VkrRenderAssets;
struct VkrAnimationGraphInstance;

/**
 * Engine entry points a module may call. The host fills it once; `size` is
 * sizeof(VkrScriptApi) of the host, so a module built against a shorter
 * table can check that the members it uses are present. Every function keeps
 * the contract of the engine function it forwards to.
 */
typedef struct VkrScriptApi {
  uint32_t version;
  uint32_t size;

  /* Diagnostics: one formatted message per call. */
  void (*log)(LogLevel level, const char *message);

  /* Entities and components. */
  bool8_t (*entity_alive)(const VkrScene *scene, VkrEntityId entity);
  VkrEntityId (*create_entity)(VkrScene *scene, VkrSceneError *out_error);
  void (*destroy_entity)(VkrScene *scene, VkrEntityId entity);
  bool8_t (*set_name)(VkrScene *scene, VkrEntityId entity, String8 name);
  bool8_t (*set_transform)(VkrScene *scene, VkrEntityId entity, Vec3 position,
                           VkrQuat rotation, Vec3 scale);
  void (*set_parent)(VkrScene *scene, VkrEntityId entity, VkrEntityId parent);
  void (*set_visibility)(VkrScene *scene, VkrEntityId entity, bool8_t visible,
                         bool8_t inherit_parent);
  /** Effective visibility through inheriting parents. */
  bool8_t (*entity_visible)(const VkrScene *scene, VkrEntityId entity);
  bool8_t (*set_shape)(VkrScene *scene, struct VkrRenderAssets *assets,
                       VkrEntityId entity, const VkrSceneShapeConfig *config,
                       VkrSceneError *out_error);
  /** The entity's evaluated world matrix; false for a dead entity. */
  bool8_t (*world_matrix)(VkrScene *scene, VkrEntityId entity, Mat4 *out);
  bool8_t (*set_evaluated_transform)(VkrScene *scene, VkrEntityId entity,
                                     const Mat4 *world);
  void (*update_transforms)(VkrScene *scene);
  /** Authored component of a registered or engine type, or NULL. */
  const void *(*get_typed)(const VkrScene *scene, VkrEntityId entity,
                           const VkrTypeDesc *type);
  bool8_t (*set_typed)(VkrScene *scene, VkrEntityId entity,
                       const VkrTypeDesc *type, const void *value);
  /** Writes up to `capacity` entities carrying `type` in entity order and
   * returns how many carry it, which may exceed `capacity`. */
  uint32_t (*find_typed)(const VkrScene *scene, const VkrTypeDesc *type,
                         VkrEntityId *out_entities, uint32_t capacity);
  /** Runtime-only component storage: never serialized or edited. */
  VkrComponentTypeId (*register_state)(VkrScene *scene, const char *name,
                                       uint32_t size, uint32_t align);
  bool8_t (*add_state)(VkrScene *scene, VkrEntityId entity,
                       VkrComponentTypeId type, const void *value);
  bool8_t (*remove_state)(VkrScene *scene, VkrEntityId entity,
                          VkrComponentTypeId type);
  bool8_t (*has_state)(const VkrScene *scene, VkrEntityId entity,
                       VkrComponentTypeId type);
  void *(*get_state)(VkrScene *scene, VkrEntityId entity,
                     VkrComponentTypeId type);
  /** The engine's Player Start component type, for set_typed. */
  const VkrTypeDesc *player_start_type;
  /** World matrix of the Player Start the scene resolves (its own, then the
   * root World's); false when neither has one. */
  bool8_t (*player_start)(const VkrScene *scene, Mat4 *out_world);

  /* Shared simulation clock (ADR-073). */
  /** Not paused, not disabled and not faulted. */
  bool8_t (*simulation_running)(const VkrScene *scene);
  uint64_t (*simulation_completed_ticks)(const VkrScene *scene);
  float64_t (*physics_time)(const VkrScene *scene);
  float64_t (*physics_debt)(const VkrScene *scene);
  bool8_t (*physics_paused)(const VkrScene *scene);
  Vec3 (*gravity)(const VkrScene *scene);

  /* Physics bodies, queries and characters (ADR-072). */
  VkrScenePhysicsSnapshot (*physics_default)(void);
  bool8_t (*physics_apply)(VkrScene *scene, VkrEntityId entity,
                           const VkrScenePhysicsSnapshot *snapshot,
                           const char **error);
  bool8_t (*physics_impulse)(VkrScene *scene, VkrEntityId entity, Vec3 impulse,
                             const Vec3 *world_point, const char **error);
  bool8_t (*raycast)(VkrScene *scene, Vec3 origin, Vec3 displacement,
                     const VkrPhysicsQueryFilter *filter,
                     VkrPhysicsRayHit *hit);
  bool8_t (*sweep_sphere)(VkrScene *scene, Vec3 origin, Vec3 displacement,
                          float32_t radius, const VkrPhysicsQueryFilter *filter,
                          VkrPhysicsRayHit *hit, bool8_t *found);
  VkrPhysicsCharacterDesc (*character_default)(void);
  /** NULL `spawn_foot` spawns at the authored root pose. */
  bool8_t (*character_create)(VkrScene *scene, VkrEntityId entity,
                              const VkrPhysicsCharacterDesc *settings,
                              const Vec3 *spawn_foot, const char **error);
  bool8_t (*character_destroy)(VkrScene *scene, VkrEntityId entity,
                               const char **error);
  bool8_t (*character_get_state)(VkrScene *scene, VkrEntityId entity,
                                 VkrPhysicsCharacterState *state,
                                 const char **error);
  bool8_t (*character_step)(VkrScene *scene, VkrEntityId entity,
                            const VkrPhysicsCharacterInput *input,
                            VkrPhysicsCharacterState *state,
                            const char **error);

  /* Animation playback of scene-bound animated meshes (ADR-071). */
  VkrAnimationPlayer *(*animation_player)(const VkrScene *scene,
                                          VkrEntityId entity);
  const struct VkrAnimationGraphInstance *(*animation_graph)(
      const VkrScene *scene, VkrEntityId entity);
  const VkrAnimationAsset *(*animation_asset)(const VkrAnimationPlayer *player);
  const Mat4 *(*animation_global_pose)(const VkrAnimationPlayer *player);
  bool8_t (*animation_select_clip)(VkrAnimationPlayer *player, uint32_t clip,
                                   bool8_t loop);
  bool8_t (*animation_crossfade)(VkrAnimationPlayer *player, uint32_t clip,
                                 bool8_t loop, float64_t duration);
  void (*animation_set_playing)(VkrAnimationPlayer *player, bool8_t playing);
  bool8_t (*animation_set_rate)(VkrAnimationPlayer *player, float64_t rate);
  float64_t (*animation_rate)(const VkrAnimationPlayer *player);
  float64_t (*animation_duration)(const VkrAnimationPlayer *player);
  float64_t (*animation_time)(const VkrAnimationPlayer *player);

  /* Keyboard state; ordered transitions arrive through the input hook. */
  bool8_t (*input_key_down)(InputState *input, Keys key);

  /* Appended members: a module checks `size` before using them. */
  /** The simulated rigid-body pose driving the entity; false when no body
   * drives it. A presentation override set with set_evaluated_transform
   * replaces this pose, so a script that decorates a physics object should
   * start from it. */
  bool8_t (*physics_world_matrix)(VkrScene *scene, VkrEntityId entity,
                                  Mat4 *out);
} VkrScriptApi;

typedef enum VkrScriptSessionFlags {
  VKR_SCRIPT_SESSION_NONE = 0u,
  /** The application asked for sample gameplay content (`--gameplay`). */
  VKR_SCRIPT_SESSION_SAMPLE_CONTENT = 1u << 0,
} VkrScriptSessionFlags;

/** One simulated scene run by every module. Host-owned and stable from
 * start until stop; modules borrow it and its pointers for that span. */
typedef struct VkrScriptSession {
  const VkrScriptApi *api;
  VkrScene *scene;
  InputState *input;
  struct VkrRenderAssets *assets;
  /** Nonzero and distinct for every session this process starts. */
  uint64_t instance_id;
  uint32_t flags; /**< VkrScriptSessionFlags. */
} VkrScriptSession;

/** Variable-rate frame state before the scene advances. */
typedef struct VkrScriptFrame {
  /** Monotonic seconds measured after the input pump. */
  float64_t now;
  /** Elapsed time the scene advances this frame. A module that owns the
   * input clock may replace it with its admitted elapsed time. */
  float64_t scene_delta;
  /** Scene keyboard and mouse belong to gameplay. */
  bool8_t input_focused;
  bool8_t simulation_running;
  /** The Scene shows its perspective camera, which scripts may drive. */
  bool8_t camera_available;
} VkrScriptFrame;

#define VKR_SCRIPT_HUD_CAPACITY 256u

/** Presentation a module publishes after the scene advanced. */
typedef struct VkrScriptView {
  bool8_t camera_valid;
  Vec3 camera_position;
  float32_t camera_yaw_degrees;
  float32_t camera_pitch_degrees;
  /** Optional overlay text; empty for none. */
  char hud[VKR_SCRIPT_HUD_CAPACITY];
} VkrScriptView;

typedef enum VkrScriptStart {
  VKR_SCRIPT_START_FAILED = 0,
  /** Nothing in this scene uses the module; it receives no further calls. */
  VKR_SCRIPT_START_IDLE,
  VKR_SCRIPT_START_ACTIVE,
} VkrScriptStart;

/**
 * A module's static description. `state` is `state_size` bytes at
 * `state_align` that the host owns and zeroes before every start; modules
 * keep no session state elsewhere. Hooks run on the scene-owning thread.
 *
 * - start: the scene is paused at a reset boundary (tick zero). Structural
 *   edits, characters and bodies may be created. A failure must leave
 *   nothing behind.
 * - before_physics/after_physics: the ADR-073 tick hooks. No structural
 *   edits. Returning false faults the simulation with `*error`, valid until
 *   return.
 * - reset: infallible restore after a native simulation reset.
 * - input: each ordered key, button and look transition while the session
 *   runs, on the producer thread before its asynchronous event (ADR-073).
 * - frame: before the scene advances, every frame.
 * - present: after the scene advanced, every frame.
 * - reload: after a hot reload swapped in this code with the running
 *   session's state, before any other hook of the new code.
 * - stop: the scene is paused; release everything start created.
 * Every hook except start and stop is optional.
 *
 * A reload keeps the running state only when `state_size`, `state_align` and
 * `state_version` match; bump `state_version` whenever the state struct
 * changes. Component types must keep their layout across a reload.
 */
typedef struct VkrScriptModuleDesc {
  uint32_t abi_version;
  uint32_t size;
  const char *name;
  const VkrTypeDesc *const *types;
  uint32_t type_count;
  uint32_t state_size;
  uint32_t state_align;
  uint32_t state_version;
  VkrScriptStart (*start)(const VkrScriptSession *session, void *state,
                          const char **error);
  void (*stop)(const VkrScriptSession *session, void *state);
  bool8_t (*before_physics)(const VkrScriptSession *session, void *state,
                            uint64_t tick, const char **error);
  bool8_t (*after_physics)(const VkrScriptSession *session, void *state,
                           uint64_t tick, const char **error);
  void (*reset)(const VkrScriptSession *session, void *state);
  void (*frame)(const VkrScriptSession *session, void *state,
                VkrScriptFrame *frame);
  void (*present)(const VkrScriptSession *session, void *state,
                  const VkrScriptFrame *frame, VkrScriptView *view);
  void (*input)(const VkrScriptSession *session, void *state,
                const VkrInputTransition *transition);
  void (*reload)(const VkrScriptSession *session, void *state);
} VkrScriptModuleDesc;

/**
 * A module's one entry point, named `vkr_script_module_<name>` so several
 * modules can link into one executable. It keeps `api` for the process and
 * returns its static description, or NULL when the table is too old.
 */
typedef const VkrScriptModuleDesc *(*VkrScriptModuleEntry)(
    const VkrScriptApi *api);

#if defined(_WIN32)
#define VKR_SCRIPT_EXPORT __declspec(dllexport)
#else
#define VKR_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

// =============================================================================
// Authoring macros
// =============================================================================

/*
 * Components and modules are declared once instead of as a struct, a
 * property table, a defaults function, a type descriptor, a module
 * description and an entry point written by hand:
 *
 *   #define DOOR_FIELDS                                                   \
 *     VKR_FIELD(F32, speed, "Speed", 0.25f, .unit = "turns/s",           \
 *               .min = -10.0f, .max = 10.0f)                             \
 *     VKR_FIELD(BOOL, locked, "Locked", false_v)
 *   VKR_SCRIPT_COMPONENT(Door, door, "Door", DOOR_FIELDS)
 *
 *   VKR_SCRIPT_MODULE(Door, DoorState, 1, (door),
 *                     .start = door_start, .stop = door_stop)
 *
 * VKR_FIELD(kind, name, label, default, options...) is one property:
 * `kind` is BOOL, I32, U32, F32, ANGLE (radians), VEC2, VEC3, VEC4, QUAT,
 * COLOR, DIRECTION or ENUM (with `.names`); `default` is any expression of
 * the member's type; options are VkrPropertyDesc designators such as `.unit`,
 * `.min`, `.max`, `.step`, `.tooltip` or `.flags`.
 *
 * VKR_SCRIPT_COMPONENT(Type, name, label, FIELDS) declares `Type`, its
 * descriptor `name_type()`, `name_get(session, entity)` returning the
 * entity's `const Type *` or NULL, and `name_find(session, out, capacity)`
 * returning how many entities carry it, at most `capacity`.
 *
 * VKR_SCRIPT_MODULE(Name, State, version, (a)(b), hooks...) defines the
 * exported `vkr_script_module_Name`: its components, `State` as the session
 * state with `version` as its state_version, and the hooks as designators.
 */

#include <stddef.h>

/* Generated helpers a script may not call. */
#if defined(_MSC_VER) && !defined(__clang__)
#define VKR_SCRIPT_MAYBE_UNUSED
#else
#define VKR_SCRIPT_MAYBE_UNUSED __attribute__((unused))
#endif

#define VKR_SCRIPT_CTYPE_BOOL bool8_t
#define VKR_SCRIPT_CTYPE_I32 int32_t
#define VKR_SCRIPT_CTYPE_U32 uint32_t
#define VKR_SCRIPT_CTYPE_F32 float32_t
#define VKR_SCRIPT_CTYPE_ANGLE float32_t
#define VKR_SCRIPT_CTYPE_VEC2 Vec2
#define VKR_SCRIPT_CTYPE_VEC3 Vec3
#define VKR_SCRIPT_CTYPE_VEC4 Vec4
#define VKR_SCRIPT_CTYPE_QUAT Vec4
#define VKR_SCRIPT_CTYPE_COLOR Vec3
#define VKR_SCRIPT_CTYPE_DIRECTION Vec3
#define VKR_SCRIPT_CTYPE_ENUM int32_t

/* A field list is a sequence of parenthesized tuples. */
#define VKR_FIELD(...) (__VA_ARGS__)

#define VKR_SCRIPT_CAT_(a, b) VKR_SCRIPT_CAT2_(a, b)
#define VKR_SCRIPT_CAT2_(a, b) a##b

/* Each walk alternates two macros over the sequence so no macro expands
   inside itself; the last one left pastes into an empty `_END`. A walk's
   output is one macro argument, so it may hold commas only inside
   parentheses: the walks emit statements and parenthesized literals. */
#define VKR_SCRIPT_MEMBER_(kind_, name_, label_, value_, ...)                  \
  VKR_SCRIPT_CTYPE_##kind_ name_;
#define VKR_SCRIPT_MEMBERS_A_(...)                                             \
  VKR_SCRIPT_MEMBER_(__VA_ARGS__) VKR_SCRIPT_MEMBERS_B_
#define VKR_SCRIPT_MEMBERS_B_(...)                                             \
  VKR_SCRIPT_MEMBER_(__VA_ARGS__) VKR_SCRIPT_MEMBERS_A_
#define VKR_SCRIPT_MEMBERS_A__END
#define VKR_SCRIPT_MEMBERS_B__END
#define VKR_SCRIPT_MEMBERS_(fields)                                            \
  VKR_SCRIPT_CAT_(VKR_SCRIPT_MEMBERS_A_ fields, _END)

#define VKR_SCRIPT_PROPERTY_(kind_, name_, label_, value_, ...)                \
  if (count < VKR_TYPE_PROPERTY_MAX) {                                         \
    properties[count++] = ((VkrPropertyDesc){                                  \
        .name = #name_,                                                        \
        .label = label_,                                                       \
        .offset = (uint32_t)offsetof(VkrScriptComponentThis, name_),           \
        .kind = VKR_PROPERTY_##kind_,                                          \
        __VA_ARGS__});                                                         \
  }
#define VKR_SCRIPT_PROPERTIES_A_(...)                                          \
  VKR_SCRIPT_PROPERTY_(__VA_ARGS__) VKR_SCRIPT_PROPERTIES_B_
#define VKR_SCRIPT_PROPERTIES_B_(...)                                          \
  VKR_SCRIPT_PROPERTY_(__VA_ARGS__) VKR_SCRIPT_PROPERTIES_A_
#define VKR_SCRIPT_PROPERTIES_A__END
#define VKR_SCRIPT_PROPERTIES_B__END
#define VKR_SCRIPT_PROPERTIES_(fields)                                         \
  VKR_SCRIPT_CAT_(VKR_SCRIPT_PROPERTIES_A_ fields, _END)

#define VKR_SCRIPT_DEFAULT_(kind_, name_, label_, value_, ...)                 \
  component->name_ = (value_);
#define VKR_SCRIPT_DEFAULTS_A_(...)                                            \
  VKR_SCRIPT_DEFAULT_(__VA_ARGS__) VKR_SCRIPT_DEFAULTS_B_
#define VKR_SCRIPT_DEFAULTS_B_(...)                                            \
  VKR_SCRIPT_DEFAULT_(__VA_ARGS__) VKR_SCRIPT_DEFAULTS_A_
#define VKR_SCRIPT_DEFAULTS_A__END
#define VKR_SCRIPT_DEFAULTS_B__END
#define VKR_SCRIPT_DEFAULTS_(fields)                                           \
  VKR_SCRIPT_CAT_(VKR_SCRIPT_DEFAULTS_A_ fields, _END)

#define VKR_SCRIPT_COMPONENT(Type_, id_, label_, fields_)                      \
  typedef struct Type_ {                                                       \
    VKR_SCRIPT_MEMBERS_(fields_)                                               \
  } Type_;                                                                     \
  static void id_##_defaults(void *value) {                                    \
    Type_ *component = value;                                                  \
    VKR_SCRIPT_DEFAULTS_(fields_)                                              \
  }                                                                            \
  /* Built on first use and stable for the process: the host maps it to        \
     its registered copy. */                                                   \
  VKR_SCRIPT_MAYBE_UNUSED static const VkrTypeDesc *id_##_type(void) {         \
    typedef Type_ VkrScriptComponentThis;                                      \
    static VkrPropertyDesc properties[VKR_TYPE_PROPERTY_MAX];                  \
    static VkrTypeDesc type;                                                   \
    if (!type.name) {                                                          \
      uint32_t count = 0u;                                                     \
      VKR_SCRIPT_PROPERTIES_(fields_)                                          \
      type = (VkrTypeDesc){                                                    \
          .name = #id_,                                                        \
          .label = label_,                                                     \
          .category = "Scripts",                                               \
          .properties = properties,                                            \
          .property_count = count,                                             \
          .size = sizeof(Type_),                                               \
          .align = _Alignof(Type_),                                            \
          .defaults = id_##_defaults,                                          \
      };                                                                       \
    }                                                                          \
    return &type;                                                              \
  }                                                                            \
  VKR_SCRIPT_MAYBE_UNUSED static inline const Type_ *id_##_get(                \
      const VkrScriptSession *session, VkrEntityId entity) {                   \
    return (const Type_ *)session->api->get_typed(session->scene, entity,      \
                                                  id_##_type());               \
  }                                                                            \
  VKR_SCRIPT_MAYBE_UNUSED static inline uint32_t id_##_find(                   \
      const VkrScriptSession *session, VkrEntityId *out, uint32_t capacity) {  \
    const uint32_t found =                                                     \
        session->api->find_typed(session->scene, id_##_type(), out, capacity); \
    return found < capacity ? found : capacity;                                \
  }

#define VKR_SCRIPT_TYPE_(id_)                                                  \
  if (count < VKR_SCRIPT_MODULE_TYPE_MAX) {                                    \
    types[count] = id_##_type();                                               \
  }                                                                            \
  ++count;
#define VKR_SCRIPT_TYPES_A_(id_) VKR_SCRIPT_TYPE_(id_) VKR_SCRIPT_TYPES_B_
#define VKR_SCRIPT_TYPES_B_(id_) VKR_SCRIPT_TYPE_(id_) VKR_SCRIPT_TYPES_A_
#define VKR_SCRIPT_TYPES_A__END
#define VKR_SCRIPT_TYPES_B__END
#define VKR_SCRIPT_TYPES_(components)                                          \
  VKR_SCRIPT_CAT_(VKR_SCRIPT_TYPES_A_ components, _END)

#define VKR_SCRIPT_MODULE(Name_, State_, version_, components_, ...)           \
  VKR_SCRIPT_EXPORT const VkrScriptModuleDesc *vkr_script_module_##Name_(      \
      const VkrScriptApi *api) {                                               \
    if (!api || api->version != VKR_SCRIPT_ABI_VERSION ||                      \
        api->size < sizeof(VkrScriptApi)) {                                    \
      return NULL;                                                             \
    }                                                                          \
    /* More than VKR_SCRIPT_MODULE_TYPE_MAX types: the host refuses it. */     \
    static const VkrTypeDesc *types[VKR_SCRIPT_MODULE_TYPE_MAX];               \
    uint32_t count = 0u;                                                       \
    VKR_SCRIPT_TYPES_(components_)                                             \
    static VkrScriptModuleDesc desc;                                           \
    desc = (VkrScriptModuleDesc){.abi_version = VKR_SCRIPT_ABI_VERSION,        \
                                 .size = sizeof(VkrScriptModuleDesc),          \
                                 .name = #Name_,                               \
                                 .types = types,                               \
                                 .type_count = count,                          \
                                 .state_size = sizeof(State_),                 \
                                 .state_align = _Alignof(State_),              \
                                 .state_version = (version_),                  \
                                 __VA_ARGS__};                                 \
    return &desc;                                                              \
  }

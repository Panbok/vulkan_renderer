#pragma once

#include "assets/vkr_collision_cooked.h"
#include "core/vkr_type_desc.h"
#include "physics/vkr_physics.h"
#include "renderer/systems/vkr_scene_system.h"

#define VKR_SCENE_PHYSICS_MAX_COLLIDERS VKR_PHYSICS_MAX_COLLIDERS
#define VKR_SCENE_PHYSICS_MAX_BODIES 1024u
#define VKR_SCENE_PHYSICS_MAX_JOINTS 16u
#define VKR_SCENE_COLLISION_ASSET_PATH_MAX 256u
#define VKR_SCENE_PHYSICS_FIXED_DT VKR_SCENE_SIMULATION_FIXED_DT
/* Scenes one physics set holds: the World, the primary scene and the added
   scenes. */
#define VKR_SCENE_PHYSICS_SET_MAX 8u
/* Two full scenes of bodies plus edit staging share the native world. */
#define VKR_SCENE_PHYSICS_SET_BODIES (VKR_SCENE_PHYSICS_MAX_BODIES * 4u)

/* Physics set (ADR-076): the root-owned native world that every loaded scene
 * shares, so bodies in different scenes collide. The driver (the primary
 * scene) steps the world once per fixed tick for every member and owns reset
 * and contact dispatch; other members follow its clock and pause state. The
 * World, when a member, owns the collision layers; otherwise the driver does.
 * Attach a scene before it creates physics state. Scene shutdown removes its
 * bodies and detaches it; destroy the set after every member shut down. */
VkrScenePhysicsSet *vkr_scene_physics_set_create(VkrAllocator *allocator);
void vkr_scene_physics_set_destroy(VkrScenePhysicsSet *set);
/* Makes a set member the driver while the set has none, as when the root
 * World is played without a scene; a scene attached later as driver takes
 * over. The member already hosts its own physics then, so nothing moves. */
void vkr_scene_physics_drive(VkrScene *scene);
bool8_t vkr_scene_physics_attach(VkrScene *scene, VkrScenePhysicsSet *set,
                                 bool8_t driver, const char **error);
/* Members of the scene's set, or the scene alone; returns the count. */
uint32_t vkr_scene_physics_set_members(const VkrScene *scene,
                                       VkrScene **members, uint32_t capacity);
/* Whether the scene's collision layers are the set's source of truth. */
bool8_t vkr_scene_physics_layers_owner(const VkrScene *scene);
/* Bodies the scene's fixed step simulates: every member's for a driver. */
uint32_t vkr_scene_physics_simulated_body_count(const VkrScene *scene);

/* One character per root entity with unit scale and no rigid body. Creation
 * copies settings but takes entity_id from the entity and foot_position from
 * `spawn_foot`, or from the authored root pose when it is NULL, such as a
 * player spawned at a Player Start (ADR-079). Authored TRS stays unchanged.
 * Create/destroy require pause and no active callbacks/prepared edits. Reset
 * transactionally rebuilds native characters at the same spawn while
 * preserving entity identity. Scene shutdown releases all native characters. */
bool8_t vkr_scene_character_create(VkrScene *scene, VkrEntityId entity,
                                   const VkrPhysicsCharacterDesc *settings,
                                   const Vec3 *spawn_foot, const char **error);
bool8_t vkr_scene_character_destroy(VkrScene *scene, VkrEntityId entity,
                                    const char **error);
/* Call once from before_physics per tick. Input dt must match the scene fixed
 * tick; body/character queries are serialized on the scene owner. Translation
 * publishes through the evaluated scene transform, never authored TRS. */
bool8_t vkr_scene_character_step(VkrScene *scene, VkrEntityId entity,
                                 const VkrPhysicsCharacterInput *input,
                                 VkrPhysicsCharacterState *state,
                                 const char **error);
bool8_t vkr_scene_character_get_state(VkrScene *scene, VkrEntityId entity,
                                      VkrPhysicsCharacterState *state,
                                      const char **error);

/* How a box, sphere or capsule collider takes its size. */
typedef enum VkrSceneColliderFit {
  /* Its pose and dimensions as authored. */
  VKR_SCENE_COLLIDER_FIT_MANUAL = 0,
  /* The box around its owner's meshes and shapes in the owner's frame,
     moved by `fit_offset` and grown by `padding` on each side; the runtime
     sets the pose and dimensions, and sets them again when that box
     changes. */
  VKR_SCENE_COLLIDER_FIT_AUTO,
  VKR_SCENE_COLLIDER_FIT_COUNT,
} VkrSceneColliderFit;

typedef struct VkrSceneColliderConfig {
  uint64_t
      authored_id; /* Nonzero, unique within its body; survives save/undo. */
  VkrPhysicsShape shape;
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  char asset_path[VKR_SCENE_COLLISION_ASSET_PATH_MAX];
  Vec3 half_extent;
  float32_t radius;
  float32_t half_height;
  bool8_t enabled;
  VkrSceneColliderFit fit;
  Vec3 fit_offset;
  Vec3 padding;
} VkrSceneColliderConfig;

/* Explicit position/rotation below offset the body from this evaluated bone.
 * Kinematic attachments follow animation before each fixed step; dynamic
 * drive_bone bodies publish the solved bone pose afterward (ragdoll). */
typedef struct VkrScenePhysicsAttachment {
  bool8_t enabled;
  SceneSourceIdentity animation_source;
  uint32_t source_node;
  bool8_t drive_bone;
  Vec3 position;
  VkrQuat rotation;
} VkrScenePhysicsAttachment;

typedef struct VkrSceneJointConfig {
  uint64_t authored_id;
  SceneSourceIdentity target_source;
  VkrPhysicsJointType type;
  Vec3 anchor_a;
  Vec3 anchor_b;
  Vec3 axis_a;
  Vec3 axis_b;
  Vec3 normal_a;
  Vec3 normal_b;
  float32_t min_limit;
  float32_t max_limit;
  float32_t swing_normal_limit;
  float32_t swing_plane_limit;
  bool8_t enabled;
} VkrSceneJointConfig;

/* Authored body settings; `vkr_scene_physics_body_type` describes them. */
typedef struct VkrScenePhysicsBody {
  VkrPhysicsMotion motion;
  float32_t mass;
  float32_t friction;
  float32_t restitution;
  float32_t gravity_factor;
  float32_t linear_damping;
  float32_t angular_damping;
  bool8_t enabled;
  bool8_t allow_sleep;
  bool8_t continuous;
  bool8_t sensor;
} VkrScenePhysicsBody;

typedef struct VkrScenePhysicsSnapshot {
  bool8_t present;
  VkrScenePhysicsBody body;
  uint16_t collision_layer;
  uint16_t collision_mask;
  VkrScenePhysicsAttachment attachment;
  uint32_t joint_count;
  VkrSceneJointConfig joints[VKR_SCENE_PHYSICS_MAX_JOINTS];
  uint32_t collider_count;
  VkrSceneColliderConfig colliders[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
} VkrScenePhysicsSnapshot;

/* Descriptors of the authored body settings and of one collider (ADR-076);
   Details, Cmd paths and validation use them. */
extern const VkrTypeDesc vkr_scene_physics_body_type;
extern const VkrTypeDesc vkr_scene_physics_collider_type;

typedef struct VkrScenePhysicsChange {
  VkrEntityId entity;
  VkrScenePhysicsSnapshot snapshot;
} VkrScenePhysicsChange;

typedef enum VkrSceneRagdollOperation {
  VKR_SCENE_RAGDOLL_CREATE,
  VKR_SCENE_RAGDOLL_ENABLE,
  VKR_SCENE_RAGDOLL_DISABLE,
  VKR_SCENE_RAGDOLL_REMOVE
} VkrSceneRagdollOperation;

bool8_t vkr_scene_physics_ragdoll_plan(const VkrScene *scene,
                                       VkrEntityId wrapper,
                                       VkrSceneRagdollOperation operation,
                                       VkrScenePhysicsChange *changes,
                                       uint32_t capacity, uint32_t *count,
                                       const char **error);

typedef struct ScenePhysicsCollider {
  VkrEntityId owner;
  uint64_t authored_id;
} ScenePhysicsCollider;

typedef struct s_VkrScenePhysicsPrepared VkrScenePhysicsPrepared;

VkrScenePhysicsSnapshot vkr_scene_physics_default(void);
bool8_t vkr_scene_physics_set_asset_root(VkrScene *scene, String8 root,
                                         const char **error);
bool8_t vkr_scene_physics_pose_valid(Vec3 position, VkrQuat rotation);
bool8_t vkr_scene_physics_transform_validate(const VkrScene *scene,
                                             VkrEntityId entity, Vec3 position,
                                             VkrQuat rotation, Vec3 scale,
                                             VkrEntityId parent,
                                             const char **error);
/* The box around `entity`'s meshes and shapes changed, as when its generated
   mesh is replaced: automatic colliders of it and of its ancestors refit
   while physics is paused. */
void vkr_scene_physics_bounds_changed(VkrScene *scene, VkrEntityId entity);
/* Reads absent bodies successfully. Collider children resolve via owner(). */
bool8_t vkr_scene_physics_read(const VkrScene *scene, VkrEntityId entity,
                               VkrScenePhysicsSnapshot *snapshot);
bool8_t
vkr_scene_physics_snapshot_validate(const VkrScenePhysicsSnapshot *snapshot,
                                    const char **error);
bool8_t vkr_scene_physics_validate(const VkrScene *scene, VkrEntityId entity,
                                   const VkrScenePhysicsSnapshot *snapshot,
                                   const char **error);
/* Edits require explicit pause. NULL snapshot removes the body and only its
 * owned collider children. Preparation owns provisional CPU/ECS allocations;
 * discard rolls them back, commit cannot fail. Do not update/render the scene
 * between prepare and commit/discard. Duplicate preparation is rejected. */
bool8_t vkr_scene_physics_prepare(VkrScene *scene, VkrEntityId entity,
                                  const VkrScenePhysicsSnapshot *snapshot,
                                  VkrScenePhysicsPrepared **prepared,
                                  const char **error);
/* Finalize the future body/joint graph after all owner preparations. With
 * nothing prepared there is nothing to finalize and it returns true. */
bool8_t vkr_scene_physics_prepare_complete(VkrScene *scene, const char **error);
void vkr_scene_physics_commit(VkrScenePhysicsPrepared *prepared);
void vkr_scene_physics_discard(VkrScenePhysicsPrepared *prepared);
bool8_t vkr_scene_physics_apply(VkrScene *scene, VkrEntityId entity,
                                const VkrScenePhysicsSnapshot *snapshot,
                                const char **error);
VkrEntityId vkr_scene_physics_owner(const VkrScene *scene, VkrEntityId entity);
VkrEntityId vkr_scene_physics_collider_entity(const VkrScene *scene,
                                              VkrEntityId owner,
                                              uint64_t authored_id);
uint32_t vkr_scene_physics_body_count(const VkrScene *scene);
VkrEntityId vkr_scene_physics_body_at(const VkrScene *scene, uint32_t index);
void vkr_scene_physics_set_paused(VkrScene *scene, bool8_t paused);
bool8_t vkr_scene_physics_is_paused(const VkrScene *scene);
bool8_t vkr_scene_physics_step(VkrScene *scene, const char **error);
bool8_t vkr_scene_physics_reset(VkrScene *scene, const char **error);
/* Origin rebase (ADR-086): moves the physics of `scenes` by -`shift` metres
   after their root entities moved by it: every native body and character
   once per physics world, and each scene's interpolation poses, authored
   world matrices, character states and generated colliders. Call between
   ticks, never during event dispatch. */
bool8_t vkr_scene_physics_shift(VkrScene *const *scenes, uint32_t count,
                                Vec3 shift, const char **error);
bool8_t vkr_scene_physics_set_disabled(VkrScene *scene, bool8_t disabled,
                                       const char **error);
bool8_t vkr_scene_physics_is_disabled(const VkrScene *scene);
bool8_t vkr_scene_physics_impulse(VkrScene *scene, VkrEntityId entity,
                                  Vec3 impulse, const Vec3 *world_point,
                                  const char **error);
bool8_t vkr_scene_physics_get_pose(const VkrScene *scene, VkrEntityId entity,
                                   VkrPhysicsPose *pose);
bool8_t vkr_scene_physics_raycast(VkrScene *scene, Vec3 origin,
                                  Vec3 displacement, VkrPhysicsRayHit *hit);
float64_t vkr_scene_physics_time(const VkrScene *scene);
float64_t vkr_scene_physics_debt(const VkrScene *scene);
/* Consumes completed fixed ticks once for scene animation, including Step. */
float64_t vkr_scene_physics_animation_delta(VkrScene *scene,
                                            float64_t fallback);
bool8_t vkr_scene_physics_set_body_disabled(VkrScene *scene, VkrEntityId entity,
                                            bool8_t disabled,
                                            const char **error);
bool8_t vkr_scene_physics_body_is_disabled(const VkrScene *scene,
                                           VkrEntityId entity);
bool8_t vkr_scene_physics_set_kinematic_target(VkrScene *scene,
                                               VkrEntityId entity,
                                               Vec3 position, VkrQuat rotation,
                                               const char **error);
bool8_t vkr_scene_physics_overlap_sphere(VkrScene *scene, Vec3 center,
                                         float32_t radius, uint16_t query_mask,
                                         VkrPhysicsOverlapHit *hits,
                                         uint32_t capacity, uint32_t *count);
bool8_t vkr_scene_physics_sensor_events(VkrScene *scene,
                                        VkrPhysicsSensorEvent *events,
                                        uint32_t capacity, uint32_t *count);
const char *vkr_scene_physics_error(const VkrScene *scene);
/* Scene-system hooks; physics world and body storage live until shutdown. */
bool8_t vkr_scene_physics_register(VkrScene *scene);
void vkr_scene_physics_update(VkrScene *scene, float64_t dt);
/* One native tick, called only by the shared scene simulation coordinator. */
bool8_t vkr_scene_physics_tick(VkrScene *scene, const char **error);
void vkr_scene_physics_shutdown(VkrScene *scene);
bool8_t vkr_scene_physics_entity_destroying(VkrScene *scene,
                                            VkrEntityId entity);
bool8_t vkr_scene_physics_world_matrix(VkrScene *scene, VkrEntityId entity,
                                       Mat4 *matrix);
bool8_t vkr_scene_physics_transform_allowed(const VkrScene *scene,
                                            VkrEntityId entity, Vec3 scale,
                                            VkrEntityId parent);

bool8_t vkr_scene_physics_resolve_world(const VkrScene *scene,
                                        VkrEntityId entity, Mat4 *world);
bool8_t vkr_scene_physics_publish_bones(VkrScene *scene, bool8_t interpolate,
                                        const char **error);

const VkrCollisionGeometry *
vkr_scene_physics_collider_geometry(const VkrScene *scene, VkrEntityId owner,
                                    uint64_t authored_id);
void vkr_scene_physics_set_contact_callback(VkrScene *scene,
                                            VkrPhysicsContactCallback callback,
                                            void *context);
bool8_t vkr_scene_physics_contact_events(VkrScene *scene,
                                         VkrPhysicsContactEvent *events,
                                         uint32_t capacity, uint32_t *count);

bool8_t vkr_scene_physics_mutations_allowed(const VkrScene *scene);

/* Static or sensor bodies the scene generates from authored data, such as
   brush collision (ADR-084). They are not
   authored state: snapshots, documents, the journal and Reset never see
   them, and shutdown destroys them. `key` names one body at the world
   origin and replaces any earlier body under it; queries report `entity`
   for the body and each collider's own entity. Colliders and their
   geometry are borrowed only during the call. */
bool8_t vkr_scene_physics_generated_set(VkrScene *scene, uint64_t key,
                                        VkrEntityId entity,
                                        const VkrPhysicsColliderDesc *colliders,
                                        uint32_t collider_count, bool8_t sensor,
                                        const char **error);
/* A kinematic generated body, such as a mover's brushes: it rests at the
   identity pose and moves to the pose vkr_scene_physics_generated_move set
   at each step, so what stands on it rides. Replacing it keeps that pose;
   a reset puts it back at rest. */
bool8_t vkr_scene_physics_generated_set_kinematic(
    VkrScene *scene, uint64_t key, VkrEntityId entity,
    const VkrPhysicsColliderDesc *colliders, uint32_t collider_count,
    const char **error);
/* The pose kinematic generated body `key` moves to from the next step: a
   rigid motion of its world-space colliders, turned by `rotation` about the
   world point `pivot` and then moved by `position`. */
bool8_t vkr_scene_physics_generated_move(VkrScene *scene, uint64_t key,
                                         Vec3 position, VkrQuat rotation,
                                         Vec3 pivot, const char **error);
void vkr_scene_physics_generated_remove(VkrScene *scene, uint64_t key);
bool8_t vkr_scene_physics_matrix_allowed(const VkrScene *scene,
                                         VkrEntityId entity, Mat4 local);
bool8_t vkr_scene_physics_raycast_query(VkrScene *scene, Vec3 origin,
                                        Vec3 displacement,
                                        const VkrPhysicsQueryFilter *filter,
                                        VkrPhysicsRayHit *hit);
bool8_t vkr_scene_physics_sweep_sphere(VkrScene *scene, Vec3 origin,
                                       Vec3 displacement, float32_t radius,
                                       const VkrPhysicsQueryFilter *filter,
                                       VkrPhysicsRayHit *hit, bool8_t *found);
/* Sweep one retained collider geometry at an explicit world pose. Scale
 * includes the body's composed world scale; authored collider offset is not
 * applied. */
bool8_t vkr_scene_physics_sweep(VkrScene *scene, VkrEntityId owner,
                                uint64_t collider_id, Vec3 origin,
                                VkrQuat rotation, Vec3 displacement,
                                const VkrPhysicsQueryFilter *filter,
                                VkrPhysicsRayHit *hit);

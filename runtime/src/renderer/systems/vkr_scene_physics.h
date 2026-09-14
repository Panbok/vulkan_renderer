#pragma once

#include "assets/vkr_collision_cooked.h"
#include "physics/vkr_physics.h"
#include "renderer/systems/vkr_scene_system.h"

#define VKR_SCENE_PHYSICS_MAX_COLLIDERS VKR_PHYSICS_MAX_COLLIDERS
#define VKR_SCENE_PHYSICS_MAX_BODIES 1024u
#define VKR_SCENE_PHYSICS_MAX_JOINTS 16u
#define VKR_SCENE_COLLISION_ASSET_PATH_MAX 256u
#define VKR_SCENE_PHYSICS_FIXED_DT VKR_SCENE_SIMULATION_FIXED_DT

/* One character per root entity with unit scale and no rigid body. Creation
 * copies settings but takes foot_position/entity_id from the authored entity.
 * Create/destroy require pause and no active callbacks/prepared edits. Reset
 * transactionally rebuilds native characters from authored root poses while
 * preserving entity identity. Scene shutdown releases all native characters. */
bool8_t vkr_scene_character_create(VkrScene *scene, VkrEntityId entity,
                                   const VkrPhysicsCharacterDesc *settings,
                                   const char **error);
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

typedef struct VkrScenePhysicsSnapshot {
  bool8_t present;
  VkrPhysicsMotion motion;
  uint16_t collision_layer;
  uint16_t collision_mask;
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
  VkrScenePhysicsAttachment attachment;
  uint32_t joint_count;
  VkrSceneJointConfig joints[VKR_SCENE_PHYSICS_MAX_JOINTS];
  uint32_t collider_count;
  VkrSceneColliderConfig colliders[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
} VkrScenePhysicsSnapshot;

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
/* Finalize the future body/joint graph after all owner preparations. */
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

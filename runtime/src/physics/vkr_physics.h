#pragma once
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s_VkrPhysicsWorld VkrPhysicsWorld;
typedef uint64_t VkrPhysicsBody;
#define VKR_PHYSICS_BODY_INVALID UINT64_C(0)
#define VKR_PHYSICS_MAX_COLLIDERS 32
#define VKR_PHYSICS_MAX_COORDINATE 1.0e7f
#define VKR_PHYSICS_SENSOR_EVENTS_PER_BODY 16
#define VKR_PHYSICS_SENSOR_PAIRS_PER_BODY 8
#define VKR_PHYSICS_CONTACT_EVENTS_PER_BODY 128
#define VKR_PHYSICS_JOINTS_PER_BODY 16
#define VKR_PHYSICS_MAX_QUERY_IGNORES 32

typedef enum VkrPhysicsMotion {
  VKR_PHYSICS_STATIC,
  VKR_PHYSICS_KINEMATIC,
  VKR_PHYSICS_DYNAMIC
} VkrPhysicsMotion;

typedef enum VkrPhysicsShape {
  VKR_PHYSICS_BOX,
  VKR_PHYSICS_SPHERE,
  VKR_PHYSICS_CAPSULE,
  VKR_PHYSICS_CONVEX_HULL,
  VKR_PHYSICS_TRIANGLE_MESH
} VkrPhysicsShape;

typedef struct VkrPhysicsGeometry {
  const float32_t *positions; // Packed XYZ, borrowed during create/query only.
  uint32_t vertex_count;
  const uint32_t *indices; // Triangle list, required for mesh; hull ignores it.
  uint32_t index_count;
} VkrPhysicsGeometry;

typedef struct VkrPhysicsColliderDesc {
  uint64_t entity_id;
  VkrPhysicsShape shape;
  float32_t position[3];
  float32_t rotation[4]; // Unit quaternion, xyzw.
  float32_t
      scale[3]; // Strictly positive; sphere/capsule require uniform scale.
  VkrPhysicsGeometry geometry;
  float32_t half_extent[3];
  float32_t radius;
  float32_t half_height; // Capsule cylinder half-height, Y axis.
  bool8_t enabled;
} VkrPhysicsColliderDesc;

typedef struct VkrPhysicsBodyDesc {
  uint64_t entity_id;
  VkrPhysicsMotion motion;
  float32_t position[3]; // Body origin, not center of mass; meters.
  float32_t rotation[4];
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
  uint16_t collision_layer; // Bit set; default layer 1.
  uint16_t collision_mask;  // Allowed opposing layers; default 0xffff.
  const VkrPhysicsColliderDesc *colliders; // Borrowed only during create.
  uint32_t collider_count;
} VkrPhysicsBodyDesc;

typedef struct VkrPhysicsPose {
  float32_t position[3];
  float32_t rotation[4];
  float32_t linear_velocity[3];
  float32_t angular_velocity[3];
  bool8_t active;
} VkrPhysicsPose;

typedef struct VkrPhysicsRayHit {
  VkrPhysicsBody body;
  uint64_t entity_id;
  uint64_t collider_entity_id;
  float32_t fraction; // Ray direction is the entire segment displacement.
  float32_t position[3];
  float32_t normal[3];
} VkrPhysicsRayHit;

typedef struct VkrPhysicsOverlapHit {
  VkrPhysicsBody body;
  uint64_t entity_id;
  uint64_t collider_entity_id;
} VkrPhysicsOverlapHit;

typedef struct VkrPhysicsSensorEvent {
  uint64_t entity_a;
  uint64_t collider_a;
  uint64_t entity_b;
  uint64_t collider_b;
  bool8_t began;
} VkrPhysicsSensorEvent;

typedef struct VkrPhysicsQueryFilter {
  uint16_t mask;
  bool8_t include_sensors;
  const uint64_t *ignored_entities;
  uint32_t ignored_count;
} VkrPhysicsQueryFilter;

typedef enum VkrPhysicsContactPhase {
  VKR_PHYSICS_CONTACT_BEGIN,
  VKR_PHYSICS_CONTACT_PERSIST,
  VKR_PHYSICS_CONTACT_END
} VkrPhysicsContactPhase;

typedef struct VkrPhysicsContactEvent {
  VkrPhysicsBody body_a;
  VkrPhysicsBody body_b;
  uint64_t entity_a;
  uint64_t collider_a;
  uint64_t entity_b;
  uint64_t collider_b;
  float32_t position[3];
  float32_t normal[3]; // From A toward B.
  VkrPhysicsContactPhase phase;
} VkrPhysicsContactEvent;

typedef uint64_t VkrPhysicsJoint;
#define VKR_PHYSICS_JOINT_INVALID UINT64_C(0)
typedef enum VkrPhysicsJointType {
  VKR_PHYSICS_JOINT_FIXED,
  VKR_PHYSICS_JOINT_HINGE,
  VKR_PHYSICS_JOINT_DISTANCE,
  VKR_PHYSICS_JOINT_SWING_TWIST
} VkrPhysicsJointType;

typedef struct VkrPhysicsJointDesc {
  VkrPhysicsJointType type;
  VkrPhysicsBody body_a;
  VkrPhysicsBody body_b;
  // Body-origin local frames (not center-of-mass local). Unit perpendicular
  // axes.
  float32_t anchor_a[3];
  float32_t anchor_b[3];
  float32_t axis_a[3]; // Hinge/twist axis; fixed frame X axis.
  float32_t axis_b[3];
  float32_t normal_a[3]; // Hinge zero-angle/plane axis; fixed frame Y axis.
  float32_t normal_b[3];
  float32_t min_limit; // Hinge/twist radians [-pi,0], or distance meters >=0.
  float32_t max_limit; // Hinge/twist radians [0,pi], or distance >=min.
  float32_t swing_normal_limit; // Swing cone half-angle [0,pi].
  float32_t swing_plane_limit;
  bool8_t enabled;
  bool8_t collide_connected;
} VkrPhysicsJointDesc;

// Material combine: sqrt(friction_a * friction_b), max(restitution_a,
// restitution_b); restitution only above 1 m/s closing speed.
#define VKR_PHYSICS_RESTITUTION_THRESHOLD 1.0f

// Synchronous, single owner thread. Calls must not overlap, including separate
// worlds. World owns bodies, shapes and scratch; destroy releases all of them.
// Capacity and invalid-input errors return false; last_error remains valid
// until the next failed call. Simulation capacity failure faults the world:
// destroy/recreate to reset. Invalid input alone does not fault the world.
// SDK allocation failure is not a recoverable process-OOM contract.
VkrPhysicsWorld *vkr_physics_world_create(uint32_t max_bodies);
void vkr_physics_world_destroy(VkrPhysicsWorld *world);
const char *vkr_physics_last_error(const VkrPhysicsWorld *world);
bool8_t vkr_physics_body_create(VkrPhysicsWorld *world,
                                const VkrPhysicsBodyDesc *desc,
                                VkrPhysicsBody *out_body);
// Reserve sensor-exit capacity for an authored transaction. While any destroy
// is reserved, stepping is rejected. Destroy/disable consumes; discard cancels.
// A body cannot be reserved twice. Other calls must not mutate a reserved body.
bool8_t vkr_physics_body_reserve_destroy(VkrPhysicsWorld *world,
                                         VkrPhysicsBody body);
void vkr_physics_body_cancel_destroy(VkrPhysicsWorld *world,
                                     VkrPhysicsBody body);
bool8_t vkr_physics_body_destroy(VkrPhysicsWorld *world, VkrPhysicsBody body);
bool8_t vkr_physics_body_set_enabled(VkrPhysicsWorld *world,
                                     VkrPhysicsBody body, bool8_t enabled);
bool8_t vkr_physics_body_set_pose(VkrPhysicsWorld *world, VkrPhysicsBody body,
                                  const float32_t position[3],
                                  const float32_t rotation[4]);
bool8_t vkr_physics_body_move_kinematic(VkrPhysicsWorld *world,
                                        VkrPhysicsBody body,
                                        const float32_t position[3],
                                        const float32_t rotation[4],
                                        float32_t dt);
bool8_t vkr_physics_body_impulse(VkrPhysicsWorld *world, VkrPhysicsBody body,
                                 const float32_t impulse[3],
                                 const float32_t world_point[3]); // NULL = COM.
bool8_t vkr_physics_body_set_velocity(VkrPhysicsWorld *world,
                                      VkrPhysicsBody body,
                                      const float32_t linear[3],
                                      const float32_t angular[3]);
bool8_t vkr_physics_body_get_pose(VkrPhysicsWorld *world, VkrPhysicsBody body,
                                  VkrPhysicsPose *out_pose);
bool8_t vkr_physics_step(VkrPhysicsWorld *world, float32_t dt);
bool8_t vkr_physics_raycast(VkrPhysicsWorld *world, const float32_t origin[3],
                            const float32_t displacement[3],
                            VkrPhysicsRayHit *out_hit);
// Ray false also means no hit; a miss clears out_hit and preserves last_error.
// Queries include sensors. Query mask selects opposing membership layers.
bool8_t vkr_physics_raycast_filtered(VkrPhysicsWorld *world,
                                     const float32_t origin[3],
                                     const float32_t displacement[3],
                                     uint16_t query_mask,
                                     VkrPhysicsRayHit *out_hit);
// Capacity failure returns false and out_count=0; output entries are
// unspecified.
bool8_t vkr_physics_overlap_sphere(VkrPhysicsWorld *world,
                                   const float32_t center[3], float32_t radius,
                                   uint16_t query_mask,
                                   VkrPhysicsOverlapHit *hits,
                                   uint32_t capacity, uint32_t *out_count);
// Events describe geometric sensor membership, including sleeping bodies.
// They remain queued until consumed; insufficient output capacity leaves queue.
bool8_t vkr_physics_sensor_events(VkrPhysicsWorld *world,
                                  VkrPhysicsSensorEvent *events,
                                  uint32_t capacity, uint32_t *out_count);
// Extended queries use mask/sensor/ignored-owner filtering. NULL filter means
// all layers and sensors. Sweep supports primitives/convex, not triangle
// meshes.
bool8_t vkr_physics_raycast_query(VkrPhysicsWorld *world,
                                  const float32_t origin[3],
                                  const float32_t displacement[3],
                                  const VkrPhysicsQueryFilter *filter,
                                  VkrPhysicsRayHit *hit);
bool8_t
vkr_physics_sweep(VkrPhysicsWorld *world, const VkrPhysicsColliderDesc *shape,
                  const float32_t origin[3], const float32_t rotation[4],
                  const float32_t displacement[3],
                  const VkrPhysicsQueryFilter *filter, VkrPhysicsRayHit *hit);
// Drains after a successful tick; no application callbacks run inside Jolt.
bool8_t vkr_physics_contact_events(VkrPhysicsWorld *world,
                                   VkrPhysicsContactEvent *events,
                                   uint32_t capacity, uint32_t *out_count);
typedef void (*VkrPhysicsContactCallback)(const VkrPhysicsContactEvent *events,
                                          uint32_t count, void *user);
// Borrows queued events only during the callback, outside solver locks. NULL
// callback drains. Read-only queries are allowed; mutation/reentrant drain
// fails.
bool8_t vkr_physics_contact_events_dispatch(VkrPhysicsWorld *world,
                                            VkrPhysicsContactCallback callback,
                                            void *user);
// Joints are removed automatically before either attached body is destroyed.
// Disabled bodies suspend attached joints; re-enable restores joint
// participation.
bool8_t vkr_physics_joint_create(VkrPhysicsWorld *world,
                                 const VkrPhysicsJointDesc *desc,
                                 VkrPhysicsJoint *out_joint);
bool8_t vkr_physics_joint_destroy(VkrPhysicsWorld *world,
                                  VkrPhysicsJoint joint);
bool8_t vkr_physics_joint_set_enabled(VkrPhysicsWorld *world,
                                      VkrPhysicsJoint joint, bool8_t enabled);
#ifdef __cplusplus
}
#endif

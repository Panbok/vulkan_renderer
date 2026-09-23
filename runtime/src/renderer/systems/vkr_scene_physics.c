#include "renderer/systems/vkr_scene_physics.h"

#include "memory/vkr_dmemory.h"
#include "physics/vkr_collision_asset.h"
#include "renderer/systems/vkr_scene_animation.h"
#include "renderer/systems/vkr_scene_collision_layers.h"
#include <math.h>

typedef struct ScenePhysicsBody {
  struct ScenePhysicsBody *next;
  VkrEntityId entity;
  VkrScenePhysicsSnapshot authored;
  VkrEntityId colliders[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
  VkrPhysicsBody body;
  VkrCollisionAsset *assets[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
  VkrPhysicsJoint joints[VKR_SCENE_PHYSICS_MAX_JOINTS];
  Vec3 world_scale;
  Mat4 authored_world;
  VkrEntityId animation_wrapper;
  bool8_t disabled;
  VkrPhysicsPose previous_pose;
  VkrPhysicsPose current_pose;
  Vec3 target_position;
  VkrQuat target_rotation;
  Vec3 last_position;
  VkrQuat last_rotation;
} ScenePhysicsBody;

typedef struct ScenePhysicsBodyComponent {
  ScenePhysicsBody *body;
} ScenePhysicsBodyComponent;

typedef struct ScenePhysicsCharacter {
  VkrEntityId entity;
  VkrPhysicsCharacter native;
  VkrPhysicsCharacterDesc settings;
  VkrPhysicsCharacterState previous;
  VkrPhysicsCharacterState current;
  uint64_t last_step_tick;
} ScenePhysicsCharacter;

typedef struct PhysicsStagedJoints {
  struct PhysicsStagedJoints *next;
  ScenePhysicsBody *body;
  VkrPhysicsJoint handles[VKR_SCENE_PHYSICS_MAX_JOINTS];
} PhysicsStagedJoints;

struct s_VkrScenePhysics {
  VkrDMemory memory;
  VkrPhysicsWorld *world;
  ScenePhysicsBody *bodies;
  ScenePhysicsBody *reset_bodies;
  VkrScenePhysicsPrepared *prepared;
  PhysicsStagedJoints *staged_joints;
  bool8_t prepared_complete;
  bool8_t dispatching;
  VkrPhysicsContactCallback contact_callback;
  void *contact_context;
  uint32_t body_count;
  VkrComponentTypeId character_component;
  ScenePhysicsCharacter characters[VKR_PHYSICS_MAX_CHARACTERS];
  uint32_t staged_additions;
  bool8_t editing;
  bool8_t faulted;
  bool8_t resetting;
  const char *error;
  char error_storage[512];
};

struct s_VkrScenePhysicsPrepared {
  VkrScenePhysicsPrepared *next;
  VkrScene *scene;
  ScenePhysicsBody *old;
  ScenePhysicsBody *replacement;
  VkrEntityId entity;
  bool8_t added_component;
  bool8_t addition_reserved;
  bool8_t destruction_reserved;
  bool8_t new_collider[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
};

static bool8_t physics_fail(const char **error, const char *message) {
  if (error) {
    *error = message;
  }
  return false_v;
}

static bool8_t physics_vec_finite(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         fabsf(value.x) <= 1.0e7f && fabsf(value.y) <= 1.0e7f &&
         fabsf(value.z) <= 1.0e7f;
}

static bool8_t physics_rotation_valid(VkrQuat value) {
  const float64_t length =
      (float64_t)value.x * value.x + (float64_t)value.y * value.y +
      (float64_t)value.z * value.z + (float64_t)value.w * value.w;
  return isfinite(length) && fabs(length - 1.0) < 0.0001;
}

bool8_t vkr_scene_physics_pose_valid(Vec3 position, VkrQuat rotation) {
  return physics_vec_finite(position) && physics_rotation_valid(rotation);
}

static bool8_t physics_unit_scale(Vec3 value) {
  return value.x == 1.0f && value.y == 1.0f && value.z == 1.0f;
}

static ScenePhysicsBody *physics_body(const VkrScene *scene,
                                      VkrEntityId entity) {
  // Body records live in the physics world's storage; without that world no
  // component may be resolved to a body.
  if (!scene || !scene->world || !scene->physics) {
    return NULL;
  }
  const ScenePhysicsBodyComponent *component =
      vkr_entity_get_component_if_alive(scene->world, entity,
                                        scene->comp_physics_body);
  return component ? component->body : NULL;
}

static ScenePhysicsCharacter *physics_character(const VkrScene *scene,
                                                VkrEntityId entity) {
  if (!scene || !scene->physics) {
    return NULL;
  }
  const uint32_t *index = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->physics->character_component);
  if (!index || *index >= ArrayCount(scene->physics->characters)) {
    return NULL;
  }
  ScenePhysicsCharacter *character = &scene->physics->characters[*index];
  return character->entity.u64 == entity.u64 ? character : NULL;
}

typedef struct PhysicsTransformOverride {
  VkrEntityId entity;
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;
  VkrEntityId parent;
} PhysicsTransformOverride;

static bool8_t physics_positive_scale(Vec3 scale) {
  return physics_vec_finite(scale) && scale.x > 1e-6f && scale.y > 1e-6f &&
         scale.z > 1e-6f;
}

static Mat4 physics_trs(Vec3 position, VkrQuat rotation, Vec3 scale) {
  return mat4_mul(mat4_translate(position),
                  mat4_mul(vkr_quat_to_mat4(rotation), mat4_scale(scale)));
}

static bool8_t physics_decompose(Mat4 matrix, Vec3 *position, VkrQuat *rotation,
                                 Vec3 *scale, const char **error) {
  for (uint32_t i = 0; i < 16; ++i) {
    if (!isfinite(matrix.elements[i])) {
      return physics_fail(error,
                          "Physics world matrix contains nonfinite values");
    }
  }
  Vec3 x = vec3_new(matrix.elements[0], matrix.elements[1], matrix.elements[2]);
  Vec3 y = vec3_new(matrix.elements[4], matrix.elements[5], matrix.elements[6]);
  Vec3 z =
      vec3_new(matrix.elements[8], matrix.elements[9], matrix.elements[10]);
  *scale = vec3_new(vec3_length(x), vec3_length(y), vec3_length(z));
  *position =
      vec3_new(matrix.elements[12], matrix.elements[13], matrix.elements[14]);
  if (!physics_positive_scale(*scale) || !physics_vec_finite(*position) ||
      fabsf(matrix.elements[3]) > 1e-6f || fabsf(matrix.elements[7]) > 1e-6f ||
      fabsf(matrix.elements[11]) > 1e-6f ||
      fabsf(matrix.elements[15] - 1) > 1e-6f) {
    return physics_fail(
        error,
        "Physics requires finite affine transforms and positive nonzero scale");
  }
  x = vec3_scale(x, 1.0f / scale->x);
  y = vec3_scale(y, 1.0f / scale->y);
  z = vec3_scale(z, 1.0f / scale->z);
  if (fabsf(vec3_dot(x, y)) > 1e-4f || fabsf(vec3_dot(y, z)) > 1e-4f ||
      fabsf(vec3_dot(z, x)) > 1e-4f || vec3_dot(vec3_cross(x, y), z) < 0.999f) {
    return physics_fail(error, "Physics rejects shear and reflected "
                               "transforms; bake geometry instead");
  }
  *rotation = vkr_quat_look_at(vec3_scale(z, -1.0f), y);
  return true_v;
}

static VkrEntityId physics_resolve_source(const VkrScene *scene,
                                          const SceneSourceIdentity *source) {
  if (!source->source_fingerprint) {
    return VKR_ENTITY_ID_INVALID;
  }
  for (uint32_t i = 0; i < scene->topo_count; ++i) {
    const SceneSourceIdentity *candidate =
        vkr_entity_get_component_if_alive_const(
            scene->world, scene->topo_order[i], scene->comp_source_identity);
    if (candidate &&
        candidate->source_fingerprint == source->source_fingerprint &&
        candidate->scene_entity_index == source->scene_entity_index &&
        candidate->gltf_node_index == source->gltf_node_index &&
        candidate->gltf_mesh_index == source->gltf_mesh_index &&
        candidate->gltf_camera_index == source->gltf_camera_index &&
        candidate->gltf_skin_index == source->gltf_skin_index &&
        candidate->gltf_light_index == source->gltf_light_index) {
      return scene->topo_order[i];
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

/* Compose from authored locals so paused edits need no prior render update.
 * A simulated dynamic ancestor supplies its evaluated world pose at runtime;
 * Reset instead composes the complete authored hierarchy. */
static bool8_t physics_entity_matrix(const VkrScene *scene, VkrEntityId entity,
                                     bool8_t evaluated,
                                     const PhysicsTransformOverride *override,
                                     Mat4 *matrix, const char **error) {
  *matrix = mat4_identity();
  VkrEntityId cursor = entity;
  for (uint32_t depth = 0; cursor.u64 != VKR_ENTITY_ID_INVALID.u64; ++depth) {
    if (depth >= scene->world->dir.capacity) {
      return physics_fail(error,
                          "Physics transform hierarchy contains a cycle");
    }
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        scene->world, cursor, scene->comp_transform);
    if (!transform) {
      return physics_fail(error, "Physics transform parent is missing");
    }
    if (depth && evaluated &&
        (!override || override->entity.u64 != cursor.u64)) {
      ScenePhysicsCharacter *parent_character =
          physics_character(scene, cursor);
      if (parent_character) {
        const float32_t *position = parent_character->current.foot_position;
        const Mat4 parent =
            physics_trs(vec3_new(position[0], position[1], position[2]),
                        transform->rotation, vec3_one());
        *matrix = mat4_mul(parent, *matrix);
        return true_v;
      }
      ScenePhysicsBody *parent_body = physics_body(scene, cursor);
      if (parent_body && parent_body->authored.motion == VKR_PHYSICS_DYNAMIC &&
          parent_body->body != VKR_PHYSICS_BODY_INVALID) {
        const VkrPhysicsPose *pose = &parent_body->current_pose;
        Mat4 parent = physics_trs(
            vec3_new(pose->position[0], pose->position[1], pose->position[2]),
            vkr_quat_new(pose->rotation[0], pose->rotation[1],
                         pose->rotation[2], pose->rotation[3]),
            parent_body->world_scale);
        *matrix = mat4_mul(parent, *matrix);
        return true_v;
      }
    }
    Mat4 local;
    VkrEntityId parent;
    if (override && override->entity.u64 == cursor.u64) {
      if (!physics_positive_scale(override->scale) ||
          !vkr_scene_physics_pose_valid(override->position,
                                        override->rotation)) {
        return physics_fail(error,
                            "Invalid candidate physics ancestor transform");
      }
      local =
          physics_trs(override->position, override->rotation, override->scale);
      parent = override->parent;
    } else {
      if (!transform->matrix_authored &&
          (!physics_positive_scale(transform->scale) ||
           !vkr_scene_physics_pose_valid(transform->position,
                                         transform->rotation))) {
        return physics_fail(error, "Physics ancestor has invalid TRS");
      }
      local = transform->matrix_authored
                  ? transform->local
                  : physics_trs(transform->position, transform->rotation,
                                transform->scale);
      parent = transform->parent;
    }
    *matrix = mat4_mul(local, *matrix);
    cursor = parent;
  }
  return true_v;
}

static bool8_t physics_body_matrix(const VkrScene *scene, VkrEntityId entity,
                                   const VkrScenePhysicsSnapshot *snapshot,
                                   bool8_t evaluated,
                                   const PhysicsTransformOverride *override,
                                   Mat4 *matrix, const char **error) {
  if (snapshot->attachment.enabled) {
    const VkrEntityId wrapper =
        physics_resolve_source(scene, &snapshot->attachment.animation_source);
    VkrAnimationPlayer *player = vkr_scene_animation_get_player(scene, wrapper);
    const VkrAnimationAsset *asset = vkr_animation_player_asset(player);
    Mat4 wrapper_world;
    if (!player || !asset ||
        snapshot->attachment.source_node >= asset->node_count ||
        !physics_entity_matrix(scene, wrapper, evaluated, override,
                               &wrapper_world, error)) {
      return physics_fail(
          error,
          "Physics bone attachment cannot resolve its animation wrapper/node");
    }
    ScenePhysicsBody *wrapper_body = physics_body(scene, wrapper);
    if (evaluated && wrapper_body &&
        wrapper_body->authored.motion == VKR_PHYSICS_DYNAMIC &&
        (!override || override->entity.u64 != wrapper.u64)) {
      vkr_scene_physics_resolve_world(scene, wrapper, &wrapper_world);
    }
    *matrix =
        mat4_mul(wrapper_world, vkr_animation_player_global_pose(
                                    player)[snapshot->attachment.source_node]);
    *matrix = mat4_mul(*matrix,
                       physics_trs(snapshot->attachment.position,
                                   snapshot->attachment.rotation, vec3_one()));
    return true_v;
  }
  return physics_entity_matrix(scene, entity, evaluated, override, matrix,
                               error);
}

static bool8_t
physics_validate_scaled_shapes(const VkrScenePhysicsSnapshot *snapshot,
                               Vec3 body_scale, const char **error) {
  for (uint32_t i = 0; i < snapshot->collider_count; ++i) {
    const VkrSceneColliderConfig *shape = &snapshot->colliders[i];
    Mat4 local =
        mat4_mul(mat4_scale(body_scale),
                 physics_trs(shape->position, shape->rotation, shape->scale));
    Vec3 position, scale;
    VkrQuat rotation;
    if (!physics_decompose(local, &position, &rotation, &scale, error)) {
      return false_v;
    }
    if ((shape->shape == VKR_PHYSICS_SPHERE ||
         shape->shape == VKR_PHYSICS_CAPSULE) &&
        (fabsf(scale.x - scale.y) > 1e-5f * scale.x ||
         fabsf(scale.x - scale.z) > 1e-5f * scale.x)) {
      return physics_fail(
          error, "Sphere and capsule collision require uniform composed scale; "
                 "use a convex asset for anisotropic round geometry");
    }
  }
  return true_v;
}

VkrScenePhysicsSnapshot vkr_scene_physics_default(void) {
  VkrScenePhysicsSnapshot result = {
      .present = true_v,
      .motion = VKR_PHYSICS_DYNAMIC,
      .collision_layer = 1,
      .collision_mask = UINT16_MAX,
      .mass = 1.0f,
      .friction = 0.5f,
      .gravity_factor = 1.0f,
      .linear_damping = 0.05f,
      .angular_damping = 0.05f,
      .enabled = true_v,
      .allow_sleep = true_v,
      .collider_count = 1,
  };
  result.colliders[0] = (VkrSceneColliderConfig){
      .authored_id = 1,
      .shape = VKR_PHYSICS_BOX,
      .rotation = vkr_quat_identity(),
      .scale = vec3_one(),
      .half_extent = vec3_new(0.5f, 0.5f, 0.5f),
      .radius = 0.5f,
      .half_height = 0.5f,
      .enabled = true_v,
  };
  return result;
}

bool8_t
vkr_scene_physics_snapshot_validate(const VkrScenePhysicsSnapshot *snapshot,
                                    const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!snapshot) {
    return true_v;
  }
  if ((snapshot->present != false_v && snapshot->present != true_v) ||
      (snapshot->enabled != false_v && snapshot->enabled != true_v) ||
      (snapshot->allow_sleep != false_v && snapshot->allow_sleep != true_v) ||
      (snapshot->continuous != false_v && snapshot->continuous != true_v) ||
      (snapshot->sensor != false_v && snapshot->sensor != true_v) ||
      snapshot->collider_count > VKR_SCENE_PHYSICS_MAX_COLLIDERS ||
      snapshot->joint_count > VKR_SCENE_PHYSICS_MAX_JOINTS ||
      (!snapshot->present &&
       (snapshot->collider_count != 0 || snapshot->joint_count != 0))) {
    return physics_fail(error, "Invalid physics flags or collider count");
  }
  if (!snapshot->present) {
    return true_v;
  }
  if (snapshot->motion < VKR_PHYSICS_STATIC ||
      snapshot->motion > VKR_PHYSICS_DYNAMIC || !isfinite(snapshot->mass) ||
      snapshot->mass <= 0.0f || !isfinite(snapshot->friction) ||
      snapshot->friction < 0.0f || snapshot->friction > 1.0f ||
      !isfinite(snapshot->restitution) || snapshot->restitution < 0.0f ||
      snapshot->restitution > 1.0f || !isfinite(snapshot->gravity_factor) ||
      snapshot->gravity_factor < 0.0f || snapshot->gravity_factor > 100.0f ||
      !isfinite(snapshot->linear_damping) || snapshot->linear_damping < 0.0f ||
      snapshot->linear_damping > 1.0f || !isfinite(snapshot->angular_damping) ||
      snapshot->angular_damping < 0.0f || snapshot->angular_damping > 1.0f) {
    return physics_fail(error, "Invalid physics body settings");
  }
  if (snapshot->collider_count > VKR_SCENE_PHYSICS_MAX_COLLIDERS) {
    return physics_fail(error, "A body supports at most 32 colliders");
  }
  if ((snapshot->attachment.enabled != false_v &&
       snapshot->attachment.enabled != true_v) ||
      (snapshot->attachment.drive_bone != false_v &&
       snapshot->attachment.drive_bone != true_v) ||
      (snapshot->attachment.enabled &&
       (!snapshot->attachment.animation_source.source_fingerprint ||
        !vkr_scene_physics_pose_valid(snapshot->attachment.position,
                                      snapshot->attachment.rotation))) ||
      (snapshot->attachment.drive_bone &&
       (!snapshot->attachment.enabled ||
        snapshot->motion != VKR_PHYSICS_DYNAMIC))) {
    return physics_fail(error, "Invalid bone attachment or ragdoll drive mode");
  }
  for (uint32_t i = 0; i < snapshot->joint_count; ++i) {
    const VkrSceneJointConfig *joint = &snapshot->joints[i];
    if (!joint->authored_id || !joint->target_source.source_fingerprint ||
        joint->type < VKR_PHYSICS_JOINT_FIXED ||
        joint->type > VKR_PHYSICS_JOINT_SWING_TWIST ||
        !physics_vec_finite(joint->anchor_a) ||
        !physics_vec_finite(joint->anchor_b) ||
        !physics_vec_finite(joint->axis_a) ||
        !physics_vec_finite(joint->axis_b) ||
        !physics_vec_finite(joint->normal_a) ||
        !physics_vec_finite(joint->normal_b) ||
        fabsf(vec3_length_squared(joint->axis_a) - 1) > 1e-4f ||
        fabsf(vec3_length_squared(joint->axis_b) - 1) > 1e-4f ||
        fabsf(vec3_length_squared(joint->normal_a) - 1) > 1e-4f ||
        fabsf(vec3_length_squared(joint->normal_b) - 1) > 1e-4f ||
        fabsf(vec3_dot(joint->axis_a, joint->normal_a)) > 1e-4f ||
        fabsf(vec3_dot(joint->axis_b, joint->normal_b)) > 1e-4f ||
        !isfinite(joint->min_limit) || !isfinite(joint->max_limit) ||
        !isfinite(joint->swing_normal_limit) ||
        !isfinite(joint->swing_plane_limit) ||
        joint->min_limit > joint->max_limit ||
        (joint->enabled != false_v && joint->enabled != true_v)) {
      return physics_fail(error, "Invalid joint identity, frames or limits");
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (snapshot->joints[j].authored_id == joint->authored_id) {
        return physics_fail(error, "Duplicate authored joint ID");
      }
    }
  }
  for (uint32_t i = 0; i < snapshot->collider_count; ++i) {
    const VkrSceneColliderConfig *shape = &snapshot->colliders[i];
    if ((shape->enabled != false_v && shape->enabled != true_v) ||
        !shape->authored_id || shape->shape < VKR_PHYSICS_BOX ||
        shape->shape > VKR_PHYSICS_TRIANGLE_MESH ||
        !physics_vec_finite(shape->position) ||
        !physics_rotation_valid(shape->rotation) ||
        !physics_positive_scale(shape->scale) ||
        !physics_vec_finite(shape->half_extent) || shape->half_extent.x <= 0 ||
        shape->half_extent.y <= 0 || shape->half_extent.z <= 0 ||
        !isfinite(shape->radius) || shape->radius <= 0 ||
        shape->radius > 1.0e7f || !isfinite(shape->half_height) ||
        shape->half_height < 0 || shape->half_height > 1.0e7f) {
      return physics_fail(error,
                          "Invalid collider ID, primitive dimensions or pose");
    }
    bool8_t terminated = false_v;
    for (uint32_t c = 0; c < sizeof(shape->asset_path); ++c) {
      if (shape->asset_path[c] == 0) {
        terminated = true_v;
        break;
      }
    }
    if (!terminated ||
        ((shape->shape == VKR_PHYSICS_CONVEX_HULL ||
          shape->shape == VKR_PHYSICS_TRIANGLE_MESH) &&
         !shape->asset_path[0]) ||
        (shape->shape == VKR_PHYSICS_TRIANGLE_MESH &&
         snapshot->motion == VKR_PHYSICS_DYNAMIC)) {
      return physics_fail(error,
                          "Cooked collision requires a bounded asset path; "
                          "triangle meshes are static/kinematic only");
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (snapshot->colliders[j].authored_id == shape->authored_id) {
        return physics_fail(
            error, "Collider authored IDs must be unique within a body");
      }
    }
  }
  return true_v;
}

bool8_t vkr_scene_physics_validate(const VkrScene *scene, VkrEntityId entity,
                                   const VkrScenePhysicsSnapshot *snapshot,
                                   const char **error) {
  if (!vkr_scene_physics_snapshot_validate(snapshot, error)) {
    return false_v;
  }
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    return physics_fail(error, "Physics owner is not a live scene entity");
  }
  if (vkr_scene_physics_owner(scene, entity).u64 != VKR_ENTITY_ID_INVALID.u64 &&
      vkr_scene_physics_owner(scene, entity).u64 != entity.u64) {
    return physics_fail(error, "Collider children cannot own bodies");
  }
  if (!snapshot || !snapshot->present) {
    return true_v;
  }
  Mat4 matrix;
  Vec3 position, scale;
  VkrQuat rotation;
  if (!physics_body_matrix(scene, entity, snapshot,
                           !scene->physics || !scene->physics->resetting, NULL,
                           &matrix, error) ||
      !physics_decompose(matrix, &position, &rotation, &scale, error) ||
      !physics_validate_scaled_shapes(snapshot, scale, error)) {
    return false_v;
  }
  return true_v;
}

bool8_t vkr_scene_physics_register(VkrScene *scene) {
  scene->physics_paused = true_v;
  scene->comp_physics_body = vkr_entity_register_component_once(
      scene->world, "ScenePhysicsBody", sizeof(ScenePhysicsBodyComponent),
      AlignOf(ScenePhysicsBodyComponent));
  scene->comp_physics_collider = vkr_entity_register_component_once(
      scene->world, "ScenePhysicsCollider", sizeof(ScenePhysicsCollider),
      AlignOf(ScenePhysicsCollider));
  return scene->comp_physics_body != VKR_COMPONENT_TYPE_INVALID &&
         scene->comp_physics_collider != VKR_COMPONENT_TYPE_INVALID;
}

static bool8_t physics_ensure(VkrScene *scene, const char **error) {
  if (scene->physics) {
    return true_v;
  }
  VkrDMemory memory;
  if (!vkr_dmemory_create(MB(1), MB(64), &memory)) {
    return physics_fail(error, "Physics scene storage allocation failed");
  }
  VkrScenePhysics *physics = vkr_dmemory_alloc(&memory, sizeof(*physics));
  if (!physics) {
    vkr_dmemory_destroy(&memory);
    return physics_fail(error, "Physics scene allocation failed");
  }
  *physics = (VkrScenePhysics){.memory = memory};
  /* Spare capacity supports staging replacement bodies for a full edit batch.
   */
  physics->world = vkr_physics_world_create(VKR_SCENE_PHYSICS_MAX_BODIES * 2u);
  if (!physics->world) {
    vkr_dmemory_destroy(&memory);
    return physics_fail(error, "Physics world creation failed");
  }
  physics->character_component =
      vkr_entity_register_component_once(scene->world, "ScenePhysicsCharacter",
                                         sizeof(uint32_t), AlignOf(uint32_t));
  if (physics->character_component == VKR_COMPONENT_TYPE_INVALID) {
    vkr_physics_world_destroy(physics->world);
    vkr_dmemory_destroy(&memory);
    return physics_fail(error, "Character component registration failed");
  }
  scene->physics = physics;
  return true_v;
}

static bool8_t physics_character_authored(const VkrScene *scene,
                                          VkrEntityId entity,
                                          VkrPhysicsCharacterDesc *settings,
                                          const char **error) {
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  if (!transform || transform->parent.u64 || physics_body(scene, entity)) {
    return physics_fail(error,
                        "Character needs a root transform and no rigid body");
  }
  const Mat4 local = transform->matrix_authored
                         ? transform->local
                         : physics_trs(transform->position, transform->rotation,
                                       transform->scale);
  Vec3 position, scale;
  VkrQuat rotation;
  if (!physics_decompose(local, &position, &rotation, &scale, error) ||
      fabsf(scale.x - 1) > 1e-5f || fabsf(scale.y - 1) > 1e-5f ||
      fabsf(scale.z - 1) > 1e-5f) {
    return physics_fail(error, "Character transform must have unit scale");
  }
  settings->entity_id = entity.u64;
  settings->foot_position[0] = position.x;
  settings->foot_position[1] = position.y;
  settings->foot_position[2] = position.z;
  return true_v;
}

bool8_t vkr_scene_character_create(VkrScene *scene, VkrEntityId entity,
                                   const VkrPhysicsCharacterDesc *settings,
                                   const char **error) {
  if (!scene || !settings || !scene->physics_paused ||
      scene->simulation.active || !vkr_scene_physics_mutations_allowed(scene) ||
      (scene->physics && scene->physics->prepared)) {
    return physics_fail(error, "Create characters at a paused scene boundary");
  }
  VkrPhysicsCharacterDesc desc = *settings;
  if (!physics_character_authored(scene, entity, &desc, error) ||
      !physics_ensure(scene, error)) {
    return false_v;
  }
  if (physics_character(scene, entity)) {
    return physics_fail(error, "Entity already owns a character");
  }
  uint32_t index = 0;
  while (index < ArrayCount(scene->physics->characters) &&
         scene->physics->characters[index].entity.u64) {
    ++index;
  }
  if (index == ArrayCount(scene->physics->characters)) {
    return physics_fail(error, "Scene character capacity exceeded");
  }
  ScenePhysicsCharacter prepared = {.entity = entity, .settings = desc};
  if (!vkr_physics_character_create(scene->physics->world, &desc,
                                    &prepared.native) ||
      !vkr_physics_character_get_state(scene->physics->world, prepared.native,
                                       &prepared.current)) {
    if (prepared.native) {
      vkr_physics_character_destroy(scene->physics->world, prepared.native);
    }
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  prepared.previous = prepared.current;
  if (!vkr_entity_add_component(scene->world, entity,
                                scene->physics->character_component, &index)) {
    vkr_physics_character_destroy(scene->physics->world, prepared.native);
    return physics_fail(error, "Character component attachment failed");
  }
  scene->physics->characters[index] = prepared;
  scene->render_full_sync_needed = true_v;
  return true_v;
}

bool8_t vkr_scene_character_destroy(VkrScene *scene, VkrEntityId entity,
                                    const char **error) {
  ScenePhysicsCharacter *character = physics_character(scene, entity);
  if (!character || !scene->physics_paused || scene->simulation.active ||
      !vkr_scene_physics_mutations_allowed(scene) || scene->physics->prepared) {
    return physics_fail(error, "Destroy characters at a paused scene boundary");
  }
  VkrPhysicsCharacterState state;
  if (!vkr_physics_character_get_state(scene->physics->world, character->native,
                                       &state)) {
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  if (!vkr_entity_remove_component(scene->world, entity,
                                   scene->physics->character_component)) {
    return physics_fail(error, "Character component removal failed");
  }
  if (!vkr_physics_character_destroy(scene->physics->world,
                                     character->native)) {
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  *character = (ScenePhysicsCharacter){0};
  SceneTransform *transform =
      vkr_entity_get_component_mut(scene->world, entity, scene->comp_transform);
  if (transform) {
    transform->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
  }
  scene->render_full_sync_needed = true_v;
  return true_v;
}

bool8_t vkr_scene_character_step(VkrScene *scene, VkrEntityId entity,
                                 const VkrPhysicsCharacterInput *input,
                                 VkrPhysicsCharacterState *state,
                                 const char **error) {
  ScenePhysicsCharacter *character = physics_character(scene, entity);
  if (!character || !input || !state || !scene->simulation.in_callback ||
      scene->simulation.phase != VKR_SCENE_SIMULATION_BEFORE_PHYSICS ||
      !scene->simulation.active || scene->physics_disabled ||
      scene->physics->dispatching || scene->physics->prepared ||
      input->dt != (float32_t)VKR_SCENE_PHYSICS_FIXED_DT ||
      character->last_step_tick == scene->simulation.completed_ticks + 1) {
    return physics_fail(
        error, "Character must advance once in its before-physics tick");
  }
  VkrPhysicsCharacterState result;
  if (!vkr_physics_character_step(scene->physics->world, character->native,
                                  input, &result)) {
    scene->physics->faulted = true_v;
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  character->previous = character->current;
  character->current = result;
  character->last_step_tick = scene->simulation.completed_ticks + 1;
  *state = result;
  return true_v;
}

bool8_t vkr_scene_character_get_state(VkrScene *scene, VkrEntityId entity,
                                      VkrPhysicsCharacterState *state,
                                      const char **error) {
  ScenePhysicsCharacter *character = physics_character(scene, entity);
  if (!character || !state || scene->physics->faulted) {
    return physics_fail(error, "No valid scene character state");
  }
  if (!vkr_physics_character_get_state(scene->physics->world, character->native,
                                       state)) {
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  return true_v;
}

bool8_t vkr_scene_physics_read(const VkrScene *scene, VkrEntityId entity,
                               VkrScenePhysicsSnapshot *snapshot) {
  if (!snapshot || !scene || !vkr_scene_entity_alive(scene, entity)) {
    return false_v;
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  *snapshot = body ? body->authored : (VkrScenePhysicsSnapshot){0};
  return true_v;
}

VkrEntityId vkr_scene_physics_owner(const VkrScene *scene, VkrEntityId entity) {
  if (!scene || !scene->world) {
    return VKR_ENTITY_ID_INVALID;
  }
  const ScenePhysicsCollider *collider = vkr_entity_get_component_if_alive(
      scene->world, entity, scene->comp_physics_collider);
  if (collider && vkr_scene_entity_alive(scene, collider->owner)) {
    return collider->owner;
  }
  return physics_body(scene, entity) ? entity : VKR_ENTITY_ID_INVALID;
}

VkrEntityId vkr_scene_physics_collider_entity(const VkrScene *scene,
                                              VkrEntityId owner,
                                              uint64_t authored_id) {
  ScenePhysicsBody *body = physics_body(scene, owner);
  if (body) {
    for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
      if (body->authored.colliders[i].authored_id == authored_id) {
        return body->colliders[i];
      }
    }
  }
  return VKR_ENTITY_ID_INVALID;
}

static void physics_capture_authored(VkrScene *scene, ScenePhysicsBody *body) {
  const SceneTransform *transform =
      vkr_scene_get_transform(scene, body->entity);
  Mat4 world;
  Vec3 position, scale;
  VkrQuat rotation;
  if (!physics_body_matrix(scene, body->entity, &body->authored,
                           !scene->physics->resetting, NULL, &world, NULL) ||
      !physics_decompose(world, &position, &rotation, &scale, NULL)) {
    return;
  }
  body->authored_world = world;
  body->world_scale = scale;
  body->target_position = position;
  body->target_rotation = rotation;
  body->last_position = transform->position;
  body->last_rotation = transform->rotation;
  body->animation_wrapper =
      body->authored.attachment.enabled
          ? physics_resolve_source(scene,
                                   &body->authored.attachment.animation_source)
          : VKR_ENTITY_ID_INVALID;
  body->current_pose = (VkrPhysicsPose){
      .position = {position.x, position.y, position.z},
      .rotation = {rotation.x, rotation.y, rotation.z, rotation.w}};
  body->previous_pose = body->current_pose;
}

bool8_t vkr_scene_physics_set_asset_root(VkrScene *scene, String8 root,
                                         const char **error) {
  if (!scene || (root.length && !root.str) ||
      root.length >= sizeof(scene->physics_asset_root) ||
      (scene->physics &&
       (scene->physics->body_count || scene->physics->prepared))) {
    return physics_fail(
        error,
        "Set a bounded collision asset root before creating physics bodies");
  }
  for (uint64_t i = 0; i < root.length; ++i) {
    if (root.str[i] == 0) {
      return physics_fail(error,
                          "Collision asset root contains an embedded null");
    }
  }
  MemCopy(scene->physics_asset_root, root.str, root.length);
  scene->physics_asset_root[root.length] = 0;
  return true_v;
}

static bool8_t physics_acquire_assets(VkrScene *scene, ScenePhysicsBody *body,
                                      const ScenePhysicsBody *old,
                                      const char **error) {
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    const VkrSceneColliderConfig *shape = &body->authored.colliders[i];
    if (shape->shape != VKR_PHYSICS_CONVEX_HULL &&
        shape->shape != VKR_PHYSICS_TRIANGLE_MESH) {
      continue;
    }
    if (old) {
      for (uint32_t j = 0; j < old->authored.collider_count; ++j) {
        if (old->assets[j] &&
            string_equals(shape->asset_path,
                          old->authored.colliders[j].asset_path)) {
          if (!vkr_collision_asset_retain(old->assets[j])) {
            return physics_fail(error,
                                "Collision asset reference count overflow");
          }
          body->assets[i] = old->assets[j];
          break;
        }
      }
    }
    if (!body->assets[i]) {
      char path[sizeof(scene->physics_asset_root) +
                VKR_SCENE_COLLISION_ASSET_PATH_MAX + 2];
      const bool8_t absolute =
          shape->asset_path[0] == '/' || shape->asset_path[1] == ':';
      if (absolute || !scene->physics_asset_root[0]) {
        string_format(path, sizeof(path), "%s", shape->asset_path);
      } else {
        string_format(path, sizeof(path), "%s/%s", scene->physics_asset_root,
                      shape->asset_path);
      }
      body->assets[i] = vkr_collision_asset_open(
          string8_create((uint8_t *)path, string_length(path)), error);
      if (!body->assets[i]) {
        return false_v;
      }
    }
    const VkrCollisionGeometry *geometry =
        vkr_collision_asset_geometry(body->assets[i]);
    if (!geometry ||
        (shape->shape == VKR_PHYSICS_CONVEX_HULL &&
         geometry->kind != VKR_COLLISION_CONVEX_HULL) ||
        (shape->shape == VKR_PHYSICS_TRIANGLE_MESH &&
         geometry->kind != VKR_COLLISION_TRIANGLE_MESH)) {
      return physics_fail(
          error, "Cooked collision asset kind does not match the collider");
    }
  }
  return true_v;
}

static void physics_release_assets(ScenePhysicsBody *body) {
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    vkr_collision_asset_close(body->assets[i]);
    body->assets[i] = NULL;
  }
}

static bool8_t physics_create_body(VkrScene *scene, ScenePhysicsBody *body,
                                   VkrPhysicsWorld *world, bool8_t enabled,
                                   VkrPhysicsBody *handle, const char **error) {
  const SceneTransform *transform =
      vkr_scene_get_transform(scene, body->entity);
  if (!transform) {
    return physics_fail(error, "Physics body lost its transform");
  }
  bool8_t has_enabled_shape = false_v;
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    has_enabled_shape |= body->authored.colliders[i].enabled;
  }
  if (!has_enabled_shape) {
    *handle = VKR_PHYSICS_BODY_INVALID;
    return true_v;
  }
  Mat4 body_world;
  Vec3 body_position, body_scale;
  VkrQuat body_rotation;
  if (!physics_body_matrix(scene, body->entity, &body->authored,
                           !scene->physics->resetting, NULL, &body_world,
                           error) ||
      !physics_decompose(body_world, &body_position, &body_rotation,
                         &body_scale, error) ||
      !physics_validate_scaled_shapes(&body->authored, body_scale, error)) {
    return false_v;
  }
  VkrPhysicsColliderDesc shapes[VKR_SCENE_PHYSICS_MAX_COLLIDERS];
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    const VkrSceneColliderConfig *source = &body->authored.colliders[i];
    Vec3 position, scale;
    VkrQuat rotation;
    Mat4 local = mat4_mul(
        mat4_scale(body_scale),
        physics_trs(source->position, source->rotation, source->scale));
    if (!physics_decompose(local, &position, &rotation, &scale, error)) {
      return false_v;
    }
    const VkrCollisionGeometry *geometry =
        body->assets[i] ? vkr_collision_asset_geometry(body->assets[i]) : NULL;
    shapes[i] = (VkrPhysicsColliderDesc){
        .entity_id = body->colliders[i].u64,
        .shape = source->shape,
        .position = {position.x, position.y, position.z},
        .rotation = {rotation.x, rotation.y, rotation.z, rotation.w},
        .scale = {scale.x, scale.y, scale.z},
        .geometry =
            geometry
                ? (VkrPhysicsGeometry){.positions = geometry->positions,
                                       .vertex_count = geometry->vertex_count,
                                       .indices = geometry->indices,
                                       .index_count = geometry->index_count}
                : (VkrPhysicsGeometry){0},
        .half_extent = {source->half_extent.x, source->half_extent.y,
                        source->half_extent.z},
        .radius = source->radius,
        .half_height = source->half_height,
        .enabled = source->enabled,
    };
  }
  const VkrScenePhysicsSnapshot *source = &body->authored;
  VkrPhysicsBodyDesc desc = {
      .entity_id = body->entity.u64,
      .motion = source->motion,
      .collision_layer = source->collision_layer,
      .collision_mask = vkr_scene_collision_layers_effective_mask(
          scene, source->collision_layer, source->collision_mask),
      .position = {body_position.x, body_position.y, body_position.z},
      .rotation = {body_rotation.x, body_rotation.y, body_rotation.z,
                   body_rotation.w},
      .mass = source->mass,
      .friction = source->friction,
      .restitution = source->restitution,
      .gravity_factor = source->gravity_factor,
      .linear_damping = source->linear_damping,
      .angular_damping = source->angular_damping,
      .enabled = enabled && source->enabled &&
                 (!scene->physics_disabled || scene->physics->resetting) &&
                 !body->disabled,
      .allow_sleep = source->allow_sleep,
      .continuous = source->continuous,
      .sensor = source->sensor,
      .colliders = shapes,
      .collider_count = source->collider_count,
  };
  if (!vkr_physics_body_create(world, &desc, handle)) {
    return physics_fail(error, vkr_physics_last_error(world));
  }
  return true_v;
}

static void physics_remove_node(VkrScene *scene, ScenePhysicsBody *body,
                                const ScenePhysicsBody *replacement) {
  VkrScenePhysics *physics = scene->physics;
  ScenePhysicsBody **link = &physics->bodies;
  while (*link && *link != body) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = body->next;
    physics->body_count--;
  }
  if (body->body != VKR_PHYSICS_BODY_INVALID) {
    vkr_physics_body_destroy(physics->world, body->body);
  }
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    bool8_t retained = false_v;
    if (replacement) {
      for (uint32_t j = 0; j < replacement->authored.collider_count; ++j) {
        retained |= body->colliders[i].u64 == replacement->colliders[j].u64;
      }
    }
    if (!retained) {
      vkr_scene_destroy_entity(scene, body->colliders[i]);
    }
  }
  physics_release_assets(body);
  vkr_dmemory_free(&physics->memory, body, sizeof(*body));
}

static void physics_discard_joint_graph(VkrScene *scene);

bool8_t vkr_scene_physics_prepare(VkrScene *scene, VkrEntityId entity,
                                  const VkrScenePhysicsSnapshot *snapshot,
                                  VkrScenePhysicsPrepared **prepared,
                                  const char **error) {
  if (prepared) {
    *prepared = NULL;
  }
  if (!prepared ||
      !vkr_scene_physics_validate(scene, entity, snapshot, error)) {
    return false_v;
  }
  if (!scene->physics_paused || scene->simulation.active) {
    return physics_fail(error, "Pause simulation before editing physics");
  }
  if (snapshot && snapshot->present && physics_character(scene, entity)) {
    return physics_fail(error,
                        "A character entity cannot also own a rigid body");
  }
  if (!physics_ensure(scene, error)) {
    return false_v;
  }
  VkrScenePhysics *physics = scene->physics;
  if (physics->prepared_complete || physics->dispatching) {
    return physics_fail(error, "Cannot add physics edits after graph "
                               "finalization or during contact dispatch");
  }
  for (VkrScenePhysicsPrepared *active = physics->prepared; active;
       active = active->next) {
    if (active->entity.u64 == entity.u64) {
      return physics_fail(error,
                          "This physics owner already has a prepared edit");
    }
  }
  ScenePhysicsBody *old = physics_body(scene, entity);
  if (!old && snapshot && snapshot->present &&
      physics->body_count + physics->staged_additions >=
          VKR_SCENE_PHYSICS_MAX_BODIES) {
    return physics_fail(error, "Scene physics supports at most 1024 bodies");
  }
  VkrScenePhysicsPrepared *pending =
      vkr_dmemory_alloc(&physics->memory, sizeof(*pending));
  if (!pending) {
    return physics_fail(error, "Physics edit allocation failed");
  }
  *pending =
      (VkrScenePhysicsPrepared){.scene = scene, .old = old, .entity = entity};
  if (old && old->body != VKR_PHYSICS_BODY_INVALID) {
    if (!vkr_physics_body_reserve_destroy(physics->world, old->body)) {
      vkr_dmemory_free(&physics->memory, pending, sizeof(*pending));
      return physics_fail(error, vkr_physics_last_error(physics->world));
    }
    pending->destruction_reserved = true_v;
  }
  if (!snapshot || !snapshot->present) {
    pending->next = physics->prepared;
    physics->prepared = pending;
    *prepared = pending;
    return true_v;
  }
  ScenePhysicsBody *body = vkr_dmemory_alloc(&physics->memory, sizeof(*body));
  if (!body) {
    vkr_scene_physics_discard(pending);
    return physics_fail(error, "Physics body allocation failed");
  }
  *body = (ScenePhysicsBody){.entity = entity, .authored = *snapshot};
  physics_capture_authored(scene, body);
  body->disabled = old ? old->disabled : false_v;
  pending->replacement = body;
  if (!old) {
    physics->staged_additions++;
    pending->addition_reserved = true_v;
  }
  physics->editing = true_v;
  scene->queries_valid = false_v;
  scene->child_index_valid = false_v;
  for (uint32_t i = 0; i < snapshot->collider_count; ++i) {
    const VkrSceneColliderConfig *shape = &snapshot->colliders[i];
    body->colliders[i] =
        vkr_scene_physics_collider_entity(scene, entity, shape->authored_id);
    if (body->colliders[i].u64 != VKR_ENTITY_ID_INVALID.u64) {
      continue;
    }
    body->colliders[i] = vkr_scene_create_entity(scene, NULL);
    if (body->colliders[i].u64 == VKR_ENTITY_ID_INVALID.u64) {
      physics_fail(error, "Collider entity allocation failed");
      goto cleanup;
    }
    pending->new_collider[i] = true_v;
    ScenePhysicsCollider collider = {.owner = entity,
                                     .authored_id = shape->authored_id};
    SceneName name = {.name = string8_lit("Collider")};
    if (!vkr_entity_add_component(scene->world, body->colliders[i],
                                  scene->comp_physics_collider, &collider) ||
        !vkr_entity_add_component(scene->world, body->colliders[i],
                                  scene->comp_name, &name) ||
        !vkr_scene_set_transform(scene, body->colliders[i], shape->position,
                                 shape->rotation, shape->scale)) {
      physics_fail(error, "Collider component allocation failed");
      goto cleanup;
    }
    vkr_scene_set_parent(scene, body->colliders[i], entity);
  }
  if (!vkr_entity_has_component(scene->world, entity,
                                scene->comp_physics_body)) {
    ScenePhysicsBodyComponent component = {0};
    if (!vkr_entity_add_component(scene->world, entity,
                                  scene->comp_physics_body, &component)) {
      physics_fail(error, "Body component allocation failed");
      goto cleanup;
    }
    pending->added_component = true_v;
  }
  if (!physics_acquire_assets(scene, body, old, error) ||
      !physics_create_body(scene, body, physics->world, false_v, &body->body,
                           error)) {
    goto cleanup;
  }
  if (old && old->body && body->body &&
      MemCompare(&old->authored, &body->authored, sizeof(body->authored)) ==
          0 &&
      MemCompare(&old->authored_world, &body->authored_world,
                 sizeof(body->authored_world)) == 0) {
    VkrPhysicsPose pose;
    if (!vkr_physics_body_get_pose(physics->world, old->body, &pose) ||
        !vkr_physics_body_set_pose(physics->world, body->body, pose.position,
                                   pose.rotation) ||
        !vkr_physics_body_set_velocity(physics->world, body->body,
                                       pose.linear_velocity,
                                       pose.angular_velocity)) {
      physics_fail(error, vkr_physics_last_error(physics->world));
      goto cleanup;
    }
    body->current_pose = pose;
    body->previous_pose = old->previous_pose;
  }
  physics->editing = false_v;
  pending->next = physics->prepared;
  physics->prepared = pending;
  *prepared = pending;
  return true_v;
cleanup:
  physics->editing = false_v;
  vkr_scene_physics_discard(pending);
  return false_v;
}

static ScenePhysicsBody *physics_future_body(VkrScene *scene,
                                             VkrEntityId entity) {
  if (scene->physics->resetting) {
    for (ScenePhysicsBody *body = scene->physics->reset_bodies; body;
         body = body->next) {
      if (body->entity.u64 == entity.u64) {
        return body;
      }
    }
    return NULL;
  }
  for (VkrScenePhysicsPrepared *pending = scene->physics->prepared; pending;
       pending = pending->next) {
    if (pending->entity.u64 == entity.u64) {
      return pending->replacement;
    }
  }
  return physics_body(scene, entity);
}

static void physics_discard_joint_graph(VkrScene *scene) {
  VkrScenePhysics *physics = scene->physics;
  while (physics->staged_joints) {
    PhysicsStagedJoints *entry = physics->staged_joints;
    physics->staged_joints = entry->next;
    for (uint32_t i = 0; i < entry->body->authored.joint_count; ++i) {
      if (entry->handles[i]) {
        vkr_physics_joint_destroy(physics->world, entry->handles[i]);
      }
    }
    vkr_dmemory_free(&physics->memory, entry, sizeof(*entry));
  }
  physics->prepared_complete = false_v;
}

static bool8_t physics_stage_body_joints(VkrScene *scene,
                                         ScenePhysicsBody *body,
                                         const char **error) {
  if (!body || !body->authored.joint_count) {
    return true_v;
  }
  VkrScenePhysics *physics = scene->physics;
  PhysicsStagedJoints *entry =
      vkr_dmemory_alloc(&physics->memory, sizeof(*entry));
  if (!entry) {
    return physics_fail(error, "Joint graph staging allocation failed");
  }
  *entry = (PhysicsStagedJoints){.next = physics->staged_joints, .body = body};
  physics->staged_joints = entry;
  for (uint32_t i = 0; i < body->authored.joint_count; ++i) {
    const VkrSceneJointConfig *source = &body->authored.joints[i];
    ScenePhysicsBody *target = physics_future_body(
        scene, physics_resolve_source(scene, &source->target_source));
    /* Retain unresolved authored endpoints as suspended links so owner deletion
     * does not silently destroy another object's persisted joint description.
     */
    if (target == body) {
      return physics_fail(error, "Joint endpoints must be different bodies");
    }
    if (!target || !body->body || !target->body) {
      continue;
    }
    VkrPhysicsJointDesc desc = {
        .type = source->type,
        .body_a = body->body,
        .body_b = target->body,
        .anchor_a = {source->anchor_a.x * body->world_scale.x,
                     source->anchor_a.y * body->world_scale.y,
                     source->anchor_a.z * body->world_scale.z},
        .anchor_b = {source->anchor_b.x * target->world_scale.x,
                     source->anchor_b.y * target->world_scale.y,
                     source->anchor_b.z * target->world_scale.z},
        .axis_a = {source->axis_a.x, source->axis_a.y, source->axis_a.z},
        .axis_b = {source->axis_b.x, source->axis_b.y, source->axis_b.z},
        .normal_a = {source->normal_a.x, source->normal_a.y,
                     source->normal_a.z},
        .normal_b = {source->normal_b.x, source->normal_b.y,
                     source->normal_b.z},
        .min_limit = source->min_limit,
        .max_limit = source->max_limit,
        .swing_normal_limit = source->swing_normal_limit,
        .swing_plane_limit = source->swing_plane_limit,
        .enabled = false_v,
    };
    if (!vkr_physics_joint_create(physics->world, &desc, &entry->handles[i])) {
      return physics_fail(error, vkr_physics_last_error(physics->world));
    }
  }
  return true_v;
}

bool8_t vkr_scene_physics_prepare_complete(VkrScene *scene,
                                           const char **error) {
  if (!scene || !scene->physics || scene->physics->prepared_complete) {
    return true_v;
  }
  VkrScenePhysics *physics = scene->physics;
  ScenePhysicsBody *writers[VKR_SCENE_PHYSICS_MAX_BODIES];
  uint32_t writer_count = 0;
  for (ScenePhysicsBody *old = physics->bodies; old; old = old->next) {
    ScenePhysicsBody *future = physics_future_body(scene, old->entity);
    if (future && future->authored.attachment.drive_bone) {
      writers[writer_count++] = future;
    }
  }
  for (VkrScenePhysicsPrepared *pending = physics->prepared; pending;
       pending = pending->next) {
    if (!pending->old && pending->replacement &&
        pending->replacement->authored.attachment.drive_bone) {
      writers[writer_count++] = pending->replacement;
    }
  }
  for (uint32_t i = 0; i < writer_count; ++i) {
    for (uint32_t j = 0; j < i; ++j) {
      if (writers[i]->animation_wrapper.u64 ==
              writers[j]->animation_wrapper.u64 &&
          writers[i]->authored.attachment.source_node ==
              writers[j]->authored.attachment.source_node) {
        return physics_fail(error,
                            "Only one rigid body may drive an animation bone");
      }
    }
  }
  for (ScenePhysicsBody *old = physics->bodies; old; old = old->next) {
    ScenePhysicsBody *future = physics_future_body(scene, old->entity);
    if (!physics_stage_body_joints(scene, future, error)) {
      physics_discard_joint_graph(scene);
      return false_v;
    }
  }
  for (VkrScenePhysicsPrepared *pending = physics->prepared; pending;
       pending = pending->next) {
    if (!pending->old &&
        !physics_stage_body_joints(scene, pending->replacement, error)) {
      physics_discard_joint_graph(scene);
      return false_v;
    }
  }
  physics->prepared_complete = true_v;
  return true_v;
}

static void physics_remove_old_joints(VkrScene *scene) {
  for (ScenePhysicsBody *body = scene->physics->bodies; body;
       body = body->next) {
    for (uint32_t i = 0; i < body->authored.joint_count; ++i) {
      if (body->joints[i]) {
        vkr_physics_joint_destroy(scene->physics->world, body->joints[i]);
        body->joints[i] = VKR_PHYSICS_JOINT_INVALID;
      }
    }
  }
}

static void physics_publish_joint_graph(VkrScene *scene) {
  VkrScenePhysics *physics = scene->physics;
  while (physics->staged_joints) {
    PhysicsStagedJoints *entry = physics->staged_joints;
    physics->staged_joints = entry->next;
    for (uint32_t i = 0; i < entry->body->authored.joint_count; ++i) {
      entry->body->joints[i] = entry->handles[i];
      if (entry->handles[i]) {
        vkr_physics_joint_set_enabled(physics->world, entry->handles[i],
                                      entry->body->authored.joints[i].enabled);
      }
    }
    vkr_dmemory_free(&physics->memory, entry, sizeof(*entry));
  }
  physics->prepared_complete = false_v;
}

static void physics_unlink_prepared(VkrScenePhysicsPrepared *prepared) {
  VkrScenePhysicsPrepared **link = &prepared->scene->physics->prepared;
  while (*link && *link != prepared) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = prepared->next;
  }
}

void vkr_scene_physics_discard(VkrScenePhysicsPrepared *prepared) {
  if (!prepared) {
    return;
  }
  VkrScene *scene = prepared->scene;
  physics_discard_joint_graph(scene);
  physics_unlink_prepared(prepared);
  VkrScenePhysics *physics = scene->physics;
  physics->editing = true_v;
  if (prepared->destruction_reserved) {
    vkr_physics_body_cancel_destroy(physics->world, prepared->old->body);
  }
  if (prepared->addition_reserved) {
    physics->staged_additions--;
  }
  if (prepared->replacement) {
    ScenePhysicsBody *body = prepared->replacement;
    if (body->body != VKR_PHYSICS_BODY_INVALID) {
      vkr_physics_body_destroy(physics->world, body->body);
    }
    for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
      if (prepared->new_collider[i]) {
        vkr_scene_destroy_entity(scene, body->colliders[i]);
      }
    }
    physics_release_assets(body);
    vkr_dmemory_free(&physics->memory, body, sizeof(*body));
  }
  if (prepared->added_component) {
    vkr_entity_remove_component(scene->world, prepared->entity,
                                scene->comp_physics_body);
  }
  physics->editing = false_v;
  scene->render_full_sync_needed = true_v;
  vkr_dmemory_free(&physics->memory, prepared, sizeof(*prepared));
}

void vkr_scene_physics_commit(VkrScenePhysicsPrepared *prepared) {
  if (!prepared) {
    return;
  }
  if (!prepared->scene->physics->prepared_complete) {
    prepared->scene->physics->error =
        "Finalize the physics graph before committing edits";
    return;
  }
  physics_remove_old_joints(prepared->scene);
  physics_unlink_prepared(prepared);
  VkrScene *scene = prepared->scene;
  VkrScenePhysics *physics = scene->physics;
  ScenePhysicsBody *body = prepared->replacement;
  if (prepared->addition_reserved) {
    physics->staged_additions--;
  }
  physics->editing = true_v;
  ScenePhysicsBodyComponent *component = vkr_entity_get_component_mut(
      scene->world, prepared->entity, scene->comp_physics_body);
  if (component) {
    component->body = body;
  }
  if (prepared->old) {
    physics_remove_node(scene, prepared->old, body);
  }
  if (body) {
    body->next = physics->bodies;
    physics->bodies = body;
    physics->body_count++;
    for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
      const VkrSceneColliderConfig *shape = &body->authored.colliders[i];
      SceneTransform *transform =
          vkr_scene_get_transform(scene, body->colliders[i]);
      transform->position = shape->position;
      transform->rotation = shape->rotation;
      transform->scale = shape->scale;
      transform->flags |=
          SCENE_TRANSFORM_DIRTY_LOCAL | SCENE_TRANSFORM_DIRTY_WORLD;
    }
    if (body->body != VKR_PHYSICS_BODY_INVALID) {
      vkr_physics_body_set_enabled(physics->world, body->body,
                                   body->authored.enabled &&
                                       !scene->physics_disabled &&
                                       !body->disabled);
    }
  } else {
    /* Keep an empty marker: removal itself must not allocate during commit. */
    SceneTransform *transform =
        vkr_scene_get_transform(scene, prepared->entity);
    if (transform) {
      transform->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }
  physics->editing = false_v;
  scene->hierarchy_dirty = true_v;
  scene->render_full_sync_needed = true_v;
  scene->structure_revision++;
  if (!physics->prepared) {
    physics_publish_joint_graph(scene);
  }
  vkr_dmemory_free(&physics->memory, prepared, sizeof(*prepared));
}

bool8_t vkr_scene_physics_apply(VkrScene *scene, VkrEntityId entity,
                                const VkrScenePhysicsSnapshot *snapshot,
                                const char **error) {
  VkrScenePhysicsPrepared *prepared = NULL;
  if (!vkr_scene_physics_prepare(scene, entity, snapshot, &prepared, error)) {
    return false_v;
  }
  if (!vkr_scene_physics_prepare_complete(scene, error)) {
    vkr_scene_physics_discard(prepared);
    return false_v;
  }
  vkr_scene_physics_commit(prepared);
  return true_v;
}

uint32_t vkr_scene_physics_body_count(const VkrScene *scene) {
  return scene && scene->physics ? scene->physics->body_count : 0;
}

VkrEntityId vkr_scene_physics_body_at(const VkrScene *scene, uint32_t index) {
  ScenePhysicsBody *body =
      scene && scene->physics ? scene->physics->bodies : NULL;
  while (body && index) {
    body = body->next;
    index--;
  }
  return body ? body->entity : VKR_ENTITY_ID_INVALID;
}

void vkr_scene_physics_set_paused(VkrScene *scene, bool8_t paused) {
  if (scene && vkr_scene_physics_mutations_allowed(scene)) {
    if (!paused && scene->physics_paused) {
      scene->simulation.overload_updates = 0;
      if (!scene->simulation.faulted) {
        scene->simulation.error = NULL;
      }
      if (scene->physics && !scene->physics->faulted) {
        scene->physics->error = NULL;
      }
    }
    scene->physics_paused = paused;
  }
}

bool8_t vkr_scene_physics_is_paused(const VkrScene *scene) {
  return !scene || scene->physics_paused;
}

static bool8_t physics_sync_authored(VkrScene *scene, const char **error) {
  VkrScenePhysics *physics = scene->physics;
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    const SceneTransform *transform =
        vkr_scene_get_transform(scene, body->entity);
    const bool8_t local_changed =
        transform->position.x != body->last_position.x ||
        transform->position.y != body->last_position.y ||
        transform->position.z != body->last_position.z ||
        transform->rotation.x != body->last_rotation.x ||
        transform->rotation.y != body->last_rotation.y ||
        transform->rotation.z != body->last_rotation.z ||
        transform->rotation.w != body->last_rotation.w;
    if (body->authored.motion == VKR_PHYSICS_DYNAMIC &&
        !scene->physics_paused && !local_changed) {
      continue;
    }
    Mat4 world;
    Vec3 position, scale;
    VkrQuat rotation;
    if (!physics_body_matrix(scene, body->entity, &body->authored, true_v, NULL,
                             &world, error) ||
        !physics_decompose(world, &position, &rotation, &scale, error)) {
      return false_v;
    }
    if (MemCompare(&world, &body->authored_world, sizeof(world)) == 0) {
      continue;
    }
    if (fabsf(scale.x - body->world_scale.x) > 1e-5f ||
        fabsf(scale.y - body->world_scale.y) > 1e-5f ||
        fabsf(scale.z - body->world_scale.z) > 1e-5f) {
      if (!scene->physics_paused) {
        return physics_fail(
            error,
            "Collision scale changed during simulation; pause and rebuild");
      }
      VkrScenePhysicsSnapshot snapshot = body->authored;
      if (!vkr_scene_physics_apply(scene, body->entity, &snapshot, error)) {
        return false_v;
      }
      return physics_sync_authored(scene, error);
    }
    body->authored_world = world;
    body->last_position = transform->position;
    body->last_rotation = transform->rotation;
    body->target_position = position;
    body->target_rotation = rotation;
    if (body->body != VKR_PHYSICS_BODY_INVALID &&
        (scene->physics_paused ||
         body->authored.motion == VKR_PHYSICS_STATIC)) {
      const float32_t p[3] = {position.x, position.y, position.z};
      const float32_t r[4] = {rotation.x, rotation.y, rotation.z, rotation.w};
      if (!vkr_physics_body_set_pose(physics->world, body->body, p, r) ||
          !vkr_physics_body_get_pose(physics->world, body->body,
                                     &body->current_pose)) {
        return physics_fail(error, vkr_physics_last_error(physics->world));
      }
      body->previous_pose = body->current_pose;
    }
  }
  return true_v;
}

bool8_t vkr_scene_physics_tick(VkrScene *scene, const char **error) {
  VkrScenePhysics *physics = scene->physics;
  if (physics) {
    for (uint32_t i = 0; i < ArrayCount(physics->characters); ++i) {
      ScenePhysicsCharacter *character = &physics->characters[i];
      if (character->entity.u64 &&
          character->last_step_tick != scene->simulation.completed_ticks + 1) {
        character->previous = character->current;
      }
    }
  }
  if (!physics || !physics->body_count || scene->physics_disabled) {
    if (scene->simulation.enabled && !scene->physics_disabled) {
      vkr_scene_animation_update(scene, VKR_SCENE_PHYSICS_FIXED_DT);
    }
    return true_v;
  }
  if (physics->prepared) {
    return physics_fail(error,
                        "Complete prepared physics edits before stepping");
  }
  if (physics->faulted) {
    return physics_fail(error,
                        "Reset the faulted physics simulation before stepping");
  }
  vkr_scene_animation_update(scene, VKR_SCENE_PHYSICS_FIXED_DT);
  if (!vkr_scene_physics_publish_bones(scene, false_v, error) ||
      !physics_sync_authored(scene, error)) {
    return false_v;
  }
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    if (body->authored.motion == VKR_PHYSICS_KINEMATIC &&
        body->authored.enabled && !body->disabled &&
        body->body != VKR_PHYSICS_BODY_INVALID) {
      const float32_t position[3] = {body->target_position.x,
                                     body->target_position.y,
                                     body->target_position.z};
      const float32_t rotation[4] = {
          body->target_rotation.x, body->target_rotation.y,
          body->target_rotation.z, body->target_rotation.w};
      if (!vkr_physics_body_move_kinematic(
              physics->world, body->body, position, rotation,
              (float32_t)VKR_SCENE_PHYSICS_FIXED_DT)) {
        return physics_fail(error, vkr_physics_last_error(physics->world));
      }
    }
  }
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    body->previous_pose = body->current_pose;
  }
  if (!vkr_physics_step(physics->world,
                        (float32_t)VKR_SCENE_PHYSICS_FIXED_DT)) {
    physics->faulted = true_v;
    return physics_fail(error, vkr_physics_last_error(physics->world));
  }
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    if (body->body != VKR_PHYSICS_BODY_INVALID) {
      vkr_physics_body_get_pose(physics->world, body->body,
                                &body->current_pose);
    }
  }
  if (!vkr_scene_physics_publish_bones(scene, false_v, error)) {
    return false_v;
  }
  physics->dispatching = true_v;
  const bool8_t dispatched = vkr_physics_contact_events_dispatch(
      physics->world, physics->contact_callback, physics->contact_context);
  physics->dispatching = false_v;
  if (!dispatched) {
    return physics_fail(error, vkr_physics_last_error(physics->world));
  }
  return true_v;
}

bool8_t vkr_scene_physics_step(VkrScene *scene, const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!scene || !scene->physics_paused) {
    return physics_fail(error, "Single step requires paused simulation");
  }
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return physics_fail(error,
                        "Cannot single step during simulation callbacks");
  }
  if (scene->physics && scene->physics->prepared) {
    return physics_fail(error,
                        "Complete prepared physics edits before stepping");
  }
  if (scene->physics && scene->physics->faulted) {
    return physics_fail(error,
                        "Reset the faulted physics simulation before stepping");
  }
  if (scene->physics && !physics_sync_authored(scene, error)) {
    return false_v;
  }
  bool8_t success = vkr_scene_simulation_tick(scene, error);
  if (!success && scene->physics) {
    scene->physics->error = error ? *error : "Physics step failed";
  }
  return success;
}

void vkr_scene_physics_update(VkrScene *scene, float64_t dt) {
  if (!scene || scene->simulation.active) {
    return;
  }
  VkrScenePhysics *physics = scene->physics;
  if (physics && physics->body_count) {
    if (physics->prepared) {
      physics->error =
          "Complete prepared physics edits before updating simulation";
      return;
    }
    const char *error = NULL;
    if (!physics_sync_authored(scene, &error)) {
      physics->error = error;
      scene->physics_paused = true_v;
      return;
    }
  }
  vkr_scene_simulation_update(scene, dt);
}

bool8_t vkr_scene_physics_reset(VkrScene *scene, const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!scene || !scene->physics_paused || scene->simulation.active ||
      !vkr_scene_physics_mutations_allowed(scene)) {
    return physics_fail(error, "Pause simulation before resetting physics");
  }
  if (!scene->physics) {
    VkrSceneAnimationReset *reset =
        vkr_scene_animation_reset_begin(scene, error);
    if (!reset) {
      return false_v;
    }
    vkr_scene_animation_reset_finish(reset, true_v);
    scene->physics_disabled = false_v;
    vkr_scene_simulation_reset_state(scene);
    return true_v;
  }
  VkrScenePhysics *physics = scene->physics;
  if (physics->prepared || physics->dispatching) {
    return physics_fail(
        error,
        "Complete prepared edits/contact dispatch before resetting simulation");
  }
  VkrPhysicsWorld *replacement =
      vkr_physics_world_create(VKR_SCENE_PHYSICS_MAX_BODIES * 2u);
  if (!replacement) {
    return physics_fail(error, "Physics reset world allocation failed");
  }
  VkrPhysicsWorld *old_world = physics->world;
  VkrPhysicsCharacter reset_characters[VKR_PHYSICS_MAX_CHARACTERS] = {0};
  VkrPhysicsCharacterState reset_states[VKR_PHYSICS_MAX_CHARACTERS];
  VkrSceneAnimationReset *animation_reset =
      vkr_scene_animation_reset_begin(scene, error);
  if (!animation_reset) {
    vkr_physics_world_destroy(replacement);
    return false_v;
  }
  physics->resetting = true_v;
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    ScenePhysicsBody *copy = vkr_dmemory_alloc(&physics->memory, sizeof(*copy));
    if (!copy) {
      physics_fail(error, "Physics reset body allocation failed");
      goto cleanup;
    }
    *copy = *body;
    copy->next = physics->reset_bodies;
    physics->reset_bodies = copy;
    copy->body = VKR_PHYSICS_BODY_INVALID;
    copy->disabled = false_v;
    MemZero(copy->joints, sizeof(copy->joints));
    if (!vkr_scene_physics_validate(scene, copy->entity, &copy->authored,
                                    error) ||
        !physics_create_body(scene, copy, replacement, true_v, &copy->body,
                             error)) {
      goto cleanup;
    }
    physics_capture_authored(scene, copy);
  }
  physics->world = replacement;
  for (ScenePhysicsBody *body = physics->reset_bodies; body;
       body = body->next) {
    if (!physics_stage_body_joints(scene, body, error)) {
      goto cleanup;
    }
  }
  for (uint32_t i = 0; i < ArrayCount(physics->characters); ++i) {
    ScenePhysicsCharacter *character = &physics->characters[i];
    if (!character->entity.u64) {
      continue;
    }
    VkrPhysicsCharacterDesc desc = character->settings;
    if (!physics_character_authored(scene, character->entity, &desc, error)) {
      goto cleanup;
    }
    if (!vkr_physics_character_create(replacement, &desc,
                                      &reset_characters[i]) ||
        !vkr_physics_character_get_state(replacement, reset_characters[i],
                                         &reset_states[i])) {
      physics_fail(error, vkr_physics_last_error(replacement));
      goto cleanup;
    }
  }
  vkr_scene_animation_reset_finish(animation_reset, true_v);
  vkr_physics_world_destroy(old_world);
  while (physics->bodies) {
    ScenePhysicsBody *old = physics->bodies;
    physics->bodies = old->next;
    /* Immutable asset references transfer to the replacement node. */
    vkr_dmemory_free(&physics->memory, old, sizeof(*old));
  }
  physics->bodies = physics->reset_bodies;
  physics->reset_bodies = NULL;
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    ScenePhysicsBodyComponent *component = vkr_entity_get_component_mut(
        scene->world, body->entity, scene->comp_physics_body);
    component->body = body;
  }
  physics->resetting = false_v;
  for (uint32_t i = 0; i < ArrayCount(physics->characters); ++i) {
    if (reset_characters[i]) {
      physics->characters[i].native = reset_characters[i];
      physics->characters[i].current = reset_states[i];
      physics->characters[i].previous = reset_states[i];
      physics->characters[i].last_step_tick = 0;
    }
  }
  physics_publish_joint_graph(scene);
  physics->faulted = false_v;
  physics->error = NULL;
  scene->physics_disabled = false_v;
  scene->render_full_sync_needed = true_v;
  vkr_scene_simulation_reset_state(scene);
  return true_v;
cleanup: {
  const char *message = error && *error ? *error : "Physics reset failed";
  string_format(physics->error_storage, sizeof(physics->error_storage), "%s",
                message);
  physics->error = physics->error_storage;
  if (error) {
    *error = physics->error;
  }
  vkr_scene_animation_reset_finish(animation_reset, false_v);
  physics->world = replacement;
  physics_discard_joint_graph(scene);
  physics->world = old_world;
  vkr_physics_world_destroy(replacement);
  while (physics->reset_bodies) {
    ScenePhysicsBody *body = physics->reset_bodies;
    physics->reset_bodies = body->next;
    vkr_dmemory_free(&physics->memory, body, sizeof(*body));
  }
  physics->resetting = false_v;
  return false_v;
}
}

bool8_t vkr_scene_physics_set_disabled(VkrScene *scene, bool8_t disabled,
                                       const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!scene) {
    return physics_fail(error, "No scene");
  }
  if (scene->simulation.active ||
      (scene->physics &&
       (scene->physics->prepared || scene->physics->dispatching))) {
    return physics_fail(
        error, "Complete prepared physics edits before disabling simulation");
  }
  if (scene->physics_disabled == disabled) {
    return true_v;
  }
  if (disabled && scene->physics) {
    VkrPhysicsBody reserved[VKR_SCENE_PHYSICS_MAX_BODIES];
    uint32_t count = 0;
    for (ScenePhysicsBody *body = scene->physics->bodies; body;
         body = body->next) {
      if (body->body == VKR_PHYSICS_BODY_INVALID) {
        continue;
      }
      if (!vkr_physics_body_reserve_destroy(scene->physics->world,
                                            body->body)) {
        for (uint32_t i = 0; i < count; ++i) {
          vkr_physics_body_cancel_destroy(scene->physics->world, reserved[i]);
        }
        return physics_fail(error,
                            vkr_physics_last_error(scene->physics->world));
      }
      reserved[count++] = body->body;
    }
  }
  if (scene->physics) {
    for (ScenePhysicsBody *body = scene->physics->bodies; body;
         body = body->next) {
      if (body->body != VKR_PHYSICS_BODY_INVALID &&
          !vkr_physics_body_set_enabled(scene->physics->world, body->body,
                                        !disabled && body->authored.enabled &&
                                            !body->disabled)) {
        return physics_fail(error,
                            vkr_physics_last_error(scene->physics->world));
      }
    }
  }
  scene->physics_disabled = disabled;
  return true_v;
}

bool8_t vkr_scene_physics_is_disabled(const VkrScene *scene) {
  return scene && scene->physics_disabled;
}

bool8_t vkr_scene_physics_impulse(VkrScene *scene, VkrEntityId entity,
                                  Vec3 impulse, const Vec3 *world_point,
                                  const char **error) {
  if (error) {
    *error = NULL;
  }
  if (scene && scene->physics && scene->physics->dispatching) {
    return physics_fail(
        error, "Queue physics mutations until contact dispatch returns");
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  if (!body || !body->authored.enabled || scene->physics_disabled ||
      body->disabled) {
    return physics_fail(error, "Impulse requires an enabled physics body");
  }
  float32_t vector[3] = {impulse.x, impulse.y, impulse.z};
  float32_t point[3] = {0};
  if (world_point) {
    point[0] = world_point->x;
    point[1] = world_point->y;
    point[2] = world_point->z;
  }
  if (!vkr_physics_body_impulse(scene->physics->world, body->body, vector,
                                world_point ? point : NULL)) {
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  return true_v;
}

bool8_t vkr_scene_physics_get_pose(const VkrScene *scene, VkrEntityId entity,
                                   VkrPhysicsPose *pose) {
  ScenePhysicsBody *body = physics_body(scene, entity);
  return body && pose &&
         vkr_physics_body_get_pose(scene->physics->world, body->body, pose);
}

bool8_t vkr_scene_physics_world_matrix(VkrScene *scene, VkrEntityId entity,
                                       Mat4 *matrix) {
  ScenePhysicsCharacter *character = physics_character(scene, entity);
  if (character) {
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        scene->world, entity, scene->comp_transform);
    if (!transform || !matrix) {
      return false_v;
    }
    const float32_t alpha =
        scene->physics_paused || scene->physics_disabled
            ? 1.0f
            : (float32_t)Min(1.0, scene->simulation.accumulator /
                                      VKR_SCENE_PHYSICS_FIXED_DT);
    const Vec3 previous = vec3_new(character->previous.foot_position[0],
                                   character->previous.foot_position[1],
                                   character->previous.foot_position[2]);
    const Vec3 current = vec3_new(character->current.foot_position[0],
                                  character->current.foot_position[1],
                                  character->current.foot_position[2]);
    *matrix = physics_trs(
        vec3_add(previous, vec3_scale(vec3_sub(current, previous), alpha)),
        transform->rotation, vec3_one());
    return true_v;
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  if (!body || !body->authored.enabled ||
      body->body == VKR_PHYSICS_BODY_INVALID) {
    return false_v;
  }
  const float32_t alpha =
      scene->physics_paused || scene->physics_disabled
          ? 1.0f
          : (float32_t)Min(1.0, scene->simulation.accumulator /
                                    VKR_SCENE_PHYSICS_FIXED_DT);
  const VkrPhysicsPose *previous = &body->previous_pose;
  const VkrPhysicsPose *current = &body->current_pose;
  const Vec3 position =
      vec3_new(previous->position[0] +
                   alpha * (current->position[0] - previous->position[0]),
               previous->position[1] +
                   alpha * (current->position[1] - previous->position[1]),
               previous->position[2] +
                   alpha * (current->position[2] - previous->position[2]));
  VkrQuat rotation =
      vkr_quat_slerp(vkr_quat_new(previous->rotation[0], previous->rotation[1],
                                  previous->rotation[2], previous->rotation[3]),
                     vkr_quat_new(current->rotation[0], current->rotation[1],
                                  current->rotation[2], current->rotation[3]),
                     alpha);
  *matrix = physics_trs(position, rotation, body->world_scale);
  return true_v;
}

bool8_t vkr_scene_physics_raycast(VkrScene *scene, Vec3 origin,
                                  Vec3 displacement, VkrPhysicsRayHit *hit) {
  if (!scene || !scene->physics || scene->physics_disabled) {
    return false_v;
  }
  const float32_t start[3] = {origin.x, origin.y, origin.z};
  const float32_t delta[3] = {displacement.x, displacement.y, displacement.z};
  return vkr_physics_raycast(scene->physics->world, start, delta, hit);
}

const char *vkr_scene_physics_error(const VkrScene *scene) {
  return scene && scene->simulation.error
             ? scene->simulation.error
             : (scene && scene->physics ? scene->physics->error : NULL);
}

float64_t vkr_scene_physics_time(const VkrScene *scene) {
  return scene ? scene->simulation.completed_ticks * VKR_SCENE_PHYSICS_FIXED_DT
               : 0.0;
}

float64_t vkr_scene_physics_debt(const VkrScene *scene) {
  return scene ? scene->simulation.accumulator : 0.0;
}

float64_t vkr_scene_physics_animation_delta(VkrScene *scene,
                                            float64_t fallback) {
  if (!scene ||
      (!scene->simulation.enabled && !vkr_scene_physics_body_count(scene))) {
    return fallback;
  }
  VkrSceneSimulation *simulation = &scene->simulation;
  const uint64_t ticks =
      simulation->completed_ticks - simulation->animation_ticks;
  simulation->animation_ticks = simulation->completed_ticks;
  return ticks * VKR_SCENE_PHYSICS_FIXED_DT;
}

bool8_t vkr_scene_physics_transform_validate(const VkrScene *scene,
                                             VkrEntityId entity, Vec3 position,
                                             VkrQuat rotation, Vec3 scale,
                                             VkrEntityId parent,
                                             const char **error) {
  if (scene && scene->physics && scene->physics->dispatching) {
    return physics_fail(error,
                        "Queue transform edits until contact dispatch returns");
  }
  if (!scene || !scene->physics || scene->physics->editing) {
    return true_v;
  }
  if (physics_character(scene, entity) &&
      (!scene->physics_paused || parent.u64 || !physics_unit_scale(scale))) {
    return physics_fail(error, "Character authoring requires pause, root and "
                               "unit scale; Reset applies pose edits");
  }
  if (vkr_entity_has_component(scene->world, entity,
                               scene->comp_physics_collider)) {
    return physics_fail(
        error, "Edit collider transforms through their owner snapshot");
  }
  const SceneTransform *old = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  const PhysicsTransformOverride override = {.entity = entity,
                                             .position = position,
                                             .rotation = rotation,
                                             .scale = scale,
                                             .parent = parent};
  for (ScenePhysicsBody *body = scene->physics->bodies; body;
       body = body->next) {
    VkrEntityId cursor = body->authored.attachment.enabled
                             ? body->animation_wrapper
                             : body->entity;
    bool8_t affected = body->entity.u64 == entity.u64;
    for (uint32_t depth = 0;
         !affected && cursor.u64 && depth < scene->world->dir.capacity;
         ++depth) {
      affected = cursor.u64 == entity.u64;
      const SceneTransform *ancestor = vkr_entity_get_component_if_alive_const(
          scene->world, cursor, scene->comp_transform);
      cursor = ancestor ? ancestor->parent : VKR_ENTITY_ID_INVALID;
    }
    if (!affected) {
      continue;
    }
    if (!scene->physics_paused &&
        ((body->entity.u64 == entity.u64 &&
          body->authored.motion == VKR_PHYSICS_DYNAMIC) ||
         !old || old->parent.u64 != parent.u64 || old->scale.x != scale.x ||
         old->scale.y != scale.y || old->scale.z != scale.z)) {
      return physics_fail(
          error,
          "Pause before editing dynamic poses, parenting or collision scale");
    }
    Mat4 world;
    Vec3 world_position, world_scale;
    VkrQuat world_rotation;
    if (!physics_body_matrix(scene, body->entity, &body->authored, true_v,
                             &override, &world, error) ||
        !physics_decompose(world, &world_position, &world_rotation,
                           &world_scale, error) ||
        !physics_validate_scaled_shapes(&body->authored, world_scale, error)) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_scene_physics_transform_allowed(const VkrScene *scene,
                                            VkrEntityId entity, Vec3 scale,
                                            VkrEntityId parent) {
  const SceneTransform *transform =
      scene ? vkr_entity_get_component_if_alive_const(scene->world, entity,
                                                      scene->comp_transform)
            : NULL;
  return !transform || vkr_scene_physics_transform_validate(
                           scene, entity, transform->position,
                           transform->rotation, scale, parent, NULL);
}

bool8_t vkr_scene_physics_entity_destroying(VkrScene *scene,
                                            VkrEntityId entity) {
  if (scene->physics && scene->physics->dispatching) {
    return false_v;
  }
  if (!scene->physics || scene->physics->editing) {
    return true_v;
  }
  for (VkrScenePhysicsPrepared *pending = scene->physics->prepared; pending;
       pending = pending->next) {
    if (pending->entity.u64 == entity.u64) {
      scene->physics->error =
          "Complete prepared physics edits before deleting their owner";
      return false_v;
    }
  }
  const ScenePhysicsCollider *collider = vkr_entity_get_component_if_alive(
      scene->world, entity, scene->comp_physics_collider);
  if (collider) {
    /* Collider removal is an owner-snapshot transaction; generic deletion must
     * not leave the compound body's retained shape alive without an entity. */
    return false_v;
  }
  VkrScenePhysicsPrepared *removals[VKR_SCENE_PHYSICS_MAX_BODIES];
  uint32_t count = 0;
  const char *error = NULL;
  for (ScenePhysicsBody *body = scene->physics->bodies; body;
       body = body->next) {
    if (body->entity.u64 != entity.u64 &&
        (!body->authored.attachment.enabled ||
         body->animation_wrapper.u64 != entity.u64)) {
      continue;
    }
    if (!vkr_scene_physics_prepare(scene, body->entity, NULL, &removals[count],
                                   &error)) {
      goto cleanup;
    }
    count++;
  }
  if (count && !vkr_scene_physics_prepare_complete(scene, &error)) {
    goto cleanup;
  }
  for (uint32_t i = 0; i < count; ++i) {
    vkr_scene_physics_commit(removals[i]);
  }
  if (physics_character(scene, entity) &&
      !vkr_scene_character_destroy(scene, entity, &error)) {
    scene->physics->error = error;
    return false_v;
  }
  return true_v;
cleanup:
  for (uint32_t i = 0; i < count; ++i) {
    vkr_scene_physics_discard(removals[i]);
  }
  scene->physics->error = error;
  return false_v;
}

void vkr_scene_physics_shutdown(VkrScene *scene) {
  if (!scene || scene->simulation.active || !scene->physics ||
      scene->physics->dispatching) {
    return;
  }
  VkrScenePhysics *physics = scene->physics;
  while (physics->prepared) {
    vkr_scene_physics_discard(physics->prepared);
  }
  vkr_physics_world_destroy(physics->world);
  for (ScenePhysicsBody *body = physics->bodies; body; body = body->next) {
    physics_release_assets(body);
  }
  VkrDMemory memory = physics->memory;
  scene->physics = NULL;
  vkr_dmemory_destroy(&memory);
}

bool8_t vkr_scene_physics_set_body_disabled(VkrScene *scene, VkrEntityId entity,
                                            bool8_t disabled,
                                            const char **error) {
  if (scene && scene->physics && scene->physics->dispatching) {
    return physics_fail(
        error, "Queue physics mutations until contact dispatch returns");
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  if (!body) {
    return physics_fail(error, "No physics body on entity");
  }
  if (body->body != VKR_PHYSICS_BODY_INVALID &&
      !vkr_physics_body_set_enabled(scene->physics->world, body->body,
                                    !disabled && body->authored.enabled &&
                                        !scene->physics_disabled)) {
    return physics_fail(error, vkr_physics_last_error(scene->physics->world));
  }
  body->disabled = disabled;
  return true_v;
}

bool8_t vkr_scene_physics_body_is_disabled(const VkrScene *scene,
                                           VkrEntityId entity) {
  ScenePhysicsBody *body = physics_body(scene, entity);
  return body && body->disabled;
}

bool8_t vkr_scene_physics_set_kinematic_target(VkrScene *scene,
                                               VkrEntityId entity,
                                               Vec3 position, VkrQuat rotation,
                                               const char **error) {
  if (scene && scene->physics && scene->physics->dispatching) {
    return physics_fail(
        error, "Queue physics mutations until contact dispatch returns");
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  if (!body || body->authored.motion != VKR_PHYSICS_KINEMATIC ||
      !physics_vec_finite(position) || !physics_rotation_valid(rotation)) {
    return physics_fail(
        error,
        "Kinematic target requires a kinematic body and finite unit pose");
  }
  body->target_position = position;
  body->target_rotation = rotation;
  return true_v;
}

bool8_t vkr_scene_physics_overlap_sphere(VkrScene *scene, Vec3 center,
                                         float32_t radius, uint16_t query_mask,
                                         VkrPhysicsOverlapHit *hits,
                                         uint32_t capacity, uint32_t *count) {
  if (count) {
    *count = 0;
  }
  if (!scene || !scene->physics || scene->physics_disabled) {
    return count != NULL;
  }
  const float32_t origin[3] = {center.x, center.y, center.z};
  return vkr_physics_overlap_sphere(scene->physics->world, origin, radius,
                                    query_mask, hits, capacity, count);
}

bool8_t vkr_scene_physics_sensor_events(VkrScene *scene,
                                        VkrPhysicsSensorEvent *events,
                                        uint32_t capacity, uint32_t *count) {
  if (count) {
    *count = 0;
  }
  if (!scene || !scene->physics) {
    return count != NULL;
  }
  return vkr_physics_sensor_events(scene->physics->world, events, capacity,
                                   count);
}

const VkrCollisionGeometry *
vkr_scene_physics_collider_geometry(const VkrScene *scene, VkrEntityId owner,
                                    uint64_t authored_id) {
  ScenePhysicsBody *body = physics_body(scene, owner);
  if (body) {
    for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
      if (body->authored.colliders[i].authored_id == authored_id &&
          body->assets[i]) {
        return vkr_collision_asset_geometry(body->assets[i]);
      }
    }
  }
  return NULL;
}

bool8_t vkr_scene_physics_resolve_world(const VkrScene *scene,
                                        VkrEntityId entity, Mat4 *world) {
  ScenePhysicsCharacter *character = physics_character(scene, entity);
  if (character) {
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        scene->world, entity, scene->comp_transform);
    if (!transform || !world) {
      return false_v;
    }
    const float32_t *position = character->current.foot_position;
    *world = physics_trs(vec3_new(position[0], position[1], position[2]),
                         transform->rotation, vec3_one());
    return true_v;
  }
  ScenePhysicsBody *body = physics_body(scene, entity);
  if (body && body->body && body->authored.motion == VKR_PHYSICS_DYNAMIC) {
    const VkrPhysicsPose *pose = &body->current_pose;
    *world = physics_trs(
        vec3_new(pose->position[0], pose->position[1], pose->position[2]),
        vkr_quat_new(pose->rotation[0], pose->rotation[1], pose->rotation[2],
                     pose->rotation[3]),
        body->world_scale);
    return true_v;
  }
  return scene && world &&
         physics_entity_matrix(scene, entity, true_v, NULL, world, NULL);
}

bool8_t vkr_scene_physics_publish_bones(VkrScene *scene, bool8_t interpolate,
                                        const char **error) {
  if (!scene || !scene->physics) {
    return true_v;
  }
  uint32_t nodes[VKR_SCENE_PHYSICS_MAX_BODIES];
  Mat4 matrices[VKR_SCENE_PHYSICS_MAX_BODIES];
  for (ScenePhysicsBody *first = scene->physics->bodies; first;
       first = first->next) {
    if (!first->authored.attachment.drive_bone || !first->body ||
        !first->authored.enabled || first->disabled) {
      continue;
    }
    bool8_t already = false_v;
    for (ScenePhysicsBody *prior = scene->physics->bodies; prior != first;
         prior = prior->next) {
      if (prior->authored.attachment.drive_bone && prior->body &&
          prior->authored.enabled && !prior->disabled &&
          prior->animation_wrapper.u64 == first->animation_wrapper.u64) {
        already = true_v;
        break;
      }
    }
    if (already) {
      continue;
    }
    uint32_t count = 0;
    for (ScenePhysicsBody *body = first; body; body = body->next) {
      if (!body->authored.attachment.drive_bone || !body->body ||
          !body->authored.enabled || body->disabled ||
          body->animation_wrapper.u64 != first->animation_wrapper.u64) {
        continue;
      }
      Mat4 world;
      if (interpolate) {
        vkr_scene_physics_world_matrix(scene, body->entity, &world);
      } else {
        vkr_scene_physics_resolve_world(scene, body->entity, &world);
      }
      const VkrScenePhysicsAttachment *attachment = &body->authored.attachment;
      matrices[count] = mat4_mul(
          world, mat4_inverse(physics_trs(attachment->position,
                                          attachment->rotation, vec3_one())));
      nodes[count++] = attachment->source_node;
    }
    if (!vkr_scene_animation_override_nodes(scene, first->animation_wrapper,
                                            nodes, matrices, count, error)) {
      scene->physics->error =
          error && *error ? *error : "Failed to publish physics bone pose";
      scene->physics_paused = true_v;
      return false_v;
    }
  }
  return true_v;
}

void vkr_scene_physics_set_contact_callback(VkrScene *scene,
                                            VkrPhysicsContactCallback callback,
                                            void *context) {
  if (scene && !scene->simulation.active &&
      vkr_scene_physics_mutations_allowed(scene) &&
      physics_ensure(scene, NULL)) {
    scene->physics->contact_callback = callback;
    scene->physics->contact_context = context;
  }
}

bool8_t vkr_scene_physics_contact_events(VkrScene *scene,
                                         VkrPhysicsContactEvent *events,
                                         uint32_t capacity, uint32_t *count) {
  if (count) {
    *count = 0;
  }
  if (!scene || !scene->physics) {
    return count != NULL;
  }
  return vkr_physics_contact_events(scene->physics->world, events, capacity,
                                    count);
}

static bool8_t physics_animation_joint(const VkrAnimationAsset *asset,
                                       uint32_t node) {
  for (uint32_t skin = 0; skin < asset->skin_count; ++skin) {
    for (uint32_t joint = 0; joint < asset->skins[skin].joint_count; ++joint) {
      if (asset->skins[skin].joints[joint] == node) {
        return true_v;
      }
    }
  }
  return false_v;
}

static VkrQuat physics_y_rotation(Vec3 direction) {
  direction = vec3_normalize(direction);
  if (direction.y < -0.99999f) {
    return vkr_quat_new(1, 0, 0, 0);
  }
  return vkr_quat_normalize(
      vkr_quat_new(direction.z, 0, -direction.x, 1 + direction.y));
}

bool8_t vkr_scene_physics_ragdoll_plan(const VkrScene *scene,
                                       VkrEntityId wrapper,
                                       VkrSceneRagdollOperation operation,
                                       VkrScenePhysicsChange *changes,
                                       uint32_t capacity, uint32_t *count,
                                       const char **error) {
  if (count) {
    *count = 0;
  }
  VkrAnimationPlayer *player = vkr_scene_animation_get_player(scene, wrapper);
  const VkrAnimationAsset *asset = vkr_animation_player_asset(player);
  const SceneSourceIdentity *wrapper_source =
      scene ? vkr_entity_get_component_if_alive_const(
                  scene->world, wrapper, scene->comp_source_identity)
            : NULL;
  if (!scene || !count || !asset || !wrapper_source ||
      operation < VKR_SCENE_RAGDOLL_CREATE ||
      operation > VKR_SCENE_RAGDOLL_REMOVE) {
    return physics_fail(
        error, "Ragdoll planning requires an imported animation wrapper");
  }
  uint32_t needed = 0;
  for (uint32_t n = 0; n < asset->node_count; ++n) {
    if (!physics_animation_joint(asset, n)) {
      continue;
    }
    VkrEntityId entity = vkr_scene_animation_node_entity(scene, wrapper, n);
    ScenePhysicsBody *body = physics_body(scene, entity);
    if (operation == VKR_SCENE_RAGDOLL_CREATE ||
        (body && body->authored.attachment.enabled &&
         body->animation_wrapper.u64 == wrapper.u64)) {
      needed++;
    }
  }
  *count = needed;
  if (!changes) {
    return true_v;
  }
  if (needed > capacity || needed > VKR_SCENE_PHYSICS_MAX_BODIES) {
    return physics_fail(error,
                        "Ragdoll planning output capacity is insufficient");
  }
  uint32_t cursor = 0;
  for (uint32_t order = 0; order < asset->node_count; ++order) {
    const uint32_t n = asset->node_order[order];
    if (!physics_animation_joint(asset, n)) {
      continue;
    }
    VkrEntityId entity = vkr_scene_animation_node_entity(scene, wrapper, n);
    ScenePhysicsBody *body = physics_body(scene, entity);
    if (operation != VKR_SCENE_RAGDOLL_CREATE &&
        (!body || !body->authored.attachment.enabled ||
         body->animation_wrapper.u64 != wrapper.u64)) {
      continue;
    }
    VkrScenePhysicsChange *change = &changes[cursor++];
    *change = (VkrScenePhysicsChange){.entity = entity};
    if (operation == VKR_SCENE_RAGDOLL_REMOVE) {
      continue;
    }
    change->snapshot = operation == VKR_SCENE_RAGDOLL_CREATE
                           ? vkr_scene_physics_default()
                           : body->authored;
    VkrScenePhysicsSnapshot *snapshot = &change->snapshot;
    snapshot->motion = operation == VKR_SCENE_RAGDOLL_DISABLE
                           ? VKR_PHYSICS_KINEMATIC
                           : VKR_PHYSICS_DYNAMIC;
    snapshot->attachment = (VkrScenePhysicsAttachment){
        .enabled = true_v,
        .animation_source = *wrapper_source,
        .source_node = n,
        .drive_bone = operation != VKR_SCENE_RAGDOLL_DISABLE,
        .position = snapshot->attachment.position,
        .rotation = operation == VKR_SCENE_RAGDOLL_CREATE
                        ? vkr_quat_identity()
                        : snapshot->attachment.rotation};
    if (operation != VKR_SCENE_RAGDOLL_CREATE) {
      continue;
    }
    snapshot->colliders[0].shape = VKR_PHYSICS_SPHERE;
    snapshot->colliders[0].radius = 0.08f;
    Mat4 bone;
    if (!vkr_scene_animation_node_world(scene, wrapper, n, &bone)) {
      return physics_fail(error, "Ragdoll source bone has no evaluated pose");
    }
    for (uint32_t child = 0; child < asset->node_count; ++child) {
      if (asset->nodes[child].parent != n ||
          !physics_animation_joint(asset, child)) {
        continue;
      }
      Mat4 child_world;
      if (!vkr_scene_animation_node_world(scene, wrapper, child,
                                          &child_world)) {
        return physics_fail(error, "Ragdoll child has no evaluated pose");
      }
      const Vec3 end =
          mat4_mul_vec3(mat4_inverse(bone), vec3_new(child_world.elements[12],
                                                     child_world.elements[13],
                                                     child_world.elements[14]));
      const float32_t length = vec3_length(end);
      if (length > 0.02f) {
        snapshot->colliders[0].shape = VKR_PHYSICS_CAPSULE;
        snapshot->colliders[0].radius = Min(0.08f, length * 0.2f);
        snapshot->colliders[0].half_height =
            Max(0.0f, length * 0.5f - snapshot->colliders[0].radius);
        snapshot->attachment.position = vec3_scale(end, 0.5f);
        snapshot->attachment.rotation = physics_y_rotation(end);
      }
      break;
    }
  }
  if (operation != VKR_SCENE_RAGDOLL_CREATE) {
    return true_v;
  }
  for (uint32_t i = 0; i < needed; ++i) {
    VkrScenePhysicsSnapshot *snapshot = &changes[i].snapshot;
    uint32_t parent = asset->nodes[snapshot->attachment.source_node].parent;
    while (parent != UINT32_MAX && !physics_animation_joint(asset, parent)) {
      parent = asset->nodes[parent].parent;
    }
    if (parent == UINT32_MAX) {
      continue;
    }
    uint32_t target = 0;
    while (target < needed &&
           changes[target].snapshot.attachment.source_node != parent) {
      target++;
    }
    if (target == needed) {
      return physics_fail(error, "Ragdoll parent was not included in the plan");
    }
    const SceneSourceIdentity *target_source =
        vkr_entity_get_component_if_alive_const(
            scene->world, changes[target].entity, scene->comp_source_identity);
    Mat4 a, b, bone;
    Vec3 position_a, position_b, scale_a, scale_b;
    VkrQuat rotation_a, rotation_b;
    if (!target_source ||
        !physics_body_matrix(scene, changes[i].entity, snapshot, true_v, NULL,
                             &a, error) ||
        !physics_body_matrix(scene, changes[target].entity,
                             &changes[target].snapshot, true_v, NULL, &b,
                             error) ||
        !physics_decompose(a, &position_a, &rotation_a, &scale_a, error) ||
        !physics_decompose(b, &position_b, &rotation_b, &scale_b, error) ||
        !vkr_scene_animation_node_world(
            scene, wrapper, snapshot->attachment.source_node, &bone)) {
      return false_v;
    }
    const Vec3 anchor =
        vec3_new(bone.elements[12], bone.elements[13], bone.elements[14]);
    const Vec3 axis_world = vkr_quat_rotate_vec3(rotation_a, vec3_new(0, 1, 0));
    const Vec3 normal_world =
        vkr_quat_rotate_vec3(rotation_a, vec3_new(1, 0, 0));
    snapshot->joint_count = 1;
    snapshot->joints[0] = (VkrSceneJointConfig){
        .authored_id = 1,
        .target_source = *target_source,
        .type = VKR_PHYSICS_JOINT_SWING_TWIST,
        .anchor_a = mat4_mul_vec3(mat4_inverse(a), anchor),
        .anchor_b = mat4_mul_vec3(mat4_inverse(b), anchor),
        .axis_a = vec3_new(0, 1, 0),
        .normal_a = vec3_new(1, 0, 0),
        .axis_b =
            vkr_quat_rotate_vec3(vkr_quat_conjugate(rotation_b), axis_world),
        .normal_b =
            vkr_quat_rotate_vec3(vkr_quat_conjugate(rotation_b), normal_world),
        .min_limit = -0.5235988f,
        .max_limit = 0.5235988f,
        .swing_normal_limit = 0.7853982f,
        .swing_plane_limit = 0.7853982f,
        .enabled = true_v};
  }
  return true_v;
}

bool8_t vkr_scene_physics_mutations_allowed(const VkrScene *scene) {
  return !scene || (!scene->simulation.in_callback &&
                    (!scene->world || !scene->world->structural_read_depth ||
                     scene->simulation.active) &&
                    (!scene->physics || !scene->physics->dispatching));
}

bool8_t vkr_scene_physics_matrix_allowed(const VkrScene *scene,
                                         VkrEntityId entity, Mat4 local) {
  if (!vkr_scene_physics_mutations_allowed(scene)) {
    return false_v;
  }
  if (!scene || !scene->physics ||
      (!scene->physics->body_count && !physics_character(scene, entity))) {
    return true_v;
  }
  Vec3 position, scale;
  VkrQuat rotation;
  const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
      scene->world, entity, scene->comp_transform);
  return transform &&
         physics_decompose(local, &position, &rotation, &scale, NULL) &&
         vkr_scene_physics_transform_validate(scene, entity, position, rotation,
                                              scale, transform->parent, NULL);
}

bool8_t vkr_scene_physics_raycast_query(VkrScene *scene, Vec3 origin,
                                        Vec3 displacement,
                                        const VkrPhysicsQueryFilter *filter,
                                        VkrPhysicsRayHit *hit) {
  if (!scene || !scene->physics || scene->physics_disabled) {
    return false_v;
  }
  const float32_t start[3] = {origin.x, origin.y, origin.z};
  const float32_t delta[3] = {displacement.x, displacement.y, displacement.z};
  return vkr_physics_raycast_query(scene->physics->world, start, delta, filter,
                                   hit);
}

bool8_t vkr_scene_physics_sweep_sphere(VkrScene *scene, Vec3 origin,
                                       Vec3 displacement, float32_t radius,
                                       const VkrPhysicsQueryFilter *filter,
                                       VkrPhysicsRayHit *hit, bool8_t *found) {
  if (!scene || !hit || !found || !physics_vec_finite(origin) ||
      !physics_vec_finite(displacement) || !isfinite(radius) || radius <= 0 ||
      radius > VKR_PHYSICS_MAX_COORDINATE ||
      (filter && (filter->ignored_count > VKR_PHYSICS_MAX_QUERY_IGNORES ||
                  (filter->ignored_count && !filter->ignored_entities)))) {
    return false_v;
  }
  if (!scene->physics || scene->physics_disabled) {
    *hit = (VkrPhysicsRayHit){0};
    *found = false_v;
    return true_v;
  }
  const float32_t start[3] = {origin.x, origin.y, origin.z};
  const float32_t delta[3] = {displacement.x, displacement.y, displacement.z};
  return vkr_physics_sweep_sphere(scene->physics->world, start, delta, radius,
                                  filter, hit, found);
}

bool8_t vkr_scene_physics_sweep(VkrScene *scene, VkrEntityId owner,
                                uint64_t collider_id, Vec3 origin,
                                VkrQuat rotation, Vec3 displacement,
                                const VkrPhysicsQueryFilter *filter,
                                VkrPhysicsRayHit *hit) {
  ScenePhysicsBody *body = physics_body(scene, owner);
  if (!body || scene->physics_disabled) {
    return false_v;
  }
  for (uint32_t i = 0; i < body->authored.collider_count; ++i) {
    const VkrSceneColliderConfig *shape = &body->authored.colliders[i];
    if (shape->authored_id != collider_id) {
      continue;
    }
    VkrPhysicsColliderDesc desc = {
        .entity_id = collider_id,
        .shape = shape->shape,
        .rotation = {0, 0, 0, 1},
        .scale = {body->world_scale.x * shape->scale.x,
                  body->world_scale.y * shape->scale.y,
                  body->world_scale.z * shape->scale.z},
        .half_extent = {shape->half_extent.x, shape->half_extent.y,
                        shape->half_extent.z},
        .radius = shape->radius,
        .half_height = shape->half_height};
    const VkrCollisionGeometry *geometry =
        vkr_collision_asset_geometry(body->assets[i]);
    if (geometry) {
      desc.geometry =
          (VkrPhysicsGeometry){geometry->positions, geometry->vertex_count,
                               geometry->indices, geometry->index_count};
    }
    const float32_t start[3] = {origin.x, origin.y, origin.z};
    const float32_t orientation[4] = {rotation.x, rotation.y, rotation.z,
                                      rotation.w};
    const float32_t delta[3] = {displacement.x, displacement.y, displacement.z};
    return vkr_physics_sweep(scene->physics->world, &desc, start, orientation,
                             delta, filter, hit);
  }
  return false_v;
}

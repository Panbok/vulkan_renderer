#include "scene_physics_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
#include "renderer/systems/vkr_scene_simulation.h"
#include "renderer/systems/vkr_scene_types.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static VkrEntityId physics_test_entity(VkrScene *scene, Vec3 position) {
  VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(entity.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(scene, entity, position, vkr_quat_identity(),
                                 vec3_one()));
  return entity;
}

typedef struct PhysicsContactTest {
  VkrScene *scene;
  VkrEntityId body;
  uint32_t begins;
  uint32_t persists;
} PhysicsContactTest;

static void physics_test_contacts(const VkrPhysicsContactEvent *events,
                                  uint32_t count, void *user) {
  PhysicsContactTest *test = user;
  for (uint32_t i = 0; i < count; ++i) {
    test->begins += events[i].phase == VKR_PHYSICS_CONTACT_BEGIN;
    test->persists += events[i].phase == VKR_PHYSICS_CONTACT_PERSIST;
  }
  if (count) {
    const char *error = NULL;
    assert(!vkr_scene_physics_impulse(test->scene, test->body, vec3_one(), NULL,
                                      &error));
    assert(vkr_scene_create_entity(test->scene, NULL).u64 ==
           VKR_ENTITY_ID_INVALID.u64);
    vkr_scene_destroy_entity(test->scene, test->body);
    assert(vkr_scene_entity_alive(test->scene, test->body));
    VkrPhysicsPose pose;
    assert(vkr_scene_physics_get_pose(test->scene, test->body, &pose));
  }
}

static void physics_test_hierarchy_contacts(VkrScene *scene) {
  const char *error = NULL;
  VkrEntityId parent = physics_test_entity(scene, vec3_new(100, 0, 0));
  VkrEntityId child = physics_test_entity(scene, vec3_new(0, 3, 0));
  vkr_scene_set_parent(scene, child, parent);
  assert(vkr_scene_set_transform(scene, child, vec3_new(0, 3, 0),
                                 vkr_quat_identity(), vec3_new(2, 1, 1)));
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  assert(vkr_scene_physics_apply(scene, child, &config, &error));
  assert(!vkr_scene_set_transform(scene, parent, vec3_new(100, 0, 0),
                                  vkr_quat_identity(), vec3_new(-1, 1, 1)));
  assert(!vkr_scene_set_transform(scene, parent, vec3_new(100, 0, 0),
                                  vkr_quat_identity(), vec3_new(0, 1, 1)));
  config.colliders[0].rotation = vkr_quat_new(0, 0, sinf(0.3f), cosf(0.3f));
  assert(!vkr_scene_physics_apply(scene, child, &config, &error));
  assert(error != NULL);
  config.colliders[0].rotation = vkr_quat_identity();
  VkrEntityId floor = physics_test_entity(scene, vec3_new(100, 0, 0));
  config.body.motion = VKR_PHYSICS_STATIC;
  config.colliders[0].half_extent = vec3_new(8, 0.5f, 8);
  assert(vkr_scene_physics_apply(scene, floor, &config, &error));
  PhysicsContactTest contacts = {.scene = scene, .body = child};
  vkr_scene_physics_set_contact_callback(scene, physics_test_contacts,
                                         &contacts);
  vkr_scene_physics_set_paused(scene, false_v);
  vkr_scene_set_position(scene, parent, vec3_new(110, 0, 0));
  for (uint32_t tick = 0; tick < 180; ++tick) {
    vkr_scene_update(scene, VKR_SCENE_PHYSICS_FIXED_DT);
    assert(!vkr_scene_physics_is_paused(scene));
  }
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(scene, child, &pose));
  assert(fabsf(pose.position[0] - 100) < 0.01f);
  assert(fabsf(pose.position[1] - 1) < 0.02f);
  assert(contacts.begins > 0 && contacts.persists > 0);
  VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX,
                                  .include_sensors = false_v,
                                  .ignored_entities = &child.u64,
                                  .ignored_count = 1};
  VkrPhysicsRayHit hit;
  assert(vkr_scene_physics_raycast_query(scene, vec3_new(100, 4, 0),
                                         vec3_new(0, -8, 0), &filter, &hit));
  assert(hit.entity_id == floor.u64);
  assert(vkr_scene_physics_sweep(scene, child, 1, vec3_new(100, 4, 0),
                                 vkr_quat_identity(), vec3_new(0, -8, 0),
                                 &filter, &hit));
  assert(hit.entity_id == floor.u64);
  vkr_scene_physics_set_contact_callback(scene, NULL, NULL);
  vkr_scene_physics_set_paused(scene, true_v);
  assert(vkr_scene_physics_reset(scene, &error));
  assert(vkr_scene_physics_get_pose(scene, child, &pose));
  assert(fabsf(pose.position[0] - 110) < 0.01f);
  config = vkr_scene_physics_default();
  config.body.motion = VKR_PHYSICS_KINEMATIC;
  assert(vkr_scene_physics_apply(scene, child, &config, &error));
  vkr_scene_physics_set_paused(scene, false_v);
  vkr_scene_set_position(scene, parent, vec3_new(112, 0, 0));
  vkr_scene_update(scene, VKR_SCENE_PHYSICS_FIXED_DT);
  assert(vkr_scene_physics_get_pose(scene, child, &pose));
  assert(fabsf(pose.position[0] - 112) < 0.01f);
  vkr_scene_physics_set_paused(scene, true_v);
  vkr_scene_destroy_entity(scene, child);
  vkr_scene_destroy_entity(scene, parent);
  vkr_scene_destroy_entity(scene, floor);
}

/* Physics set (ADR-076). A dynamic box in one scene rests on a static floor
 * in another only when both bodies share the set's native world; alone it
 * would fall without bound. The member follows the driver's pause state and
 * cannot step or reset, the driver's reset rebuilds both scenes, and a
 * member's shutdown leaves the driver simulating its own floor. */
static void physics_test_shared_set(VkrAllocator *allocator) {
  VkrScene floor_scene;
  VkrScene box_scene;
  assert(vkr_scene_init(&floor_scene, allocator, 0, 16, NULL));
  assert(vkr_scene_init(&box_scene, allocator, 1, 16, NULL));
  VkrScenePhysicsSet *set = vkr_scene_physics_set_create(allocator);
  assert(set);
  const char *error = NULL;
  assert(vkr_scene_physics_attach(&floor_scene, set, true_v, &error));
  assert(vkr_scene_physics_attach(&box_scene, set, false_v, &error));
  assert(!vkr_scene_physics_attach(&box_scene, set, false_v, &error));

  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  config.body.motion = VKR_PHYSICS_STATIC;
  config.colliders[0].half_extent = vec3_new(8, 0.5f, 8);
  const VkrEntityId floor = physics_test_entity(&floor_scene, vec3_zero());
  assert(vkr_scene_physics_apply(&floor_scene, floor, &config, &error));
  config = vkr_scene_physics_default();
  const VkrEntityId box = physics_test_entity(&box_scene, vec3_new(0, 3, 0));
  assert(vkr_scene_physics_apply(&box_scene, box, &config, &error));
  assert(vkr_scene_physics_simulated_body_count(&floor_scene) == 2u);
  assert(vkr_scene_physics_simulated_body_count(&box_scene) == 0u);

  vkr_scene_physics_set_paused(&floor_scene, false_v);
  assert(!vkr_scene_physics_is_paused(&box_scene));
  assert(!vkr_scene_physics_apply(&box_scene, box, &config, &error));
  assert(!vkr_scene_physics_step(&box_scene, &error));
  for (uint32_t tick = 0; tick < 180; ++tick) {
    vkr_scene_update(&floor_scene, VKR_SCENE_PHYSICS_FIXED_DT);
    vkr_scene_update(&box_scene, VKR_SCENE_PHYSICS_FIXED_DT);
  }
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(&box_scene, box, &pose));
  assert(fabsf(pose.position[1] - 1.0f) < 0.05f);
  VkrPhysicsQueryFilter filter = {.mask = UINT16_MAX};
  VkrPhysicsRayHit hit;
  assert(vkr_scene_physics_raycast_query(&floor_scene, vec3_new(0, 5, 0),
                                         vec3_new(0, -10, 0), &filter, &hit));
  assert(hit.entity_id == box.u64);

  vkr_scene_physics_set_paused(&floor_scene, true_v);
  assert(!vkr_scene_physics_reset(&box_scene, &error));
  assert(vkr_scene_physics_reset(&floor_scene, &error));
  assert(vkr_scene_physics_get_pose(&box_scene, box, &pose));
  assert(fabsf(pose.position[1] - 3.0f) < 1e-4f);

  vkr_scene_shutdown(&box_scene, NULL);
  assert(vkr_scene_physics_simulated_body_count(&floor_scene) == 1u);
  assert(!vkr_scene_physics_raycast_query(&floor_scene, vec3_new(20, 5, 0),
                                          vec3_new(0, -10, 0), &filter, &hit));
  vkr_scene_physics_set_paused(&floor_scene, false_v);
  for (uint32_t tick = 0; tick < 10; ++tick) {
    vkr_scene_update(&floor_scene, VKR_SCENE_PHYSICS_FIXED_DT);
  }
  assert(vkr_scene_physics_raycast_query(&floor_scene, vec3_new(0, 5, 0),
                                         vec3_new(0, -10, 0), &filter, &hit));
  assert(hit.entity_id == floor.u64);
  vkr_scene_physics_set_paused(&floor_scene, true_v);
  vkr_scene_shutdown(&floor_scene, NULL);
  vkr_scene_physics_set_destroy(set);
}

/* Gravity/impulse have analytic oracles independent of contact generation.
 * Real ECS create/remove and evaluated hierarchy detect stale references and
 * accidental publication of simulated poses into authored save state. */
/* The body and collider descriptors carry the rules snapshot validation
   applies, and hide dimensions a collider shape does not use. */
static void physics_test_descriptors(void) {
  VkrScenePhysicsSnapshot snapshot = vkr_scene_physics_default();
  const char *error = NULL;
  assert(vkr_type_validate(&vkr_scene_physics_body_type, &snapshot.body, NULL,
                           0u));
  assert(vkr_scene_physics_snapshot_validate(&snapshot, &error));
  snapshot.body.mass = 0.0f;
  assert(!vkr_type_validate(&vkr_scene_physics_body_type, &snapshot.body, NULL,
                            0u));
  assert(!vkr_scene_physics_snapshot_validate(&snapshot, &error));
  snapshot = vkr_scene_physics_default();
  snapshot.body.friction = 1.5f;
  assert(!vkr_scene_physics_snapshot_validate(&snapshot, &error));

  snapshot = vkr_scene_physics_default();
  VkrSceneColliderConfig *collider = &snapshot.colliders[0];
  collider->scale = vec3_new(1.0f, 0.0f, 1.0f);
  assert(
      !vkr_type_validate(&vkr_scene_physics_collider_type, collider, NULL, 0u));
  assert(!vkr_scene_physics_snapshot_validate(&snapshot, &error));
  snapshot = vkr_scene_physics_default();
  collider->radius = 0.0f;
  assert(!vkr_scene_physics_snapshot_validate(&snapshot, &error));
  snapshot = vkr_scene_physics_default();
  collider->shape = VKR_PHYSICS_CONVEX_HULL;
  assert(!vkr_scene_physics_snapshot_validate(&snapshot, &error));
  snprintf(collider->asset_path, sizeof(collider->asset_path),
           "collision/rock.vkc");
  assert(vkr_scene_physics_snapshot_validate(&snapshot, &error));

  const VkrTypeDesc *type = &vkr_scene_physics_collider_type;
  const uint32_t radius = vkr_type_find_property(type, string8_lit("radius"));
  const uint32_t extent =
      vkr_type_find_property(type, string8_lit("half_extent"));
  collider->shape = VKR_PHYSICS_BOX;
  assert(vkr_type_property_state(type, collider, radius, NULL).flags &
         VKR_PROPERTY_STATE_HIDDEN);
  assert(!(vkr_type_property_state(type, collider, extent, NULL).flags &
           VKR_PROPERTY_STATE_HIDDEN));
  collider->shape = VKR_PHYSICS_SPHERE;
  assert(!(vkr_type_property_state(type, collider, radius, NULL).flags &
           VKR_PROPERTY_STATE_HIDDEN));
  assert(vkr_type_property_state(type, collider, extent, NULL).flags &
         VKR_PROPERTY_STATE_HIDDEN);
}

/* The World's physics settings set the gravity a scene's bodies fall with.
   They are World-only: a scene cannot hold them and resolves the World's
   even when it does not inherit the World. */
static void physics_test_world_gravity(VkrAllocator *allocator) {
  VkrScene root;
  assert(vkr_scene_init(&root, allocator, VKR_SCENE_WORLD_ROOT_ID, 16, NULL));
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 29, 16, NULL));
  VkrEntityId body = physics_test_entity(&scene, vec3_new(0, 10, 0));
  ScenePhysicsSettings settings = {.gravity = vec3_new(0.0f, -1.0f, 0.0f)};
  assert(!vkr_scene_set_typed(&scene, vkr_scene_create_entity(&scene, NULL),
                              &vkr_scene_physics_settings_type, &settings));
  VkrEntityId settings_entity = vkr_scene_create_entity(&root, NULL);
  assert(vkr_scene_set_typed(&root, settings_entity,
                             &vkr_scene_physics_settings_type, &settings));
  scene.settings.inherit_world = false_v;
  vkr_scene_set_world_fallback(&scene, &root);
  (void)vkr_scene_resolve_world(&scene);
  assert(vkr_scene_gravity(&scene).y == -1.0f);

  /* An edit to the World re-resolves the scene. */
  const SceneAnimationWorldSettings animation = {.time_scale = 0.5f};
  assert(vkr_scene_animation_time_scale(&scene) == 1.0f);
  assert(vkr_scene_set_typed(&root, settings_entity,
                             &vkr_scene_animation_settings_type, &animation));
  assert(vkr_scene_resolve_world(&scene));
  assert(vkr_scene_animation_time_scale(&scene) == 0.5f);
  vkr_scene_update(&scene, 0.0);
  const char *error = NULL;
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  config.body.linear_damping = 0;
  assert(vkr_scene_physics_apply(&scene, body, &config, &error));
  for (uint32_t i = 0; i < 60; ++i) {
    assert(vkr_scene_physics_step(&scene, &error));
  }
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(&scene, body, &pose));
  assert(fabsf(pose.linear_velocity[1] + 1.0f) < 0.01f);
  settings.gravity = vec3_new(0.0f, 2000.0f, 0.0f);
  assert(!vkr_type_validate(&vkr_scene_physics_settings_type, &settings, NULL,
                            0u));
  vkr_scene_shutdown(&scene, NULL);
  vkr_scene_shutdown(&root, NULL);
}

/* An edit journal finalizes the graph after every edit, physics or not. A
   finalize with nothing prepared must not leave the graph finalized, or the
   next body edit is refused. */
static void physics_test_empty_finalize(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 7, 16, NULL));
  const char *error = NULL;
  const VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  const VkrEntityId first = physics_test_entity(&scene, vec3_new(0, 5, 0));
  assert(vkr_scene_physics_apply(&scene, first, &config, &error));
  assert(vkr_scene_physics_prepare_complete(&scene, &error));
  const VkrEntityId second = physics_test_entity(&scene, vec3_new(3, 5, 0));
  assert(vkr_scene_physics_apply(&scene, second, &config, &error));
  assert(vkr_scene_physics_body_count(&scene) == 2u);
  vkr_scene_shutdown(&scene, NULL);
}

static void physics_test_after_tick(VkrScene *scene, uint64_t tick,
                                    void *context) {
  (void)scene;
  (void)tick;
  (void)context;
}

/* Generated bodies alone (brush collision, trigger sensors) step the scene,
 * report sensor pairs, and come back after a reset rebuilds the world. */
static void physics_test_generated(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 9, 16, NULL));
  const char *error = NULL;
  const VkrEntityId sensor = physics_test_entity(&scene, vec3_zero());
  const VkrEntityId floor = physics_test_entity(&scene, vec3_zero());
  const VkrEntityId walker = physics_test_entity(&scene, vec3_zero());
  VkrPhysicsColliderDesc box = {.entity_id = sensor.u64,
                                .shape = VKR_PHYSICS_BOX,
                                .position = {0, 1, 0},
                                .rotation = {0, 0, 0, 1},
                                .scale = {1, 1, 1},
                                .half_extent = {1, 1, 1},
                                .enabled = true_v};
  assert(vkr_scene_physics_generated_set(&scene, 1u, sensor, &box, 1u, true_v,
                                         &error));
  box.entity_id = floor.u64;
  box.position[1] = -10.0f;
  box.half_extent[1] = 0.5f;
  assert(vkr_scene_physics_generated_set(&scene, 2u, floor, &box, 1u, false_v,
                                         &error));
  const VkrPhysicsCharacterDesc settings = vkr_physics_character_default();
  const Vec3 foot = vec3_new(0, 0.5f, 0);
  assert(vkr_scene_character_create(&scene, walker, &settings, &foot, &error));
  assert(vkr_scene_physics_body_count(&scene) == 0u);
  /* A session's callbacks run the clock, as the script host's do. */
  const VkrSceneSimulationCallbacks callbacks = {.after_tick =
                                                     physics_test_after_tick};
  assert(vkr_scene_simulation_configure(&scene, &callbacks, &error));
  vkr_scene_physics_set_paused(&scene, false_v);
  for (uint32_t i = 0; i < 3u; ++i) {
    vkr_scene_update(&scene, VKR_SCENE_PHYSICS_FIXED_DT);
  }
  VkrPhysicsSensorEvent events[8];
  uint32_t count = 0u;
  assert(vkr_scene_physics_sensor_events(&scene, events, 8u, &count));
  assert(count == 1u && events[0].began);
  assert(
      (events[0].entity_a == sensor.u64 && events[0].entity_b == walker.u64) ||
      (events[0].entity_b == sensor.u64 && events[0].entity_a == walker.u64));

  /* After a reset the generated floor still stops a ray. */
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_scene_simulation_detach(&scene, NULL));
  VkrPhysicsRayHit hit;
  assert(vkr_scene_physics_raycast(&scene, vec3_new(0, -5, 0),
                                   vec3_new(0, -10, 0), &hit));
  assert(hit.entity_id == floor.u64);
  vkr_scene_shutdown(&scene, NULL);
}

/* Origin rebase (ADR-086). Oracles: the floor stops a ray at the shifted
 * place, a falling body keeps falling onto it after the shift instead of
 * jumping back, and restoring returns a root to its exact authored bits. */
static void physics_test_rebase(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 11, 16, NULL));
  const char *error = NULL;
  const VkrEntityId floor = physics_test_entity(&scene, vec3_zero());
  const VkrPhysicsColliderDesc box = {.entity_id = floor.u64,
                                      .shape = VKR_PHYSICS_BOX,
                                      .position = {4100.3f, -0.5f, 0.0f},
                                      .rotation = {0, 0, 0, 1},
                                      .scale = {1, 1, 1},
                                      .half_extent = {8, 0.5f, 8},
                                      .enabled = true_v};
  assert(vkr_scene_physics_generated_set(&scene, 1u, floor, &box, 1u, false_v,
                                         &error));
  const Vec3 marked = vec3_new(4100.123f, 0.0f, 7.77f);
  const VkrEntityId marker = physics_test_entity(&scene, marked);
  const VkrEntityId ball =
      physics_test_entity(&scene, vec3_new(4100.7f, 3.0f, 0.3f));
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  assert(vkr_scene_physics_apply(&scene, ball, &config, &error));
  vkr_scene_update(&scene, 0.0);
  for (uint32_t i = 0; i < 10u; ++i) {
    assert(vkr_scene_physics_step(&scene, &error));
  }
  VkrPhysicsPose before;
  assert(vkr_scene_physics_get_pose(&scene, ball, &before));

  const Vec3 shift = vec3_new(4096.0f, 0.0f, 0.0f);
  VkrScene *scenes[1] = {&scene};
  assert(vkr_scene_shift_origin(&scene, shift));
  assert(vkr_scene_physics_shift(scenes, 1u, shift, &error));
  VkrPhysicsPose after;
  assert(vkr_scene_physics_get_pose(&scene, ball, &after));
  assert(fabsf(after.position[0] - (before.position[0] - 4096.0f)) < 1e-3f);
  assert(fabsf(after.position[1] - before.position[1]) < 1e-4f);
  VkrPhysicsRayHit hit;
  assert(vkr_scene_physics_raycast(&scene, vec3_new(0.3f, 5.0f, -5.0f),
                                   vec3_new(0.0f, -10.0f, 0.0f), &hit));
  assert(hit.entity_id == floor.u64);
  for (uint32_t i = 0; i < 120u; ++i) {
    assert(vkr_scene_physics_step(&scene, &error));
  }
  vkr_scene_update(&scene, 0.0);
  assert(vkr_scene_physics_get_pose(&scene, ball, &after));
  assert(after.position[1] > 0.0f && after.position[1] < 1.0f &&
         fabsf(after.position[0] - 4.7f) < 0.5f);

  const Vec3 undone = vkr_scene_restore_origin(&scene);
  assert(undone.x == 4096.0f);
  assert(
      vkr_scene_physics_shift(scenes, 1u, vec3_scale(undone, -1.0f), &error));
  const SceneTransform *transform = vkr_scene_get_transform(&scene, marker);
  assert(MemCompare(&transform->position, &marked, sizeof(marked)) == 0);
  assert(vkr_scene_physics_raycast(&scene, vec3_new(4096.3f, 5.0f, -5.0f),
                                   vec3_new(0.0f, -10.0f, 0.0f), &hit));
  assert(hit.entity_id == floor.u64);
  vkr_scene_shutdown(&scene, NULL);
}

/* The floor's top as a downward ray at (x, z) meets it, or NAN on a miss. */
static float32_t physics_test_floor_top(VkrScene *scene, float32_t x,
                                        float32_t z) {
  VkrPhysicsRayHit hit;
  if (!vkr_scene_physics_raycast(scene, vec3_new(x, 5.0f, z),
                                 vec3_new(0.0f, -10.0f, 0.0f), &hit)) {
    return NAN;
  }
  return hit.position[1];
}

/* A kinematic generated body, as a mover's brushes use (ADR-084). Oracles:
 * a downward ray meets its top where the targets moved it, an origin rebase
 * keeps it there while it keeps its target, and a reset puts it back at
 * rest. A body that ignored the rebase would jump back 100 m. */
static void physics_test_generated_kinematic(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 13, 16, NULL));
  const char *error = NULL;
  const VkrEntityId floor = physics_test_entity(&scene, vec3_zero());
  const VkrPhysicsColliderDesc box = {.entity_id = floor.u64,
                                      .shape = VKR_PHYSICS_BOX,
                                      .position = {0.0f, -0.5f, 0.0f},
                                      .rotation = {0, 0, 0, 1},
                                      .scale = {1, 1, 1},
                                      .half_extent = {4.0f, 0.5f, 4.0f},
                                      .enabled = true_v};
  assert(vkr_scene_physics_generated_set_kinematic(&scene, 7u, floor, &box, 1u,
                                                   false_v, &error));
  assert(fabsf(physics_test_floor_top(&scene, 0.5f, 0.5f)) < 1e-3f);
  /* Only a kinematic generated body takes a target. */
  assert(!vkr_scene_physics_generated_move(
      &scene, 9u, vec3_zero(), vkr_quat_identity(), vec3_zero(), &error));
  /* A session's callbacks run the clock, as the script host's do. */
  const VkrSceneSimulationCallbacks callbacks = {.after_tick =
                                                     physics_test_after_tick};
  assert(vkr_scene_simulation_configure(&scene, &callbacks, &error));

  /* One metre up over 60 ticks. */
  for (uint32_t i = 1; i <= 60u; ++i) {
    assert(vkr_scene_physics_generated_move(
        &scene, 7u, vec3_new(0.0f, (float32_t)i / 60.0f, 0.0f),
        vkr_quat_identity(), vec3_zero(), &error));
    assert(vkr_scene_physics_step(&scene, &error));
  }
  assert(fabsf(physics_test_floor_top(&scene, 0.5f, 0.5f) - 1.0f) < 1e-3f);

  /* An origin rebase moves it with the world, and its target stays an
     offset from rest. */
  const Vec3 shift = vec3_new(100.0f, 0.0f, 0.0f);
  VkrScene *scenes[1] = {&scene};
  assert(vkr_scene_shift_origin(&scene, shift));
  assert(vkr_scene_physics_shift(scenes, 1u, shift, &error));
  assert(vkr_scene_physics_step(&scene, &error));
  assert(fabsf(physics_test_floor_top(&scene, -99.5f, 0.5f) - 1.0f) < 1e-3f);
  assert(isnan(physics_test_floor_top(&scene, 0.5f, 0.5f)));

  /* A reset rebuilds it at rest. */
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(fabsf(physics_test_floor_top(&scene, -99.5f, 0.5f)) < 1e-3f);
  assert(vkr_scene_physics_step(&scene, &error));
  assert(fabsf(physics_test_floor_top(&scene, -99.5f, 0.5f)) < 1e-3f);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_simulation_detach(&scene, NULL));
  vkr_scene_shutdown(&scene, NULL);
}

/* The first hit of a ray from `from` along `along`, or NAN. */
static float32_t physics_test_hit_x(VkrScene *scene, Vec3 from, Vec3 along) {
  VkrPhysicsRayHit hit = {0};
  if (!vkr_scene_physics_raycast(scene, from, along, &hit)) {
    return NAN;
  }
  return hit.position[0];
}

/* A turning mover's body (ADR-084): a 2 m door from x 3 to 5 hinged at
 * x = 3 turns 90 degrees about +Y, so +X swings to -Z and the door then
 * stands along z from 0 to -2 at x = 3. Oracle: a ray along +X at z = -1
 * misses it at rest and meets its face at x = 2.95 once turned; a turn
 * about the body's origin instead would leave nothing there. */
static void physics_test_generated_turn(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 14, 16, NULL));
  const char *error = NULL;
  const VkrEntityId door = physics_test_entity(&scene, vec3_zero());
  const VkrPhysicsColliderDesc box = {.entity_id = door.u64,
                                      .shape = VKR_PHYSICS_BOX,
                                      .position = {4.0f, 1.0f, 0.0f},
                                      .rotation = {0, 0, 0, 1},
                                      .scale = {1, 1, 1},
                                      .half_extent = {1.0f, 1.0f, 0.05f},
                                      .enabled = true_v};
  assert(vkr_scene_physics_generated_set_kinematic(&scene, 8u, door, &box, 1u,
                                                   false_v, &error));
  const VkrSceneSimulationCallbacks callbacks = {.after_tick =
                                                     physics_test_after_tick};
  assert(vkr_scene_simulation_configure(&scene, &callbacks, &error));
  const Vec3 from = vec3_new(0.0f, 1.0f, -1.0f);
  const Vec3 along = vec3_new(10.0f, 0.0f, 0.0f);
  assert(isnan(physics_test_hit_x(&scene, from, along)));

  const VkrQuat quarter =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 0.5f * VKR_PI);
  assert(vkr_scene_physics_generated_move(&scene, 8u, vec3_zero(), quarter,
                                          vec3_new(3.0f, 0.0f, 0.0f), &error));
  assert(vkr_scene_physics_step(&scene, &error));
  assert(vkr_scene_physics_step(&scene, &error));
  assert(fabsf(physics_test_hit_x(&scene, from, along) - 2.95f) < 1e-3f);

  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_simulation_detach(&scene, NULL));
  vkr_scene_shutdown(&scene, NULL);
}

/* An entity moves at runtime, and so casts as a moving object and stays out
   of bakes, when it or an ancestor has an enabled kinematic or dynamic
   body, or, for a brush, a mover: a dynamic body's child moves, a static
   body's does not, and below a mover a brush moves while a plain entity does
   not, since movers move only brushes. A body edit asks for the meshes'
   mobility to be reclassified. */
static void physics_test_moves(VkrAllocator *allocator) {
  VkrScene scene;
  assert(vkr_scene_init(&scene, allocator, 11, 16, NULL));
  const char *error = NULL;
  const VkrEntityId crate = physics_test_entity(&scene, vec3_new(0, 5, 0));
  const VkrEntityId label = physics_test_entity(&scene, vec3_zero());
  vkr_scene_set_parent(&scene, label, crate);
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  config.body.motion = VKR_PHYSICS_DYNAMIC;
  scene.shadow_mobility_dirty = false_v;
  assert(vkr_scene_physics_apply(&scene, crate, &config, &error));
  assert(scene.shadow_mobility_dirty);
  assert(vkr_scene_entity_moves(&scene, crate));
  assert(vkr_scene_entity_moves(&scene, label));

  const VkrEntityId ledge = physics_test_entity(&scene, vec3_new(5, 0, 0));
  config.body.motion = VKR_PHYSICS_STATIC;
  assert(vkr_scene_physics_apply(&scene, ledge, &config, &error));
  assert(!vkr_scene_entity_moves(&scene, ledge));

  const VkrEntityId door = physics_test_entity(&scene, vec3_new(-5, 0, 0));
  SceneMover mover;
  vkr_scene_mover_type.defaults(&mover);
  mover.direction = vec3_new(0, 1, 0);
  mover.distance = 2.0f;
  assert(vkr_scene_set_typed(&scene, door, &vkr_scene_mover_type, &mover));
  const VkrEntityId leaf = physics_test_entity(&scene, vec3_zero());
  const VkrEntityId sign = physics_test_entity(&scene, vec3_zero());
  vkr_scene_set_parent(&scene, leaf, door);
  vkr_scene_set_parent(&scene, sign, door);
  SceneBrushSettings brush;
  vkr_scene_brush_type.defaults(&brush);
  assert(vkr_scene_set_typed(&scene, leaf, &vkr_scene_brush_type, &brush));
  assert(vkr_scene_entity_moves(&scene, leaf));
  assert(!vkr_scene_entity_moves(&scene, sign));
  vkr_scene_shutdown(&scene, NULL);
}

bool32_t run_scene_physics_tests(void) {
  printf("--- Starting Scene Physics Tests ---\n");
  physics_test_descriptors();
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  physics_test_world_gravity(&allocator);
  physics_test_empty_finalize(&allocator);
  physics_test_generated(&allocator);
  physics_test_generated_kinematic(&allocator);
  physics_test_generated_turn(&allocator);
  physics_test_rebase(&allocator);
  physics_test_moves(&allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 31, 16, NULL));
  VkrEntityId owner = physics_test_entity(&scene, vec3_new(0, 10, 0));
  VkrEntityId visual = physics_test_entity(&scene, vec3_new(0, 2, 0));
  vkr_scene_set_parent(&scene, visual, owner);
  vkr_scene_update(&scene, 0.0);
  assert(vkr_scene_get_transform(&scene, visual)->world.elements[13] == 12);
  assert(scene.physics == NULL);

  const char *error = NULL;
  VkrScenePhysicsSnapshot config = vkr_scene_physics_default();
  config.body.mass = 2;
  config.body.linear_damping = 0;
  config.body.angular_damping = 0;
  assert(vkr_scene_physics_apply(&scene, owner, &config, &error));
  VkrEntityId collider = vkr_scene_physics_collider_entity(&scene, owner, 1);
  assert(vkr_scene_entity_alive(&scene, collider));
  assert(vkr_scene_get_transform(&scene, collider)->parent.u64 == owner.u64);
  assert(vkr_scene_physics_owner(&scene, collider).u64 == owner.u64);
  assert(vkr_scene_get_render_id(&scene, collider) == 0);
  assert(!vkr_scene_ensure_render_id(&scene, collider, NULL));
  vkr_scene_set_position(&scene, collider, vec3_new(100, 0, 0));
  assert(vkr_scene_get_transform(&scene, collider)->position.x == 0);
  vkr_scene_destroy_entity(&scene, collider);
  assert(vkr_scene_entity_alive(&scene, collider));

  assert(vkr_scene_physics_impulse(&scene, owner, vec3_new(6, 0, 0), NULL,
                                   &error));
  VkrPhysicsPose pose;
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(fabsf(pose.linear_velocity[0] - 3) < 0.0001f);
  for (uint32_t i = 0; i < 60; ++i) {
    assert(vkr_scene_physics_step(&scene, &error));
  }
  vkr_scene_update(&scene, 0.0);
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(fabsf(pose.position[0] - 3) < 0.01f);
  assert(fabsf(pose.position[1] - 5.095f) < 0.15f);
  assert(fabsf(pose.linear_velocity[1] + 9.81f) < 0.01f);
  assert(fabs(vkr_scene_physics_time(&scene) - 1.0) < 1e-9);
  assert(vkr_scene_get_transform(&scene, owner)->position.y == 10);
  assert(vkr_scene_get_transform(&scene, owner)->position.x == 0);
  assert(fabsf(vkr_scene_get_transform(&scene, visual)->world.elements[13] -
               pose.position[1] - 2) < 0.001f);
  VkrScenePhysicsSnapshot read;
  assert(vkr_scene_physics_read(&scene, owner, &read));
  assert(read.body.mass == 2 && read.colliders[0].position.y == 0);

  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(pose.position[1] == 10 && pose.linear_velocity[0] == 0);
  assert(vkr_scene_physics_time(&scene) == 0);
  vkr_scene_physics_set_paused(&scene, false_v);
  assert(!vkr_scene_physics_apply(&scene, owner, &config, &error));
  assert(!vkr_scene_physics_step(&scene, &error));
  vkr_scene_set_position(&scene, owner, vec3_zero());
  assert(vkr_scene_get_transform(&scene, owner)->position.y == 10);
  vkr_scene_update(&scene, VKR_SCENE_PHYSICS_FIXED_DT * 0.5);
  assert(vkr_scene_physics_time(&scene) == 0);
  vkr_scene_update(&scene, VKR_SCENE_PHYSICS_FIXED_DT * 0.5);
  assert(fabs(vkr_scene_physics_time(&scene) - VKR_SCENE_PHYSICS_FIXED_DT) <
         1e-12);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_set_disabled(&scene, true_v, &error));
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  float32_t stopped_y = pose.position[1];
  assert(vkr_scene_physics_step(&scene, &error));
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(pose.position[1] == stopped_y);
  assert(vkr_scene_physics_set_body_disabled(&scene, owner, true_v, &error));
  assert(vkr_scene_physics_body_is_disabled(&scene, owner));
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(!vkr_scene_physics_is_disabled(&scene));
  assert(!vkr_scene_physics_body_is_disabled(&scene, owner));
  assert(vkr_scene_physics_read(&scene, owner, &read) && read.body.enabled);
  assert(vkr_scene_physics_step(&scene, &error));
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(pose.linear_velocity[1] < 0);

  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_scene_physics_impulse(&scene, owner, vec3_new(6, 0, 0), NULL,
                                   &error));
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, 1.5 * VKR_SCENE_PHYSICS_FIXED_DT);
  assert(fabsf(vkr_scene_get_transform(&scene, owner)->world.elements[12] -
               0.025f) < 0.001f);
  vkr_scene_physics_set_paused(&scene, true_v);
  vkr_scene_update(&scene, 0.0);
  assert(fabsf(vkr_scene_get_transform(&scene, owner)->world.elements[12] -
               0.05f) < 0.001f);
  assert(vkr_scene_physics_reset(&scene, &error));
  vkr_scene_physics_set_paused(&scene, false_v);
  vkr_scene_update(&scene, 0.5);
  assert(fabs(vkr_scene_physics_time(&scene) - 8 * VKR_SCENE_PHYSICS_FIXED_DT) <
         1e-9);
  assert(vkr_scene_physics_debt(&scene) > 0.36);
  for (uint32_t i = 0; i < 3; ++i) {
    vkr_scene_update(&scene, 0.0);
  }
  assert(fabs(vkr_scene_physics_time(&scene) - 0.5) < 1e-9);
  assert(vkr_scene_physics_debt(&scene) < 1e-9);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_step(&scene, &error));
  assert(fabs(vkr_scene_physics_animation_delta(&scene, 0.0) -
              VKR_SCENE_PHYSICS_FIXED_DT) < 1e-9);
  assert(vkr_scene_physics_animation_delta(&scene, 0.0) == 0.0);

  assert(vkr_scene_physics_reset(&scene, &error));
  vkr_scene_physics_set_paused(&scene, false_v);
  for (uint32_t i = 0; i < 60; ++i) {
    vkr_scene_update(&scene, 1.0);
  }
  assert(vkr_scene_physics_is_paused(&scene));
  assert(vkr_scene_physics_error(&scene) != NULL);
  assert(vkr_scene_physics_debt(&scene) > 51.0);
  vkr_scene_physics_set_paused(&scene, true_v);
  assert(vkr_scene_physics_debt(&scene) > 51.0);
  assert(vkr_scene_physics_reset(&scene, &error));
  assert(vkr_scene_physics_debt(&scene) == 0.0);
  assert(vkr_scene_physics_error(&scene) == NULL);

  VkrScenePhysicsPrepared *prepared = NULL;
  config.collider_count = 2;
  config.colliders[1] = config.colliders[0];
  config.colliders[1].authored_id = 2;
  config.colliders[1].position.x = 2;
  assert(vkr_scene_physics_prepare(&scene, owner, &config, &prepared, &error));
  vkr_scene_physics_discard(prepared);
  assert(vkr_scene_physics_read(&scene, owner, &read));
  assert(read.collider_count == 1);
  assert(vkr_scene_physics_collider_entity(&scene, owner, 1).u64 ==
         collider.u64);
  assert(vkr_scene_physics_apply(&scene, owner, &config, &error));
  assert(vkr_scene_physics_collider_entity(&scene, owner, 1).u64 ==
         collider.u64);
  VkrEntityId second = vkr_scene_physics_collider_entity(&scene, owner, 2);
  assert(vkr_scene_entity_alive(&scene, second));
  config.colliders[1].authored_id = 1;
  assert(!vkr_scene_physics_apply(&scene, owner, &config, &error));
  config.colliders[1].authored_id = 2;
  assert(vkr_scene_physics_apply(&scene, visual, &config, &error));
  assert(vkr_scene_physics_apply(&scene, visual, NULL, &error));

  config.collider_count = 0;
  assert(vkr_scene_physics_apply(&scene, owner, &config, &error));
  assert(!vkr_scene_entity_alive(&scene, collider));
  assert(!vkr_scene_entity_alive(&scene, second));
  assert(vkr_scene_physics_read(&scene, owner, &read) && read.present);
  assert(vkr_scene_physics_prepare(&scene, owner, &config, &prepared, &error));
  VkrScenePhysicsPrepared *duplicate = NULL;
  assert(
      !vkr_scene_physics_prepare(&scene, owner, &config, &duplicate, &error));
  assert(duplicate == NULL);
  vkr_scene_physics_discard(prepared);
  assert(!vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(vkr_scene_physics_step(&scene, &error));
  assert(vkr_scene_physics_apply(&scene, owner, NULL, &error));
  vkr_scene_update(&scene, 0.0);
  assert(vkr_scene_get_transform(&scene, owner)->world.elements[13] == 10);
  assert(vkr_scene_physics_body_count(&scene) == 0);

  config = vkr_scene_physics_default();
  config.body.motion = VKR_PHYSICS_KINEMATIC;
  assert(vkr_scene_physics_apply(&scene, owner, &config, &error));
  assert(vkr_scene_physics_set_kinematic_target(
      &scene, owner, vec3_new(2, 10, 0), vkr_quat_identity(), &error));
  assert(vkr_scene_physics_step(&scene, &error));
  assert(vkr_scene_physics_get_pose(&scene, owner, &pose));
  assert(fabsf(pose.position[0] - 2) < 0.001f);
  assert(vkr_scene_get_transform(&scene, owner)->position.x == 0);
  collider = vkr_scene_physics_collider_entity(&scene, owner, 1);
  vkr_scene_destroy_entity(&scene, owner);
  assert(!vkr_scene_entity_alive(&scene, collider));
  assert(vkr_scene_entity_alive(&scene, visual));
  assert(vkr_scene_physics_body_count(&scene) == 0);
  assert(!vkr_scene_physics_impulse(&scene, owner, vec3_one(), NULL, &error));
  assert(!vkr_scene_physics_read(&scene, owner, &read));
  physics_test_hierarchy_contacts(&scene);
  vkr_scene_shutdown(&scene, NULL);
  physics_test_shared_set(&allocator);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("--- Scene Physics Tests Passed ---\n");
  return true_v;
}

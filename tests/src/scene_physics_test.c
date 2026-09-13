#include "scene_physics_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"
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
  config.motion = VKR_PHYSICS_STATIC;
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
  config.motion = VKR_PHYSICS_KINEMATIC;
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

/* Gravity/impulse have analytic oracles independent of contact generation.
 * Real ECS create/remove and evaluated hierarchy detect stale references and
 * accidental publication of simulated poses into authored save state. */
bool32_t run_scene_physics_tests(void) {
  printf("--- Starting Scene Physics Tests ---\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
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
  config.mass = 2;
  config.linear_damping = 0;
  config.angular_damping = 0;
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
  assert(read.mass == 2 && read.colliders[0].position.y == 0);

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
  assert(vkr_scene_physics_read(&scene, owner, &read) && read.enabled);
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
  config.motion = VKR_PHYSICS_KINEMATIC;
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
  vkr_dmemory_allocator_destroy(&allocator);
  printf("--- Scene Physics Tests Passed ---\n");
  return true_v;
}

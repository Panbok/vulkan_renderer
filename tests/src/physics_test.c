#include "physics_test.h"
#include "physics/vkr_physics.h"

static bool8_t step_and_drain(VkrPhysicsWorld *world, float32_t dt) {
  if (!vkr_physics_step(world, dt)) {
    return false_v;
  }
  VkrPhysicsContactEvent events[1024];
  uint32_t count;
  return vkr_physics_contact_events(world, events, ArrayCount(events), &count);
}

static VkrPhysicsColliderDesc box(uint64_t id) {
  VkrPhysicsColliderDesc shape = {
      .entity_id = id,
      .shape = VKR_PHYSICS_BOX,
      .rotation = {0, 0, 0, 1},
      .scale = {1, 1, 1},
      .half_extent = {0.5f, 0.5f, 0.5f},
      .enabled = true_v,
  };
  return shape;
}

static VkrPhysicsBodyDesc body_desc(uint64_t id,
                                    const VkrPhysicsColliderDesc *collider) {
  VkrPhysicsBodyDesc desc = {
      .entity_id = id,
      .motion = VKR_PHYSICS_DYNAMIC,
      .rotation = {0, 0, 0, 1},
      .mass = 2.0f,
      .friction = 0.5f,
      .gravity_factor = 1.0f,
      .enabled = true_v,
      .allow_sleep = true_v,
      .collision_layer = 1,
      .collision_mask = UINT16_MAX,
      .colliders = collider,
      .collider_count = 1,
  };
  return desc;
}

static void test_gravity_impulse_and_handles(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world);
  VkrPhysicsColliderDesc collider = box(UINT64_C(0xabcdef1200000001));
  collider.position[0] = 2.0f;
  VkrPhysicsBodyDesc desc = body_desc(UINT64_C(0xfedcba9800000001), &collider);
  desc.position[1] = 10.0f;
  VkrPhysicsBody body;
  assert(vkr_physics_body_create(world, &desc, &body));
  VkrPhysicsPose pose;
  assert(vkr_physics_body_get_pose(world, body, &pose));
  assert(fabsf(pose.position[0]) < 0.0001f);
  const float32_t impulse[3] = {0, 4, 0};
  assert(vkr_physics_body_impulse(world, body, impulse, NULL));
  assert(vkr_physics_body_get_pose(world, body, &pose));
  assert(fabsf(pose.linear_velocity[1] - 2.0f) < 0.0001f);
  // Apply away from the COM (x=2): expected torque along negative Z.
  const float32_t point[3] = {0, 10, 0};
  assert(vkr_physics_body_impulse(world, body, impulse, point));
  assert(vkr_physics_body_get_pose(world, body, &pose));
  assert(pose.angular_velocity[2] < -1.0f);
  assert(vkr_physics_body_destroy(world, body));
  assert(!vkr_physics_body_get_pose(world, body, &pose));
  VkrPhysicsBody replacement;
  collider.position[0] = 0;
  assert(vkr_physics_body_create(world, &desc, &replacement));
  assert(body != replacement);
  assert(!vkr_physics_body_destroy(world, body));
  for (uint32_t tick = 0; tick < 60; ++tick) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  assert(vkr_physics_body_get_pose(world, replacement, &pose));
  assert(fabsf(pose.linear_velocity[1] + 9.81f) < 0.002f);
  // Semi-implicit integration differs from analytic gravity by <= one step.
  assert(fabsf(pose.position[1] - 5.095f) < 0.09f);
  const float32_t held_y = pose.position[1];
  const float32_t held_velocity = pose.linear_velocity[1];
  assert(vkr_physics_body_set_enabled(world, replacement, false_v));
  assert(step_and_drain(world, 1.0f / 60.0f));
  assert(vkr_physics_body_get_pose(world, replacement, &pose));
  assert(!pose.active && pose.position[1] == held_y);
  assert(pose.linear_velocity[1] == held_velocity);
  assert(vkr_physics_body_set_enabled(world, replacement, true_v));
  assert(step_and_drain(world, 1.0f / 60.0f));
  assert(vkr_physics_body_get_pose(world, replacement, &pose));
  assert(pose.active && pose.position[1] < held_y);

  VkrPhysicsWorld *other = vkr_physics_world_create(2);
  assert(other);
  VkrPhysicsBody other_body;
  assert(vkr_physics_body_create(other, &desc, &other_body));
  assert(!vkr_physics_body_get_pose(other, replacement, &pose));
  vkr_physics_world_destroy(other);
  vkr_physics_world_destroy(world);
}

static void test_compound_queries_and_capacity(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(1);
  assert(world);
  VkrPhysicsColliderDesc shapes[2] = {box(UINT64_C(0xabcdef1200000001)),
                                      box(UINT64_C(0xabcdef1200000002))};
  shapes[0].position[0] = -2;
  shapes[1].position[0] = 2;
  VkrPhysicsBodyDesc desc = body_desc(UINT64_C(0xfedcba9800000001), shapes);
  desc.collider_count = 2;
  desc.motion = VKR_PHYSICS_STATIC;
  VkrPhysicsBody body;
  assert(vkr_physics_body_create(world, &desc, &body));
  VkrPhysicsBody excess;
  desc.entity_id++;
  assert(!vkr_physics_body_create(world, &desc, &excess));
  const float32_t origin[3] = {2, 3, 0};
  const float32_t ray[3] = {0, -6, 0};
  VkrPhysicsRayHit hit;
  assert(vkr_physics_raycast(world, origin, ray, &hit));
  assert(hit.entity_id == UINT64_C(0xfedcba9800000001));
  assert(hit.collider_entity_id == shapes[1].entity_id);
  assert(fabsf(hit.position[1] - 0.5f) < 0.001f);
  assert(!vkr_physics_raycast_filtered(world, origin, ray, 2, &hit));
  VkrPhysicsOverlapHit overlaps[2];
  uint32_t count;
  const float32_t center[3] = {0, 0, 0};
  assert(vkr_physics_overlap_sphere(world, center, 3, 1, overlaps, 2, &count));
  assert(count == 2);
  assert(!vkr_physics_overlap_sphere(world, center, 3, 1, overlaps, 1, &count));
  assert(count == 0);
  assert(vkr_physics_body_set_enabled(world, body, false_v));
  assert(!vkr_physics_raycast(world, origin, ray, &hit));
  assert(vkr_physics_body_set_enabled(world, body, true_v));
  assert(vkr_physics_raycast(world, origin, ray, &hit));
  vkr_physics_world_destroy(world);
}

static void test_stack_sleep_and_support_removal(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(8);
  assert(world);
  VkrPhysicsColliderDesc collider = box(1);
  collider.half_extent[0] = 5;
  collider.half_extent[2] = 5;
  VkrPhysicsBodyDesc desc = body_desc(1, &collider);
  desc.motion = VKR_PHYSICS_STATIC;
  desc.position[1] = -0.5f;
  VkrPhysicsBody floor;
  assert(vkr_physics_body_create(world, &desc, &floor));
  VkrPhysicsBody stack[3];
  collider = box(2);
  desc.motion = VKR_PHYSICS_DYNAMIC;
  for (uint32_t i = 0; i < ArrayCount(stack); ++i) {
    desc.entity_id = i + 2;
    collider.entity_id = i + 2;
    desc.position[1] = 0.51f + (float32_t)i * 1.01f;
    assert(vkr_physics_body_create(world, &desc, &stack[i]));
  }
  for (uint32_t i = 0; i < 600; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  VkrPhysicsPose pose;
  for (uint32_t i = 0; i < ArrayCount(stack); ++i) {
    assert(vkr_physics_body_get_pose(world, stack[i], &pose));
    assert(fabsf(pose.position[1] - (0.5f + (float32_t)i)) < 0.06f);
    assert(fabsf(pose.linear_velocity[1]) < 0.05f);
    assert(!pose.active);
  }
  assert(vkr_physics_body_destroy(world, floor));
  for (uint32_t i = 0; i < 30; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  assert(vkr_physics_body_get_pose(world, stack[0], &pose));
  assert(pose.position[1] < -0.5f);
  vkr_physics_world_destroy(world);
}

static void test_sensor_sleep_and_disable(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world);
  VkrPhysicsColliderDesc collider = box(100);
  collider.half_extent[0] = 2;
  collider.half_extent[1] = 2;
  collider.half_extent[2] = 2;
  VkrPhysicsBodyDesc desc = body_desc(10, &collider);
  desc.motion = VKR_PHYSICS_STATIC;
  desc.sensor = true_v;
  VkrPhysicsBody sensor;
  assert(vkr_physics_body_create(world, &desc, &sensor));
  collider = box(200);
  desc.entity_id = 20;
  desc.motion = VKR_PHYSICS_DYNAMIC;
  desc.sensor = false_v;
  desc.gravity_factor = 0;
  VkrPhysicsBody occupant;
  assert(vkr_physics_body_create(world, &desc, &occupant));
  assert(step_and_drain(world, 1.0f / 60.0f));
  VkrPhysicsSensorEvent events[8];
  uint32_t count;
  assert(vkr_physics_sensor_events(world, events, 8, &count));
  assert(count == 1 && events[0].began);
  assert(events[0].entity_a == 10 && events[0].entity_b == 20);
  assert(events[0].collider_a == 100 && events[0].collider_b == 200);
  for (uint32_t i = 0; i < 120; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  VkrPhysicsPose pose;
  assert(vkr_physics_body_get_pose(world, occupant, &pose));
  assert(!pose.active);
  assert(vkr_physics_sensor_events(world, events, 8, &count));
  assert(count == 0); // Sleep does not emit a geometric exit.
  // Preparing and discarding a disabled replacement must not change the live
  // body's sleep state or sensor membership, even though its owner ID matches.
  desc.enabled = false_v;
  VkrPhysicsBody staged;
  assert(vkr_physics_body_create(world, &desc, &staged));
  assert(vkr_physics_body_reserve_destroy(world, occupant));
  assert(!step_and_drain(world, 1.0f / 60.0f));
  assert(!vkr_physics_body_set_enabled(world, staged, true_v));
  assert(vkr_physics_body_destroy(world, staged));
  vkr_physics_body_cancel_destroy(world, occupant);
  assert(vkr_physics_body_get_pose(world, occupant, &pose));
  assert(!pose.active);
  assert(vkr_physics_sensor_events(world, events, 8, &count));
  assert(count == 0);
  assert(vkr_physics_body_reserve_destroy(world, sensor));
  assert(vkr_physics_body_set_enabled(world, sensor, false_v));
  assert(vkr_physics_sensor_events(world, events, 8, &count));
  assert(count == 1 && !events[0].began);
  assert(step_and_drain(world, 1.0f / 60.0f));
  assert(vkr_physics_sensor_events(world, events, 8, &count));
  assert(count == 0);
  vkr_physics_world_destroy(world);
}

static void test_shapes_ccd_and_layers(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world);
  VkrPhysicsColliderDesc collider = box(1);
  collider.half_extent[0] = 5;
  collider.half_extent[1] = 0.05f;
  collider.half_extent[2] = 5;
  VkrPhysicsBodyDesc desc = body_desc(1, &collider);
  desc.motion = VKR_PHYSICS_STATIC;
  VkrPhysicsBody floor;
  assert(vkr_physics_body_create(world, &desc, &floor));
  collider = box(2);
  collider.shape = VKR_PHYSICS_SPHERE;
  collider.radius = 0.1f;
  desc.entity_id = 2;
  desc.motion = VKR_PHYSICS_DYNAMIC;
  desc.position[1] = 2;
  desc.continuous = true_v;
  VkrPhysicsBody sphere;
  assert(vkr_physics_body_create(world, &desc, &sphere));
  const float32_t impulse[3] = {0, -400, 0};
  assert(vkr_physics_body_impulse(world, sphere, impulse, NULL));
  assert(step_and_drain(world, 1.0f / 60.0f));
  VkrPhysicsPose pose;
  assert(vkr_physics_body_get_pose(world, sphere, &pose));
  assert(pose.position[1] > 0); // CCD catches a 10 cm slab crossed in one tick.
  assert(vkr_physics_body_destroy(world, sphere));
  collider.shape = VKR_PHYSICS_CAPSULE;
  collider.radius = 0.25f;
  collider.half_height = 0.5f;
  desc.continuous = false_v;
  assert(vkr_physics_body_create(world, &desc, &sphere));
  for (uint32_t i = 0; i < 180; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  assert(vkr_physics_body_get_pose(world, sphere, &pose));
  assert(fabsf(pose.position[1] - 0.8f) < 0.04f);
  assert(vkr_physics_body_destroy(world, sphere));
  desc.collision_mask =
      2; // Floor membership is 1: exclude pair in both phases.
  assert(vkr_physics_body_create(world, &desc, &sphere));
  for (uint32_t i = 0; i < 60; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  assert(vkr_physics_body_get_pose(world, sphere, &pose));
  assert(pose.position[1] < -2);
  vkr_physics_world_destroy(world);
}

static void test_sensor_mutation_reservation(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world);
  VkrPhysicsColliderDesc collider = box(1);
  VkrPhysicsBodyDesc desc = body_desc(1, &collider);
  desc.motion = VKR_PHYSICS_STATIC;
  desc.sensor = true_v;
  VkrPhysicsBody sensor;
  assert(vkr_physics_body_create(world, &desc, &sensor));
  desc.entity_id = 2;
  collider.entity_id = 2;
  desc.sensor = false_v;
  VkrPhysicsBody occupant;
  assert(vkr_physics_body_create(world, &desc, &occupant));
  for (uint32_t i = 0; i < 15; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
    assert(vkr_physics_body_set_enabled(world, sensor, false_v));
    assert(vkr_physics_body_set_enabled(world, sensor, true_v));
  }
  assert(step_and_drain(world, 1.0f / 60.0f)); // 31 events; one slot remains.
  assert(vkr_physics_body_reserve_destroy(world, sensor));
  assert(!vkr_physics_body_reserve_destroy(world, occupant));
  assert(
      vkr_physics_body_destroy(world, sensor)); // Reserved exit fits exactly.
  VkrPhysicsPose pose;
  assert(vkr_physics_body_get_pose(world, occupant, &pose));
  VkrPhysicsSensorEvent events[32];
  uint32_t count;
  assert(vkr_physics_sensor_events(world, events, 32, &count));
  assert(count == 32 && !events[31].began);
  vkr_physics_world_destroy(world);
}

static void test_sensor_queue_fault(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world);
  VkrPhysicsColliderDesc collider = box(1);
  VkrPhysicsBodyDesc desc = body_desc(1, &collider);
  desc.motion = VKR_PHYSICS_STATIC;
  desc.sensor = true_v;
  VkrPhysicsBody sensor;
  assert(vkr_physics_body_create(world, &desc, &sensor));
  desc.entity_id = 2;
  collider.entity_id = 2;
  desc.sensor = false_v;
  VkrPhysicsBody occupant;
  assert(vkr_physics_body_create(world, &desc, &occupant));
  for (uint32_t i = 0; i < 16; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
    assert(vkr_physics_body_set_enabled(world, sensor, false_v));
    assert(vkr_physics_body_set_enabled(world, sensor, true_v));
  }
  assert(!step_and_drain(world, 1.0f / 60.0f));
  VkrPhysicsPose pose;
  assert(!vkr_physics_body_get_pose(world, occupant, &pose));
  const float32_t origin[3] = {0, 2, 0};
  const float32_t ray[3] = {0, -4, 0};
  VkrPhysicsRayHit hit;
  assert(!vkr_physics_raycast(world, origin, ray, &hit));
  vkr_physics_world_destroy(world);
}

static void test_geometry_scale_and_sweep(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world);
  const float32_t vertices[] = {-5, 0, -5, -5, 0, 5, 5, 0, 5, 5, 0, -5};
  const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
  VkrPhysicsColliderDesc mesh = box(101);
  mesh.shape = VKR_PHYSICS_TRIANGLE_MESH;
  mesh.geometry = (VkrPhysicsGeometry){vertices, 4, indices, 6};
  mesh.scale[0] = 2;
  VkrPhysicsBodyDesc desc = body_desc(100, &mesh);
  VkrPhysicsBody floor;
  assert(!vkr_physics_body_create(world, &desc,
                                  &floor)); // Dynamic mesh forbidden.
  desc.motion = VKR_PHYSICS_STATIC;
  assert(vkr_physics_body_create(world, &desc, &floor));
  const float32_t cube[] = {-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f,
                            -0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f,
                            -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,
                            -0.5f, 0.5f,  0.5f,  0.5f, 0.5f,  0.5f};
  VkrPhysicsColliderDesc hull = box(201);
  hull.shape = VKR_PHYSICS_CONVEX_HULL;
  hull.geometry = (VkrPhysicsGeometry){cube, 8, NULL, 0};
  hull.scale[1] = 2;
  desc = body_desc(200, &hull);
  desc.position[1] = 3;
  VkrPhysicsBody falling;
  assert(vkr_physics_body_create(world, &desc, &falling));
  for (uint32_t i = 0; i < 180; ++i) {
    assert(step_and_drain(world, 1.0f / 60.0f));
  }
  VkrPhysicsPose pose;
  assert(vkr_physics_body_get_pose(world, falling, &pose));
  assert(fabsf(pose.position[1] - 1.0f) < 0.05f);
  VkrPhysicsColliderDesc query = box(999);
  query.shape = VKR_PHYSICS_SPHERE;
  query.radius = 0.5f;
  const float32_t origin[3] = {3, 3, 0};
  const float32_t rotation[4] = {0, 0, 0, 1};
  const float32_t displacement[3] = {0, -6, 0};
  VkrPhysicsRayHit hit;
  assert(vkr_physics_sweep(world, &query, origin, rotation, displacement, NULL,
                           &hit));
  assert(hit.entity_id == 100 && hit.collider_entity_id == 101);
  assert(fabsf(hit.fraction - 2.5f / 6.0f) < 0.005f);
  assert(fabsf(hit.position[1]) < 0.01f && hit.normal[1] > 0.99f);
  uint64_t ignored = 100;
  VkrPhysicsQueryFilter filter = {UINT16_MAX, true_v, &ignored, 1};
  assert(!vkr_physics_sweep(world, &query, origin, rotation, displacement,
                            &filter, &hit));
  assert(
      !vkr_physics_raycast_query(world, origin, displacement, &filter, &hit));
  filter.ignored_count = 0;
  filter.mask = 2;
  assert(!vkr_physics_sweep(world, &query, origin, rotation, displacement,
                            &filter, &hit));
  query.scale[0] = 2; // Ellipsoids are not silently approximated as spheres.
  assert(!vkr_physics_sweep(world, &query, origin, rotation, displacement, NULL,
                            &hit));
  hull.scale[0] = -1;
  desc.entity_id = 300;
  assert(!vkr_physics_body_create(world, &desc, &falling));
  vkr_physics_world_destroy(world);
}

static void test_contact_identity_and_reservation(void) {
  VkrPhysicsWorld *world = vkr_physics_world_create(2);
  assert(world);
  VkrPhysicsColliderDesc shape = box(UINT64_C(0xabcdef1200000001));
  VkrPhysicsBodyDesc desc = body_desc(UINT64_C(0xfedcba9800000001), &shape);
  desc.motion = VKR_PHYSICS_STATIC;
  desc.position[1] = -0.5f;
  VkrPhysicsBody floor;
  assert(vkr_physics_body_create(world, &desc, &floor));
  shape.entity_id++;
  desc.entity_id++;
  desc.motion = VKR_PHYSICS_DYNAMIC;
  desc.position[1] = 0.49f;
  VkrPhysicsBody body;
  assert(vkr_physics_body_create(world, &desc, &body));
  assert(vkr_physics_step(world, 1.0f / 60.0f));
  VkrPhysicsContactEvent events[256];
  uint32_t count;
  assert(vkr_physics_contact_events(world, events, 256, &count));
  assert(count == 1 && events[0].phase == VKR_PHYSICS_CONTACT_BEGIN);
  assert(events[0].body_a == floor && events[0].body_b == body);
  assert(events[0].collider_a == UINT64_C(0xabcdef1200000001));
  assert(events[0].collider_b == UINT64_C(0xabcdef1200000002));
  for (uint32_t i = 0; i < 255; ++i) {
    assert(vkr_physics_step(world, 1.0f / 60.0f));
  }
  assert(vkr_physics_body_reserve_destroy(world, floor));
  assert(!vkr_physics_body_reserve_destroy(world, body));
  assert(vkr_physics_body_destroy(world, floor));
  assert(vkr_physics_contact_events(world, events, 256, &count));
  assert(count == 256);
  assert(events[0].phase == VKR_PHYSICS_CONTACT_PERSIST);
  assert(events[255].phase == VKR_PHYSICS_CONTACT_END);
  assert(events[255].body_a == floor && events[255].body_b == body);
  assert(vkr_physics_step(world, 1.0f / 60.0f));
  assert(vkr_physics_contact_events(world, events, 256, &count));
  assert(count == 0);
  vkr_physics_world_destroy(world);
}

static void test_joint_limits_and_lifetime(void) {
  for (uint32_t type = VKR_PHYSICS_JOINT_FIXED;
       type <= VKR_PHYSICS_JOINT_SWING_TWIST; ++type) {
    VkrPhysicsWorld *world = vkr_physics_world_create(2);
    assert(world);
    VkrPhysicsColliderDesc shape = box(1);
    VkrPhysicsBodyDesc desc = body_desc(1, &shape);
    desc.motion = VKR_PHYSICS_STATIC;
    VkrPhysicsBody anchor;
    assert(vkr_physics_body_create(world, &desc, &anchor));
    desc.entity_id = 2;
    shape.entity_id = 2;
    desc.motion = VKR_PHYSICS_DYNAMIC;
    desc.position[1] = -2;
    VkrPhysicsBody body;
    assert(vkr_physics_body_create(world, &desc, &body));
    VkrPhysicsJointDesc joint_desc = {
        .type = (VkrPhysicsJointType)type,
        .body_a = anchor,
        .body_b = body,
        .anchor_a = {0, -1, 0},
        .anchor_b = {0, 1, 0},
        .axis_a = {0, 0, 1},
        .axis_b = {0, 0, 1},
        .normal_a = {1, 0, 0},
        .normal_b = {1, 0, 0},
        .min_limit = -0.25f,
        .max_limit = 0.25f,
        .swing_normal_limit = 0.2f,
        .swing_plane_limit = 0.2f,
        .enabled = true_v,
    };
    if (type == VKR_PHYSICS_JOINT_DISTANCE) {
      MemZero(joint_desc.anchor_a, sizeof(joint_desc.anchor_a));
      MemZero(joint_desc.anchor_b, sizeof(joint_desc.anchor_b));
      joint_desc.min_limit = 2;
      joint_desc.max_limit = 2;
    }
    VkrPhysicsJoint joint;
    assert(vkr_physics_joint_create(world, &joint_desc, &joint));
    const float32_t impulse[3] = {0, 10, 0};
    const float32_t point[3] = {1, -2, 0};
    assert(vkr_physics_body_impulse(world, body, impulse, point));
    for (uint32_t i = 0; i < 120; ++i) {
      assert(step_and_drain(world, 1.0f / 60.0f));
    }
    VkrPhysicsPose pose;
    assert(vkr_physics_body_get_pose(world, body, &pose));
    if (type == VKR_PHYSICS_JOINT_DISTANCE) {
      float32_t distance = sqrtf(pose.position[0] * pose.position[0] +
                                 pose.position[1] * pose.position[1] +
                                 pose.position[2] * pose.position[2]);
      assert(fabsf(distance - 2) < 0.04f);
    } else {
      const float32_t *q = pose.rotation;
      float32_t anchor_x = pose.position[0] + 2 * (q[0] * q[1] - q[3] * q[2]);
      float32_t anchor_y =
          pose.position[1] + 1 - 2 * (q[0] * q[0] + q[2] * q[2]);
      assert(fabsf(anchor_x) < 0.04f && fabsf(anchor_y + 1) < 0.04f);
      float32_t angle = 2 * acosf(fminf(1, fabsf(q[3])));
      assert(angle < (type == VKR_PHYSICS_JOINT_FIXED ? 0.02f : 0.29f));
    }
    assert(vkr_physics_joint_set_enabled(world, joint, false_v));
    assert(vkr_physics_joint_set_enabled(world, joint, true_v));
    assert(vkr_physics_body_reserve_destroy(world, body));
    assert(vkr_physics_body_destroy(world, body));
    assert(!vkr_physics_joint_set_enabled(world, joint, true_v));
    assert(!vkr_physics_joint_destroy(world, joint));
    vkr_physics_world_destroy(world);
  }
}

bool32_t run_physics_tests(void) {
  printf("Running physics tests...\n");
  test_gravity_impulse_and_handles();
  test_compound_queries_and_capacity();
  test_stack_sleep_and_support_removal();
  test_sensor_sleep_and_disable();
  test_shapes_ccd_and_layers();
  test_sensor_queue_fault();
  test_sensor_mutation_reservation();
  test_geometry_scale_and_sweep();
  test_contact_identity_and_reservation();
  test_joint_limits_and_lifetime();
  printf("Physics tests PASSED\n");
  return true_v;
}

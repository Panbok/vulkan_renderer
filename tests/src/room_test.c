#include "room_test.h"

#include "math/mat.h"
#include "math/vkr_quat.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_rooms.h"
#include "renderer/systems/vkr_scene_types.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Room visibility (large-level rendering, section 4). The oracle casts rays
 * from the camera through the view: a ray leaves its room only through an
 * open portal of that room, and the outside only through a portal or into a
 * room's wall. Every room a ray reaches must be visible; a room behind a
 * closed portal or with no portal must not be. */

#define ROOM_TEST_ROOMS 6u
#define ROOM_TEST_PORTALS 5u
#define ROOM_TEST_RAYS 96u
/* Walls are 0.4 m thick; a portal sits in the middle of one. */
#define ROOM_TEST_WALL_GAP 0.6f

typedef struct RoomTestBox {
  Vec3 center;
  Vec3 size;
} RoomTestBox;

typedef struct RoomTestPortal {
  Vec3 center;
  /* Width and height in the portal's plane. */
  float32_t width;
  float32_t height;
  /* Its normal runs along X instead of Z. */
  bool8_t across_x;
  bool8_t open;
  /* Room indices on its two sides; ROOM_TEST_ROOMS is the outside. */
  uint32_t sides[2];
} RoomTestPortal;

/* A (camera), B through the door ahead, C through a door in B, D through a
   closed door, E without a portal, F reached from the outside, which A's
   west window shows. */
static const RoomTestBox s_rooms[ROOM_TEST_ROOMS] = {
    {{0.0f, 2.0f, 0.0f}, {10.0f, 4.0f, 10.0f}},
    {{0.0f, 2.0f, 10.4f}, {10.0f, 4.0f, 10.0f}},
    {{0.0f, 2.0f, 20.8f}, {10.0f, 4.0f, 10.0f}},
    {{10.4f, 2.0f, 0.0f}, {10.0f, 4.0f, 10.0f}},
    {{-30.0f, 2.0f, 0.0f}, {10.0f, 4.0f, 10.0f}},
    {{-13.0f, 2.0f, 14.7f}, {10.0f, 4.0f, 10.0f}},
};

static const RoomTestPortal s_portals[ROOM_TEST_PORTALS] = {
    {{0.0f, 1.5f, 5.2f}, 2.0f, 3.0f, false_v, true_v, {0u, 1u}},
    {{3.0f, 1.5f, 15.6f}, 2.0f, 3.0f, false_v, true_v, {1u, 2u}},
    {{5.2f, 1.5f, 0.0f}, 2.0f, 3.0f, true_v, false_v, {0u, 3u}},
    {{-5.2f, 2.0f, 2.0f}, 3.0f, 2.0f, true_v, true_v, {0u, ROOM_TEST_ROOMS}},
    {{-13.0f, 2.0f, 9.5f}, 2.0f, 3.0f, false_v, true_v, {ROOM_TEST_ROOMS, 5u}},
};

typedef struct RoomTest {
  VkrDMemory memory;
  VkrAllocator allocator;
} RoomTest;

/* Where a ray from `origin` along `dir` leaves a box it is inside. */
static float32_t room_test_exit(const RoomTestBox *box, Vec3 origin, Vec3 dir) {
  float32_t exit = INFINITY;
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    const float32_t d = dir.elements[axis];
    if (fabsf(d) < 1e-6f) {
      continue;
    }
    const float32_t face = box->center.elements[axis] +
                           (d > 0.0f ? 0.5f : -0.5f) * box->size.elements[axis];
    exit = fminf(exit, (face - origin.elements[axis]) / d);
  }
  return exit;
}

/* Where a ray enters a box after `after`, or infinity. */
static float32_t room_test_entry(const RoomTestBox *box, Vec3 origin, Vec3 dir,
                                 float32_t after) {
  float32_t near = 0.0f;
  float32_t far = INFINITY;
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    const float32_t lo =
        box->center.elements[axis] - 0.5f * box->size.elements[axis];
    const float32_t hi =
        box->center.elements[axis] + 0.5f * box->size.elements[axis];
    const float32_t d = dir.elements[axis];
    if (fabsf(d) < 1e-6f) {
      if (origin.elements[axis] < lo || origin.elements[axis] > hi) {
        return INFINITY;
      }
      continue;
    }
    float32_t t0 = (lo - origin.elements[axis]) / d;
    float32_t t1 = (hi - origin.elements[axis]) / d;
    near = fmaxf(near, fminf(t0, t1));
    far = fminf(far, fmaxf(t0, t1));
  }
  return near <= far && near > after ? near : INFINITY;
}

/* Where a ray crosses a portal's opening, or infinity. */
static float32_t room_test_crossing(const RoomTestPortal *portal, Vec3 origin,
                                    Vec3 dir) {
  const uint32_t normal = portal->across_x ? 0u : 2u;
  const uint32_t along = portal->across_x ? 2u : 0u;
  if (fabsf(dir.elements[normal]) < 1e-6f) {
    return INFINITY;
  }
  const float32_t t =
      (portal->center.elements[normal] - origin.elements[normal]) /
      dir.elements[normal];
  if (!(t > 1e-4f)) {
    return INFINITY;
  }
  const Vec3 hit = vec3_add(origin, vec3_scale(dir, t));
  return fabsf(hit.elements[along] - portal->center.elements[along]) <=
                     0.5f * portal->width &&
                 fabsf(hit.y - portal->center.y) <= 0.5f * portal->height
             ? t
             : INFINITY;
}

/* Marks in `reached` every room (and, at ROOM_TEST_ROOMS, the outside) the
   ray reaches from room 0. */
static void room_test_trace(Vec3 origin, Vec3 dir,
                            bool8_t reached[ROOM_TEST_ROOMS + 1u]) {
  uint32_t node = 0u;
  float32_t t = 0.0f;
  reached[node] = true_v;
  for (uint32_t step = 0; step < 16u; ++step) {
    /* How far the ray may go before a wall stops it. */
    float32_t limit = INFINITY;
    if (node < ROOM_TEST_ROOMS) {
      limit = room_test_exit(&s_rooms[node], origin, dir) + ROOM_TEST_WALL_GAP;
    } else {
      for (uint32_t r = 0; r < ROOM_TEST_ROOMS; ++r) {
        limit = fminf(limit, room_test_entry(&s_rooms[r], origin, dir, t) +
                                 ROOM_TEST_WALL_GAP);
      }
    }
    uint32_t next = UINT32_MAX;
    float32_t best = limit;
    for (uint32_t p = 0; p < ROOM_TEST_PORTALS; ++p) {
      const RoomTestPortal *portal = &s_portals[p];
      const uint32_t side = portal->sides[0] == node   ? 1u
                            : portal->sides[1] == node ? 0u
                                                       : 2u;
      if (side == 2u || !portal->open) {
        continue;
      }
      const float32_t crossing = room_test_crossing(portal, origin, dir);
      if (crossing > t + 1e-3f && crossing < best) {
        best = crossing;
        next = portal->sides[side];
      }
    }
    if (next == UINT32_MAX) {
      return;
    }
    node = next;
    t = best;
    reached[node] = true_v;
  }
}

static VkrEntityId room_test_box(VkrScene *scene, Vec3 center, Vec3 size,
                                 VkrQuat rotation) {
  const VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(entity.u64);
  assert(vkr_scene_set_transform(scene, entity, center, rotation, size));
  return entity;
}

static void test_room_visibility_against_rays(void) {
  RoomTest test;
  assert(vkr_dmemory_create(MB(8), MB(64), &test.memory));
  test.allocator = (VkrAllocator){.ctx = &test.memory};
  vkr_dmemory_allocator_create(&test.allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &test.allocator, 0, 8, NULL));

  for (uint32_t r = 0; r < ROOM_TEST_ROOMS; ++r) {
    const VkrEntityId entity = room_test_box(
        &scene, s_rooms[r].center, s_rooms[r].size, vkr_quat_identity());
    const SceneRoom room = {0};
    assert(vkr_scene_set_typed(&scene, entity, &vkr_scene_room_type, &room));
  }
  const VkrQuat across_x =
      vkr_quat_from_axis_angle(vec3_up(), 0.5f * 3.14159265f);
  for (uint32_t p = 0; p < ROOM_TEST_PORTALS; ++p) {
    const RoomTestPortal *portal = &s_portals[p];
    const VkrEntityId entity = room_test_box(
        &scene, portal->center, vec3_new(portal->width, portal->height, 1.0f),
        portal->across_x ? across_x : vkr_quat_identity());
    const ScenePortal value = {.open = portal->open};
    assert(vkr_scene_set_typed(&scene, entity, &vkr_scene_portal_type, &value));
  }
  vkr_scene_update_transforms(&scene);

  /* Two views from room A: ahead through the doors, and west at the
     window. */
  const Vec3 eye = vec3_new(0.0f, 1.7f, -3.0f);
  const Vec3 targets[] = {vec3_new(0.0f, 1.7f, 10.0f),
                          vec3_new(-5.2f, 2.0f, 2.0f)};
  bool8_t ever[ROOM_TEST_ROOMS + 1u] = {0};
  for (uint32_t view = 0; view < ArrayCount(targets); ++view) {
    const Mat4 projection =
        mat4_perspective_reversed(1.7f, 16.0f / 9.0f, 0.1f, 1000.0f);
    const Mat4 view_projection =
        mat4_mul(projection, mat4_look_at(eye, targets[view], vec3_up()));
    const VkrSceneRoomFrame *frame =
        vkr_scene_rooms_frame(&scene, eye, view_projection);
    assert(frame && frame->room_count == ROOM_TEST_ROOMS);

    bool8_t reached[ROOM_TEST_ROOMS + 1u] = {0};
    const Mat4 inverse = mat4_inverse(view_projection);
    for (uint32_t y = 0; y < ROOM_TEST_RAYS; ++y) {
      for (uint32_t x = 0; x < ROOM_TEST_RAYS; ++x) {
        const float32_t nx =
            -1.0f + 2.0f * ((float32_t)x + 0.5f) / (float32_t)ROOM_TEST_RAYS;
        const float32_t ny =
            -1.0f + 2.0f * ((float32_t)y + 0.5f) / (float32_t)ROOM_TEST_RAYS;
        const Vec4 clip = mat4_mul_vec4(inverse, vec4_new(nx, ny, 0.5f, 1.0f));
        const Vec3 point =
            vec3_new(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
        room_test_trace(eye, vec3_normalize(vec3_sub(point, eye)), reached);
      }
    }
    for (uint32_t r = 0; r < ROOM_TEST_ROOMS; ++r) {
      const bool8_t visible = (frame->visible[r / 64u] >> (r % 64u)) & 1u;
      /* Every room a ray reaches is visible. */
      assert(!reached[r] || visible);
      ever[r] = ever[r] || reached[r];
    }
    assert(!reached[ROOM_TEST_ROOMS] || frame->outside_visible);
    ever[ROOM_TEST_ROOMS] = ever[ROOM_TEST_ROOMS] || reached[ROOM_TEST_ROOMS];
    /* The closed door and the room without a portal hide theirs. */
    assert(!((frame->visible[0] >> 3u) & 1u));
    assert(!((frame->visible[0] >> 4u) & 1u));
    /* A light in room D lights only D while its range stays inside it; one
       crossing the wall into A, the camera's room, shows, and so does one
       in A. Unshadowed light leaks through walls, so reaching any visible
       room or the outside keeps it. */
    assert(
        vkr_scene_rooms_light_hidden(frame, vec3_new(12.0f, 2.0f, 0.0f), 1.5f));
    assert(
        !vkr_scene_rooms_light_hidden(frame, vec3_new(6.0f, 2.0f, 0.0f), 3.0f));
    assert(
        !vkr_scene_rooms_light_hidden(frame, vec3_new(0.0f, 2.0f, 0.0f), 1.0f));
    /* A light without a range reaches everywhere. */
    assert(!vkr_scene_rooms_light_hidden(frame, vec3_new(12.0f, 2.0f, 0.0f),
                                         0.0f));
  }
  /* The views do reach through both doors, the window and the outside
     portal, so the assertions above are not vacuous. */
  assert(ever[1] && ever[2] && ever[ROOM_TEST_ROOMS] && ever[5]);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&test.allocator);
}

/* Whether room `index` is visible from room A looking east at the doorway
   to room D. */
static bool8_t room_test_east_visible(VkrScene *scene, uint32_t index) {
  vkr_scene_update_transforms(scene);
  const Vec3 eye = vec3_new(0.0f, 1.7f, 0.0f);
  const Mat4 view_projection =
      mat4_mul(mat4_perspective_reversed(1.7f, 16.0f / 9.0f, 0.1f, 1000.0f),
               mat4_look_at(eye, vec3_new(10.0f, 1.7f, 0.0f), vec3_up()));
  const VkrSceneRoomFrame *frame =
      vkr_scene_rooms_frame(scene, eye, view_projection);
  assert(frame && frame->room_count == 2u);
  return (frame->visible[0] >> index) & 1u;
}

/* A portal whose door is a mover closes while the door rests at its saved
   pose and opens when play moves it; a door that is no mover leaves the
   portal open. */
static void test_room_portal_door(void) {
  RoomTest test;
  assert(vkr_dmemory_create(MB(8), MB(64), &test.memory));
  test.allocator = (VkrAllocator){.ctx = &test.memory};
  vkr_dmemory_allocator_create(&test.allocator);
  VkrScene scene;
  assert(vkr_scene_init(&scene, &test.allocator, 0, 8, NULL));

  const SceneRoom room = {0};
  for (uint32_t r = 0; r < 2u; ++r) {
    const uint32_t source = r == 0u ? 0u : 3u;
    const VkrEntityId entity =
        room_test_box(&scene, s_rooms[source].center, s_rooms[source].size,
                      vkr_quat_identity());
    assert(vkr_scene_set_typed(&scene, entity, &vkr_scene_room_type, &room));
  }
  const VkrQuat across_x =
      vkr_quat_from_axis_angle(vec3_up(), 0.5f * 3.14159265f);
  const RoomTestPortal *doorway = &s_portals[2];
  const VkrEntityId door = room_test_box(
      &scene, doorway->center, vec3_new(0.2f, 3.0f, 2.0f), vkr_quat_identity());
  VkrEntityRef door_ref;
  vkr_scene_entity_ref_generate(&door_ref);
  assert(vkr_scene_set_entity_ref(&scene, door, &door_ref));
  const VkrEntityId portal =
      room_test_box(&scene, doorway->center,
                    vec3_new(doorway->width, doorway->height, 1.0f), across_x);
  const ScenePortal value = {.open = true_v, .door = door_ref};
  assert(vkr_scene_set_typed(&scene, portal, &vkr_scene_portal_type, &value));

  /* Not a mover yet: the portal stays open, and the check reports it. */
  assert(room_test_east_visible(&scene, 1u));
  VkrSceneRoomIssue issues[4];
  assert(vkr_scene_rooms_check(&scene, NULL, NULL, issues, 4u) == 1u &&
         issues[0].kind == VKR_SCENE_ROOM_ISSUE_DOOR &&
         issues[0].entity.u64 == portal.u64);
  const SceneMover mover = {.direction = vec3_up(), .speed = 1.0f};
  assert(vkr_scene_set_typed(&scene, door, &vkr_scene_mover_type, &mover));
  assert(vkr_scene_rooms_check(&scene, NULL, NULL, issues, 4u) == 0u);
  /* At its saved pose, as outside play, it shuts the portal. */
  assert(!room_test_east_visible(&scene, 1u));
  /* Play poses it 2.5 m up: open. */
  const Mat4 raised = mat4_mul(mat4_translate(vec3_new(0.0f, 2.5f, 0.0f)),
                               vkr_scene_get_transform(&scene, door)->world);
  assert(vkr_scene_set_evaluated_transform(&scene, door, &raised));
  assert(room_test_east_visible(&scene, 1u));
  /* Back at rest under an evaluated pose: shut again. */
  const Mat4 rest = vkr_scene_get_transform(&scene, door)->local;
  assert(vkr_scene_set_evaluated_transform(&scene, door, &rest));
  assert(!room_test_east_visible(&scene, 1u));
  /* A closed portal stays closed whatever the door does. */
  assert(vkr_scene_set_evaluated_transform(&scene, door, &raised));
  const ScenePortal closed = {.open = false_v, .door = door_ref};
  assert(vkr_scene_set_typed(&scene, portal, &vkr_scene_portal_type, &closed));
  assert(!room_test_east_visible(&scene, 1u));

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_allocator_destroy(&test.allocator);
}

bool32_t run_room_tests(void) {
  printf("--- Running room tests... ---\n");
  test_room_visibility_against_rays();
  test_room_portal_door();
  printf("--- Room tests completed. ---\n");
  return true;
}

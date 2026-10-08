#include "editor_label_sight_test.h"

#include "../../editor/src/editor_internal.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_scene_physics.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* Occluded, visible and far icons of the convergence test. */
#define SIGHT_TEST_HIDDEN 100u
#define SIGHT_TEST_VISIBLE 20u
#define SIGHT_TEST_FAR 4u
#define SIGHT_TEST_ICONS                                                       \
  (SIGHT_TEST_HIDDEN + SIGHT_TEST_VISIBLE + SIGHT_TEST_FAR + 1u)
#define SIGHT_TEST_DT (1.0f / 60.0f)

/* Kept off the stack: the cache holds one entry per possible icon. */
static VkrEditorLabelSights s_sights;

static VkrEntityId sight_test_entity(VkrScene *scene, Vec3 position) {
  const VkrEntityId entity = vkr_scene_create_entity(scene, NULL);
  assert(entity.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(scene, entity, position, vkr_quat_identity(),
                                 vec3_one()));
  return entity;
}

static VkrPhysicsColliderDesc sight_test_box(VkrEntityId entity, Vec3 center,
                                             Vec3 half_extent) {
  return (VkrPhysicsColliderDesc){
      .entity_id = entity.u64,
      .shape = VKR_PHYSICS_BOX,
      .position = {center.x, center.y, center.z},
      .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
      .scale = {1.0f, 1.0f, 1.0f},
      .half_extent = {half_extent.x, half_extent.y, half_extent.z},
      .enabled = true_v,
  };
}

/* A camera at `eye` looking down -Z with a 90 degree lens. */
static Mat4 sight_test_camera(Vec3 eye) {
  const Mat4 view =
      mat4_look_at(eye, vec3_add(eye, vec3_new(0.0f, 0.0f, -1.0f)),
                   vec3_new(0.0f, 1.0f, 0.0f));
  return mat4_mul(mat4_perspective(1.5707963f, 1.0f, 0.1f, 1000.0f), view);
}

static bool8_t sight_test_occluded(const VkrSampleUiFrame *frame, Vec3 point,
                                   const VkrScene *owner, VkrEntityId entity) {
  return vkr_editor_label_occluded(frame, frame->view_projection,
                                   mat4_inverse(frame->view_projection), point,
                                   owner, entity);
}

/* The wall (a box from z -5.25 to -4.75 over x and y in [-2, 2]) stands
   between the camera at the origin and points behind it. Oracles: points
   behind it hide; a point in front, one just inside its face, one inside
   the icon's own body, one inside its parent's body and one behind the
   camera do not; with physics off nothing hides. */
static void sight_test_occlusion(VkrScene *scene, VkrSampleUiFrame *frame,
                                 VkrEntityId wall) {
  printf("  Running sight_test_occlusion...\n");
  const char *error = NULL;
  assert(vkr_editor_label_occlusion_available(frame));
  assert(sight_test_occluded(frame, vec3_new(0.0f, 0.0f, -10.0f), NULL,
                             VKR_ENTITY_ID_INVALID));
  assert(sight_test_occluded(frame, vec3_new(1.5f, -1.5f, -9.0f), NULL,
                             VKR_ENTITY_ID_INVALID));
  assert(!sight_test_occluded(frame, vec3_new(0.0f, 0.0f, -3.0f), NULL,
                              VKR_ENTITY_ID_INVALID));
  assert(!sight_test_occluded(frame, vec3_new(5.0f, 0.0f, -10.0f), NULL,
                              VKR_ENTITY_ID_INVALID));
  /* Two centimetres inside the face: within the surface slack. */
  assert(!sight_test_occluded(frame, vec3_new(0.0f, 0.0f, -4.77f), NULL,
                              VKR_ENTITY_ID_INVALID));
  /* Behind the camera nothing is tested. */
  assert(!sight_test_occluded(frame, vec3_new(0.0f, 0.0f, 10.0f), NULL,
                              VKR_ENTITY_ID_INVALID));

  /* The wall's own icon, at its center, is not hidden by the wall. */
  const Vec3 wall_center = vec3_new(0.0f, 0.0f, -5.0f);
  assert(sight_test_occluded(frame, wall_center, NULL, VKR_ENTITY_ID_INVALID));
  assert(!sight_test_occluded(frame, wall_center, scene, wall));

  /* A button parented to a mover sits inside the mover's kinematic body:
     the mover hides neither its own icon nor its button's. */
  const Vec3 mover_center = vec3_new(3.0f, 0.0f, -6.0f);
  const VkrEntityId mover = sight_test_entity(scene, mover_center);
  const VkrPhysicsColliderDesc mover_box =
      sight_test_box(mover, mover_center, vec3_new(0.5f, 0.5f, 0.5f));
  assert(vkr_scene_physics_generated_set_kinematic(scene, 2u, mover, &mover_box,
                                                   1u, false_v, &error));
  const VkrEntityId button = sight_test_entity(scene, vec3_zero());
  vkr_scene_set_parent(scene, button, mover);
  assert(vkr_scene_get_transform(scene, button)->parent.u64 == mover.u64);
  assert(sight_test_occluded(frame, mover_center, NULL, VKR_ENTITY_ID_INVALID));
  assert(!sight_test_occluded(frame, mover_center, scene, mover));
  assert(!sight_test_occluded(frame, mover_center, scene, button));
  /* An unrelated icon at the same place stays hidden. */
  const VkrEntityId other = sight_test_entity(scene, mover_center);
  assert(sight_test_occluded(frame, mover_center, scene, other));
  vkr_scene_physics_generated_remove(scene, 2u);

  /* Physics off: no collision hides an icon. */
  assert(vkr_scene_physics_set_disabled(scene, true_v, &error));
  assert(!vkr_editor_label_occlusion_available(frame));
  assert(!sight_test_occluded(frame, vec3_new(0.0f, 0.0f, -10.0f), NULL,
                              VKR_ENTITY_ID_INVALID));
  assert(vkr_scene_physics_set_disabled(scene, false_v, &error));
  assert(vkr_editor_label_occlusion_available(frame));
  assert(sight_test_occluded(frame, vec3_new(0.0f, 0.0f, -10.0f), NULL,
                             VKR_ENTITY_ID_INVALID));
  printf("  sight_test_occlusion PASSED\n");
}

typedef struct SightTestIcon {
  VkrEntityId entity;
  Vec3 position;
  float32_t alpha;
} SightTestIcon;

/* One build: every icon in order except `skip` (UINT32_MAX for none).
   Returns the rays it cast. */
static uint32_t sight_test_frame(VkrSampleUiFrame *frame, const VkrScene *scene,
                                 SightTestIcon *icons, uint32_t skip,
                                 VkrAllocator *scratch) {
  vkr_editor_label_sights_begin(&s_sights, frame, true_v, 80.0f, SIGHT_TEST_DT,
                                false_v, scratch);
  const uint32_t rays = s_sights.rays_left;
  for (uint32_t i = 0; i < SIGHT_TEST_ICONS; ++i) {
    if (i != skip) {
      icons[i].alpha = vkr_editor_label_sight(
          &s_sights, frame, scene, icons[i].entity, true_v, icons[i].position);
    }
  }
  vkr_editor_label_sights_end(&s_sights);
  return rays - s_sights.rays_left;
}

/* 100 icons behind the wall, 20 in front, 4 past the 80 m icon distance
   and one at 72 m. Oracles: the first build casts exactly the ray budget;
   within 12 builds (3 of rays, then 0.12 s of fade at 60 Hz) hidden icons
   reach 0, visible ones 1, far ones 0 and the 72 m one half; a quiet
   camera casts no rays until the one-second refresh; a moved camera
   sweeps again; an icon that leaves the order keeps the others' state;
   the selection shows through the wall. */
static void sight_test_cache(VkrScene *scene, VkrSampleUiFrame *frame,
                             VkrAllocator *scratch, Arena *arena) {
  printf("  Running sight_test_cache...\n");
  static SightTestIcon icons[SIGHT_TEST_ICONS];
  uint32_t count = 0u;
  for (uint32_t i = 0; i < SIGHT_TEST_HIDDEN; ++i) {
    const Vec3 position = vec3_new(-1.5f + 0.3f * (float32_t)(i % 10u),
                                   -1.5f + 0.3f * (float32_t)(i / 10u), -10.0f);
    icons[count++] =
        (SightTestIcon){sight_test_entity(scene, position), position, 0.0f};
  }
  for (uint32_t i = 0; i < SIGHT_TEST_VISIBLE; ++i) {
    const Vec3 position = vec3_new(-1.0f + 0.1f * (float32_t)i, 0.5f, -3.0f);
    icons[count++] =
        (SightTestIcon){sight_test_entity(scene, position), position, 0.0f};
  }
  for (uint32_t i = 0; i < SIGHT_TEST_FAR; ++i) {
    const Vec3 position = vec3_new(50.0f + (float32_t)i, 0.0f, -100.0f);
    icons[count++] =
        (SightTestIcon){sight_test_entity(scene, position), position, 0.0f};
  }
  const Vec3 fading = vec3_new(40.0f, 0.0f, -72.0f);
  icons[count++] =
      (SightTestIcon){sight_test_entity(scene, fading), fading, 0.0f};
  assert(count == SIGHT_TEST_ICONS);
  MemZero(&s_sights, sizeof(s_sights));

  /* The far icons spend no rays, so 121 icons need three builds. */
  assert(sight_test_frame(frame, scene, icons, UINT32_MAX, scratch) ==
         VKR_EDITOR_LABEL_RAYS_PER_FRAME);
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  for (uint32_t f = 1; f < 12u; ++f) {
    (void)sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
    arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  for (uint32_t i = 0; i < SIGHT_TEST_HIDDEN; ++i) {
    assert(icons[i].alpha < VKR_EDITOR_LABEL_ALPHA_MIN);
  }
  for (uint32_t i = SIGHT_TEST_HIDDEN;
       i < SIGHT_TEST_HIDDEN + SIGHT_TEST_VISIBLE; ++i) {
    assert(icons[i].alpha == 1.0f);
  }
  for (uint32_t i = SIGHT_TEST_HIDDEN + SIGHT_TEST_VISIBLE;
       i < SIGHT_TEST_ICONS - 1u; ++i) {
    assert(icons[i].alpha < VKR_EDITOR_LABEL_ALPHA_MIN);
  }
  assert(fabsf(icons[SIGHT_TEST_ICONS - 1u].alpha - 0.5f) < 0.01f);
  assert(s_sights.occluded_count == SIGHT_TEST_HIDDEN);

  /* Nothing changed after the sweep: no rays until the refresh. */
  uint32_t rays = 0u;
  for (uint32_t f = 0; f < 30u; ++f) {
    rays += sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
    arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  assert(rays == 0u);
  for (uint32_t f = 0; f < 40u; ++f) {
    rays += sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
    arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  assert(rays > 0u);

  /* A moved camera sweeps again within the budget. */
  frame->view_projection = sight_test_camera(vec3_new(0.0f, 0.0f, 0.5f));
  rays = sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(rays > 0u && rays <= VKR_EDITOR_LABEL_RAYS_PER_FRAME);

  /* An icon leaves the order: the rest keep their opacity. */
  (void)sight_test_frame(frame, scene, icons, 50u, scratch);
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(icons[SIGHT_TEST_HIDDEN].alpha == 1.0f);
  assert(icons[SIGHT_TEST_HIDDEN + SIGHT_TEST_VISIBLE - 1u].alpha == 1.0f);

  /* The selection shows through the wall and fades in. */
  frame->selected_entity = icons[0].entity;
  (void)sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(icons[0].alpha > 0.0f && icons[0].alpha < 1.0f);
  for (uint32_t f = 0; f < 10u; ++f) {
    (void)sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
    arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  assert(icons[0].alpha == 1.0f);
  assert(icons[1].alpha < VKR_EDITOR_LABEL_ALPHA_MIN);
  frame->selected_entity = VKR_ENTITY_ID_INVALID;

  /* An orthographic view fades nothing with distance. */
  const Mat4 view = mat4_look_at(vec3_zero(), vec3_new(0.0f, 0.0f, -1.0f),
                                 vec3_new(0.0f, 1.0f, 0.0f));
  frame->view_projection = mat4_mul(
      mat4_ortho_zo_yinv(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 1000.0f),
      view);
  for (uint32_t f = 0; f < 20u; ++f) {
    (void)sight_test_frame(frame, scene, icons, UINT32_MAX, scratch);
    arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  assert(icons[SIGHT_TEST_HIDDEN + SIGHT_TEST_VISIBLE].alpha == 1.0f);
  assert(icons[0].alpha < VKR_EDITOR_LABEL_ALPHA_MIN);
  printf("  sight_test_cache PASSED\n");
}

bool32_t run_editor_label_sight_tests(void) {
  printf("--- Starting Editor Label Sight Tests ---\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(32), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));

  VkrScene scene;
  assert(vkr_scene_init(&scene, &allocator, 5, 16, NULL));
  const char *error = NULL;
  const Vec3 wall_center = vec3_new(0.0f, 0.0f, -5.0f);
  const VkrEntityId wall = sight_test_entity(&scene, wall_center);
  const VkrPhysicsColliderDesc wall_box =
      sight_test_box(wall, wall_center, vec3_new(2.0f, 2.0f, 0.25f));
  assert(vkr_scene_physics_generated_set(&scene, 1u, wall, &wall_box, 1u,
                                         false_v, &error));
  VkrSampleUiFrame frame = {
      .scene = &scene,
      .view_projection = sight_test_camera(vec3_zero()),
      .selected_entity = VKR_ENTITY_ID_INVALID,
  };
  sight_test_occlusion(&scene, &frame, wall);
  sight_test_cache(&scene, &frame, &scratch, arena);

  vkr_scene_shutdown(&scene, NULL);
  arena_destroy(arena);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("--- Editor Label Sight Tests Passed ---\n");
  return true_v;
}

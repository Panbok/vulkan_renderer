#include "lighting_system_tests.h"

#include "math/vkr_math.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_dynamic_volume.h"
#include "renderer/systems/vkr_lighting_system.h"
#include "renderer/systems/vkr_scene_types.h"

vkr_internal bool8_t lighting_test_vec3_near(Vec3 left, Vec3 right) {
  return fabsf(left.x - right.x) < 0.00001f &&
         fabsf(left.y - right.y) < 0.00001f &&
         fabsf(left.z - right.z) < 0.00001f;
}

static VkrPointLight make_gltf_point(uint32_t render_id, Vec3 position,
                                     float32_t range) {
  return (VkrPointLight){
      .position = position,
      .color = vec3_one(),
      .intensity = 10.0f,
      .range = range,
      .direction = vec3_new(0.0f, 0.0f, -1.0f),
      .inner_cone_angle = 0.0f,
      .outer_cone_angle = 0.785398163f,
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
      .render_id = render_id,
  };
}

/* The tiled pipeline keeps the lights nearest the camera by the distance to
 * their range, in table order, and shadows the nearest casters among them
 * (ADR-087). The first call snaps every kept light to full intensity. */
static bool32_t test_point_light_limit_keeps_nearest(void) {
  printf("  Running test_point_light_limit_keeps_nearest...\n");
  VkrLightingSystem system = {0};
  /* Range 2 at x = 10 * i: light i's range starts 10 * i - 2 from the
     origin, so the table order is the distance order reversed. */
  const uint32_t count = 6u;
  assert(vkr_lighting_system_reserve_point_lights(&system, count));
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t slot = count - 1u - i;
    system.point_lights[slot] =
        make_gltf_point(slot + 1u, vec3_new(10.0f * (float32_t)i, 0, 0), 2.0f);
    system.point_lights[slot].casts_shadow = i != 1u;
  }
  system.point_light_count = count;

  vkr_lighting_system_limit_point_lights(&system, vec3_zero(), 1.0f / 60.0f, 4u,
                                         2u);
  /* The four nearest, x = 0..30, remain in their table order (ids 3..6);
     of the casters among them, x = 0 and x = 20, keep shadows. */
  assert(system.point_light_count == 4u);
  assert(system.point_light_dropped_count == 2u);
  const uint32_t ids[] = {3u, 4u, 5u, 6u};
  const bool8_t shadowed[] = {false_v, true_v, false_v, true_v};
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(system.point_lights[i].render_id == ids[i]);
    assert(system.point_lights[i].casts_shadow == shadowed[i]);
    assert(system.point_lights[i].intensity == 10.0f);
  }
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_limit_keeps_nearest PASSED\n");
  return true_v;
}

/* One frame of two shadow-casting lights 20 m apart on the x axis whose
   ranges both hold the camera, of which the limit keeps one: the table is
   rebuilt as the scene sync rebuilds it every frame. */
static void limit_test_frame(VkrLightingSystem *system, float32_t camera_x) {
  assert(vkr_lighting_system_reserve_point_lights(system, 2u));
  system->point_light_count = 2u;
  system->point_lights[0] =
      make_gltf_point(1u, vec3_new(10.0f, 0.0f, 0.0f), 100.0f);
  system->point_lights[1] =
      make_gltf_point(2u, vec3_new(-10.0f, 0.0f, 0.0f), 100.0f);
  system->point_lights[0].casts_shadow = true_v;
  system->point_lights[1].casts_shadow = true_v;
  vkr_lighting_system_limit_point_lights(system, vec3_new(camera_x, 0.0f, 0.0f),
                                         1.0f / 60.0f, 1u, 1u);
}

/* Lights whose ranges all hold the camera tie on the distance to their
   range; the distance to their centre decides instead of table order. The
   kept light stays while a rival is less than the incumbent bonus nearer,
   and once replaced fades out over VKR_POINT_LIGHT_LIMIT_FADE_SECONDS, without
   its shadow, before the rival fades in; a camera cut snaps (ADR-087). */
static bool32_t test_point_light_limit_holds_and_fades(void) {
  printf("  Running test_point_light_limit_holds_and_fades...\n");
  VkrLightingSystem system = {0};
  const float32_t step = 10.0f * (1.0f / 60.0f) / 0.25f;

  /* Light 2 is nearer, though light 1 comes first in the table. */
  limit_test_frame(&system, -1.0f);
  assert(system.point_light_count == 1u);
  assert(system.point_lights[0].render_id == 2u);
  assert(system.point_lights[0].intensity == 10.0f);
  assert(system.point_lights[0].casts_shadow);

  /* Small moves past the midpoint keep light 2 at full intensity. */
  for (int32_t tenth = -9; tenth <= 5; ++tenth) {
    limit_test_frame(&system, 0.1f * (float32_t)tenth);
    assert(system.point_light_count == 1u);
    assert(system.point_lights[0].render_id == 2u);
    assert(system.point_lights[0].intensity == 10.0f);
    assert(system.point_lights[0].casts_shadow);
  }

  /* At x = 1.5, light 1 is 8.5 m away and light 2 11.5 m, more than the
     bonus: light 2 fades out in its slot, then light 1 fades in. */
  float32_t previous = 10.0f;
  uint32_t frames = 0u;
  for (;;) {
    assert(frames < 20u);
    limit_test_frame(&system, 1.5f);
    assert(system.point_light_count == 1u);
    if (system.point_lights[0].render_id == 1u) {
      break;
    }
    const float32_t intensity = system.point_lights[0].intensity;
    assert(fabsf(intensity - (previous - step)) < 1e-3f);
    assert(!system.point_lights[0].casts_shadow);
    previous = intensity;
    frames++;
  }
  assert(frames >= 14u);
  previous = 0.0f;
  while (previous < 10.0f) {
    const float32_t intensity = system.point_lights[0].intensity;
    assert(system.point_lights[0].render_id == 1u);
    assert(system.point_lights[0].casts_shadow);
    assert(fabsf(intensity - Min(previous + step, 10.0f)) < 1e-3f);
    previous = intensity;
    limit_test_frame(&system, 1.5f);
  }

  /* A cut to light 2's side shows light 2 at once. */
  limit_test_frame(&system, -30.0f);
  assert(system.point_light_count == 1u);
  assert(system.point_lights[0].render_id == 2u);
  assert(system.point_lights[0].intensity == 10.0f);
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_limit_holds_and_fades PASSED\n");
  return true_v;
}

/* One frame of the scene table's selection over `count` lights, offered as
   a scene sync offers them. */
static void table_test_frame(VkrLightingSystem *system,
                             const VkrPointLight *lights, uint32_t count,
                             Vec3 camera) {
  vkr_lighting_system_begin_point_lights(system, camera, 1.0f / 60.0f);
  for (uint32_t i = 0u; i < count; ++i) {
    vkr_lighting_system_offer_point_light(system, &lights[i]);
  }
  vkr_lighting_system_select_point_lights(system);
}

static const VkrPointLight *table_test_find(const VkrLightingSystem *system,
                                            uint32_t render_id) {
  for (uint32_t i = 0u; i < system->point_light_count; ++i) {
    if (system->point_lights[i].render_id == render_id) {
      return &system->point_lights[i];
    }
  }
  return NULL;
}

/* Past VKR_MAX_SCENE_POINT_LIGHTS the table keeps the lights nearest the
   camera, not the lowest render ids (ADR-019 rejects a first-N list),
   however the lights are offered, and its candidates stay bounded. The
   VKR_POINT_LIGHT_GRID_LIGHT_MAX nearest casters form the head, the only
   shadowed rows; a first frame places the head in the head rows and the
   rest after it, each in render id order. */
static bool32_t test_point_light_table_keeps_nearest_past_capacity(void) {
  printf("  Running test_point_light_table_keeps_nearest_past_capacity...\n");
  static VkrLightingSystem system;
  MemZero(&system, sizeof(system));
  /* Light k sits 10 * k + 5 m from the camera; the farthest has the lowest
     render id, so a first-N rule would keep the farthest. 7 is coprime to
     the count, so the offer order is a permutation. */
  const uint32_t count = VKR_MAX_SCENE_POINT_LIGHTS + 72u;
  static VkrPointLight lights[VKR_MAX_SCENE_POINT_LIGHTS + 72u];
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t k = (i * 7u) % count;
    lights[i] = make_gltf_point(
        count - k, vec3_new(10.0f * (float32_t)k + 5.0f, 0, 0), 2.0f);
    lights[i].casts_shadow = true_v;
  }
  table_test_frame(&system, lights, count, vec3_zero());

  assert(system.point_light_count == VKR_MAX_SCENE_POINT_LIGHTS);
  assert(system.point_light_dropped_count == 72u);
  /* With nothing showing yet, only the best stay candidates. */
  assert(system.point_light_candidates.count == VKR_MAX_SCENE_POINT_LIGHTS);
  /* The nearest `head`, render ids count down from count, are the head; the
     rest of the table, render ids 73 up, follow. All show at full
     intensity: the first frame snaps. */
  const uint32_t head = VKR_POINT_LIGHT_GRID_LIGHT_MAX;
  for (uint32_t i = 0u; i < VKR_MAX_SCENE_POINT_LIGHTS; ++i) {
    const uint32_t render_id =
        i < head ? count - head + 1u + i : 73u + i - head;
    assert(system.point_lights[i].render_id == render_id);
    assert(system.point_lights[i].intensity == 10.0f);
    assert(system.point_lights[i].casts_shadow == (i < head));
  }
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_table_keeps_nearest_past_capacity PASSED\n");
  return true_v;
}

#if VKR_MAX_SCENE_POINT_LIGHTS > VKR_POINT_LIGHT_GRID_LIGHT_MAX
/* One frame of `VKR_POINT_LIGHT_GRID_LIGHT_MAX - 1` shadow casters at the
   camera, which always rank first, and light 1 at x = 10 and light 2 at
   x = -10, whose ranges both hold the camera and which compete for the
   head's last place. The table has room for all of them. */
static void head_contest_frame(VkrLightingSystem *system, float32_t camera_x) {
  static VkrPointLight lights[VKR_POINT_LIGHT_GRID_LIGHT_MAX + 1u];
  const uint32_t near_count = VKR_POINT_LIGHT_GRID_LIGHT_MAX - 1u;
  const Vec3 camera = vec3_new(camera_x, 0.0f, 0.0f);
  for (uint32_t i = 0u; i < near_count; ++i) {
    lights[i] = make_gltf_point(
        1000u + i, vec3_new(camera_x, 0.001f * (float32_t)i, 0), 1000.0f);
    lights[i].casts_shadow = true_v;
  }
  lights[near_count] = make_gltf_point(1u, vec3_new(10.0f, 0.0f, 0.0f), 100.0f);
  lights[near_count + 1u] =
      make_gltf_point(2u, vec3_new(-10.0f, 0.0f, 0.0f), 100.0f);
  lights[near_count].casts_shadow = true_v;
  lights[near_count + 1u].casts_shadow = true_v;
  table_test_frame(system, lights, ArrayCount(lights), camera);
  assert(system->point_light_count == ArrayCount(lights));
}

/* A light keeps its head place, and its shadow, until a rival is more than
   the incumbent bonus nearer; then the two trade rows at once, both still
   drawn at full intensity, and only the head light casts a shadow. */
static bool32_t test_point_light_head_holds_and_trades(void) {
  printf("  Running test_point_light_head_holds_and_trades...\n");
  static VkrLightingSystem system;
  MemZero(&system, sizeof(system));
  const uint32_t head = VKR_POINT_LIGHT_GRID_LIGHT_MAX;

  head_contest_frame(&system, -1.0f);
  assert(table_test_find(&system, 2u) - system.point_lights < head);
  assert(table_test_find(&system, 2u)->casts_shadow);
  assert(table_test_find(&system, 1u) - system.point_lights == head);
  assert(!table_test_find(&system, 1u)->casts_shadow);

  /* Small moves past the midpoint keep light 2 in the head. */
  for (int32_t tenth = -9; tenth <= 5; ++tenth) {
    head_contest_frame(&system, 0.1f * (float32_t)tenth);
    assert(table_test_find(&system, 2u)->casts_shadow);
    assert(!table_test_find(&system, 1u)->casts_shadow);
  }

  /* At x = 1.5 light 1 is 8.5 m away and light 2 11.5 m: they trade. */
  head_contest_frame(&system, 1.5f);
  assert(table_test_find(&system, 1u) - system.point_lights < head);
  assert(table_test_find(&system, 1u)->casts_shadow);
  assert(table_test_find(&system, 1u)->intensity == 10.0f);
  assert(table_test_find(&system, 2u) - system.point_lights == head);
  assert(!table_test_find(&system, 2u)->casts_shadow);
  assert(table_test_find(&system, 2u)->intensity == 10.0f);
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_head_holds_and_trades PASSED\n");
  return true_v;
}
#endif

/* One frame of a full table: shadow casters at the camera always rank
   first, so light 1 at x = 10 and light 2 at x = -10, whose ranges both
   hold the camera, compete for the last slot and, past the head, cast no
   shadow. */
static void table_contest_frame(VkrLightingSystem *system, float32_t camera_x) {
  static VkrPointLight lights[VKR_MAX_SCENE_POINT_LIGHTS + 1u];
  const uint32_t near_count = VKR_MAX_SCENE_POINT_LIGHTS - 1u;
  const Vec3 camera = vec3_new(camera_x, 0.0f, 0.0f);
  for (uint32_t i = 0u; i < near_count; ++i) {
    /* Within a metre of the camera however large the table. */
    const float32_t offset = (float32_t)i / (float32_t)near_count;
    lights[i] =
        make_gltf_point(10000u + i, vec3_new(camera_x, offset, 0), 1000.0f);
    lights[i].casts_shadow = true_v;
  }
  lights[near_count] = make_gltf_point(1u, vec3_new(10.0f, 0.0f, 0.0f), 100.0f);
  lights[near_count + 1u] =
      make_gltf_point(2u, vec3_new(-10.0f, 0.0f, 0.0f), 100.0f);
  lights[near_count].casts_shadow = true_v;
  lights[near_count + 1u].casts_shadow = true_v;
  table_test_frame(system, lights, ArrayCount(lights), camera);
  assert(system->point_light_count <= VKR_MAX_SCENE_POINT_LIGHTS);
}

/* While lights compete for the table, the kept light stays until a rival is
   more than the incumbent bonus nearer, then fades out in its slot before
   the rival fades in; a camera cut snaps. Both cast a shadow only while the
   table fits in the head. */
static bool32_t test_point_light_table_holds_and_fades(void) {
  printf("  Running test_point_light_table_holds_and_fades...\n");
  static VkrLightingSystem system;
  MemZero(&system, sizeof(system));
  const float32_t step = 10.0f * (1.0f / 60.0f) / 0.25f;
  const bool8_t in_head =
      VKR_MAX_SCENE_POINT_LIGHTS <= VKR_POINT_LIGHT_GRID_LIGHT_MAX;

  table_contest_frame(&system, -1.0f);
  assert(system.point_light_count == VKR_MAX_SCENE_POINT_LIGHTS);
  assert(system.point_light_dropped_count == 1u);
  assert(!table_test_find(&system, 1u));
  assert(table_test_find(&system, 2u)->intensity == 10.0f);

  /* Small moves past the midpoint keep light 2 at full intensity. */
  for (int32_t tenth = -9; tenth <= 5; ++tenth) {
    table_contest_frame(&system, 0.1f * (float32_t)tenth);
    assert(!table_test_find(&system, 1u));
    assert(table_test_find(&system, 2u)->intensity == 10.0f);
  }

  /* At x = 1.5 light 1 is 8.5 m away and light 2 11.5 m: light 2 fades out
     in its slot, then light 1 fades in. */
  float32_t previous = 10.0f;
  uint32_t frames = 0u;
  for (;;) {
    assert(frames < 20u);
    table_contest_frame(&system, 1.5f);
    assert(system.point_light_count == VKR_MAX_SCENE_POINT_LIGHTS);
    const VkrPointLight *leaving = table_test_find(&system, 2u);
    if (!leaving) {
      break;
    }
    assert(!table_test_find(&system, 1u));
    assert(fabsf(leaving->intensity - (previous - step)) < 1e-3f);
    assert(leaving->casts_shadow == in_head);
    previous = leaving->intensity;
    frames++;
  }
  assert(frames >= 14u);
  previous = 0.0f;
  while (previous < 10.0f) {
    const VkrPointLight *entering = table_test_find(&system, 1u);
    assert(entering && entering->casts_shadow == in_head);
    assert(fabsf(entering->intensity - Min(previous + step, 10.0f)) < 1e-3f);
    previous = entering->intensity;
    table_contest_frame(&system, 1.5f);
  }

  /* A cut to light 2's side shows light 2 at once. */
  table_contest_frame(&system, -30.0f);
  assert(!table_test_find(&system, 1u));
  assert(table_test_find(&system, 2u)->intensity == 10.0f);
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_table_holds_and_fades PASSED\n");
  return true_v;
}

/* Without competition every light shows at full intensity: a light added
   after the first frame is new content and does not fade in. */
static bool32_t test_point_light_table_without_competition_snaps(void) {
  printf("  Running test_point_light_table_without_competition_snaps...\n");
  static VkrLightingSystem system;
  MemZero(&system, sizeof(system));
  VkrPointLight lights[4];
  for (uint32_t i = 0u; i < ArrayCount(lights); ++i) {
    lights[i] = make_gltf_point(4u - i, vec3_new((float32_t)i, 0, 0), 5.0f);
  }
  table_test_frame(&system, lights, 3u, vec3_zero());
  assert(system.point_light_count == 3u);
  table_test_frame(&system, lights, 4u, vec3_new(0.1f, 0.0f, 0.0f));
  assert(system.point_light_count == 4u);
  assert(system.point_light_dropped_count == 0u);
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(system.point_lights[i].intensity == 10.0f);
  }
  table_test_frame(&system, &lights[1], 3u, vec3_new(0.2f, 0.0f, 0.0f));
  assert(system.point_light_count == 3u && !table_test_find(&system, 4u));
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_table_without_competition_snaps PASSED\n");
  return true_v;
}

static void table_test_expect_rows(const VkrLightingSystem *system,
                                   const uint32_t *render_ids, uint32_t count) {
  assert(system->point_light_count == count);
  for (uint32_t row = 0u; row < count; ++row) {
    assert(system->point_lights[row].render_id == render_ids[row]);
  }
}

/* A light keeps its row while it stays in the table, so a backend's table
   changes only where lights do (ADR-019): a new light takes the next row
   whatever its render id, a removal moves only the last row into the hole,
   and a camera cut, which snaps fades, keeps every row. */
static bool32_t test_point_light_table_keeps_rows(void) {
  printf("  Running test_point_light_table_keeps_rows...\n");
  static VkrLightingSystem system;
  MemZero(&system, sizeof(system));
  VkrPointLight lights[5];
  for (uint32_t i = 0u; i < ArrayCount(lights); ++i) {
    lights[i] = make_gltf_point(5u - i, vec3_new((float32_t)i, 0, 0), 5.0f);
  }

  /* A first frame places lights in render id order. */
  table_test_frame(&system, lights, 3u, vec3_zero());
  const uint32_t first[] = {3u, 4u, 5u};
  table_test_expect_rows(&system, first, ArrayCount(first));

  table_test_frame(&system, lights, 5u, vec3_new(0.1f, 0.0f, 0.0f));
  const uint32_t joined[] = {3u, 4u, 5u, 1u, 2u};
  table_test_expect_rows(&system, joined, ArrayCount(joined));

  /* Light 4 leaves row 1; light 2 moves there from the last row. */
  const VkrPointLight without_four[] = {lights[0], lights[2], lights[3],
                                        lights[4]};
  table_test_frame(&system, without_four, ArrayCount(without_four),
                   vec3_new(0.2f, 0.0f, 0.0f));
  const uint32_t removed[] = {3u, 2u, 5u, 1u};
  table_test_expect_rows(&system, removed, ArrayCount(removed));

  table_test_frame(&system, without_four, ArrayCount(without_four),
                   vec3_new(-300.0f, 0.0f, 0.0f));
  table_test_expect_rows(&system, removed, ArrayCount(removed));
  vkr_lighting_system_shutdown(&system);
  printf("  test_point_light_table_keeps_rows PASSED\n");
  return true_v;
}

/* Baked static lamps leave the table but stay candidates for their moving
   casters' shadows (ADR-104); past VKR_POINT_LIGHT_GRID_LIGHT_MAX, the
   shadowed head's width, the sync keeps those nearest the camera, not the
   first it meets. */
static bool32_t test_baked_lamps_past_capacity_keep_nearest(void) {
  printf("  Running test_baked_lamps_past_capacity_keep_nearest...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  static VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  /* Lamps are created farthest first, 10 m apart from x = 1300 down. */
  const uint32_t count = VKR_POINT_LIGHT_GRID_LIGHT_MAX + 2u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrEntityId lamp = vkr_scene_create_entity(&scene, &error);
    assert(lamp.u64 != VKR_ENTITY_ID_INVALID.u64);
    assert(vkr_scene_set_transform(
        &scene, lamp, vec3_new(10.0f * (float32_t)(count - i), 1.0f, 0.0f),
        vkr_quat_identity(), vec3_one()));
    const ScenePointLight light = {
        .color = vec3_one(),
        .intensity = 1.0f,
        .range = 2.0f,
        .outer_cone_angle = 0.785398163f,
        .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
        .enabled = true_v,
        .casts_shadow = true_v,
        .mobility = VKR_LIGHT_MOBILITY_STATIC,
    };
    assert(vkr_scene_set_point_light(&scene, lamp, &light));
  }
  vkr_scene_update(&scene, 0.0);
  system.static_lights_baked = true_v;
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.point_light_count == 0u);
  assert(system.baked_lamp_count == VKR_POINT_LIGHT_GRID_LIGHT_MAX);
  for (uint32_t i = 0u; i < system.baked_lamp_count; ++i) {
    assert(system.baked_lamps[i].position.x <=
           10.0f * (float32_t)VKR_POINT_LIGHT_GRID_LIGHT_MAX + 0.001f);
  }

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_baked_lamps_past_capacity_keep_nearest PASSED\n");
  return true_v;
}

/* The slot of light group `name`, or VKR_SCENE_LIGHT_GROUP_NONE. */
static uint8_t lighting_test_group_slot(const VkrScene *scene,
                                        const char *name) {
  for (uint32_t i = 0u; i < scene->light_groups.count; ++i) {
    if (strcmp(scene->light_groups.names[i], name) == 0) {
      return (uint8_t)i;
    }
  }
  return VKR_SCENE_LIGHT_GROUP_NONE;
}

/* Lamp lightmaps cannot take one lamp's light away, so a static lamp
   changed since the bake takes its whole group out of the bake: the
   group's layers weigh nothing and its lamps light live, while the other
   groups stay baked (ADR-119). The second lamp has the empty group name,
   which its bake record spells in full. */
static bool32_t test_changed_lamp_group_leaves_bake(void) {
  printf("  Running test_changed_lamp_group_leaves_bake...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  static VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  const char *const groups[2] = {"street", ""};
  const char *const recorded_groups[2] = {"street", VKR_LIGHT_GROUP_DEFAULT};
  VkrEntityId lamps[2];
  ScenePointLight lights[2];
  for (uint32_t i = 0u; i < 2u; ++i) {
    lamps[i] = vkr_scene_create_entity(&scene, &error);
    assert(lamps[i].u64 != VKR_ENTITY_ID_INVALID.u64);
    assert(vkr_scene_set_transform(&scene, lamps[i],
                                   vec3_new(5.0f * (float32_t)i, 1.0f, 0.0f),
                                   vkr_quat_identity(), vec3_one()));
    lights[i] = (ScenePointLight){
        .color = vec3_one(),
        .intensity = 2.0f,
        .range = 3.0f,
        .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
        .enabled = true_v,
        .mobility = VKR_LIGHT_MOBILITY_STATIC,
    };
    snprintf(lights[i].light_group, sizeof(lights[i].light_group), "%s",
             groups[i]);
    assert(vkr_scene_set_point_light(&scene, lamps[i], &lights[i]));
  }
  vkr_scene_update(&scene, 0.0);
  (void)vkr_scene_resolve_world(&scene);
  const uint8_t street = lighting_test_group_slot(&scene, "street");
  const uint8_t default_group =
      lighting_test_group_slot(&scene, VKR_LIGHT_GROUP_DEFAULT);
  assert(street != VKR_SCENE_LIGHT_GROUP_NONE &&
         default_group != VKR_SCENE_LIGHT_GROUP_NONE);

  /* The bake recorded both lamps as they stand; lamp lightmaps hold them. */
  VkrSceneBakedLight records[2];
  MemZero(records, sizeof(records));
  for (uint32_t i = 0u; i < 2u; ++i) {
    records[i] = (VkrSceneBakedLight){
        .kind = 1u,
        .position = vec3_new(5.0f * (float32_t)i, 1.0f, 0.0f),
        .color = vec3_one(),
        .intensity = 2.0f,
        .range = 3.0f,
    };
    snprintf(records[i].group, sizeof(records[i].group), "%s",
             recorded_groups[i]);
  }
  VkrSceneDiffuseVolume volume = {
      .baked_lights = records,
      .baked_light_count = 2u,
      .baked_lights_known = true_v,
  };
  VkrSceneLightmaps lightmaps = {.texture = {.id = 1u}, .lamps_baked = true_v};
  scene.diffuse_volume = &volume;
  scene.lightmaps = &lightmaps;
  system.static_lights_baked = true_v;

  vkr_dynamic_volume_update_baked_groups(&scene, true_v);
  assert(scene.light_groups.left_bake == 0u);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.point_light_count == 0u);

  /* The street lamp brightens: its group leaves, the default group stays. */
  lights[0].intensity = 4.0f;
  assert(vkr_scene_set_point_light(&scene, lamps[0], &lights[0]));
  vkr_scene_update(&scene, 0.0);
  vkr_dynamic_volume_update_baked_groups(&scene, true_v);
  assert(scene.light_groups.left_bake == (1u << street));
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.point_light_count == 1u &&
         fabsf(system.point_lights[0].position.x) < 1.0e-4f);
  VkrLightLayer layers[2];
  MemZero(layers, sizeof(layers));
  for (uint32_t i = 0u; i < 2u; ++i) {
    layers[i].kind = VKR_LIGHT_LAYER_LAMP_GROUP;
    layers[i].index = i;
    snprintf(layers[i].name, sizeof(layers[i].name), "%s", recorded_groups[i]);
  }
  float32_t weights[2];
  vkr_scene_light_layer_weights(&scene, layers, 2u, weights);
  assert(weights[0] == 0.0f && weights[1] == 1.0f);

  /* Nothing lights a group live without the dynamic layer's trace, and a
     volume without lamp lightmaps takes one lamp away by itself. */
  vkr_dynamic_volume_update_baked_groups(&scene, false_v);
  assert(scene.light_groups.left_bake == 0u);
  scene.lightmaps = NULL;
  vkr_dynamic_volume_update_baked_groups(&scene, true_v);
  assert(scene.light_groups.left_bake == 0u);

  /* A recorded lamp that is gone takes its group out too. */
  scene.lightmaps = &lightmaps;
  lights[0].intensity = 2.0f;
  assert(vkr_scene_set_point_light(&scene, lamps[0], &lights[0]));
  lights[1].enabled = false_v;
  assert(vkr_scene_set_point_light(&scene, lamps[1], &lights[1]));
  vkr_scene_update(&scene, 0.0);
  vkr_dynamic_volume_update_baked_groups(&scene, true_v);
  assert(scene.light_groups.left_bake == (1u << default_group));

  scene.diffuse_volume = NULL;
  scene.lightmaps = NULL;
  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_changed_lamp_group_leaves_bake PASSED\n");
  return true_v;
}

static bool32_t test_point_light_gpu_row_packing(void) {
  printf("  Running test_point_light_gpu_row_packing...\n");
  VkrPointLight light = {
      .position = vec3_new(1.0f, 2.0f, 3.0f),
      .color = vec3_new(4.0f, 5.0f, 6.0f),
      .intensity = 7.0f,
      .constant = 8.0f,
      .linear = 9.0f,
      .quadratic = 10.0f,
      .range = 11.0f,
      .direction = vec3_new(12.0f, 13.0f, 14.0f),
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
  };
  VkrGpuPointLightRow row = {0};
  vkr_point_light_pack(&light, 1.0f, &row);
  assert(row.p0.x == 1.0f && row.p0.w == 8.0f);
  assert(row.p1.z == 6.0f && row.p1.w == 9.0f);
  assert(row.p2.x == 7.0f && row.p2.y == 10.0f && row.p2.z == 11.0f);
  assert(row.p3.x == 12.0f && row.p3.z == 14.0f && row.p3.w == 0.0f);

  light.kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT;
  light.inner_cone_angle = 0.25f;
  light.outer_cone_angle = 0.75f;
  vkr_point_light_pack(&light, 1.0f, &row);
  assert(fabsf(row.p0.w - cosf(light.inner_cone_angle)) < 0.000001f);
  assert(fabsf(row.p1.w - cosf(light.outer_cone_angle)) < 0.000001f);

  printf("  test_point_light_gpu_row_packing PASSED\n");
  return true_v;
}

/* A dynamic point light for the diffuse volume's layer at `position`. */
vkr_internal VkrEntityId lighting_test_dynamic_light(VkrScene *scene,
                                                     Vec3 position,
                                                     float32_t intensity) {
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  const VkrEntityId entity = vkr_scene_create_entity(scene, &error);
  assert(entity.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(scene, entity, position, vkr_quat_identity(),
                                 vec3_one()));
  const ScenePointLight light = {
      .color = vec3_one(),
      .intensity = intensity,
      .range = 1.0f,
      .kind = VKR_POINT_LIGHT_KIND_GLTF_POINT,
      .enabled = true_v,
      .mobility = VKR_LIGHT_MOBILITY_DYNAMIC,
  };
  assert(vkr_scene_set_point_light(scene, entity, &light));
  return entity;
}

/* Three bricks over a 6 x 3 x 6 indirection grid with 1 m probes: brick 0
   at level 0 from entry (0, 0, 0), brick 1 at level 1 from (3, 0, 0), which
   covers entries 3 to 5 in x, y and z up to 2, and brick 2 at level 0 from
   (0, 0, 4). */
vkr_internal VkrSceneDiffuseVolume
lighting_test_brick_volume(uint32_t *entries, uint8_t *probe_valid) {
  entries[0] = 0u;
  entries[1] = (1u << 30u) | 3u;
  entries[2] = 4u << 20u;
  MemSet(probe_valid, 1, 3u * 64u);
  return (VkrSceneDiffuseVolume){
      .spacing = 1.0f,
      .dimensions = {6u, 3u, 6u},
      .probe_count = 3u * 64u,
      .brick_entries = entries,
      .brick_count = 3u,
      .probe_valid = probe_valid,
      .dynamic_sh = {.id = 1u},
  };
}

/* The dynamic layer marks a brick active when a source's sphere reaches its
   box, whatever the brick's level, and no other: each source looks its
   bricks up in the indirection entries inside its bounds. */
static bool32_t test_dynamic_volume_marks_reached_bricks(void) {
  printf("  Running test_dynamic_volume_marks_reached_bricks...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  uint32_t entries[3];
  uint8_t probe_valid[3u * 64u];
  VkrSceneDiffuseVolume volume =
      lighting_test_brick_volume(entries, probe_valid);
  scene.diffuse_volume = &volume;

  /* One inside brick 0, one 0.5 m short of brick 1's box (x from 9 m), and
     one 1.5 m short of brick 2's box (z from 12 m), past its 1 m range. */
  (void)lighting_test_dynamic_light(&scene, vec3_new(1.5f, 1.5f, 1.5f), 1.0f);
  (void)lighting_test_dynamic_light(&scene, vec3_new(8.5f, 1.0f, 1.0f), 1.0f);
  (void)lighting_test_dynamic_light(&scene, vec3_new(1.5f, 1.5f, 10.5f), 1.0f);
  vkr_scene_update(&scene, 0.0);
  (void)vkr_scene_resolve_world(&scene);

  VkrDynamicVolumeSelection selection = {0};
  static VkrDynamicVolumeFrame frame;
  assert(
      vkr_dynamic_volume_select(&selection, &scene, NULL, NULL, 64u, &frame));
  assert(frame.input.point_light_count == 3u);
  assert(selection.active_bricks[0] ==
         ((UINT64_C(1) << 0u) | (UINT64_C(1) << 1u)));

  vkr_dynamic_volume_selection_destroy(&selection);
  scene.diffuse_volume = NULL;
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_dynamic_volume_marks_reached_bricks PASSED\n");
  return true_v;
}

/* Frames take turns through the active bricks' valid probes in brick
   order: three frames of 50 walk bricks 0 and 1, every seventh probe
   invalid, without a repeat, and wrap to the first probe after the last. */
static bool32_t test_dynamic_volume_probes_take_turns(void) {
  printf("  Running test_dynamic_volume_probes_take_turns...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(4), MB(8), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  uint32_t entries[3];
  uint8_t probe_valid[3u * 64u];
  VkrSceneDiffuseVolume volume =
      lighting_test_brick_volume(entries, probe_valid);
  for (uint32_t probe = 0u; probe < 3u * 64u; probe += 7u) {
    probe_valid[probe] = 0u;
  }
  scene.diffuse_volume = &volume;
  (void)lighting_test_dynamic_light(&scene, vec3_new(1.5f, 1.5f, 1.5f), 1.0f);
  (void)lighting_test_dynamic_light(&scene, vec3_new(8.5f, 1.0f, 1.0f), 1.0f);
  vkr_scene_update(&scene, 0.0);
  (void)vkr_scene_resolve_world(&scene);

  uint32_t expected[128];
  uint32_t expected_count = 0u;
  for (uint32_t probe = 0u; probe < 2u * 64u; ++probe) {
    if (probe_valid[probe]) {
      expected[expected_count++] = probe;
    }
  }
  VkrDynamicVolumeSelection selection = {0};
  static VkrDynamicVolumeFrame frame;
  uint32_t position = 0u;
  for (uint32_t turn = 0u; turn < 3u; ++turn) {
    assert(
        vkr_dynamic_volume_select(&selection, &scene, NULL, NULL, 50u, &frame));
    assert(frame.input.trace_probe_count == 50u);
    for (uint32_t i = 0u; i < 50u; ++i) {
      assert(frame.trace_probes[i] == expected[position % expected_count]);
      ++position;
    }
  }

  vkr_dynamic_volume_selection_destroy(&selection);
  scene.diffuse_volume = NULL;
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_dynamic_volume_probes_take_turns PASSED\n");
  return true_v;
}

/* Past VKR_DYNAMIC_VOLUME_LIGHTS_MAX the layer keeps the brightest sources:
   three bright lights last in scene order replace three dim ones rather
   than being left out. */
static bool32_t test_dynamic_volume_keeps_strongest_lights(void) {
  printf("  Running test_dynamic_volume_keeps_strongest_lights...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(64), MB(128), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID,
                        VKR_DYNAMIC_VOLUME_LIGHTS_MAX + 8u, &error));
  uint32_t entries[3];
  uint8_t probe_valid[3u * 64u];
  VkrSceneDiffuseVolume volume =
      lighting_test_brick_volume(entries, probe_valid);
  scene.diffuse_volume = &volume;

  for (uint32_t i = 0u; i < VKR_DYNAMIC_VOLUME_LIGHTS_MAX; ++i) {
    (void)lighting_test_dynamic_light(&scene, vec3_new(1.5f, 1.5f, 1.5f), 1.0f);
  }
  for (uint32_t i = 0u; i < 3u; ++i) {
    (void)lighting_test_dynamic_light(&scene, vec3_new(8.5f, 1.0f, 1.0f), 5.0f);
  }
  vkr_scene_update(&scene, 0.0);
  (void)vkr_scene_resolve_world(&scene);

  VkrDynamicVolumeSelection selection = {0};
  static VkrDynamicVolumeFrame frame;
  assert(
      vkr_dynamic_volume_select(&selection, &scene, NULL, NULL, 64u, &frame));
  assert(frame.input.point_light_count == VKR_DYNAMIC_VOLUME_LIGHTS_MAX);
  uint32_t bright = 0u;
  for (uint32_t i = 0u; i < frame.input.point_light_count; ++i) {
    bright += frame.point_lights[i].intensity > 2.0f ? 1u : 0u;
  }
  assert(bright == 3u);
  /* The bright lights' brick joins with them. */
  assert(selection.active_bricks[0] & (UINT64_C(1) << 1u));

  vkr_dynamic_volume_selection_destroy(&selection);
  scene.diffuse_volume = NULL;
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_dynamic_volume_keeps_strongest_lights PASSED\n");
  return true_v;
}

/* A scaled parent must move the rectangle's center, but must not distort its
 * authored area or replace the rigid parent/child orientation with world-matrix
 * axes. This independently catches the local-rotation-only regression. */
static bool32_t test_rectangle_light_uses_rigid_parent_rotation(void) {
  printf("  Running test_rectangle_light_uses_rigid_parent_rotation...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  const VkrQuat parent_rotation =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 1.57079632679f);
  const VkrQuat child_rotation =
      vkr_quat_from_axis_angle(vec3_new(1.0f, 0.0f, 0.0f), 1.57079632679f);
  VkrEntityId parent = vkr_scene_create_entity(&scene, &error);
  VkrEntityId child = vkr_scene_create_entity(&scene, &error);
  assert(parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
         child.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(&scene, parent, vec3_new(3.0f, 4.0f, 5.0f),
                                 parent_rotation, vec3_new(2.0f, 3.0f, 4.0f)));
  assert(vkr_scene_set_transform(&scene, child, vec3_new(1.0f, 2.0f, 3.0f),
                                 child_rotation, vec3_new(5.0f, 7.0f, 11.0f)));
  vkr_scene_set_parent(&scene, child, parent);
  assert(vkr_scene_set_rectangle_light(&scene, child,
                                       &(SceneRectangleLight){
                                           .color = vec3_one(),
                                           .radiance = 2.0f,
                                           .size = vec2_new(2.0f, 4.0f),
                                           .enabled = true_v,
                                       }));
  vkr_scene_update(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);

  assert(system.rectangle_light_count == 1u);
  const VkrRectangleLight *light = &system.rectangle_lights[0];
  // Ry(90) * Rx(90) maps +X to -Z and +Y to +X. Parent TRS maps
  // child translation (1,2,3) to (15,10,3); child scale cannot change it.
  assert(lighting_test_vec3_near(light->right, vec3_new(0.0f, 0.0f, -1.0f)));
  assert(lighting_test_vec3_near(light->up, vec3_new(1.0f, 0.0f, 0.0f)));
  assert(
      lighting_test_vec3_near(light->position, vec3_new(15.0f, 10.0f, 3.0f)));
  assert(light->half_width == 1.0f && light->half_height == 2.0f);

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_rectangle_light_uses_rigid_parent_rotation PASSED\n");
  return true_v;
}

/* A spot light under a rotated parent points where the parent turns it: the
 * local rotation alone left it pointing along its authored axis. A parent
 * with an authored matrix has no rotation chain, so its matrix turns it. */
static bool32_t test_spot_light_follows_parent_rotation(void) {
  printf("  Running test_spot_light_follows_parent_rotation...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 32u, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  const VkrQuat quarter_turn =
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 1.57079632679f);
  VkrEntityId rigid_parent = vkr_scene_create_entity(&scene, &error);
  VkrEntityId matrix_parent = vkr_scene_create_entity(&scene, &error);
  VkrEntityId rigid_spot = vkr_scene_create_entity(&scene, &error);
  VkrEntityId matrix_spot = vkr_scene_create_entity(&scene, &error);
  assert(rigid_parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
         matrix_parent.u64 != VKR_ENTITY_ID_INVALID.u64 &&
         rigid_spot.u64 != VKR_ENTITY_ID_INVALID.u64 &&
         matrix_spot.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(&scene, rigid_parent,
                                 vec3_new(0.0f, 2.0f, 0.0f), quarter_turn,
                                 vec3_new(1.0f, 3.0f, 1.0f)));
  assert(vkr_scene_set_local_matrix(
      &scene, matrix_parent,
      mat4_mul(mat4_translate(vec3_new(10.0f, 2.0f, 0.0f)),
               mat4_mul(vkr_quat_to_mat4(quarter_turn),
                        mat4_scale(vec3_new(2.0f, 2.0f, 2.0f))))));
  assert(vkr_scene_set_transform(&scene, rigid_spot, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_transform(&scene, matrix_spot, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_set_parent(&scene, rigid_spot, rigid_parent);
  vkr_scene_set_parent(&scene, matrix_spot, matrix_parent);
  const ScenePointLight spot = {
      .color = vec3_one(),
      .intensity = 1.0f,
      .range = 8.0f,
      .direction_local = vec3_new(0.0f, 0.0f, -1.0f),
      .inner_cone_angle = 0.3f,
      .outer_cone_angle = 0.6f,
      .kind = VKR_POINT_LIGHT_KIND_GLTF_SPOT,
      .enabled = true_v,
      .mobility = VKR_LIGHT_MOBILITY_DYNAMIC,
  };
  assert(vkr_scene_set_point_light(&scene, rigid_spot, &spot));
  assert(vkr_scene_set_point_light(&scene, matrix_spot, &spot));
  vkr_scene_update(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);

  // Ry(90) maps -Z to -X; the parents' scale must not change the direction.
  assert(system.point_light_count == 2u);
  for (uint32_t i = 0u; i < system.point_light_count; ++i) {
    assert(lighting_test_vec3_near(system.point_lights[i].direction,
                                   vec3_new(-1.0f, 0.0f, 0.0f)));
  }

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_spot_light_follows_parent_rotation PASSED\n");
  return true_v;
}

/* An enabled atmosphere turns the sun light into its sun: the light must not
 * light the frame directly before or besides the published atmosphere sun. */
static bool32_t test_atmosphere_replaces_direct_sun_light(void) {
  printf("  Running test_atmosphere_replaces_direct_sun_light...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_directional_light(
      &scene, sun,
      &(SceneDirectionalLight){
          .color = vec3_one(),
          .intensity = 1.0f,
          .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
          .enabled = true_v,
      }));
  vkr_scene_update(&scene, 0.0);
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.directional.enabled == true_v);

  VkrAtmosphereSettings atmosphere = vkr_atmosphere_settings_defaults();
  atmosphere.enabled = true_v;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &atmosphere, &clouds, 0.0f));
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.directional.enabled == false_v);

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_atmosphere_replaces_direct_sun_light PASSED\n");
  return true_v;
}

/* The atmosphere sun light drives an enabled atmosphere: the sun points
 * against the light's rotated direction with its tinted colour times
 * intensity. Other directional lights are ignored, an unchanged light queues
 * no revision, a hard-edged light keeps the authored disc, and without an
 * enabled sun light the sky has no sun (ADR-058). */
static bool32_t test_sun_light_drives_atmosphere_sun(void) {
  printf("  Running test_sun_light_drives_atmosphere_sun...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));

  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  authored.sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  authored.sun_angular_diameter_degrees = 0.75f;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));
  const VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  uint64_t revision = atmosphere->requested_revision;

  // An imported directional light, such as a glTF file's sun, is not one.
  VkrEntityId imported = vkr_scene_create_entity(&scene, &error);
  assert(imported.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_directional_light(
      &scene, imported,
      &(SceneDirectionalLight){
          .intensity = 0.0f,
          .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
          .enabled = true_v,
      }));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(scene.sun.found == false_v);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(
      atmosphere->requested_settings.solar_irradiance, vec3_zero()));

  // A quarter turn about +Y carries the local ray (0, -0.6, -0.8) to
  // (-0.8, -0.6, 0), so the sun sits toward +X at elevation asin(0.6).
  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  assert(vkr_scene_set_transform(
      &scene, sun, vec3_zero(),
      vkr_quat_from_axis_angle(vec3_new(0.0f, 1.0f, 0.0f), 1.57079632679f),
      vec3_one()));
  SceneDirectionalLight light = {
      .color = vec3_new(1.0f, 0.5f, 0.25f),
      .intensity = 2.0f,
      .direction_local = vec3_new(0.0f, -0.6f, -0.8f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  const VkrAtmosphereSettings *requested = &atmosphere->requested_settings;
  assert(lighting_test_vec3_near(requested->sun_direction,
                                 vec3_new(0.8f, 0.6f, 0.0f)));
  assert(lighting_test_vec3_near(requested->solar_irradiance,
                                 vec3_new(2.0f, 1.0f, 0.5f)));
  assert(requested->sun_angular_diameter_degrees == 0.75f);

  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == revision);

  // A 3000 K white light keeps its luminance and turns warm.
  light.color = vec3_one();
  light.temperature_kelvin = 3000.0f;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  const Vec3 warm = requested->solar_irradiance;
  assert(fabsf(0.2126f * warm.x + 0.7152f * warm.y + 0.0722f * warm.z - 2.0f) <
         1e-3f);
  assert(warm.x > warm.y && warm.y > warm.z);

  light.sun_angular_diameter_degrees = 2.0f;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);
  assert(requested->sun_angular_diameter_degrees == 2.0f);

  light.enabled = false_v;
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(scene.sun.found == false_v);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(requested->solar_irradiance, vec3_zero()));
  assert(requested->sun_angular_diameter_degrees == 0.75f);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_sun_light_drives_atmosphere_sun PASSED\n");
  return true_v;
}

/* A scene without a sun light takes the World's while it inherits the
 * World (ADR-076); hiding the World's light, or not inheriting, leaves the
 * sky without a sun. The oracle is the requested sun, not the light list. */
static bool32_t test_world_sun_light_is_fallback(void) {
  printf("  Running test_world_sun_light_is_fallback...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(4), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene root;
  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&root, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  assert(vkr_scene_init(&scene, &allocator, 0u, 8u, &error));
  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));

  VkrEntityId sun = vkr_scene_create_entity(&root, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  vkr_scene_set_visibility(&root, sun, true_v, true_v);
  assert(vkr_scene_set_directional_light(
      &root, sun,
      &(SceneDirectionalLight){
          .color = vec3_one(),
          .intensity = 3.0f,
          .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
          .enabled = true_v,
          .atmosphere_sun = true_v,
      }));
  vkr_scene_set_world_fallback(&scene, &root);
  vkr_scene_update(&scene, 0.0);
  vkr_scene_sync_sun(&scene, 0.0);
  const VkrAtmosphereSettings *requested = &scene.atmosphere.requested_settings;
  assert(scene.sun.found);
  assert(lighting_test_vec3_near(requested->sun_direction,
                                 vec3_new(0.0f, 1.0f, 0.0f)));
  assert(lighting_test_vec3_near(requested->solar_irradiance,
                                 vec3_new(3.0f, 3.0f, 3.0f)));

  vkr_scene_set_visibility(&root, sun, false_v, true_v);
  vkr_scene_sync_sun(&scene, 0.0);
  assert(!scene.sun.found);
  assert(lighting_test_vec3_near(requested->solar_irradiance, vec3_zero()));

  vkr_scene_set_visibility(&root, sun, true_v, true_v);
  scene.settings.inherit_world = false_v;
  vkr_scene_set_world_fallback(&scene, &root);
  vkr_scene_sync_sun(&scene, 0.0);
  assert(!scene.sun.found);

  vkr_scene_shutdown(&scene, NULL);
  vkr_scene_shutdown(&root, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_world_sun_light_is_fallback PASSED\n");
  return true_v;
}

/* A moving sun light draws at once: the frame pairs the published medium with
 * the live sun. Once a revision is published the sky light follows at most
 * once per refresh interval, a request that has not started baking follows the
 * light for free, and the last sun always gets its own revision. */
static bool32_t test_sun_refresh_follows_moving_light(void) {
  printf("  Running test_sun_refresh_follows_moving_light...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 31u, 8u, &error));
  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  authored.sun_direction = vec3_new(0.0f, 1.0f, 0.0f);
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  SceneDirectionalLight light = {
      .color = vkr_atmosphere_settings_defaults().solar_irradiance,
      .intensity = 1.0f,
      .direction_local = vec3_new(0.0f, -1.0f, 0.0f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);

  // Publish the first revision with a medium the authored settings lack.
  VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  atmosphere->active_settings = atmosphere->requested_settings;
  atmosphere->active_settings.rayleigh_density_scale = 2.0f;
  atmosphere->active_revision = atmosphere->requested_revision;
  atmosphere->candidate_revision = 0u;
  uint64_t revision = atmosphere->requested_revision;

  light.direction_local = vec3_new(0.0f, -0.6f, -0.8f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  const VkrAtmosphereSettings frame =
      vkr_scene_atmosphere_frame_settings(&scene);
  assert(
      lighting_test_vec3_near(frame.sun_direction, vec3_new(0.0f, 0.6f, 0.8f)));
  assert(frame.rayleigh_density_scale == 2.0f);
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(atmosphere->requested_settings.sun_direction,
                                 vec3_new(0.0f, 0.6f, 0.8f)));

  // Replacing the waiting request costs nothing, so it tracks every frame.
  light.direction_local = vec3_new(0.0f, -0.8f, -0.6f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(atmosphere->requested_revision == ++revision);

  // A baking request holds the next one back until the interval passes.
  atmosphere->candidate_revision = atmosphere->requested_revision;
  light.direction_local = vec3_new(0.0f, -0.5f, -0.866f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.1);
  assert(atmosphere->requested_revision == revision);
  vkr_scene_sync_sun(&scene, 0.2);
  assert(atmosphere->requested_revision == ++revision);
  assert(lighting_test_vec3_near(atmosphere->requested_settings.sun_direction,
                                 vec3_normalize(vec3_new(0.0f, 0.5f, 0.866f))));

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_sun_refresh_follows_moving_light PASSED\n");
  return true_v;
}

/* Light travelling away from a sun `depression_degrees` below the horizon. */
static Vec3 lighting_test_light_below_horizon(float32_t depression_degrees) {
  const float32_t radians = depression_degrees * (VKR_PI / 180.0f);
  return vec3_new(0.0f, vkr_sin_f32(radians), -vkr_cos_f32(radians));
}

/* Past the dark depression the bake is black (ADR-058), so a sun that keeps
 * turning through the night requests no revision, while the turn back into
 * the lit range does. A light without irradiance is dark in any direction. */
static bool32_t test_dark_sun_requests_no_revision(void) {
  printf("  Running test_dark_sun_requests_no_revision...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 32u, 8u, &error));
  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));
  const VkrAtmosphereGpuParams sea_level = vkr_atmosphere_prepare(&authored);
  const float32_t dark = vkr_atmosphere_dark_depression_degrees(&sea_level);
  assert(dark > 40.0f && dark < 41.0f);

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  SceneDirectionalLight light = {
      .color = vkr_atmosphere_settings_defaults().solar_irradiance,
      .intensity = 1.0f,
      .direction_local = lighting_test_light_below_horizon(dark + 10.0f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 0.0);
  VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  atmosphere->active_settings = atmosphere->requested_settings;
  atmosphere->active_revision = atmosphere->requested_revision;
  atmosphere->candidate_revision = 0u;
  const uint64_t revision = atmosphere->requested_revision;

  // Deeper into the night: the sky light is already black.
  light.direction_local = lighting_test_light_below_horizon(dark + 20.0f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 1.0);
  assert(atmosphere->requested_revision == revision);

  // A light without irradiance bakes black whichever way it points.
  light.intensity = 0.0f;
  light.direction_local = lighting_test_light_below_horizon(-30.0f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 1.0);
  assert(atmosphere->requested_revision == revision);

  // Inside the dark depression the sky can scatter light again.
  light.intensity = 1.0f;
  light.direction_local = lighting_test_light_below_horizon(dark - 2.0f);
  assert(vkr_scene_set_directional_light(&scene, sun, &light));
  vkr_scene_sync_sun(&scene, 1.0);
  assert(atmosphere->requested_revision == revision + 1u);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_dark_sun_requests_no_revision PASSED\n");
  return true_v;
}

/* The moon is the atmosphere's second light (ADR-081). A moon light is never
 * the sun, even with its default sun flag, and it becomes the direct light
 * only while the sun is below the observer's horizon. */
static bool32_t test_moon_light_lights_the_night(void) {
  printf("  Running test_moon_light_lights_the_night...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&scene, &allocator, 33u, 8u, &error));
  VkrAtmosphereSettings authored = vkr_atmosphere_settings_defaults();
  authored.enabled = true_v;
  const VkrCloudSettings clouds = vkr_cloud_settings_defaults();
  assert(vkr_scene_request_atmosphere(&scene, &authored, &clouds, 0.0f));

  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(sun.u64 != VKR_ENTITY_ID_INVALID.u64);
  SceneDirectionalLight sun_light = {
      .color = vkr_atmosphere_settings_defaults().solar_irradiance,
      .intensity = 1.0f,
      .direction_local = lighting_test_light_below_horizon(20.0f),
      .enabled = true_v,
      .atmosphere_sun = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, sun, &sun_light));
  VkrEntityId moon = vkr_scene_create_entity(&scene, &error);
  assert(moon.u64 != VKR_ENTITY_ID_INVALID.u64);
  const SceneDirectionalLight moon_light = {
      .color = vec3_one(),
      .intensity = 4.5e-6f,
      .direction_local = vec3_new(0.0f, -0.5f, 0.8660254f),
      .sun_angular_diameter_degrees = 0.52f,
      .enabled = true_v,
      .atmosphere_sun = true_v,
      .atmosphere_moon = true_v,
  };
  assert(vkr_scene_set_directional_light(&scene, moon, &moon_light));
  vkr_scene_sync_sun(&scene, 0.0);
  assert(scene.sun.found && scene.sun.entity.u64 == sun.u64);
  assert(scene.moon.found && scene.moon.entity.u64 == moon.u64);
  VkrSceneAtmosphere *atmosphere = &scene.atmosphere;
  const VkrAtmosphereSettings *requested = &atmosphere->requested_settings;
  assert(lighting_test_vec3_near(requested->moon_direction,
                                 vec3_new(0.0f, 0.5f, -0.8660254f)));
  assert(fabsf(requested->lunar_irradiance.x - 4.5e-6f) < 1e-12f &&
         fabsf(requested->lunar_irradiance.z - 4.5e-6f) < 1e-12f);
  assert(requested->moon_angular_diameter_degrees == 0.52f);

  // Published, the night's direct light is the moon, attenuated by the air.
  atmosphere->active_settings = atmosphere->requested_settings;
  atmosphere->active_revision = atmosphere->requested_revision;
  atmosphere->candidate_revision = 0u;
  VkrSceneKeyLight key = vkr_scene_atmosphere_frame_key_light(&scene);
  assert(key.moon);
  assert(lighting_test_vec3_near(key.toward, requested->moon_direction));
  assert(key.irradiance.x > 0.0f && key.irradiance.x < 4.5e-6f);
  assert(key.angular_diameter_degrees == 0.52f);

  // Once the sun rises it is the direct light again.
  sun_light.direction_local = vec3_new(0.0f, -0.5f, -0.8660254f);
  assert(vkr_scene_set_directional_light(&scene, sun, &sun_light));
  vkr_scene_sync_sun(&scene, 0.0);
  key = vkr_scene_atmosphere_frame_key_light(&scene);
  assert(!key.moon);
  assert(lighting_test_vec3_near(key.toward, vec3_new(0.0f, 0.5f, 0.8660254f)));
  assert(key.irradiance.y > 0.1f);

  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_moon_light_lights_the_night PASSED\n");
  return true_v;
}

/* A full moon under street lamps lights nothing that shows, so it must not
 * cost the frame its cascades (ADR-081); a long exposure makes the same moon
 * visible again, and an unknown exposure never hides a light. */
static bool32_t test_invisible_key_light_is_disabled(void) {
  printf("  Running test_invisible_key_light_is_disabled...\n");
  VkrLightingSystem system = {0};
  const Vec3 toward = vec3_new(0.0f, 0.5f, -0.8660254f);
  const Vec3 moonlight = vec3_new(3.6e-6f, 3.6e-6f, 3.6e-6f);
  vkr_lighting_system_apply_atmosphere_light(&system, toward, moonlight, 0.52f,
                                             10.8f);
  assert(!system.directional.enabled);
  vkr_lighting_system_apply_atmosphere_light(&system, toward, moonlight, 0.52f,
                                             262144.0f);
  assert(system.directional.enabled);
  assert(lighting_test_vec3_near(system.directional.direction,
                                 vec3_negate(toward)));
  vkr_lighting_system_apply_atmosphere_light(&system, toward, moonlight, 0.52f,
                                             0.0f);
  assert(system.directional.enabled);
  printf("  test_invisible_key_light_is_disabled PASSED\n");
  return true_v;
}

/* The point light the system kept with intensity `intensity`, or NULL. */
static const VkrPointLight *time_test_point(const VkrLightingSystem *system,
                                            float32_t intensity) {
  for (uint32_t i = 0u; i < system->point_light_count; ++i) {
    const float32_t value = system->point_lights[i].intensity;
    if (fabsf(value - intensity) < 1.0e-5f) {
      return &system->point_lights[i];
    }
  }
  return NULL;
}

/* A time of day turns the sun about the celestial pole by the hour, the way
 * the sky turns (setting toward +X under the default pole), and its night
 * groups light only while the sun is down; a script-set hour and group
 * intensities scale static lights alone and last until the simulation
 * resets (ADR-090). */
static bool32_t test_time_of_day_turns_sun_and_switches_night_groups(void) {
  printf("  Running test_time_of_day_turns_sun_and_switches_night_groups...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(1), MB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);

  VkrScene scene;
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(
      vkr_scene_init(&scene, &allocator, VKR_SCENE_WORLD_ROOT_ID, 8u, &error));
  VkrLightingSystem system;
  assert(vkr_lighting_system_init(&system));

  /* Noon sun 45 degrees above the horizon, perpendicular to the default
     pole (0, 0.7071, 0.7071). */
  const float32_t h = 0.70710678f;
  VkrEntityId sun = vkr_scene_create_entity(&scene, &error);
  assert(vkr_scene_set_directional_light(
      &scene, sun,
      &(SceneDirectionalLight){
          .color = vec3_one(),
          .intensity = 1.0f,
          .direction_local = vec3_new(0.0f, -h, h),
          .enabled = true_v,
      }));
  const struct {
    const char *group;
    VkrLightMobility mobility;
    float32_t intensity;
  } lamps[] = {{"street", VKR_LIGHT_MOBILITY_STATIC, 2.0f},
               {"", VKR_LIGHT_MOBILITY_STATIC, 3.0f},
               {"", VKR_LIGHT_MOBILITY_DYNAMIC, 5.0f}};
  for (uint32_t i = 0u; i < ArrayCount(lamps); ++i) {
    VkrEntityId lamp = vkr_scene_create_entity(&scene, &error);
    assert(vkr_scene_set_transform(&scene, lamp, vec3_new((float32_t)i, 1, 0),
                                   vkr_quat_identity(), vec3_one()));
    ScenePointLight light = {.color = vec3_one(),
                             .intensity = lamps[i].intensity,
                             .constant = 1.0f,
                             .enabled = true_v,
                             .mobility = lamps[i].mobility};
    snprintf(light.light_group, sizeof(light.light_group), "%s",
             lamps[i].group);
    assert(vkr_scene_set_point_light(&scene, lamp, &light));
  }
  VkrEntityId panel = vkr_scene_create_entity(&scene, &error);
  assert(vkr_scene_set_transform(&scene, panel, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  assert(vkr_scene_set_rectangle_light(&scene, panel,
                                       &(SceneRectangleLight){
                                           .color = vec3_one(),
                                           .radiance = 4.0f,
                                           .size = vec2_new(1.0f, 1.0f),
                                           .enabled = true_v,
                                           .light_group = "porch",
                                       }));
  ScenePointLight invalid = {.color = vec3_one(), .light_group = "two words"};
  assert(!vkr_scene_set_point_light(&scene, panel, &invalid));

  VkrEntityId clock = vkr_scene_create_entity(&scene, &error);
  SceneTimeOfDay time;
  vkr_type_defaults(&vkr_scene_time_of_day_type, &time);
  snprintf(time.night_groups, sizeof(time.night_groups), "street, porch");
  assert(
      vkr_scene_set_typed(&scene, clock, &vkr_scene_time_of_day_type, &time));
  vkr_scene_update(&scene, 0.0);
  (void)vkr_scene_resolve_world(&scene);
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  /* Noon keeps the authored sun; the night groups are off. */
  assert(lighting_test_vec3_near(system.directional.direction,
                                 vec3_new(0.0f, -h, h)));
  assert(system.point_light_count == 2u && !time_test_point(&system, 2.0f));
  assert(system.rectangle_light_count == 0u);

  /* At 18:00 the sun sets toward +X and the night groups light. */
  time.hour = 18.0f;
  assert(
      vkr_scene_set_typed(&scene, clock, &vkr_scene_time_of_day_type, &time));
  (void)vkr_scene_resolve_world(&scene);
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(lighting_test_vec3_near(system.directional.direction,
                                 vec3_new(-1.0f, 0.0f, 0.0f)));
  assert(system.point_light_count == 3u && time_test_point(&system, 2.0f));
  assert(system.rectangle_light_count == 1u &&
         system.rectangle_lights[0].radiance == 4.0f);

  /* A pipeline that bakes static light keeps the dynamic lights alone
     (ADR-087). */
  system.static_lights_baked = true_v;
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.point_light_count == 1u && time_test_point(&system, 5.0f));
  assert(system.rectangle_light_count == 0u);
  system.static_lights_baked = false_v;

  /* Group intensities scale static lights only. */
  assert(vkr_scene_set_light_group_intensity(&scene, "street", 0.5f));
  assert(vkr_scene_set_light_group_intensity(&scene, "", 0.0f));
  assert(!vkr_scene_set_light_group_intensity(&scene, "street", -1.0f));
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(system.point_light_count == 2u && time_test_point(&system, 1.0f) &&
         time_test_point(&system, 5.0f));

  /* A script-set 06:00 raises the sun from -X; a day of 24 real minutes
     then advances it an hour per 3,600 ticks. */
  assert(vkr_scene_set_time_of_day_hour(&scene, 6.0));
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(lighting_test_vec3_near(system.directional.direction,
                                 vec3_new(1.0f, 0.0f, 0.0f)));
  time.day_minutes = 24.0f;
  assert(
      vkr_scene_set_typed(&scene, clock, &vkr_scene_time_of_day_type, &time));
  (void)vkr_scene_resolve_world(&scene);
  scene.simulation.completed_ticks += 3600u;
  vkr_scene_sync_sun(&scene, 0.0);
  assert(fabs(scene.clock.hour - 7.0) < 1.0e-6);

  /* A reset returns to the authored hour and full group intensities. */
  vkr_scene_simulation_reset_state(&scene);
  vkr_scene_sync_sun(&scene, 0.0);
  vkr_lighting_system_sync_from_scene(&system, &scene, vec3_zero(),
                                      1.0f / 60.0f);
  assert(fabs(scene.clock.hour - 18.0) < 1.0e-6);
  assert(system.point_light_count == 3u && time_test_point(&system, 2.0f) &&
         time_test_point(&system, 3.0f));

  SceneTimeOfDay rejected = time;
  snprintf(rejected.night_groups, sizeof(rejected.night_groups), "street,a b");
  assert(!vkr_type_validate(&vkr_scene_time_of_day_type, &rejected, NULL, 0u));

  vkr_lighting_system_shutdown(&system);
  vkr_scene_shutdown(&scene, NULL);
  vkr_dmemory_destroy(&memory);
  printf("  test_time_of_day_turns_sun_and_switches_night_groups PASSED\n");
  return true_v;
}

bool32_t run_lighting_system_tests(void) {
  printf("--- Running Lighting System tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_atmosphere_replaces_direct_sun_light();
  passed &= test_sun_light_drives_atmosphere_sun();
  passed &= test_world_sun_light_is_fallback();
  passed &= test_sun_refresh_follows_moving_light();
  passed &= test_dark_sun_requests_no_revision();
  passed &= test_moon_light_lights_the_night();
  passed &= test_invisible_key_light_is_disabled();
  passed &= test_point_light_limit_keeps_nearest();
  passed &= test_point_light_limit_holds_and_fades();
  passed &= test_point_light_table_keeps_nearest_past_capacity();
  passed &= test_point_light_table_holds_and_fades();
#if VKR_MAX_SCENE_POINT_LIGHTS > VKR_POINT_LIGHT_GRID_LIGHT_MAX
  passed &= test_point_light_head_holds_and_trades();
#endif
  passed &= test_point_light_table_keeps_rows();
  passed &= test_point_light_table_without_competition_snaps();
  passed &= test_baked_lamps_past_capacity_keep_nearest();
  passed &= test_changed_lamp_group_leaves_bake();
  passed &= test_point_light_gpu_row_packing();
  passed &= test_dynamic_volume_marks_reached_bricks();
  passed &= test_dynamic_volume_keeps_strongest_lights();
  passed &= test_dynamic_volume_probes_take_turns();
  passed &= test_rectangle_light_uses_rigid_parent_rotation();
  passed &= test_spot_light_follows_parent_rotation();
  passed &= test_time_of_day_turns_sun_and_switches_night_groups();
  printf("--- Lighting System tests completed. ---\n");
  return passed;
}

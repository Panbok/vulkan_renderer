#include "entity_test.h"

typedef struct Position {
  float32_t x;
  float32_t y;
  float32_t z;
} Position;

typedef struct Velocity {
  float32_t x;
  float32_t y;
  float32_t z;
} Velocity;

static Arena *world_arena = NULL;
static Arena *scratch_arena = NULL;
static VkrAllocator world_alloc = {0};
static VkrAllocator scratch_alloc = {0};

static void setup_suite(void) {
  world_arena = arena_create(MB(2), MB(2));
  scratch_arena = arena_create(MB(1), MB(1));
  assert(world_arena && "arena_create failed");
  assert(scratch_arena && "arena_create failed");

  world_alloc = (VkrAllocator){.ctx = world_arena};
  vkr_allocator_arena(&world_alloc);
  scratch_alloc = (VkrAllocator){.ctx = scratch_arena};
  vkr_allocator_arena(&scratch_alloc);
}

static void teardown_suite(void) {
  if (world_arena) {
    arena_destroy(world_arena);
    world_arena = NULL;
  }
  if (scratch_arena) {
    arena_destroy(scratch_arena);
    scratch_arena = NULL;
  }
  world_alloc = (VkrAllocator){0};
  scratch_alloc = (VkrAllocator){0};
}

static VkrWorld *create_world(uint16_t world_id) {
  VkrWorldCreateInfo info = {
      .alloc = &world_alloc,
      .scratch_alloc = &scratch_alloc,
      .world_id = world_id,
      .initial_entities = 32,
      .initial_components = 16,
      .initial_archetypes = 8,
  };
  return vkr_entity_create_world(&info);
}

static void test_world_create_destroy(void) {
  printf("  Running test_world_create_destroy...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");
  assert(world->arch_count == 1 && "Expected empty archetype");

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_world_create_destroy PASSED\n");
}

static void test_component_registration_lookup(void) {
  printf("  Running test_component_registration_lookup...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world, "Position", sizeof(Position),
                                    AlignOf(Position));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);

  VkrComponentTypeId found =
      vkr_entity_find_component(world, "Position");
  assert(found == pos_id);

  VkrComponentTypeId once_id =
      vkr_entity_register_component_once(world, "Position", sizeof(Position),
                                         AlignOf(Position));
  assert(once_id == pos_id);

  VkrComponentTypeId mismatch =
      vkr_entity_register_component_once(world, "Position",
                                         sizeof(Position) + 4,
                                         AlignOf(Position));
  assert(mismatch == VKR_COMPONENT_TYPE_INVALID);

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_component_registration_lookup PASSED\n");
}

static void test_entity_add_remove_component(void) {
  printf("  Running test_entity_add_remove_component...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world, "Position", sizeof(Position),
                                    AlignOf(Position));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);

  VkrEntityId entity = vkr_entity_create_entity(world);
  assert(entity.u64 != 0);

  Position pos = {.x = 1.0f, .y = 2.0f, .z = 3.0f};
  assert(vkr_entity_add_component(world, entity, pos_id, &pos));
  assert(vkr_entity_has_component(world, entity, pos_id));

  Position *pos_ptr =
      (Position *)vkr_entity_get_component_mut(world, entity, pos_id);
  assert(pos_ptr);
  assert(pos_ptr->x == pos.x && pos_ptr->y == pos.y && pos_ptr->z == pos.z);

  assert(vkr_entity_remove_component(world, entity, pos_id));
  assert(!vkr_entity_has_component(world, entity, pos_id));
  assert(vkr_entity_get_component(world, entity, pos_id) == NULL);

  Position pos2 = {.x = 4.0f, .y = 5.0f, .z = 6.0f};
  assert(vkr_entity_add_component(world, entity, pos_id, &pos2));
  pos_ptr = (Position *)vkr_entity_get_component_mut(world, entity, pos_id);
  assert(pos_ptr);
  assert(pos_ptr->x == pos2.x && pos_ptr->y == pos2.y && pos_ptr->z == pos2.z);

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_entity_add_remove_component PASSED\n");
}

static void test_create_entity_with_components(void) {
  printf("  Running test_create_entity_with_components...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world, "Position", sizeof(Position),
                                    AlignOf(Position));
  VkrComponentTypeId vel_id =
      vkr_entity_register_component(world, "Velocity", sizeof(Velocity),
                                    AlignOf(Velocity));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);
  assert(vel_id != VKR_COMPONENT_TYPE_INVALID);

  Position pos = {.x = 7.0f, .y = 8.0f, .z = 9.0f};
  Velocity vel = {.x = -1.0f, .y = -2.0f, .z = -3.0f};
  VkrComponentTypeId types[] = {pos_id, vel_id};
  const void *inits[] = {&pos, &vel};

  VkrEntityId entity =
      vkr_entity_create_entity_with_components(world, types, inits,
                                               ArrayCount(types));
  assert(entity.u64 != 0);
  assert(vkr_entity_has_component(world, entity, pos_id));
  assert(vkr_entity_has_component(world, entity, vel_id));

  const Position *pos_ptr =
      (const Position *)vkr_entity_get_component(world, entity, pos_id);
  const Velocity *vel_ptr =
      (const Velocity *)vkr_entity_get_component(world, entity, vel_id);
  assert(pos_ptr && vel_ptr);
  assert(pos_ptr->x == pos.x && pos_ptr->y == pos.y && pos_ptr->z == pos.z);
  assert(vel_ptr->x == vel.x && vel_ptr->y == vel.y && vel_ptr->z == vel.z);

  uint32_t arch_count = world->arch_count;
  VkrEntityId entity2 =
      vkr_entity_create_entity_with_components(world, types, inits,
                                               ArrayCount(types));
  assert(entity2.u64 != 0);
  assert(world->arch_count == arch_count);

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_create_entity_with_components PASSED\n");
}

static void test_create_many_entities(void) {
  printf("  Running test_create_many_entities...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world, "Position", sizeof(Position),
                                    AlignOf(Position));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);

  const uint32_t count = 900;
  VkrComponentTypeId types[] = {pos_id};
  Position pos = {.x = 1.0f, .y = 2.0f, .z = 3.0f};
  const void *inits[] = {&pos};

  for (uint32_t i = 0; i < count; ++i) {
    VkrEntityId entity = vkr_entity_create_entity_with_components(
        world, types, inits, ArrayCount(types));
    assert(entity.u64 != 0);
    assert(vkr_entity_has_component(world, entity, pos_id));
  }

  assert(world->dir.capacity >= count);
  assert(world->arch_count >= 2);

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_create_many_entities PASSED\n");
}

static void query_count_chunk(const VkrArchetype *arch, VkrChunk *chunk,
                              void *user) {
  (void)arch;
  uint32_t *count = (uint32_t *)user;
  *count += vkr_entity_chunk_count(chunk);
}

static void test_query_and_compiled(void) {
  printf("  Running test_query_and_compiled...\n");
  setup_suite();

  VkrWorld *world = create_world(1);
  assert(world && "World create failed");

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world, "Position", sizeof(Position),
                                    AlignOf(Position));
  VkrComponentTypeId vel_id =
      vkr_entity_register_component(world, "Velocity", sizeof(Velocity),
                                    AlignOf(Velocity));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);
  assert(vel_id != VKR_COMPONENT_TYPE_INVALID);

  VkrEntityId e1 = vkr_entity_create_entity(world);
  Position p1 = {.x = 1.0f, .y = 0.0f, .z = 0.0f};
  assert(vkr_entity_add_component(world, e1, pos_id, &p1));

  VkrEntityId e2 = vkr_entity_create_entity(world);
  Position p2 = {.x = 2.0f, .y = 0.0f, .z = 0.0f};
  Velocity v2 = {.x = 0.0f, .y = 1.0f, .z = 0.0f};
  assert(vkr_entity_add_component(world, e2, pos_id, &p2));
  assert(vkr_entity_add_component(world, e2, vel_id, &v2));

  VkrEntityId e3 = vkr_entity_create_entity(world);
  Velocity v3 = {.x = 0.0f, .y = 0.0f, .z = 1.0f};
  assert(vkr_entity_add_component(world, e3, vel_id, &v3));

  VkrComponentTypeId include[] = {pos_id};
  VkrComponentTypeId exclude[] = {vel_id};
  VkrQuery query = {0};
  vkr_entity_query_build(world, include, ArrayCount(include), exclude,
                         ArrayCount(exclude), &query);

  uint32_t count = 0;
  vkr_entity_query_each_chunk(world, &query, query_count_chunk, &count);
  assert(count == 1);

  VkrQueryCompiled compiled = {0};
  assert(vkr_entity_query_compile(world, &query, &world_alloc, &compiled));
  count = 0;
  vkr_entity_query_compiled_each_chunk(&compiled, query_count_chunk, &count);
  assert(count == 1);
  vkr_entity_query_compiled_destroy(&world_alloc, &compiled);

  vkr_entity_destroy_world(world);
  teardown_suite();
  printf("  test_query_and_compiled PASSED\n");
}

static void test_world_id_validation(void) {
  printf("  Running test_world_id_validation...\n");
  setup_suite();

  VkrWorld *world_a = create_world(1);
  VkrWorld *world_b = create_world(2);
  assert(world_a && world_b);

  VkrComponentTypeId pos_id =
      vkr_entity_register_component(world_a, "Position", sizeof(Position),
                                    AlignOf(Position));
  assert(pos_id != VKR_COMPONENT_TYPE_INVALID);

  VkrEntityId entity = vkr_entity_create_entity(world_a);
  Position pos = {.x = 1.0f, .y = 2.0f, .z = 3.0f};
  assert(vkr_entity_add_component(world_a, entity, pos_id, &pos));

  assert(vkr_entity_is_alive(world_a, entity));
  assert(!vkr_entity_is_alive(world_b, entity));

  vkr_entity_destroy_world(world_a);
  vkr_entity_destroy_world(world_b);
  teardown_suite();
  printf("  test_world_id_validation PASSED\n");
}

bool32_t run_entity_tests(void) {
  printf("--- Running Entity tests... ---\n");
  test_world_create_destroy();
  test_component_registration_lookup();
  test_entity_add_remove_component();
  test_create_entity_with_components();
  test_create_many_entities();
  test_query_and_compiled();
  test_world_id_validation();
  printf("--- Entity tests completed. ---\n");
  return true_v;
}

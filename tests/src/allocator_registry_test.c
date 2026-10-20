/**
 * @file allocator_registry_test.c
 * @brief Tests the named allocator registry that a recording samples once per
 * frame. The registry keeps its own counters, so these assertions hold with
 * VKR_ALLOCATOR_DISABLE_STATS set as well.
 */
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"

#include <assert.h>
#include <stdio.h>

bool32_t run_allocator_registry_tests(void);

// One spare element detects a sample that writes past its capacity.
static VkrAllocatorRegistrySample
    registry_samples[VKR_ALLOCATOR_REGISTRY_CAPACITY + 1];

static VkrAllocator overflow_allocators[VKR_ALLOCATOR_REGISTRY_CAPACITY + 1];

static bool8_t registry_name_equals(const char *name, const char *expected) {
  uint32_t index = 0;
  while (expected[index] != '\0') {
    if (name[index] != expected[index]) {
      return false_v;
    }
    index++;
  }

  return name[index] == '\0';
}

static uint32_t registry_sample_all(void) {
  uint32_t count = vkr_allocator_registry_sample(
      registry_samples, VKR_ALLOCATOR_REGISTRY_CAPACITY);
  assert(count <= VKR_ALLOCATOR_REGISTRY_CAPACITY);
  return count;
}

/* Samples the registry and returns the live entry in the slot; asserts it is
 * present. */
static VkrAllocatorRegistrySample registry_find(uint32_t slot) {
  uint32_t count = registry_sample_all();
  for (uint32_t i = 0; i < count; ++i) {
    if (registry_samples[i].slot == slot) {
      return registry_samples[i];
    }
  }

  assert(false && "registered slot missing from the sample");
  return (VkrAllocatorRegistrySample){0};
}

static bool8_t registry_contains(uint32_t slot, uint32_t generation) {
  uint32_t count = registry_sample_all();
  for (uint32_t i = 0; i < count; ++i) {
    if (registry_samples[i].slot == slot &&
        registry_samples[i].generation == generation) {
      return true_v;
    }
  }

  return false_v;
}

static void test_registry_counts_registered_dmemory(void) {
  printf("  Running test_registry_counts_registered_dmemory...\n");

  VkrDMemory dmemory;
  assert(vkr_dmemory_create(MB(1), MB(2), &dmemory));
  VkrAllocator allocator = {.ctx = &dmemory};
  vkr_dmemory_allocator_create(&allocator);

  assert(vkr_allocator_register_name(&allocator, "test.registry.dmemory"));
  assert(allocator.registry_slot != 0);
  assert(!vkr_allocator_register_name(&allocator, "test.registry.twice"));

  void *small =
      vkr_allocator_alloc(&allocator, KB(1), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *medium =
      vkr_allocator_alloc(&allocator, KB(2), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void *large =
      vkr_allocator_alloc(&allocator, KB(4), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(small != NULL && medium != NULL && large != NULL);

  vkr_allocator_free(&allocator, medium, KB(2), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  small = vkr_allocator_realloc(&allocator, small, KB(1), KB(3),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(small != NULL);

  VkrAllocatorRegistrySample sample = registry_find(allocator.registry_slot);
  assert(registry_name_equals(sample.name, "test.registry.dmemory"));
  assert(sample.type == VKR_ALLOCATOR_TYPE_DMEMORY);
  assert(sample.owner == VKR_ALLOCATOR_REGISTRY_OWNER_ALLOCATOR);
  assert(sample.alloc_count == 3);
  assert(sample.free_count == 1);
  assert(sample.realloc_count == 1);
  assert(sample.live_bytes == KB(3) + KB(4));
  assert(!sample.has_arena);

  vkr_allocator_free(&allocator, small, KB(3), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&allocator, large, KB(4), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  sample = registry_find(allocator.registry_slot);
  assert(sample.free_count == 3);
  assert(sample.live_bytes == 0);

  uint32_t slot = allocator.registry_slot;
  uint32_t generation = sample.generation;
  vkr_allocator_unregister(&allocator);
  assert(allocator.registry_slot == 0);
  assert(!registry_contains(slot, generation));

  vkr_dmemory_allocator_destroy(&allocator);
}

static void test_registry_arena_scopes_and_footprint(void) {
  printf("  Running test_registry_arena_scopes_and_footprint...\n");

  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_register_name(&allocator, "test.registry.arena"));
  assert(arena->registry_slot == allocator.registry_slot);

  void *persistent =
      vkr_allocator_alloc(&allocator, KB(4), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  assert(persistent != NULL);

  VkrAllocatorRegistrySample sample = registry_find(allocator.registry_slot);
  assert(sample.type == VKR_ALLOCATOR_TYPE_ARENA);
  assert(sample.has_arena);
  assert(sample.alloc_count == 1);
  assert(sample.live_bytes == KB(4));
  assert(sample.arena_used_bytes >= KB(4));
  assert(sample.arena_committed_bytes >= sample.arena_used_bytes);
  assert(sample.arena_reserved_bytes >= sample.arena_committed_bytes);
  uint64_t used_before_scopes = sample.arena_used_bytes;

  VkrAllocatorScope outer = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&outer));
  assert(vkr_allocator_alloc(&allocator, KB(8),
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY) != NULL);

  VkrAllocatorScope middle = vkr_allocator_begin_scope(&allocator);
  VkrAllocatorScope inner = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&middle));
  assert(vkr_allocator_scope_is_valid(&inner));

  // Larger than the initial commit, so the committed size must grow.
  assert(vkr_allocator_alloc(&allocator, KB(256),
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY) != NULL);
  vkr_allocator_end_scope(&inner, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_end_scope(&middle, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  sample = registry_find(allocator.registry_slot);
  assert(sample.scope_count == 3);
  assert(sample.scope_depth == 1);
  assert(sample.scope_depth_high == 3);
  assert(sample.scope_alloc_bytes == KB(8) + KB(256));
  assert(sample.live_bytes == KB(4) + KB(8));
  assert(sample.arena_used_bytes < used_before_scopes + KB(256));
  assert(sample.arena_interval_peak_used_bytes >=
         used_before_scopes + KB(8) + KB(256));
  assert(sample.arena_committed_bytes >= KB(4) + KB(8) + KB(256));

  /* The previous sample restarted the interval maxima: the depth high-water
   * mark is the depth still open and the used peak is the current size. */
  sample = registry_find(allocator.registry_slot);
  assert(sample.scope_depth_high == 1);
  assert(sample.arena_interval_peak_used_bytes == sample.arena_used_bytes);
  assert(sample.arena_peak_used_bytes >= used_before_scopes + KB(8) + KB(256));

  // The outer scope was open during this interval, so it still counts.
  vkr_allocator_end_scope(&outer, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  sample = registry_find(allocator.registry_slot);
  assert(sample.scope_depth == 0);
  assert(sample.scope_depth_high == 1);
  assert(sample.live_bytes == KB(4));
  assert(sample.arena_used_bytes == used_before_scopes);

  sample = registry_find(allocator.registry_slot);
  assert(sample.scope_depth_high == 0);

  vkr_allocator_unregister(&allocator);
  assert(allocator.registry_slot == 0);
  assert(arena->registry_slot == 0);

  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
}

static void test_registry_ignores_unregistered_allocators(void) {
  printf("  Running test_registry_ignores_unregistered_allocators...\n");

  /* The sentinel takes the lowest free slot. A writer that treated slot 0
   * as the first entry would count into it. */
  VkrAllocator sentinel = {.type = VKR_ALLOCATOR_TYPE_UNKNOWN};
  assert(vkr_allocator_register_name(&sentinel, "test.registry.sentinel"));

  uint32_t count_before = registry_sample_all();
  static VkrAllocatorRegistrySample before[VKR_ALLOCATOR_REGISTRY_CAPACITY];
  for (uint32_t i = 0; i < count_before; ++i) {
    before[i] = registry_samples[i];
  }

  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&scope));
  for (uint32_t i = 0; i < 16; ++i) {
    assert(vkr_allocator_alloc(&allocator, KB(16),
                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY) != NULL);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  Arena *raw_arena = arena_create(MB(1), KB(64));
  assert(raw_arena != NULL);
  assert(arena_alloc(raw_arena, KB(128), ARENA_MEMORY_TAG_ARRAY) != NULL);
  arena_clear(raw_arena, ARENA_MEMORY_TAG_ARRAY);

  assert(allocator.registry_slot == 0);
  assert(arena->registry_slot == 0);
  assert(raw_arena->registry_slot == 0);

  uint32_t count_after = registry_sample_all();
  assert(count_after == count_before);
  for (uint32_t i = 0; i < count_after; ++i) {
    const VkrAllocatorRegistrySample *old_sample = &before[i];
    const VkrAllocatorRegistrySample *new_sample = &registry_samples[i];
    assert(new_sample->slot == old_sample->slot);
    assert(new_sample->generation == old_sample->generation);
    assert(new_sample->alloc_count == old_sample->alloc_count);
    assert(new_sample->free_count == old_sample->free_count);
    assert(new_sample->live_bytes == old_sample->live_bytes);
    assert(new_sample->scope_count == old_sample->scope_count);
    assert(new_sample->scope_depth_high <= old_sample->scope_depth_high);
    assert(new_sample->arena_used_bytes == old_sample->arena_used_bytes);
  }

  VkrAllocatorRegistrySample sentinel_sample =
      registry_find(sentinel.registry_slot);
  assert(sentinel_sample.alloc_count == 0);
  assert(sentinel_sample.scope_count == 0);
  assert(sentinel_sample.arena_used_bytes == 0);

  arena_destroy(raw_arena);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  vkr_allocator_unregister(&sentinel);
}

static void test_registry_reused_slot_reports_new_identity(void) {
  printf("  Running test_registry_reused_slot_reports_new_identity...\n");

  VkrDMemory dmemory;
  assert(vkr_dmemory_create(MB(1), MB(2), &dmemory));
  VkrAllocator first = {.ctx = &dmemory};
  vkr_dmemory_allocator_create(&first);

  const char *first_name = "test.registry.reuse.first.with.a.long.name";
  assert(vkr_allocator_register_name(&first, first_name));
  void *block =
      vkr_allocator_alloc(&first, KB(1), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert(block != NULL);
  vkr_allocator_free(&first, block, KB(1), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  uint32_t slot = first.registry_slot;
  VkrAllocatorRegistrySample first_sample = registry_find(slot);
  assert(first_sample.alloc_count == 1);
  vkr_allocator_unregister(&first);

  // Registration takes the lowest free slot, which the first entry released.
  VkrAllocator second = {.type = VKR_ALLOCATOR_TYPE_UNKNOWN};
  assert(vkr_allocator_register_name(&second, "b"));
  assert(second.registry_slot == slot);

  VkrAllocatorRegistrySample second_sample = registry_find(slot);
  assert(second_sample.generation != first_sample.generation);
  assert(registry_name_equals(second_sample.name, "b"));
  assert(second_sample.type == VKR_ALLOCATOR_TYPE_UNKNOWN);
  assert(second_sample.alloc_count == 0);
  assert(second_sample.free_count == 0);

  uint32_t count = registry_sample_all();
  for (uint32_t i = 0; i < count; ++i) {
    assert(!registry_name_equals(registry_samples[i].name, first_name));
  }

  // A stale handle for the reused slot must not release the new entry.
  first.registry_slot = slot;
  vkr_allocator_unregister(&first);
  assert(first.registry_slot == 0);
  assert(registry_contains(slot, second_sample.generation));

  vkr_allocator_unregister(&second);
  assert(!registry_contains(slot, second_sample.generation));
  vkr_dmemory_allocator_destroy(&first);
}

static void test_registry_reports_capacity_overflow(void) {
  printf("  Running test_registry_reports_capacity_overflow...\n");

  uint32_t live_before = registry_sample_all();
  uint32_t rejected_before = vkr_allocator_registry_rejected_count();

  uint32_t registered = 0;
  uint32_t rejected = 0;
  for (uint32_t i = 0; i < ArrayCount(overflow_allocators); ++i) {
    overflow_allocators[i] = (VkrAllocator){.type = VKR_ALLOCATOR_TYPE_UNKNOWN};
    if (vkr_allocator_register_name(&overflow_allocators[i],
                                    "test.registry.fill")) {
      registered++;
    } else {
      assert(overflow_allocators[i].registry_slot == 0);
      rejected++;
    }
  }

  assert(registered == VKR_ALLOCATOR_REGISTRY_CAPACITY - live_before);
  assert(rejected == ArrayCount(overflow_allocators) - registered);
  assert(rejected >= 1);
  assert(vkr_allocator_registry_rejected_count() == rejected_before + rejected);

  // A truncated sample reports every live entry and writes only capacity.
  registry_samples[2] = (VkrAllocatorRegistrySample){.slot = UINT32_MAX};
  uint32_t found = vkr_allocator_registry_sample(registry_samples, 2);
  assert(found == VKR_ALLOCATOR_REGISTRY_CAPACITY);
  assert(registry_samples[2].slot == UINT32_MAX);
  assert(vkr_allocator_registry_sample(NULL, 0) ==
         VKR_ALLOCATOR_REGISTRY_CAPACITY);

  for (uint32_t i = 0; i < ArrayCount(overflow_allocators); ++i) {
    vkr_allocator_unregister(&overflow_allocators[i]);
  }
  assert(registry_sample_all() == live_before);
}

static void test_registry_raw_arena(void) {
  printf("  Running test_registry_raw_arena...\n");

  Arena *arena = arena_create(MB(1), KB(64));
  assert(arena != NULL);
  assert(vkr_allocator_register_arena_name(arena, "test.registry.raw"));
  uint32_t slot = arena->registry_slot;
  assert(slot != 0);

  assert(arena_alloc(arena, KB(1), ARENA_MEMORY_TAG_ARRAY) != NULL);
  assert(arena_alloc(arena, KB(2), ARENA_MEMORY_TAG_ARRAY) != NULL);

  VkrAllocatorRegistrySample sample = registry_find(slot);
  assert(sample.owner == VKR_ALLOCATOR_REGISTRY_OWNER_ARENA);
  assert(sample.type == VKR_ALLOCATOR_TYPE_ARENA);
  assert(sample.alloc_count == 2);
  assert(sample.arena_used_bytes >= KB(3));
  assert(sample.live_bytes == sample.arena_used_bytes);
  uint64_t used_before_clear = sample.arena_used_bytes;

  arena_clear(arena, ARENA_MEMORY_TAG_ARRAY);
  sample = registry_find(slot);
  assert(sample.arena_used_bytes == 0);
  assert(sample.live_bytes == 0);
  assert(sample.arena_interval_peak_used_bytes == used_before_clear);
  assert(sample.arena_peak_used_bytes == used_before_clear);

  // arena_destroy releases a raw arena's slot.
  uint32_t generation = sample.generation;
  arena_destroy(arena);
  assert(!registry_contains(slot, generation));
}

bool32_t run_allocator_registry_tests(void) {
  printf("--- Starting Allocator Registry Tests ---\n");

  test_registry_counts_registered_dmemory();
  test_registry_arena_scopes_and_footprint();
  test_registry_ignores_unregistered_allocators();
  test_registry_reused_slot_reports_new_identity();
  test_registry_reports_capacity_overflow();
  test_registry_raw_arena();

  printf("--- Allocator Registry Tests Completed ---\n");
  return true_v;
}

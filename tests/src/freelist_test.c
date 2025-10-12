#include "freelist_test.h"
#include <stdlib.h>

////////////////////////
//// Freelist Tests ////
////////////////////////

static void test_freelist_create(void) {
  printf("  Running test_freelist_create...\n");

  VkrFreeList freelist;
  const uint64_t TOTAL_SIZE = 1024;

  // Allocate memory for freelist nodes
  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);

  assert(vkr_freelist_create(memory, mem_size, TOTAL_SIZE, &freelist));

  assert(freelist.total_size == TOTAL_SIZE);
  assert(freelist.head != NULL);
  assert(freelist.nodes != NULL);
  assert(freelist.head == &freelist.nodes[0]);
  assert(freelist.head->offset == 0);
  assert(freelist.head->size == TOTAL_SIZE);
  assert(vkr_freelist_free_space(&freelist) == TOTAL_SIZE);

  vkr_freelist_destroy(&freelist);
  free(memory);
  printf("  test_freelist_create PASSED\n");
}

static void test_allocate_exact_and_refill(void) {
  printf("  Running test_allocate_exact_and_refill...\n");

  VkrFreeList freelist;
  const uint64_t TOTAL_SIZE = 1024;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL_SIZE, &freelist));

  uint64_t offset = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, TOTAL_SIZE, &offset));
  assert(offset == 0);
  assert(vkr_freelist_free_space(&freelist) == 0);
  // Next allocation should fail
  uint64_t off2 = VKR_INVALID_ID;
  assert(!vkr_freelist_allocate(&freelist, 1, &off2));

  // Free and allocate again
  assert(vkr_freelist_free(&freelist, TOTAL_SIZE, 0));
  assert(vkr_freelist_free_space(&freelist) == TOTAL_SIZE);
  assert(vkr_freelist_allocate(&freelist, TOTAL_SIZE, &offset));
  assert(offset == 0);

  vkr_freelist_destroy(&freelist);
  free(memory);
  printf("  test_allocate_exact_and_refill PASSED\n");
}

static void test_allocate_split_then_coalesce(void) {
  printf("  Running test_allocate_split_then_coalesce...\n");

  VkrFreeList freelist;
  const uint64_t TOTAL_SIZE = 1024;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL_SIZE, &freelist));

  uint64_t o1 = VKR_INVALID_ID;
  uint64_t o2 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 200, &o1));
  assert(o1 == 0);
  assert(vkr_freelist_free_space(&freelist) == TOTAL_SIZE - 200);

  assert(vkr_freelist_allocate(&freelist, 100, &o2));
  assert(o2 == 200);
  assert(vkr_freelist_free_space(&freelist) == TOTAL_SIZE - 300);

  // Free the middle block and ensure it merges forward
  assert(vkr_freelist_free(&freelist, 100, 200));
  // Now free the first block and ensure full coalesce
  assert(vkr_freelist_free(&freelist, 200, 0));
  assert(vkr_freelist_free_space(&freelist) == TOTAL_SIZE);
  assert(freelist.head->offset == 0);
  assert(freelist.head->size == TOTAL_SIZE);

  vkr_freelist_destroy(&freelist);
  free(memory);
  printf("  test_allocate_split_then_coalesce PASSED\n");
}

static void test_multiple_alloc_free_patterns(void) {
  printf("  Running test_multiple_alloc_free_patterns...\n");

  VkrFreeList fr;
  const uint64_t TOTAL = 2048;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL, &fr));

  uint64_t a = VKR_INVALID_ID, b = VKR_INVALID_ID, c = VKR_INVALID_ID,
           d = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&fr, 256, &a)); // [0..256)
  assert(vkr_freelist_allocate(&fr, 512, &b)); // [256..768)
  assert(vkr_freelist_allocate(&fr, 128, &c)); // [768..896)
  assert(vkr_freelist_allocate(&fr, 256, &d)); // [896..1152)
  assert(vkr_freelist_free_space(&fr) == TOTAL - (256 + 512 + 128 + 256));

  // Free in non-adjacent order and ensure proper coalescing when possible
  assert(vkr_freelist_free(&fr, 256, d)); // Free [896..1152)
  assert(vkr_freelist_free(&fr, 256, a)); // Free [0..256)
  // Now free middle chunks to trigger multi-block coalesce
  assert(vkr_freelist_free(&fr, 512, b)); // Free [256..768)
  assert(vkr_freelist_free(&fr, 128, c)); // Free [768..896)

  assert(vkr_freelist_free_space(&fr) == TOTAL);
  assert(fr.head->offset == 0 && fr.head->size == TOTAL);

  vkr_freelist_destroy(&fr);
  free(memory);
  printf("  test_multiple_alloc_free_patterns PASSED\n");
}

static void test_double_free_detection(void) {
  printf("  Running test_double_free_detection...\n");

  VkrFreeList fr;
  const uint64_t TOTAL = 1024;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL, &fr));

  uint64_t off = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&fr, 128, &off)); // [0..128)
  assert(vkr_freelist_free(&fr, 128, off));
  // Double free of the same range should be rejected
  assert(!vkr_freelist_free(&fr, 128, off));

  vkr_freelist_destroy(&fr);
  free(memory);
  printf("  test_double_free_detection PASSED\n");
}

static void test_clear_resets_to_single_block(void) {
  printf("  Running test_clear_resets_to_single_block...\n");

  VkrFreeList fr;
  const uint64_t TOTAL = 4096;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL, &fr));

  uint64_t o1 = VKR_INVALID_ID, o2 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&fr, 512, &o1));
  assert(vkr_freelist_allocate(&fr, 512, &o2));
  assert(vkr_freelist_free_space(&fr) == TOTAL - 1024);

  vkr_freelist_clear(&fr);
  assert(fr.head != NULL);
  assert(fr.head->offset == 0);
  assert(fr.head->size == TOTAL);
  assert(vkr_freelist_free_space(&fr) == TOTAL);

  vkr_freelist_destroy(&fr);
  free(memory);
  printf("  test_clear_resets_to_single_block PASSED\n");
}

static void test_insert_into_empty_list(void) {
  printf("  Running test_insert_into_empty_list...\n");

  VkrFreeList fr;
  const uint64_t TOTAL = 256;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(TOTAL);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, TOTAL, &fr));

  uint64_t off = VKR_INVALID_ID;
  // Consume entire range
  assert(vkr_freelist_allocate(&fr, TOTAL, &off));
  assert(vkr_freelist_free_space(&fr) == 0);

  // Insert a block into an empty free list
  assert(vkr_freelist_free(&fr, 64, 0));
  assert(fr.head != NULL);
  assert(fr.head->offset == 0);
  assert(fr.head->size == 64);
  assert(vkr_freelist_free_space(&fr) == 64);

  vkr_freelist_destroy(&fr);
  free(memory);
  printf("  test_insert_into_empty_list PASSED\n");
}

static void test_freelist_resize_empty(void) {
  printf("  Running test_freelist_resize_empty...\n");

  VkrFreeList freelist;
  const uint64_t INITIAL_SIZE = 1024;
  const uint64_t NEW_SIZE = 2048;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(INITIAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, INITIAL_SIZE, &freelist));

  // Resize with no allocations
  uint64_t new_mem_size = vkr_freelist_calculate_memory_requirement(NEW_SIZE);
  void *new_memory = malloc(new_mem_size);
  assert(new_memory != NULL);

  void *old_memory = NULL;
  assert(vkr_freelist_resize(&freelist, NEW_SIZE, new_memory, &old_memory));
  assert(old_memory == memory);

  // Verify new size
  assert(freelist.total_size == NEW_SIZE);
  assert(vkr_freelist_free_space(&freelist) == NEW_SIZE);

  // Allocate from new space
  uint64_t offset = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 1500, &offset));
  assert(offset == 0);

  vkr_freelist_destroy(&freelist);
  free(new_memory);
  free(old_memory);
  printf("  test_freelist_resize_empty PASSED\n");
}

static void test_freelist_resize_with_allocations(void) {
  printf("  Running test_freelist_resize_with_allocations...\n");

  VkrFreeList freelist;
  const uint64_t INITIAL_SIZE = 1024;
  const uint64_t NEW_SIZE = 2048;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(INITIAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, INITIAL_SIZE, &freelist));

  // Make some allocations
  uint64_t off1 = VKR_INVALID_ID, off2 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 256, &off1));
  assert(vkr_freelist_allocate(&freelist, 128, &off2));
  assert(off1 == 0);
  assert(off2 == 256);

  uint64_t free_before = vkr_freelist_free_space(&freelist);

  // Resize
  uint64_t new_mem_size = vkr_freelist_calculate_memory_requirement(NEW_SIZE);
  void *new_memory = malloc(new_mem_size);
  assert(new_memory != NULL);

  void *old_memory = NULL;
  assert(vkr_freelist_resize(&freelist, NEW_SIZE, new_memory, &old_memory));

  // Verify allocations are preserved (free space should have grown by NEW_SIZE
  // - INITIAL_SIZE)
  uint64_t free_after = vkr_freelist_free_space(&freelist);
  assert(free_after == free_before + (NEW_SIZE - INITIAL_SIZE));

  // We should be able to free the old allocations at same offsets
  assert(vkr_freelist_free(&freelist, 256, off1));
  assert(vkr_freelist_free(&freelist, 128, off2));

  assert(vkr_freelist_free_space(&freelist) == NEW_SIZE);

  vkr_freelist_destroy(&freelist);
  free(new_memory);
  free(old_memory);
  printf("  test_freelist_resize_with_allocations PASSED\n");
}

static void test_freelist_resize_and_allocate_new(void) {
  printf("  Running test_freelist_resize_and_allocate_new...\n");

  VkrFreeList freelist;
  const uint64_t INITIAL_SIZE = 512;
  const uint64_t NEW_SIZE = 1024;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(INITIAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, INITIAL_SIZE, &freelist));

  // Fill initial space
  uint64_t off1 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, INITIAL_SIZE, &off1));

  // Resize
  uint64_t new_mem_size = vkr_freelist_calculate_memory_requirement(NEW_SIZE);
  void *new_memory = malloc(new_mem_size);
  assert(new_memory != NULL);

  void *old_memory = NULL;
  assert(vkr_freelist_resize(&freelist, NEW_SIZE, new_memory, &old_memory));

  // Now allocate from the new space
  uint64_t off2 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 256, &off2));
  assert(off2 == INITIAL_SIZE); // Should be at the start of new space

  vkr_freelist_destroy(&freelist);
  free(new_memory);
  free(old_memory);
  printf("  test_freelist_resize_and_allocate_new PASSED\n");
}

static void test_freelist_resize_coalescing(void) {
  printf("  Running test_freelist_resize_coalescing...\n");

  VkrFreeList freelist;
  const uint64_t INITIAL_SIZE = 1024;
  const uint64_t NEW_SIZE = 2048;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(INITIAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, INITIAL_SIZE, &freelist));

  // Allocate from beginning, leaving tail free
  uint64_t off1 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 512, &off1));

  // Resize - the new space should coalesce with the free tail
  uint64_t new_mem_size = vkr_freelist_calculate_memory_requirement(NEW_SIZE);
  void *new_memory = malloc(new_mem_size);
  assert(new_memory != NULL);

  void *old_memory = NULL;
  assert(vkr_freelist_resize(&freelist, NEW_SIZE, new_memory, &old_memory));

  // Should be able to allocate a large block from the coalesced free space
  uint64_t off2 = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 1024, &off2));
  assert(off2 == 512); // Should start where first allocation ended

  vkr_freelist_destroy(&freelist);
  free(new_memory);
  free(old_memory);
  printf("  test_freelist_resize_coalescing PASSED\n");
}

static void test_freelist_resize_node_copy(void) {
  printf("  Running test_freelist_resize_node_copy...\n");

  VkrFreeList freelist;
  const uint64_t INITIAL_SIZE = 2048;
  const uint64_t NEW_SIZE = 4096;

  uint64_t mem_size = vkr_freelist_calculate_memory_requirement(INITIAL_SIZE);
  void *memory = malloc(mem_size);
  assert(memory != NULL);
  assert(vkr_freelist_create(memory, mem_size, INITIAL_SIZE, &freelist));

  // Create a fragmented allocation pattern
  uint64_t offsets[5];
  assert(vkr_freelist_allocate(&freelist, 256, &offsets[0]));
  assert(vkr_freelist_allocate(&freelist, 256, &offsets[1]));
  assert(vkr_freelist_allocate(&freelist, 256, &offsets[2]));
  assert(vkr_freelist_allocate(&freelist, 256, &offsets[3]));
  assert(vkr_freelist_allocate(&freelist, 256, &offsets[4]));

  // Free every other allocation to create fragmentation
  assert(vkr_freelist_free(&freelist, 256, offsets[1]));
  assert(vkr_freelist_free(&freelist, 256, offsets[3]));

  uint64_t free_before = vkr_freelist_free_space(&freelist);

  // Resize
  uint64_t new_mem_size = vkr_freelist_calculate_memory_requirement(NEW_SIZE);
  void *new_memory = malloc(new_mem_size);
  assert(new_memory != NULL);

  void *old_memory = NULL;
  assert(vkr_freelist_resize(&freelist, NEW_SIZE, new_memory, &old_memory));

  // Free space should be previous free + growth
  uint64_t free_after = vkr_freelist_free_space(&freelist);
  assert(free_after == free_before + (NEW_SIZE - INITIAL_SIZE));

  // All operations should still work
  uint64_t new_off = VKR_INVALID_ID;
  assert(vkr_freelist_allocate(&freelist, 200, &new_off));
  assert(vkr_freelist_free(&freelist, 256, offsets[0]));
  assert(vkr_freelist_free(&freelist, 256, offsets[2]));
  assert(vkr_freelist_free(&freelist, 256, offsets[4]));
  assert(vkr_freelist_free(&freelist, 200, new_off));

  assert(vkr_freelist_free_space(&freelist) == NEW_SIZE);

  vkr_freelist_destroy(&freelist);
  free(new_memory);
  free(old_memory);
  printf("  test_freelist_resize_node_copy PASSED\n");
}

bool32_t run_freelist_tests() {
  printf("--- Starting Freelist Tests ---\n");

  test_freelist_create();
  test_allocate_exact_and_refill();
  test_allocate_split_then_coalesce();
  test_multiple_alloc_free_patterns();
  test_double_free_detection();
  test_clear_resets_to_single_block();
  test_insert_into_empty_list();

  // Resize tests
  test_freelist_resize_empty();
  test_freelist_resize_with_allocations();
  test_freelist_resize_and_allocate_new();
  test_freelist_resize_coalescing();
  test_freelist_resize_node_copy();

  printf("--- Freelist Tests Completed ---\n");
  return true;
}
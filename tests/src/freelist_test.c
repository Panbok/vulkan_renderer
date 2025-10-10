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

bool32_t run_freelist_tests() {
  printf("--- Starting Freelist Tests ---\n");

  test_freelist_create();
  test_allocate_exact_and_refill();
  test_allocate_split_then_coalesce();
  test_multiple_alloc_free_patterns();
  test_double_free_detection();
  test_clear_resets_to_single_block();
  test_insert_into_empty_list();

  printf("--- Freelist Tests Completed ---\n");
  return true;
}
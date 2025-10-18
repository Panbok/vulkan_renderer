#include "dmemory_test.h"
#include <stdlib.h>
#include <string.h>

/////////////////////////
//// DMemory Tests /////
/////////////////////////

static void test_dmemory_create(void) {
  printf("  Running test_dmemory_create...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Verify initialization
  assert(dmemory.base_memory != NULL);
  assert(dmemory.total_size >= TOTAL_SIZE); // May be aligned up
  assert(dmemory.page_size > 0);
  assert(dmemory.freelist_memory != NULL);
  assert(vkr_dmemory_get_free_space(&dmemory) == dmemory.total_size);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_create PASSED\n");
}

static void test_dmemory_alloc_basic(void) {
  printf("  Running test_dmemory_alloc_basic...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate a simple block
  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1024);
  assert(ptr1 != NULL);
  assert(ptr1 >= dmemory.base_memory);
  assert(ptr1 < (void *)((uint8_t *)dmemory.base_memory + dmemory.total_size));

  // Write to the allocated memory
  memset(ptr1, 0xAB, 1024);

  // Verify we can read it back
  uint8_t *data = (uint8_t *)ptr1;
  for (uint32_t i = 0; i < 1024; i++) {
    assert(data[i] == 0xAB);
  }

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_alloc_basic PASSED\n");
}

static void test_dmemory_multiple_allocs(void) {
  printf("  Running test_dmemory_multiple_allocs...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  uint64_t initial_free = vkr_dmemory_get_free_space(&dmemory);

  // Allocate multiple blocks
  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1024);
  void *ptr2 = vkr_dmemory_alloc(&dmemory, 2048);
  void *ptr3 = vkr_dmemory_alloc(&dmemory, 512);

  assert(ptr1 != NULL);
  assert(ptr2 != NULL);
  assert(ptr3 != NULL);

  // Ensure they don't overlap
  assert(ptr1 != ptr2);
  assert(ptr1 != ptr3);
  assert(ptr2 != ptr3);

  // Free space should have decreased
  uint64_t current_free = vkr_dmemory_get_free_space(&dmemory);
  assert(current_free < initial_free);

  // Write different patterns to each block
  memset(ptr1, 0x11, 1024);
  memset(ptr2, 0x22, 2048);
  memset(ptr3, 0x33, 512);

  // Verify patterns
  uint8_t *data1 = (uint8_t *)ptr1;
  uint8_t *data2 = (uint8_t *)ptr2;
  uint8_t *data3 = (uint8_t *)ptr3;

  for (uint32_t i = 0; i < 1024; i++)
    assert(data1[i] == 0x11);
  for (uint32_t i = 0; i < 2048; i++)
    assert(data2[i] == 0x22);
  for (uint32_t i = 0; i < 512; i++)
    assert(data3[i] == 0x33);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_multiple_allocs PASSED\n");
}

static void test_dmemory_free_and_realloc(void) {
  printf("  Running test_dmemory_free_and_realloc...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate some blocks
  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1024);
  void *ptr2 = vkr_dmemory_alloc(&dmemory, 2048);
  void *ptr3 = vkr_dmemory_alloc(&dmemory, 512);

  assert(ptr1 != NULL && ptr2 != NULL && ptr3 != NULL);

  uint64_t free_before_free = vkr_dmemory_get_free_space(&dmemory);

  // Free the middle block
  assert(vkr_dmemory_free(&dmemory, ptr2, 2048));

  uint64_t free_after_free = vkr_dmemory_get_free_space(&dmemory);
  assert(free_after_free > free_before_free);

  // Allocate a smaller block - should reuse freed space
  void *ptr4 = vkr_dmemory_alloc(&dmemory, 1024);
  assert(ptr4 != NULL);

  // Free all remaining blocks
  assert(vkr_dmemory_free(&dmemory, ptr1, 1024));
  assert(vkr_dmemory_free(&dmemory, ptr3, 512));
  assert(vkr_dmemory_free(&dmemory, ptr4, 1024));

  // Should have all space back
  uint64_t final_free = vkr_dmemory_get_free_space(&dmemory);
  assert(final_free == dmemory.total_size);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_free_and_realloc PASSED\n");
}

static void test_dmemory_out_of_memory(void) {
  printf("  Running test_dmemory_out_of_memory...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = KB(64); // Small size for testing
  const uint64_t RESERVE_SIZE = MB(1);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate almost all memory
  void *ptr1 = vkr_dmemory_alloc(&dmemory, KB(32));
  void *ptr2 = vkr_dmemory_alloc(&dmemory, KB(16));

  assert(ptr1 != NULL);
  assert(ptr2 != NULL);

  // Try to allocate more than available - should fail
  void *ptr3 = vkr_dmemory_alloc(&dmemory, KB(32));
  assert(ptr3 == NULL);

  // Free some space
  assert(vkr_dmemory_free(&dmemory, ptr2, KB(16)));

  // Now we should be able to allocate smaller blocks
  void *ptr4 = vkr_dmemory_alloc(&dmemory, KB(8));
  assert(ptr4 != NULL);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_out_of_memory PASSED\n");
}

static void test_dmemory_upfront_commit(void) {
  printf("  Running test_dmemory_upfront_commit...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // All memory should be committed upfront
  assert(dmemory.committed_size == dmemory.total_size);
  assert(dmemory.committed_size >= TOTAL_SIZE);

  uint64_t initial_committed = dmemory.committed_size;

  // Allocate blocks - committed size should stay the same (no syscalls)
  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1024);
  assert(ptr1 != NULL);
  assert(dmemory.committed_size == initial_committed);

  void *ptr2 = vkr_dmemory_alloc(&dmemory, KB(64));
  assert(ptr2 != NULL);
  assert(dmemory.committed_size == initial_committed);

  void *ptr3 = vkr_dmemory_alloc(&dmemory, KB(128));
  assert(ptr3 != NULL);
  assert(dmemory.committed_size == initial_committed);

  // Free blocks - committed size should still stay the same
  vkr_dmemory_free(&dmemory, ptr1, 1024);
  vkr_dmemory_free(&dmemory, ptr2, KB(64));
  vkr_dmemory_free(&dmemory, ptr3, KB(128));
  assert(dmemory.committed_size == initial_committed);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_upfront_commit PASSED\n");
}

static void test_dmemory_free_pattern(void) {
  printf("  Running test_dmemory_free_pattern...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate several blocks
  void *ptrs[5];
  uint32_t sizes[5] = {1024, 2048, 512, 4096, 256};

  for (int i = 0; i < 5; i++) {
    ptrs[i] = vkr_dmemory_alloc(&dmemory, sizes[i]);
    assert(ptrs[i] != NULL);
  }

  uint64_t free_after_allocs = vkr_dmemory_get_free_space(&dmemory);

  // Free in non-sequential order
  assert(vkr_dmemory_free(&dmemory, ptrs[2], sizes[2])); // Free middle
  assert(vkr_dmemory_free(&dmemory, ptrs[0], sizes[0])); // Free first
  assert(vkr_dmemory_free(&dmemory, ptrs[4], sizes[4])); // Free last
  assert(vkr_dmemory_free(&dmemory, ptrs[1], sizes[1])); // Free second
  assert(vkr_dmemory_free(&dmemory, ptrs[3], sizes[3])); // Free remaining

  // All memory should be free now
  uint64_t final_free = vkr_dmemory_get_free_space(&dmemory);
  assert(final_free == dmemory.total_size);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_free_pattern PASSED\n");
}

static void test_dmemory_invalid_free(void) {
  printf("  Running test_dmemory_invalid_free...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1024);
  assert(ptr1 != NULL);

  // Try to free with invalid pointer (outside range)
  void *invalid_ptr = (void *)0x12345678;
  assert(!vkr_dmemory_free(&dmemory, invalid_ptr, 1024));

  // Free the valid pointer
  assert(vkr_dmemory_free(&dmemory, ptr1, 1024));

  // Try double free - should fail
  assert(!vkr_dmemory_free(&dmemory, ptr1, 1024));

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_invalid_free PASSED\n");
}

static void test_dmemory_fragmentation(void) {
  printf("  Running test_dmemory_fragmentation...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = KB(256);
  const uint64_t RESERVE_SIZE = MB(5);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Create a fragmented memory pattern
  void *ptrs[10];
  for (int i = 0; i < 10; i++) {
    ptrs[i] = vkr_dmemory_alloc(&dmemory, KB(8));
    assert(ptrs[i] != NULL);
  }

  // Free every other block
  for (int i = 0; i < 10; i += 2) {
    assert(vkr_dmemory_free(&dmemory, ptrs[i], KB(8)));
  }

  // Now we have fragmented free space
  uint64_t free_space = vkr_dmemory_get_free_space(&dmemory);
  assert(free_space > 0);

  // Try to allocate small blocks that should fit in gaps
  void *new_ptr1 = vkr_dmemory_alloc(&dmemory, KB(4));
  void *new_ptr2 = vkr_dmemory_alloc(&dmemory, KB(4));
  assert(new_ptr1 != NULL);
  assert(new_ptr2 != NULL);

  // Clean up remaining blocks
  for (int i = 1; i < 10; i += 2) {
    vkr_dmemory_free(&dmemory, ptrs[i], KB(8));
  }
  vkr_dmemory_free(&dmemory, new_ptr1, KB(4));
  vkr_dmemory_free(&dmemory, new_ptr2, KB(4));

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_fragmentation PASSED\n");
}

static void test_dmemory_boundary_conditions(void) {
  printf("  Running test_dmemory_boundary_conditions...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = KB(64);
  const uint64_t RESERVE_SIZE = MB(1);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Test allocation at start
  void *ptr1 = vkr_dmemory_alloc(&dmemory, 1);
  assert(ptr1 != NULL);

  // Test maximum possible allocation
  uint64_t remaining = vkr_dmemory_get_free_space(&dmemory);
  void *ptr2 = vkr_dmemory_alloc(&dmemory, (uint32_t)remaining);
  assert(ptr2 != NULL);

  // Should be out of memory now
  void *ptr3 = vkr_dmemory_alloc(&dmemory, 1);
  assert(ptr3 == NULL);

  // Free and verify
  vkr_dmemory_free(&dmemory, ptr1, 1);
  vkr_dmemory_free(&dmemory, ptr2, (uint32_t)remaining);

  assert(vkr_dmemory_get_free_space(&dmemory) == dmemory.total_size);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_boundary_conditions PASSED\n");
}

static void test_dmemory_write_read_integrity(void) {
  printf("  Running test_dmemory_write_read_integrity...\n");

  VkrDMemory dmemory;
  const uint64_t TOTAL_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(TOTAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate and write different patterns
  struct TestData {
    void *ptr;
    uint32_t size;
    uint8_t pattern;
  };

  struct TestData blocks[5] = {{NULL, 1024, 0xAA},
                               {NULL, 2048, 0xBB},
                               {NULL, 512, 0xCC},
                               {NULL, 4096, 0xDD},
                               {NULL, 256, 0xEE}};

  // Allocate and write
  for (int i = 0; i < 5; i++) {
    blocks[i].ptr = vkr_dmemory_alloc(&dmemory, blocks[i].size);
    assert(blocks[i].ptr != NULL);
    memset(blocks[i].ptr, blocks[i].pattern, blocks[i].size);
  }

  // Verify all blocks still have correct data
  for (int i = 0; i < 5; i++) {
    uint8_t *data = (uint8_t *)blocks[i].ptr;
    for (uint32_t j = 0; j < blocks[i].size; j++) {
      assert(data[j] == blocks[i].pattern);
    }
  }

  // Free some blocks
  vkr_dmemory_free(&dmemory, blocks[1].ptr, blocks[1].size);
  vkr_dmemory_free(&dmemory, blocks[3].ptr, blocks[3].size);

  // Verify remaining blocks still have correct data
  for (int i = 0; i < 5; i++) {
    if (i == 1 || i == 3)
      continue; // Skip freed blocks
    uint8_t *data = (uint8_t *)blocks[i].ptr;
    for (uint32_t j = 0; j < blocks[i].size; j++) {
      assert(data[j] == blocks[i].pattern);
    }
  }

  // Clean up
  vkr_dmemory_free(&dmemory, blocks[0].ptr, blocks[0].size);
  vkr_dmemory_free(&dmemory, blocks[2].ptr, blocks[2].size);
  vkr_dmemory_free(&dmemory, blocks[4].ptr, blocks[4].size);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_write_read_integrity PASSED\n");
}

static void test_dmemory_resize_empty(void) {
  printf("  Running test_dmemory_resize_empty...\n");

  VkrDMemory dmemory;
  const uint64_t INITIAL_SIZE = MB(1);
  const uint64_t NEW_SIZE = MB(2);
  const uint64_t RESERVE_SIZE = MB(5);

  assert(vkr_dmemory_create(INITIAL_SIZE, RESERVE_SIZE, &dmemory));

  // Verify initial state
  assert(vkr_dmemory_get_free_space(&dmemory) == dmemory.total_size);

  // Resize with no allocations
  assert(vkr_dmemory_resize(&dmemory, NEW_SIZE));

  // Verify new size
  assert(dmemory.total_size >= NEW_SIZE);
  assert(vkr_dmemory_get_free_space(&dmemory) == dmemory.total_size);

  // Allocate from new space
  void *ptr = vkr_dmemory_alloc(&dmemory, MB(1) + KB(512));
  assert(ptr != NULL);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_resize_empty PASSED\n");
}

static void test_dmemory_resize_with_allocations(void) {
  printf("  Running test_dmemory_resize_with_allocations...\n");

  VkrDMemory dmemory;
  const uint64_t INITIAL_SIZE = MB(1);
  const uint64_t NEW_SIZE = MB(2);
  const uint64_t RESERVE_SIZE = MB(5);

  assert(vkr_dmemory_create(INITIAL_SIZE, RESERVE_SIZE, &dmemory));

  // Make some allocations and write data
  void *ptr1 = vkr_dmemory_alloc(&dmemory, KB(64));
  void *ptr2 = vkr_dmemory_alloc(&dmemory, KB(128));
  assert(ptr1 != NULL && ptr2 != NULL);

  // Write patterns
  memset(ptr1, 0xAA, KB(64));
  memset(ptr2, 0xBB, KB(128));

  uint64_t free_before = vkr_dmemory_get_free_space(&dmemory);

  // Resize
  assert(vkr_dmemory_resize(&dmemory, NEW_SIZE));

  // With sparse buffers, pointers remain valid after resize!
  uint8_t *data1 = (uint8_t *)ptr1;
  uint8_t *data2 = (uint8_t *)ptr2;

  // Verify data is preserved
  for (uint32_t i = 0; i < KB(64); i++) {
    assert(data1[i] == 0xAA);
  }
  for (uint32_t i = 0; i < KB(128); i++) {
    assert(data2[i] == 0xBB);
  }

  // Verify free space increased appropriately
  uint64_t free_after = vkr_dmemory_get_free_space(&dmemory);
  uint64_t expected_growth = dmemory.total_size - INITIAL_SIZE;

  // Free space should have grown by approximately the size increase
  assert(free_after >= free_before + expected_growth - KB(64) - KB(128));

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_resize_with_allocations PASSED\n");
}

static void test_dmemory_resize_and_allocate(void) {
  printf("  Running test_dmemory_resize_and_allocate...\n");

  VkrDMemory dmemory;
  const uint64_t INITIAL_SIZE = KB(512);
  const uint64_t NEW_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(5);

  assert(vkr_dmemory_create(INITIAL_SIZE, RESERVE_SIZE, &dmemory));

  // Fill initial space
  void *ptr1 = vkr_dmemory_alloc(&dmemory, KB(512));
  assert(ptr1 != NULL);

  // Resize
  assert(vkr_dmemory_resize(&dmemory, NEW_SIZE));

  // Now allocate from new space
  void *ptr2 = vkr_dmemory_alloc(&dmemory, KB(256));
  assert(ptr2 != NULL);

  // Write and verify
  memset(ptr2, 0xCC, KB(256));
  uint8_t *data = (uint8_t *)ptr2;
  for (uint32_t i = 0; i < KB(256); i++) {
    assert(data[i] == 0xCC);
  }

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_resize_and_allocate PASSED\n");
}

static void test_dmemory_resize_shrink_rejected(void) {
  printf("  Running test_dmemory_resize_shrink_rejected...\n");

  VkrDMemory dmemory;
  const uint64_t INITIAL_SIZE = MB(2);
  const uint64_t NEW_SIZE = MB(1);
  const uint64_t RESERVE_SIZE = MB(10);

  assert(vkr_dmemory_create(INITIAL_SIZE, RESERVE_SIZE, &dmemory));

  // Allocate more than NEW_SIZE
  void *ptr1 = vkr_dmemory_alloc(&dmemory, MB(1) + KB(256));
  assert(ptr1 != NULL);

  // Try to resize to smaller size - should fail
  assert(!vkr_dmemory_resize(&dmemory, NEW_SIZE));

  // Original size should be unchanged
  assert(dmemory.total_size >= INITIAL_SIZE);

  vkr_dmemory_destroy(&dmemory);
  printf("  test_dmemory_resize_shrink_rejected PASSED\n");
}

bool32_t run_dmemory_tests(void) {
  printf("--- Starting DMemory Tests ---\n");

  test_dmemory_create();
  test_dmemory_alloc_basic();
  test_dmemory_multiple_allocs();
  test_dmemory_free_and_realloc();
  test_dmemory_out_of_memory();
  test_dmemory_upfront_commit();
  test_dmemory_free_pattern();
  test_dmemory_invalid_free();
  test_dmemory_fragmentation();
  test_dmemory_boundary_conditions();
  test_dmemory_write_read_integrity();

  // Resize tests
  test_dmemory_resize_empty();
  test_dmemory_resize_with_allocations();
  test_dmemory_resize_and_allocate();
  test_dmemory_resize_shrink_rejected();

  printf("--- DMemory Tests Completed ---\n");
  return true;
}

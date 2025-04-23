#include "arena_test.h" // Removed

// Helper to get the initial position (header size aligned up)
static size_t get_initial_pos() {
  return AlginPow2(sizeof(Arena), AlignOf(void *));
}

static void test_arena_creation() {
  printf("Running test_arena_creation...\n");
  // Use smaller sizes for testing to make calculations easier
  size_t test_rsv = KB(64);
  size_t test_cmt = KB(4);
  Arena *arena = arena_create(test_rsv, test_cmt);

  assert(arena != NULL && "Arena creation failed");
  assert(arena->current == arena && "Initial current pointer incorrect");
  assert(arena->prev == NULL && "Initial prev pointer incorrect");
  assert(arena->rsv >= test_rsv + sizeof(Arena) &&
         "Reserved size too small"); // >= because of alignment
  assert(arena->cmt >= test_cmt + sizeof(Arena) &&
         "Committed size too small"); // >= because of alignment
  assert(arena->pos == get_initial_pos() && "Initial position incorrect");
  assert(arena->base_pos == 0 && "Initial base position incorrect");
  assert(arena->free_last == NULL && "Initial free list incorrect");
  assert(arena->free_size == 0 && "Initial free size incorrect");
  // Check internal default sizes are stored (may differ due to alignment)
  assert(arena->rsv_size >= test_rsv && "Stored rsv_size incorrect");
  assert(arena->cmt_size >= test_cmt && "Stored cmt_size incorrect");

  arena_destroy(arena);
  printf("test_arena_creation PASSED\n");
}

static void test_arena_simple_alloc() {
  printf("Running test_arena_simple_alloc...\n");
  Arena *arena = arena_create();
  size_t initial_pos = arena_pos(arena);
  assert(initial_pos == get_initial_pos() && "Initial pos mismatch");

  size_t alloc_size1 = 100;
  void *ptr1 = arena_alloc(arena, alloc_size1);
  assert(ptr1 != NULL && "Allocation 1 failed");
  size_t expected_pos1 = initial_pos + alloc_size1;
  assert(arena_pos(arena) >= expected_pos1 &&
         "Position after alloc 1 too small"); // >= due to internal alignment
  assert(arena_pos(arena) < expected_pos1 + AlignOf(void *) &&
         "Position after alloc 1 too large");
  assert((uintptr_t)ptr1 % AlignOf(void *) == 0 && "Pointer 1 not aligned");
  memset(ptr1, 0xAA, alloc_size1); // Write to allocated memory

  size_t current_pos = arena_pos(arena);
  size_t alloc_size2 = 200;
  void *ptr2 = arena_alloc(arena, alloc_size2);
  assert(ptr2 != NULL && "Allocation 2 failed");
  size_t expected_pos2 = current_pos + alloc_size2;
  assert(arena_pos(arena) >= expected_pos2 &&
         "Position after alloc 2 too small");
  assert(arena_pos(arena) < expected_pos2 + AlignOf(void *) &&
         "Position after alloc 2 too large");
  assert((uintptr_t)ptr2 % AlignOf(void *) == 0 && "Pointer 2 not aligned");
  memset(ptr2, 0xBB, alloc_size2); // Write to allocated memory

  // Verify data is still there (simple check)
  assert(*(unsigned char *)ptr1 == 0xAA);
  assert(*(unsigned char *)ptr2 == 0xBB);

  arena_destroy(arena);
  printf("test_arena_simple_alloc PASSED\n");
}

static void test_arena_commit_grow() {
  printf("Running test_arena_commit_grow...\n");
  // Force commit growth by allocating slightly more than initial commit
  size_t test_rsv = KB(64);
  size_t test_cmt = KB(4); // Small initial commit
  Arena *arena = arena_create(test_rsv, test_cmt);
  size_t initial_cmt = arena->cmt;
  size_t initial_pos = arena->pos;

  // Allocate slightly more than initial commit (minus header)
  size_t alloc_size = initial_cmt - initial_pos + 10;
  assert(alloc_size > 0);

  void *ptr = arena_alloc(arena, alloc_size);
  assert(ptr != NULL && "Allocation failed");
  assert(arena->cmt > initial_cmt && "Commit size did not grow");
  assert(arena->pos >= initial_pos + alloc_size &&
         "Position did not advance correctly");
  assert(arena->cmt <= arena->rsv && "Commit exceeded reserve");
  memset(ptr, 0xCC, alloc_size); // Write test

  arena_destroy(arena);
  printf("test_arena_commit_grow PASSED\n");
}

static void test_arena_block_grow() {
  printf("Running test_arena_block_grow...\n");
  // Force block growth by allocating more than reserved size
  size_t test_rsv = KB(4); // Very small reserve
  size_t test_cmt = KB(4);
  Arena *arena = arena_create(test_rsv, test_cmt);
  Arena *first_block = arena->current;
  size_t first_block_rsv = first_block->rsv;
  size_t initial_pos = arena->pos;

  // Allocate slightly more than first block's capacity
  size_t alloc_size = first_block_rsv - initial_pos + 10;
  assert(alloc_size > 0);

  void *ptr = arena_alloc(arena, alloc_size);
  assert(ptr != NULL && "Allocation failed");
  assert(arena->current != first_block &&
         "Arena did not switch to a new block");
  assert(arena->current->prev == first_block &&
         "New block's prev pointer incorrect");
  assert(arena->current->base_pos == first_block->base_pos + first_block->rsv &&
         "New block's base_pos incorrect");
  assert(
      arena->current->pos == get_initial_pos() + alloc_size &&
      "Position in new block incorrect"); // Pos is relative to new block start
  assert(arena_pos(arena) == arena->current->base_pos + arena->current->pos &&
         "Absolute position incorrect");

  memset(ptr, 0xDD, alloc_size); // Write test

  arena_destroy(arena);
  printf("test_arena_block_grow PASSED\n");
}

static void test_arena_reset_to() {
  printf("Running test_arena_reset_to...\n");
  Arena *arena = arena_create();
  size_t pos0 = arena_pos(arena);

  void *p1 = arena_alloc(arena, 100);
  size_t pos1 = arena_pos(arena);
  void *p2 = arena_alloc(arena, 200);
  size_t pos2 = arena_pos(arena);

  assert(p1 && p2);
  assert(pos1 > pos0);
  assert(pos2 > pos1);

  // Reset back to after p1
  arena_reset_to(arena, pos1);
  assert(arena_pos(arena) == pos1 && "Position incorrect after reset to pos1");

  // Allocate again, should reuse space after p1
  void *p3 = arena_alloc(arena, 50);
  size_t pos3 = arena_pos(arena);
  assert(p3 != NULL && "Allocation after reset failed");
  // The new pointer p3 might reuse the memory location of p2, or be slightly
  // different due to alignment We mainly check that the position is advanced
  // correctly from the reset point.
  assert(pos3 >= pos1 + 50 && "Position after reset+alloc too small");
  assert(pos3 < pos1 + 50 + AlignOf(void *) &&
         "Position after reset+alloc too large");

  // Reset back to start
  arena_reset_to(arena, pos0);
  assert(arena_pos(arena) == pos0 && "Position incorrect after reset to pos0");

  // Allocate again
  void *p4 = arena_alloc(arena, 75);
  size_t pos4 = arena_pos(arena);
  assert(p4 != NULL && "Allocation after reset to 0 failed");
  assert(pos4 >= pos0 + 75 && "Position after reset0+alloc too small");
  assert(pos4 < pos0 + 75 + AlignOf(void *) &&
         "Position after reset0+alloc too large");

  arena_destroy(arena);
  printf("test_arena_reset_to PASSED\n");
}

static void test_arena_clear() {
  printf("Running test_arena_clear...\n");
  Arena *arena = arena_create();
  size_t initial_pos = arena_pos(arena);

  arena_alloc(arena, 100);
  arena_alloc(arena, 200);
  assert(arena_pos(arena) > initial_pos && "Position didn't advance");

  arena_clear(arena);
  assert(arena_pos(arena) == initial_pos && "Position not reset by clear");

  // Allocate again to ensure it works after clear
  void *ptr = arena_alloc(arena, 50);
  assert(ptr != NULL && "Alloc after clear failed");
  assert(arena_pos(arena) >= initial_pos + 50 &&
         "Position incorrect after clear+alloc");

  arena_destroy(arena);
  printf("test_arena_clear PASSED\n");
}

static void test_arena_scratch() {
  printf("Running test_arena_scratch...\n");
  Arena *arena = arena_create();
  size_t pos0 = arena_pos(arena);

  void *p_before = arena_alloc(arena, 50);
  size_t pos_before = arena_pos(arena);
  assert(p_before);

  // Start scratch 1
  Scratch scratch1 = scratch_create(arena);
  assert(scratch1.arena == arena && "Scratch arena mismatch");
  assert(scratch1.pos == pos_before && "Scratch 1 position incorrect");

  void *p_s1_1 = arena_alloc(arena, 100);
  size_t pos_s1_1 = arena_pos(arena);
  assert(p_s1_1);

  // Start nested scratch 2
  Scratch scratch2 = scratch_create(arena);
  assert(scratch2.arena == arena);
  assert(scratch2.pos == pos_s1_1);

  void *p_s2_1 = arena_alloc(arena, 200);
  size_t pos_s2_1 = arena_pos(arena);
  assert(p_s2_1);

  // End nested scratch 2
  scratch_destroy(scratch2);
  assert(arena_pos(arena) == pos_s1_1 &&
         "Position not reset after scratch 2 destroy");

  void *p_s1_2 = arena_alloc(arena, 75);
  size_t pos_s1_2 = arena_pos(arena);
  assert(p_s1_2);
  assert(pos_s1_2 >= pos_s1_1 + 75 &&
         "Position incorrect after nested scratch");

  // End scratch 1
  scratch_destroy(scratch1);
  assert(arena_pos(arena) == pos_before &&
         "Position not reset after scratch 1 destroy");

  // Allocate again to ensure arena is usable
  void *p_after = arena_alloc(arena, 25);
  assert(p_after);
  assert(arena_pos(arena) >= pos_before + 25 &&
         "Position incorrect after all scratches");

  arena_destroy(arena);
  printf("test_arena_scratch PASSED\n");
}

static void test_arena_alignment() {
  printf("Running test_arena_alignment...\n");
  Arena *arena = arena_create();
  size_t alignment = AlignOf(void *); // Platform default alignment

  // Allocate small sizes to check alignment
  for (int i = 1; i < (int)alignment * 2; ++i) {
    void *ptr = arena_alloc(arena, i);
    assert(ptr != NULL && "Alignment alloc failed");
    assert((uintptr_t)ptr % alignment == 0 && "Pointer not aligned correctly");
    memset(ptr, 0, i); // Touch memory
  }

  // Allocate a larger struct
  typedef struct {
    long double ld; // Often requires 16-byte alignment
    char c;
    int i;
    double d;
  } TestStruct;

  size_t struct_align = AlignOf(TestStruct);
  TestStruct *ts_ptr = (TestStruct *)arena_alloc(arena, sizeof(TestStruct));
  assert(ts_ptr != NULL && "Struct allocation failed");
  assert((uintptr_t)ts_ptr % struct_align == 0 &&
         "Struct pointer not aligned correctly");
  ts_ptr->ld = 1.23L; // Touch memory

  arena_destroy(arena);
  printf("test_arena_alignment PASSED\n");
}

// --- Test Runner ---
bool run_arena_tests() {
  printf("--- Starting Arena Tests ---\n");
  // Add calls to all test functions here
  test_arena_creation();
  test_arena_simple_alloc();
  test_arena_commit_grow();
  test_arena_block_grow();
  test_arena_reset_to();
  test_arena_clear();
  test_arena_scratch();
  test_arena_alignment();
  // Call other tests...

  printf("--- Arena Tests Completed ---\n");
  return true; // Assumes asserts halt on failure
}

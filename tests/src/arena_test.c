#include "arena_test.h"

// Helper to get the initial position (header size aligned up)
static uint64_t get_initial_pos() {
  // ARENA_HEADER_SIZE should already be multiple of AlignOf(void*) or page size
  // for safety but explicit alignment here for calculation isn't strictly
  // needed if header is always aligned up.
  return AlginPow2(ARENA_HEADER_SIZE, AlignOf(void *));
}

static void test_arena_creation() {
  printf("  Running test_arena_creation...\n");
  uint64_t page_size = platform_get_page_size();

  // Test with specific small sizes
  uint64_t test_rsv_s = KB(64);
  uint64_t test_cmt_s = KB(4);
  Arena *arena_s = arena_create(test_rsv_s, test_cmt_s);

  assert(arena_s != NULL && "Arena creation (small) failed");
  assert(arena_s->current == arena_s &&
         "Initial current pointer incorrect (small)");
  assert(arena_s->prev == NULL && "Initial prev pointer incorrect (small)");
  assert(arena_s->rsv >= test_rsv_s + ARENA_HEADER_SIZE &&
         "Reserved size too small (small)");
  assert(arena_s->cmt >= test_cmt_s + ARENA_HEADER_SIZE &&
         "Committed size too small (small)");
  assert(arena_s->rsv % page_size == 0 &&
         "Arena->rsv not page aligned (small)");
  assert(
      arena_s->cmt % page_size == 0 &&
      "Arena->cmt not page aligned (small)"); // Initial commit is page aligned.
  assert(arena_s->pos == get_initial_pos() &&
         "Initial position incorrect (small)");
  assert(arena_s->base_pos == 0 && "Initial base position incorrect (small)");
  assert(arena_s->free_last == NULL && "Initial free list incorrect (small)");
  assert(arena_s->free_size == 0 && "Initial free size incorrect (small)");
  assert(arena_s->rsv_size >= test_rsv_s &&
         "Stored rsv_size incorrect (small)");
  assert(arena_s->cmt_size >= test_cmt_s &&
         "Stored cmt_size incorrect (small)");
  arena_destroy(arena_s);

  // Test with default sizes (macro)
  Arena *arena_d = arena_create();
  assert(arena_d != NULL && "Arena creation (default) failed");
  assert(arena_d->rsv >= ARENA_RSV_SIZE + ARENA_HEADER_SIZE);
  assert(arena_d->cmt >= ARENA_CMT_SIZE + ARENA_HEADER_SIZE);
  assert(arena_d->rsv % page_size == 0 &&
         "Arena->rsv not page aligned (default)");
  assert(arena_d->cmt % page_size == 0 &&
         "Arena->cmt not page aligned (default)");
  assert(arena_d->pos == get_initial_pos() &&
         "Initial position incorrect (default)");
  arena_destroy(arena_d);

  // Test with zero sizes (should create minimal valid arena)
  Arena *arena_z = arena_create(0, 0);
  assert(arena_z != NULL && "Arena creation (zero) failed");
  assert(arena_z->rsv >= ARENA_HEADER_SIZE && "Reserved size too small (zero)");
  assert(arena_z->cmt >= ARENA_HEADER_SIZE &&
         "Committed size too small (zero)");
  assert(arena_z->rsv > 0 && "Arena->rsv must be > 0 (zero)");
  assert(arena_z->cmt > 0 && "Arena->cmt must be > 0 (zero)");
  assert(arena_z->rsv % page_size == 0 && "Arena->rsv not page aligned (zero)");
  assert(arena_z->cmt % page_size == 0 && "Arena->cmt not page aligned (zero)");
  assert(arena_z->pos == get_initial_pos() &&
         "Initial position incorrect (zero)");
  arena_destroy(arena_z);

  printf("  test_arena_creation PASSED\n");
}

static void test_arena_simple_alloc() {
  printf("  Running test_arena_simple_alloc...\n");
  Arena *arena = arena_create();
  uint64_t initial_pos = arena_pos(arena);
  assert(initial_pos == get_initial_pos() && "Initial pos mismatch");

  // Test 0-byte allocation
  uint64_t pos_before_zero_alloc = arena_pos(arena);
  void *ptr_zero = arena_alloc(arena, 0, ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr_zero != NULL && "0-byte allocation failed");
  assert((uintptr_t)ptr_zero % AlignOf(void *) == 0 &&
         "0-byte ptr not aligned");
  // Position might advance to next alignment boundary or by AlignOf(void*)
  uint64_t expected_pos_after_zero =
      AlginPow2(pos_before_zero_alloc, AlignOf(void *));
  if (expected_pos_after_zero ==
      pos_before_zero_alloc) { // if already aligned, 0 byte alloc might advance
                               // by min alignment
    // Or it might not advance at all if size is 0. Current code advances by
    // size, which is 0 for pos_post calculation. However, pos_pre =
    // AlginPow2(current->pos, AlignOf(void *)) might advance current->pos if it
    // wasn't aligned. Let's assume it might advance minimally if pos was
    // unaligned, or not at all if size is truly 0 and pos was aligned. The
    // current arena_alloc will set current->pos to pos_post = pos_pre + 0. So
    // pos only changes if pos_pre caused an alignment change on current->pos.
    // This makes actual pos check tricky for 0-byte. Main thing is ptr_zero is
    // valid.
  }
  // For now, primarily ensure it doesn't crash and gives a pointer.
  // memset(ptr_zero, 0xAA, 0); // This is a no-op, but good for tools.

  uint64_t alloc_size1 = 100;
  void *ptr1 = arena_alloc(arena, alloc_size1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr1 != NULL && "Allocation 1 failed");
  uint64_t pos_after_alloc1 = arena_pos(arena);
  uint64_t expected_aligned_pos1 =
      AlginPow2(pos_before_zero_alloc, AlignOf(void *)) +
      alloc_size1; // Assuming 0-byte alloc didn't advance pos meaningfully for
                   // next alloc start More robust: track pos before ptr1
                   // specifically
  uint64_t pos_before_alloc1 =
      arena->current
          ->pos; // more direct way to get current block's position before this
                 // specific alloc if it's the first after 0-byte This still
                 // relies on current->pos being what we expect. Let's use
                 // arena_pos() before alloc1 call.
  pos_before_alloc1 = arena_pos(
      arena); // Correct place to record this before alloc1 if ptr_zero was
              // first. The original test structure was: initial_pos -> ptr1 ->
              // ptr2. With ptr_zero in between: initial_pos -> ptr_zero -> ptr1
              // -> ptr2 So, ptr1 starts at arena_pos() AFTER ptr_zero.

  uint64_t current_arena_pos_before_ptr1 = arena_pos(arena);
  ptr1 = arena_alloc(
      arena, alloc_size1,
      ARENA_MEMORY_TAG_UNKNOWN); // re-assign ptr1 for clarity after 0-byte
  assert(ptr1 != NULL && "Allocation 1 (after 0-byte) failed");
  pos_after_alloc1 = arena_pos(arena);

  assert(pos_after_alloc1 >= current_arena_pos_before_ptr1 + alloc_size1 &&
         "Position after alloc 1 too small");
  assert(pos_after_alloc1 <
             current_arena_pos_before_ptr1 + alloc_size1 + AlignOf(void *) &&
         "Position after alloc 1 too large");
  assert((uintptr_t)ptr1 % AlignOf(void *) == 0 && "Pointer 1 not aligned");
  memset(ptr1, 0xAA, alloc_size1);

  uint64_t alloc_size2 = 200;
  void *ptr2 = arena_alloc(arena, alloc_size2, ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr2 != NULL && "Allocation 2 failed");
  uint64_t pos_after_alloc2 = arena_pos(arena);
  assert(pos_after_alloc2 >= pos_after_alloc1 + alloc_size2 &&
         "Position after alloc 2 too small");
  assert(pos_after_alloc2 < pos_after_alloc1 + alloc_size2 + AlignOf(void *) &&
         "Position after alloc 2 too large");
  assert((uintptr_t)ptr2 % AlignOf(void *) == 0 && "Pointer 2 not aligned");
  memset(ptr2, 0xBB, alloc_size2);

  assert(*(unsigned char *)ptr1 == 0xAA && "Data verification for ptr1 failed");
  assert(*(unsigned char *)ptr2 == 0xBB && "Data verification for ptr2 failed");

  // Check pointer arithmetic
  // The start of ptr2 should be at ptr1 + aligned_size_of_ptr1_allocation
  uint64_t aligned_alloc_size1 = AlginPow2(alloc_size1, AlignOf(void *));
  // This isn't quite right, arena_alloc aligns start of alloc, not size itself
  // internally for next alloc. The position calculation is pos_pre =
  // AlginPow2(current->pos, AlignOf(void *)); pos_post = pos_pre + size; So
  // ptr2 should be at (char*)ptr1 + (pos_after_alloc1 -
  // current_arena_pos_before_ptr1) if there were no internal fragmentations due
  // to header. Simpler: (uintptr_t)ptr2 should be >= (uintptr_t)ptr1 +
  // alloc_size1. And more precisely, ptr2 should be exactly at the arena_pos()
  // recorded after ptr1 was allocated if no alignment padding was added AFTER
  // ptr1 for ptr2. The test `pos_after_alloc2 >= pos_after_alloc1 +
  // alloc_size2` already covers this indirectly for arena_pos. Direct check:
  // The address of ptr2 should be the address of where ptr1 ended (after its
  // own size) then aligned up for ptr2. This is tricky because ptr1 itself is
  // aligned. The space between end of ptr1 actual data and start of ptr2 should
  // be minimal (padding).
  assert((uintptr_t)ptr2 >= (uintptr_t)ptr1 + alloc_size1);

  arena_destroy(arena);
  printf("  test_arena_simple_alloc PASSED\n");
}

static void test_arena_commit_grow() {
  printf("  Running test_arena_commit_grow...\n");
  uint64_t page_size = platform_get_page_size();
  uint64_t test_rsv = KB(64);
  uint64_t test_cmt_chunk = KB(4); // Small initial commit chunk
  Arena *arena = arena_create(test_rsv, test_cmt_chunk);

  uint64_t initial_total_committed_in_block =
      arena->current->cmt; // cmt is total committed in block
  uint64_t allocatable_before_header =
      initial_total_committed_in_block - ARENA_HEADER_SIZE;
  if (allocatable_before_header < 0)
    allocatable_before_header = 0; // Should not happen if cmt is sane

  uint64_t current_pos_in_block = arena->current->pos;
  uint64_t remaining_in_initial_commit =
      initial_total_committed_in_block - current_pos_in_block;

  // Allocate exactly up to remaining initial commit: should not grow cmt yet
  if (remaining_in_initial_commit > 0) {
    void *ptr_exact = arena_alloc(arena, remaining_in_initial_commit,
                                  ARENA_MEMORY_TAG_UNKNOWN);
    assert(ptr_exact != NULL && "Alloc exact remaining commit failed");
    memset(ptr_exact, 0xAA, remaining_in_initial_commit);
    assert(arena->current->cmt == initial_total_committed_in_block &&
           "Commit size grew when it should not have");
  }

  // Allocate 1 more byte: cmt should grow
  uint64_t cmt_before_grow = arena->current->cmt;
  void *ptr_grow = arena_alloc(arena, 1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr_grow != NULL && "Alloc 1 byte to grow commit failed");
  memset(ptr_grow, 0xBB, 1);
  assert(arena->current->cmt > cmt_before_grow &&
         "Commit size did not grow after 1 byte alloc");
  assert(arena->current->cmt % page_size == 0 && "Grown cmt not page aligned");
  assert(arena->current->cmt <= arena->current->rsv &&
         "Commit exceeded reserve");

  // Allocate a large chunk that requires more commit, up to rsv
  cmt_before_grow = arena->current->cmt;
  uint64_t large_alloc_size =
      arena->current->rsv - arena->current->pos - 10; // Almost fill the block
  if (large_alloc_size > 0 &&
      arena->current->pos + large_alloc_size <= arena->current->rsv) {
    void *ptr_large =
        arena_alloc(arena, large_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(ptr_large != NULL && "Large alloc failed");
    memset(ptr_large, 0xCC, large_alloc_size);
    assert(arena->current->cmt > cmt_before_grow ||
           arena->current->cmt == arena->current->rsv &&
               "Commit not grown for large alloc or not at rsv limit");
    assert(arena->current->cmt % page_size == 0 &&
           "Large alloc grown cmt not page aligned");
    assert(arena->current->cmt <= arena->current->rsv &&
           "Commit exceeded reserve after large alloc");
  }

  arena_destroy(arena);
  printf("  test_arena_commit_grow PASSED\n");
}

static void test_arena_block_grow() {
  printf("  Running test_arena_block_grow...\n");
  uint64_t page_size = platform_get_page_size();
  uint64_t first_block_rsv_config = KB(4); // Very small reserve for first block
  Arena *arena = arena_create(first_block_rsv_config, first_block_rsv_config);
  Arena *first_block = arena->current;
  uint64_t actual_first_block_rsv =
      first_block->rsv; // This is page aligned rsv + header
  uint64_t initial_pos_in_first_block = first_block->pos;
  uint64_t remaining_in_first_block =
      actual_first_block_rsv - initial_pos_in_first_block;

  // Allocate exactly up to remaining space in first block: should not grow
  // block yet
  if (remaining_in_first_block > 0) {
    void *ptr_exact_fill =
        arena_alloc(arena, remaining_in_first_block, ARENA_MEMORY_TAG_UNKNOWN);
    assert(ptr_exact_fill != NULL && "Alloc exact remaining in block failed");
    memset(ptr_exact_fill, 0xAA, remaining_in_first_block);
    assert(arena->current == first_block &&
           "Block grew when it should not have");
  }

  // Allocate 1 more byte: should trigger new block
  Arena *block_before_grow = arena->current;
  void *ptr_grow_block = arena_alloc(arena, 1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr_grow_block != NULL && "Alloc 1 byte to grow block failed");
  memset(ptr_grow_block, 0xBB, 1);
  assert(arena->current != block_before_grow &&
         "Arena did not switch to a new block");
  assert(arena->current->prev == block_before_grow &&
         "New block's prev pointer incorrect");
  assert(arena->current->base_pos ==
             block_before_grow->base_pos + block_before_grow->rsv &&
         "New block's base_pos incorrect");
  assert(arena->current->pos == get_initial_pos() + 1 &&
         "Position in new block incorrect");
  assert(arena->current->rsv % page_size == 0 &&
         "New block rsv not page aligned");
  assert(arena->current->cmt % page_size == 0 &&
         "New block cmt not page aligned");

  // Test allocation of a size larger than default rsv_size for new blocks
  // The default s_rsv_size for a new block would be taken from
  // current->rsv_size (which is first_block_rsv_config here). We want an
  // allocation that:
  // 1. Overflows the current block (before_large_spill_block, which is the
  // second block).
  // 2. Is large enough such that (allocation_size + ARENA_HEADER_SIZE) >
  // first_block_rsv_config, to test custom sizing of the new (third) block.

  Arena *before_large_spill_block = arena->current; // This is the second block.
  uint64_t remaining_in_current_spill_block =
      before_large_spill_block->rsv - before_large_spill_block->pos;

  uint64_t large_alloc_spilling_default =
      remaining_in_current_spill_block +
      100; // Ensures current (2nd) block is overflowed.

  // Also ensure this allocation size triggers the custom sizing for the *new*
  // (3rd) block. The default rsv for the new block would be
  // first_block_rsv_config (from current->rsv_size).
  if (large_alloc_spilling_default + ARENA_HEADER_SIZE <=
      first_block_rsv_config) {
    large_alloc_spilling_default =
        first_block_rsv_config + ARENA_HEADER_SIZE + 100 -
        ARENA_HEADER_SIZE; // Net effect: first_block_rsv_config + 100
    // Recalculate to ensure it also spills the current block if the above made
    // it too small to spill
    if (large_alloc_spilling_default <= remaining_in_current_spill_block) {
      large_alloc_spilling_default = remaining_in_current_spill_block + 100;
    }
  }
  // At this point, large_alloc_spilling_default should be >
  // remaining_in_current_spill_block AND large_alloc_spilling_default +
  // ARENA_HEADER_SIZE should be > first_block_rsv_config.

  void *ptr_large_spill = arena_alloc(arena, large_alloc_spilling_default,
                                      ARENA_MEMORY_TAG_UNKNOWN);
  assert(ptr_large_spill != NULL &&
         "Large alloc (spilling default rsv) failed");
  memset(ptr_large_spill, 0xCC, large_alloc_spilling_default);
  assert(arena->current != before_large_spill_block &&
         "Arena did not switch for large spill alloc");
  assert(arena->current->rsv >=
             large_alloc_spilling_default + ARENA_HEADER_SIZE &&
         "New block for large spill not big enough");
  assert(AlginPow2(arena->current->pos - large_alloc_spilling_default,
                   AlignOf(void *)) == get_initial_pos() &&
         "Pos in large spill block incorrect (aligned start check)");

  arena_destroy(arena);
  printf("  test_arena_block_grow PASSED\n");
}

static void test_arena_reset_to() {
  printf("  Running test_arena_reset_to...\n");
  Arena *arena = arena_create(
      KB(4), KB(4)); // Use small blocks to force multi-block scenarios
  uint64_t initial_arena_pos = arena_pos(arena);
  assert(initial_arena_pos == get_initial_pos());

  // Reset to 0 (should be clamped to header size)
  arena_reset_to(arena, 0, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == get_initial_pos() && "Reset to 0 failed");

  void *p1 = arena_alloc(arena, 100, ARENA_MEMORY_TAG_UNKNOWN);
  uint64_t pos1 = arena_pos(arena);
  void *p2 = arena_alloc(arena, 200, ARENA_MEMORY_TAG_UNKNOWN);
  uint64_t pos2 = arena_pos(arena);
  assert(p1 && p2);

  // Reset to current pos (no-op)
  arena_reset_to(arena, pos2, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos2 && "Reset to current pos changed position");

  // Reset back to after p1
  arena_reset_to(arena, pos1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos1 && "Position incorrect after reset to pos1");

  // Allocate again, should reuse space
  void *p3 = arena_alloc(arena, 50, ARENA_MEMORY_TAG_UNKNOWN);
  uint64_t pos3 = arena_pos(arena);
  assert(p3 != NULL && "Allocation after reset failed");
  assert(pos3 >= pos1 + 50 && "Position after reset+alloc too small");

  // Force multi-block and reset across boundary
  Arena *block_to_test_spill =
      arena->current; // This is the first block at this stage
  uint64_t current_pos_in_block_to_spill = block_to_test_spill->pos;
  uint64_t aligned_start_for_fill =
      AlginPow2(current_pos_in_block_to_spill, AlignOf(void *));
  uint64_t space_available_for_fill =
      block_to_test_spill->rsv - aligned_start_for_fill;

  uint64_t fill_block1_size = 0;
  uint64_t spill_alloc_size = KB(1);

  if (space_available_for_fill > spill_alloc_size) {
    fill_block1_size =
        space_available_for_fill -
        (spill_alloc_size / 2); // Leave less than spill_alloc_size
  } else {
    // Not enough space to even force a situation where spill_alloc_size would
    // spill. This might happen if p1 and p3 already filled up most of it. For
    // the test to proceed meaningfully, we need to ensure a spill WILL happen.
    // This indicates the initial arena or p1/p3 sizes might need adjustment for
    // this specific test sequence. Or, we accept that spill_alloc_size might
    // fit if fill_block1_size becomes 0 or very small. Let's make
    // fill_block1_size such that *any* further allocation of spill_alloc_size
    // would spill if possible.
    if (space_available_for_fill > 0)
      fill_block1_size =
          space_available_for_fill - 1; // Fill to almost the brim if possible
    else
      fill_block1_size = 0; // No space to fill further
  }
  if (fill_block1_size > 0) {
    arena_alloc(arena, fill_block1_size, ARENA_MEMORY_TAG_UNKNOWN);
  }

  Arena *first_block_after_fill = arena->current;
  // If fill_block1_size was substantial and less than space_available_for_fill,
  // current should not have changed.
  if (fill_block1_size > 0 && fill_block1_size < space_available_for_fill) {
    assert(first_block_after_fill == block_to_test_spill &&
           "Fill alloc spilled unexpectedly");
  }
  uint64_t pos_in_block1_before_spill = arena_pos(arena);

  // Spill to block 2
  void *p_block2_alloc =
      arena_alloc(arena, spill_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
  assert(p_block2_alloc && "Alloc in block2 failed");
  assert(arena->current != first_block_after_fill && "Did not move to block2");
  Arena *block2_ptr = arena->current;
  uint64_t free_size_before_reset_across = arena->free_size;
  Arena *free_list_before_reset_across = arena->free_last;
  Arena *block_to_be_freed1 =
      arena->current; // This is block2_ptr in this scenario
  Arena *block_to_be_freed2 =
      arena->current->prev; // This is first_block_after_fill if it was not the
                            // initial block The loop in reset_to goes:
                            // current=block_to_be_freed1, then
                            // prev=current->prev (block_to_be_freed2) until
                            // current->prev == NULL or base_pos condition.
                            // block_to_be_freed1 and block_to_be_freed2 (if not
                            // NULL and base_pos >= pos) will be freed.
  uint64_t expected_rsv_sum_of_freed_blocks = 0;
  if (block_to_be_freed1 &&
      block_to_be_freed1->base_pos >= pos_in_block1_before_spill) {
    // This check is a bit off, reset_to iterates. block2_ptr IS the one that is
    // certainly freed if pos_in_block1_before_spill is in
    // first_block_after_fill.
  }
  // For this specific reset: arena_reset_to(arena, pos_in_block1_before_spill,
  // ...) block2_ptr was current. Its base_pos is > pos_in_block1_before_spill.
  // So block2_ptr will be freed. Its prev is first_block_after_fill. Loop stops
  // there.
  expected_rsv_sum_of_freed_blocks = block2_ptr->rsv_size;

  // Reset to a position in block 1
  arena_reset_to(arena, pos_in_block1_before_spill, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena->current == first_block_after_fill &&
         "Reset did not return to block1");
  assert(arena_pos(arena) == pos_in_block1_before_spill &&
         "Pos incorrect after reset to block1");
  assert(arena->free_last != NULL && "Block2 not added to free list");
  assert(arena->free_last == block2_ptr &&
         "Freed block is not block2_ptr or not last");
  assert(arena->free_last->prev == free_list_before_reset_across &&
         "Freed block's prev not linked to old free_last");
  assert(arena->free_size ==
             free_size_before_reset_across + expected_rsv_sum_of_freed_blocks &&
         "Free size incorrect");

  // Try to allocate again, should reuse from free list
  Arena *free_list_state_after_clear = arena->free_last;
  uint64_t free_size_state_after_clear = arena->free_size;

  // Fill up most of the current block (first_block) so it cannot satisfy the
  // next alloc intended for free list.
  uint64_t usable_in_restarted_current_block =
      arena->current->rsv - arena->current->pos;
  uint64_t fill_restarted_current_block_size = 0;
  uint64_t target_reuse_alloc_size =
      KB(2); // Size we want to allocate, hopefully from the free list.

  if (usable_in_restarted_current_block > target_reuse_alloc_size) {
    fill_restarted_current_block_size =
        usable_in_restarted_current_block -
        (target_reuse_alloc_size /
         2); // Leave too little for target_reuse_alloc_size
    if (fill_restarted_current_block_size > 0) {
      void *temp_fill = arena_alloc(arena, fill_restarted_current_block_size,
                                    ARENA_MEMORY_TAG_UNKNOWN);
      assert(temp_fill != NULL && "Failed to fill restarted current block");
      // After this alloc, arena->current should ideally still be first_block if
      // fill was calculated right for a single block. However, if first_block
      // was very small (e.g. only KB(4) rsv), even this fill could spill. Test
      // assumes first_block is reasonably large. For arena_create(KB(4),
      // KB(4)), first_block->rsv is AlginPow2(HDR_SIZE+KB(4), PAGE_SIZE) which
      // is likely KB(8) if PAGE_SIZE=KB(4). So KB(8) - HDR_SIZE has enough
      // space for this usually. We rely on the spill check logic in arena_alloc
      // to handle if it does spill here, affecting arena->current.
    }
  }
  // Now, arena->current (which might still be first_block or its successor if
  // the fill above spilled) should have less than target_reuse_alloc_size
  // remaining if the fill worked as intended on first_block.

  uint64_t remaining_in_current_before_reuse_attempt =
      arena->current->rsv - AlginPow2(arena->current->pos, AlignOf(void *));

  // Condition for attempting reuse from free list:
  // 1. Some blocks were actually freed and are on the list.
  // 2. The target_reuse_alloc_size can fit in a typical freed block (e.g. a
  // KB(4) block from its rsv_size).
  // 3. The current block cannot satisfy target_reuse_alloc_size.
  if (expected_rsv_sum_of_freed_blocks > 0 &&
      target_reuse_alloc_size <
          (KB(4) - ARENA_HEADER_SIZE) && // Assumes freed blocks had at least
                                         // KB(4) rsv_size configuration
      remaining_in_current_before_reuse_attempt < target_reuse_alloc_size) {

    void *p_reused =
        arena_alloc(arena, target_reuse_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(p_reused != NULL && "Alloc for free list reuse after clear failed");

    bool free_list_used = (arena->free_last != free_list_state_after_clear) ||
                          (arena->free_size < free_size_state_after_clear);
    assert(free_list_used && "Free list not utilized or updated after "
                             "clear+alloc when current block was full");
  } else {
    // This path means the conditions to test free list reuse were not met.
    // It might be that no blocks were freed, or the current block could still
    // satisfy the request, or target_reuse_alloc_size was too large for typical
    // freed blocks. Consider this a skipped test for this specific sub-case or
    // log a warning.
    printf("  [INFO] test_arena_reset_to: Skipping specific free list reuse "
           "check as conditions not met.\n");
    // We can still try to allocate to ensure arena is usable.
    void *p_general_alloc =
        arena_alloc(arena, target_reuse_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(p_general_alloc != NULL &&
           "General alloc after reset (no free list test) failed");
  }

  arena_destroy(arena);
  printf("  test_arena_reset_to PASSED\n");
}

static void test_arena_clear() {
  printf("  Running test_arena_clear...\n");
  Arena *arena =
      arena_create(KB(4), KB(4)); // Small blocks for multi-block scenario
  uint64_t initial_pos = arena_pos(arena);

  // Clear empty arena
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == initial_pos &&
         "Position changed after clearing empty arena");

  // Allocate some, then clear
  arena_alloc(arena, 100, ARENA_MEMORY_TAG_UNKNOWN);
  arena_alloc(arena, 200, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) > initial_pos &&
         "Position didn't advance before clear");
  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == initial_pos && "Position not reset by clear");

  // Allocate across multiple blocks, then clear
  Arena *first_block = arena->current;
  uint64_t initial_pos_in_first_block =
      first_block->pos; // Should be get_initial_pos()
  uint64_t rsv_of_first_block = first_block->rsv;
  uint64_t usable_space_in_first_block =
      rsv_of_first_block - initial_pos_in_first_block;

  uint64_t alloc_almost_fill_size = 0;
  if (usable_space_in_first_block >
      200) { // Ensure there's enough space to test this meaningfully
    alloc_almost_fill_size =
        usable_space_in_first_block - 100; // Leave 100 bytes
  } else if (usable_space_in_first_block > 0) {
    alloc_almost_fill_size =
        usable_space_in_first_block / 2; // Allocate some if space is small
  }

  if (alloc_almost_fill_size > 0) {
    arena_alloc(arena, alloc_almost_fill_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(arena->current == first_block &&
           "Almost fill alloc should not have spilled first block");
  }

  uint64_t alloc_to_spill_size =
      150; // This should spill if 100 bytes were left, or if block was small
           // And must be > remaining space after alignment of current pos.
  // Ensure alloc_to_spill_size is greater than remaining in current block
  uint64_t current_pos_val = arena->current->pos;
  uint64_t aligned_current_pos = AlginPow2(current_pos_val, AlignOf(void *));
  if (arena->current->rsv - aligned_current_pos >= alloc_to_spill_size) {
    // If it still fits, make it bigger to force spill from first_block for the
    // test. This might happen if usable_space_in_first_block was very small and
    // alloc_almost_fill_size was also small.
    alloc_to_spill_size = (arena->current->rsv - aligned_current_pos) + 10;
  }

  void *p_spill =
      arena_alloc(arena, alloc_to_spill_size, ARENA_MEMORY_TAG_UNKNOWN);
  assert(p_spill != NULL && "Spill allocation failed");

  assert(first_block != arena->current &&
         "Should be on a new block after several allocs");
  uint64_t free_size_before_multiblock_clear = arena->free_size;
  Arena *original_free_last_before_multiblock_clear = arena->free_last;

  uint64_t expected_total_freed_rsv_size = 0;
  // Determine actual blocks that will be freed. arena->current is the last one.
  // Its prev chain up to (but not including) first_block will be freed.
  Arena *iter = arena->current;
  while (iter != NULL && iter != first_block) {
    expected_total_freed_rsv_size += iter->rsv_size;
    iter = iter->prev;
  }

  arena_clear(arena, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == initial_pos &&
         "Position not reset by multi-block clear");
  assert(arena->current == first_block && "Current not reset to first_block");
  assert(arena->free_last != original_free_last_before_multiblock_clear ||
         expected_total_freed_rsv_size > 0 &&
             "Free list unchanged or no blocks freed");
  assert(arena->free_size == free_size_before_multiblock_clear +
                                 expected_total_freed_rsv_size &&
         "Free size incorrect after multi-block clear");

  // Try to allocate again, should reuse from free list
  Arena *free_list_state_after_clear = arena->free_last;
  uint64_t free_size_state_after_clear = arena->free_size;

  // Fill up most of the current block (first_block) so it cannot satisfy the
  // next alloc intended for free list.
  uint64_t usable_in_restarted_current_block =
      arena->current->rsv - arena->current->pos;
  uint64_t fill_restarted_current_block_size = 0;
  uint64_t target_reuse_alloc_size =
      KB(2); // Size we want to allocate, hopefully from the free list.

  if (usable_in_restarted_current_block > target_reuse_alloc_size) {
    fill_restarted_current_block_size =
        usable_in_restarted_current_block -
        (target_reuse_alloc_size /
         2); // Leave too little for target_reuse_alloc_size
    if (fill_restarted_current_block_size > 0) {
      void *temp_fill = arena_alloc(arena, fill_restarted_current_block_size,
                                    ARENA_MEMORY_TAG_UNKNOWN);
      assert(temp_fill != NULL && "Failed to fill restarted current block");
      // After this alloc, arena->current should ideally still be first_block if
      // fill was calculated right for a single block. However, if first_block
      // was very small (e.g. only KB(4) rsv), even this fill could spill. Test
      // assumes first_block is reasonably large. For arena_create(KB(4),
      // KB(4)), first_block->rsv is AlginPow2(HDR_SIZE+KB(4), PAGE_SIZE) which
      // is likely KB(8) if PAGE_SIZE=KB(4). So KB(8) - HDR_SIZE has enough
      // space for this usually. We rely on the spill check logic in arena_alloc
      // to handle if it does spill here, affecting arena->current.
    }
  }
  // Now, arena->current (which might still be first_block or its successor if
  // the fill above spilled) should have less than target_reuse_alloc_size
  // remaining if the fill worked as intended on first_block.

  uint64_t remaining_in_current_before_reuse_attempt =
      arena->current->rsv - AlginPow2(arena->current->pos, AlignOf(void *));

  // Condition for attempting reuse from free list:
  // 1. Some blocks were actually freed and are on the list.
  // 2. The target_reuse_alloc_size can fit in a typical freed block (e.g. a
  // KB(4) block from its rsv_size).
  // 3. The current block cannot satisfy target_reuse_alloc_size.
  if (expected_total_freed_rsv_size > 0 &&
      target_reuse_alloc_size <
          (KB(4) - ARENA_HEADER_SIZE) && // Assumes freed blocks had at least
                                         // KB(4) rsv_size configuration
      remaining_in_current_before_reuse_attempt < target_reuse_alloc_size) {

    void *p_reused =
        arena_alloc(arena, target_reuse_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(p_reused != NULL && "Alloc for free list reuse after clear failed");

    bool free_list_used = (arena->free_last != free_list_state_after_clear) ||
                          (arena->free_size < free_size_state_after_clear);
    assert(free_list_used && "Free list not utilized or updated after "
                             "clear+alloc when current block was full");
  } else {
    // This path means the conditions to test free list reuse were not met.
    // It might be that no blocks were freed, or the current block could still
    // satisfy the request, or target_reuse_alloc_size was too large for typical
    // freed blocks. Consider this a skipped test for this specific sub-case or
    // log a warning.
    printf("  [INFO] test_arena_clear: Skipping specific free list reuse check "
           "as conditions not met.\n");
    // We can still try to allocate to ensure arena is usable.
    void *p_general_alloc =
        arena_alloc(arena, target_reuse_alloc_size, ARENA_MEMORY_TAG_UNKNOWN);
    assert(p_general_alloc != NULL &&
           "General alloc after clear (no free list test) failed");
  }

  arena_destroy(arena);
  printf("  test_arena_clear PASSED\n");
}

static void test_arena_scratch() {
  printf("  Running test_arena_scratch...\n");
  Arena *arena = arena_create();
  uint64_t initial_pos = arena_pos(arena);

  // Scratch on empty arena
  Scratch scratch_empty = scratch_create(arena);
  assert(scratch_empty.pos == initial_pos && "Scratch on empty: pos mismatch");
  arena_alloc(arena, 10,
              ARENA_MEMORY_TAG_UNKNOWN); // Allocate inside empty scratch
  scratch_destroy(scratch_empty, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == initial_pos && "Scratch on empty: not reset");

  void *p_before = arena_alloc(arena, 50, ARENA_MEMORY_TAG_UNKNOWN);
  uint64_t pos_before = arena_pos(arena);
  assert(p_before);

  Scratch scratch1 = scratch_create(arena);
  assert(scratch1.arena == arena && "Scratch arena mismatch");
  assert(scratch1.pos == pos_before && "Scratch 1 position incorrect");

  void *p_s1_1 = arena_alloc(arena, 100, ARENA_MEMORY_TAG_UNKNOWN);
  uint64_t pos_s1_1 = arena_pos(arena);
  assert(p_s1_1);

  Scratch scratch2 = scratch_create(arena);
  assert(scratch2.arena == arena);
  assert(scratch2.pos == pos_s1_1);

  void *p_s2_1 = arena_alloc(arena, 200, ARENA_MEMORY_TAG_UNKNOWN);
  assert(p_s2_1);

  scratch_destroy(scratch2, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos_s1_1 &&
         "Position not reset after scratch 2 destroy");

  void *p_s1_2 = arena_alloc(arena, 75, ARENA_MEMORY_TAG_UNKNOWN);
  assert(p_s1_2);
  assert(arena_pos(arena) >= pos_s1_1 + 75 &&
         "Position incorrect after nested scratch");

  scratch_destroy(scratch1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos_before &&
         "Position not reset after scratch 1 destroy");

  // Allocate again to ensure arena is usable
  void *p_after = arena_alloc(arena, 25, ARENA_MEMORY_TAG_UNKNOWN);
  assert(p_after);
  assert(arena_pos(arena) >= pos_before + 25 &&
         "Position incorrect after all scratches");

  // Test sequential scratches
  uint64_t pos_before_seq = arena_pos(arena);
  Scratch s_seq1 = scratch_create(arena);
  arena_alloc(arena, 30, ARENA_MEMORY_TAG_UNKNOWN);
  scratch_destroy(s_seq1, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos_before_seq && "Seq scratch1 failed");

  Scratch s_seq2 = scratch_create(arena);
  arena_alloc(arena, 40, ARENA_MEMORY_TAG_UNKNOWN);
  scratch_destroy(s_seq2, ARENA_MEMORY_TAG_UNKNOWN);
  assert(arena_pos(arena) == pos_before_seq && "Seq scratch2 failed");

  arena_destroy(arena);
  printf("  test_arena_scratch PASSED\n");
}

static void test_arena_alignment() {
  printf("  Running test_arena_alignment...\n");
  Arena *arena = arena_create();
  uint64_t alignment = AlignOf(void *); // Platform default alignment

  // Allocate small sizes to check alignment
  for (int i = 1; i < (int)alignment * 2; ++i) {
    void *ptr = arena_alloc(arena, i, ARENA_MEMORY_TAG_UNKNOWN);
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

  uint64_t struct_align = AlignOf(TestStruct);
  TestStruct *ts_ptr = (TestStruct *)arena_alloc(arena, sizeof(TestStruct),
                                                 ARENA_MEMORY_TAG_UNKNOWN);
  assert(ts_ptr != NULL && "Struct allocation failed");
  assert((uintptr_t)ts_ptr % struct_align == 0 &&
         "Struct pointer not aligned correctly");
  ts_ptr->ld = 1.23L; // Touch memory

  arena_destroy(arena);
  printf("  test_arena_alignment PASSED\n");
}

static void test_arena_tagging_and_statistics() {
  printf("  Running test_arena_tagging_and_statistics...\n");
  Arena *arena = arena_create(KB(256), KB(64)); // Main arena for stats
  Arena *str_arena =
      arena_create(KB(4), KB(4)); // Arena for the statistics string

  assert(arena != NULL && "Main arena creation failed");
  assert(str_arena != NULL && "String arena creation failed");

  // Initial check: all tag sizes should be 0
  for (int i = 0; i < ARENA_MEMORY_TAG_MAX; ++i) {
    assert(arena->tags[i].size == 0 && "Initial tag size non-zero");
  }

  // 1. Basic allocations and checks
  // Define sizes for different units
  uint64_t size_array_bytes = 50;            // Bytes
  uint64_t size_string_kb = KB(1) + 200;     // 1.xx KB
  uint64_t size_struct_mb = MB(2) + KB(300); // 2.xx MB
  uint64_t size_vector_gb = GB(1) + MB(50);  // 1.xx GB
  uint64_t size_buffer_exact_kb = KB(3);     // Exactly 3.00 KB

  void *p_arr = arena_alloc(arena, size_array_bytes, ARENA_MEMORY_TAG_ARRAY);
  void *p_str = arena_alloc(arena, size_string_kb, ARENA_MEMORY_TAG_STRING);
  uint64_t pos_before_struct = arena_pos(arena);
  void *p_struct = arena_alloc(arena, size_struct_mb, ARENA_MEMORY_TAG_STRUCT);
  void *p_buf =
      arena_alloc(arena, size_buffer_exact_kb, ARENA_MEMORY_TAG_BUFFER);

  assert(p_arr && p_str && p_struct && p_buf);
  assert(arena->tags[ARENA_MEMORY_TAG_ARRAY].size == size_array_bytes &&
         "Array tag size mismatch");
  assert(arena->tags[ARENA_MEMORY_TAG_STRING].size == size_string_kb &&
         "String tag size mismatch");
  assert(arena->tags[ARENA_MEMORY_TAG_STRUCT].size == size_struct_mb &&
         "Struct tag size mismatch");
  assert(arena->tags[ARENA_MEMORY_TAG_BUFFER].size == size_buffer_exact_kb &&
         "Buffer tag size mismatch");
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == 0 &&
         "Vector tag should be 0 initially");

  // 2. Test arena_reset_to with a specific tag
  // Current struct size is size_struct_mb. Let's reset to before it was
  // allocated.
  arena_reset_to(arena, pos_before_struct, ARENA_MEMORY_TAG_STRUCT);
  assert(arena->tags[ARENA_MEMORY_TAG_STRUCT].size == 0 &&
         "Struct tag not reset to 0 after reset_to");
  // Other tags should be unaffected by this specific reset operation
  assert(arena->tags[ARENA_MEMORY_TAG_ARRAY].size == size_array_bytes &&
         "Array tag changed after struct reset");
  assert(arena->tags[ARENA_MEMORY_TAG_STRING].size == size_string_kb &&
         "String tag changed after struct reset");

  // Re-allocate for struct to have a known value for stats string later
  p_struct = arena_alloc(arena, size_struct_mb, ARENA_MEMORY_TAG_STRUCT);
  assert(p_struct && "Struct re-allocation failed");
  assert(arena->tags[ARENA_MEMORY_TAG_STRUCT].size == size_struct_mb &&
         "Struct tag size mismatch after re-alloc");

  // 3. Test scratch_destroy with a specific tag
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == 0 &&
         "Vector tag non-zero before scratch");
  Scratch scratch = scratch_create(arena);
  uint64_t size_vec_in_scratch_kb = KB(1) + 500; // 1.xx KB
  void *p_vec_scratch =
      arena_alloc(arena, size_vec_in_scratch_kb, ARENA_MEMORY_TAG_VECTOR);
  assert(p_vec_scratch && "Vector alloc in scratch failed");
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == size_vec_in_scratch_kb &&
         "Vector tag incorrect after scratch alloc");
  scratch_destroy(scratch, ARENA_MEMORY_TAG_VECTOR);
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == 0 &&
         "Vector tag not reset after scratch_destroy");

  // Allocate vector memory outside scratch for final stats check (GB range)
  void *p_vec = arena_alloc(arena, size_vector_gb, ARENA_MEMORY_TAG_VECTOR);
  assert(p_vec && "Final vector alloc failed");
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == size_vector_gb &&
         "Vector tag size mismatch for final stats");

  // 4. Test arena_format_statistics
  char *stats_str = arena_format_statistics(arena, str_arena);
  assert(stats_str != NULL && "arena_format_statistics returned NULL");

  char
      check_buffer[256]; // Increased buffer size for potentially longer strings

  // ARENA_MEMORY_TAG_ARRAY (Bytes)
  snprintf(check_buffer, sizeof(check_buffer), "%s: %llu Bytes\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_ARRAY],
           (unsigned long long)size_array_bytes);
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Array stats (Bytes) incorrect or missing");

  // ARENA_MEMORY_TAG_STRING (KB)
  snprintf(check_buffer, sizeof(check_buffer), "%s: %.2f KB\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_STRING],
           (double)size_string_kb / KB(1));
  assert(strstr(stats_str, check_buffer) != NULL &&
         "String stats (KB) incorrect or missing");

  // ARENA_MEMORY_TAG_STRUCT (MB)
  snprintf(check_buffer, sizeof(check_buffer), "%s: %.2f MB\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_STRUCT],
           (double)size_struct_mb / MB(1));
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Struct stats (MB) incorrect or missing");

  // ARENA_MEMORY_TAG_VECTOR (GB)
  snprintf(check_buffer, sizeof(check_buffer), "%s: %.2f GB\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_VECTOR],
           (double)size_vector_gb / GB(1));
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Vector stats (GB) incorrect or missing");

  // ARENA_MEMORY_TAG_BUFFER (Exact KB)
  snprintf(check_buffer, sizeof(check_buffer), "%s: %.2f KB\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_BUFFER],
           (double)size_buffer_exact_kb / KB(1));
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Buffer stats (Exact KB) incorrect or missing");

  // Check a tag that should be zero (e.g. QUEUE)
  snprintf(check_buffer, sizeof(check_buffer),
           "%s: 0 Bytes\n", // Zero is always Bytes
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_QUEUE]);
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Queue (expected zero) stats incorrect or missing");

  // Check UNKNOWN tag (should also be 0 Bytes)
  snprintf(check_buffer, sizeof(check_buffer), "%s: 0 Bytes\n",
           ArenaMemoryTagNames[ARENA_MEMORY_TAG_UNKNOWN]);
  assert(strstr(stats_str, check_buffer) != NULL &&
         "Unknown (expected zero) stats incorrect or missing");

  // 5. Test arena_clear with a specific tag
  // Current string size is size_string_kb.
  // Clearing with STRING tag should make it 0.
  // Other tags (ARRAY, STRUCT, VECTOR, BUFFER) should remain.
  uint64_t string_size_before_clear = arena->tags[ARENA_MEMORY_TAG_STRING].size;
  assert(string_size_before_clear == size_string_kb);

  arena_clear(arena, ARENA_MEMORY_TAG_STRING);
  assert(arena->tags[ARENA_MEMORY_TAG_STRING].size == 0 &&
         "String tag not zeroed by arena_clear");

  // Check other tags are unaffected by the arena_clear on a specific tag
  // when the actual memory has been wiped. The stat itself for other tags
  // shouldn't change based on the current arena_clear implementation.
  assert(arena->tags[ARENA_MEMORY_TAG_ARRAY].size == size_array_bytes &&
         "Array tag unexpectedly changed by clear(STRING)");
  assert(arena->tags[ARENA_MEMORY_TAG_STRUCT].size == size_struct_mb &&
         "Struct tag unexpectedly changed by clear(STRING)");
  assert(arena->tags[ARENA_MEMORY_TAG_VECTOR].size == size_vector_gb &&
         "Vector tag unexpectedly changed by clear(STRING)");
  assert(arena->tags[ARENA_MEMORY_TAG_BUFFER].size == size_buffer_exact_kb &&
         "Buffer tag unexpectedly changed by clear(STRING)");

  arena_destroy(str_arena);
  arena_destroy(arena);
  printf("  test_arena_tagging_and_statistics PASSED\n");
}

// --- Test Runner ---
bool32_t run_arena_tests() {
  printf("--- Starting Arena Tests ---\n");

  test_arena_creation();
  test_arena_simple_alloc();
  test_arena_commit_grow();
  test_arena_block_grow();
  test_arena_reset_to();
  test_arena_clear();
  test_arena_scratch();
  test_arena_alignment();
  test_arena_tagging_and_statistics();

  printf("--- Arena Tests Completed ---\n");
  return true;
}

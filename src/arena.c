/**
 * @file arena.c
 * @brief Implementation of the arena allocator.
 *
 * This implementation uses platform-specific virtual memory functions
 * (`mmap`/`mprotect` on POSIX-like systems) to reserve large chunks of virtual
 * address space and commit physical memory pages incrementally as needed.
 *
 * Key Implementation Details:
 * - **Linked Blocks:** The arena grows by linking new memory blocks (`Arena`
 * structs) when the current block runs out of reserved space. `arena->current`
 * always points to the active block for allocations.
 * - **Commit-on-Demand:** Physical memory is committed only when an allocation
 *   needs space beyond the currently committed region within a block. This
 *   avoids wasting physical memory for unused reserved virtual space.
 * - **Free List:** When the arena is reset (`arena_reset_to`, `arena_clear`),
 *   blocks that are no longer needed are not immediately released but are
 *   added to a singly linked free list (`arena->free_last`). `arena_alloc`
 *   checks this free list before creating a new block, allowing for reuse.
 * - **Alignment:** Allocations are aligned to `sizeof(void*)` (implicitly via
 * ALIGN_UP with page size granularity in commit logic, and header size
 * alignment). Commit/Reserve sizes are page-aligned.
 * - **Header:** Each block starts with an `Arena` struct containing metadata.
 *   Allocations happen *after* this header. `ARENA_HEADER_SIZE` needs careful
 * adjustment if the `Arena` struct changes significantly.
 */
#include "arena.h"

Arena *arena_create(size_t rsv_size, size_t cmt_size) {
  assert(sizeof(Arena) <= ARENA_HEADER_SIZE && "Arena struct is too large");

  size_t s_rsv_size = AlginPow2(ARENA_HEADER_SIZE + rsv_size, AlignOf(void *));
  size_t s_cmt_size = AlginPow2(ARENA_HEADER_SIZE + cmt_size, AlignOf(void *));

  Arena *arena = (Arena *)mem_reserve(rsv_size);
  if (!mem_commit(arena, cmt_size) || !arena) {
    assert(0 && "Failed to commit memory");
  }

  arena->current = arena;
  arena->rsv_size = rsv_size;
  arena->cmt_size = cmt_size;
  arena->base_pos = 0;
  arena->pos = ARENA_HEADER_SIZE;
  arena->cmt = s_cmt_size;
  arena->rsv = s_rsv_size;
  arena->prev = NULL;
  arena->free_size = 0;
  arena->free_last = NULL;
  return arena;
}

void arena_destroy(Arena *arena) {
  for (Arena *current = arena->current, *prev = NULL; current != NULL;
       current = prev) {
    prev = current->prev;
    mem_release(current, current->rsv);
  }
}

/**
 * Allocation Strategy:
 * 1. Calculate the aligned start position (`pos_pre`) and end position
 * (`pos_post`) for the requested `size` within the `current` block.
 * 2. **Check Reserved Space:** If `pos_post` exceeds `current->rsv`, the
 * current block is too small. a. **Try Free List:** Search the
 * `arena->free_last` list for a block with `rsv >= required_size`. If found,
 * remove it from the free list, link it as the new `current` block, and update
 * `base_pos`. b. **Allocate New Block:** If no suitable free block exists, call
 *       `arena_create_internal` to reserve and commit a new block. The size
 *       might be larger than the default if the requested `size` itself is
 * large. Link the new block as the `current` block. c. Update `current`,
 * `pos_pre`, `pos_post` based on the new block.
 * 3. **Check Committed Space:** If `pos_post` (potentially in the new block)
 * exceeds `current->cmt`, more physical memory needs to be committed. a.
 * Calculate the required commit end position, aligned up to
 * `current->cmt_size`. b. Clamp the commit size to not exceed the block's
 * reserved size (`current->rsv`). c. Call `mem_commit` to make the physical
 * pages available. d. Update `current->cmt`.
 * 4. **Allocate:** If `current->cmt >= pos_post`, the space is available.
 *    a. Calculate the result pointer: `(char*)current + pos_pre`.
 *    b. Update the current block's position: `current->pos = pos_post`.
 *    c. Return the result pointer.
 * 5. **Failure:** If memory cannot be reserved or committed, the program exits
 * (or could return NULL).
 */
void *arena_alloc(Arena *arena, size_t size) {
  Arena *current = arena->current;

  size_t pos_pre = AlginPow2(current->pos, AlignOf(void *));
  size_t pos_post = pos_pre + size;

  if (current->rsv < pos_post) {
    Arena *new_block = NULL;

    Arena *prev_block = NULL;
    for (new_block = arena->free_last, prev_block = NULL; new_block != NULL;
         prev_block = new_block, new_block = new_block->prev) {
      if (new_block->rsv >= AlginPow2(size, AlignOf(void *))) {
        if (prev_block) {
          prev_block->prev = new_block->prev;
        } else {
          arena->free_last = new_block->prev;
        }

        arena->free_size -= new_block->rsv_size;
        break;
      }
    }

    if (new_block == NULL) {
      size_t s_rsv_size = current->rsv_size;
      size_t s_cmt_size = current->cmt_size;
      if (size + ARENA_HEADER_SIZE > s_rsv_size) {
        s_rsv_size = AlginPow2(size + ARENA_HEADER_SIZE, AlignOf(void *));
        s_cmt_size = AlginPow2(size + ARENA_HEADER_SIZE, AlignOf(void *));
      }

      new_block = arena_create(s_rsv_size, s_cmt_size);
    }

    new_block->base_pos = current->base_pos + current->rsv;
    SingleListAppend(arena->current, new_block, prev);

    current = new_block;
    pos_pre = AlginPow2(current->pos, AlignOf(void *));
    pos_post = pos_pre + size;
  }

  if (current->cmt < pos_post) {
    size_t cmt_pst_aligned = pos_post + current->cmt_size - 1;
    cmt_pst_aligned -= cmt_pst_aligned % current->cmt_size;
    size_t cmt_pst_clamped = ClampTop(cmt_pst_aligned, current->rsv);
    size_t cmt_size = cmt_pst_clamped - current->cmt;
    uint8_t *cmt_ptr = (uint8_t *)current + current->cmt;

    if (!mem_commit(cmt_ptr, cmt_size)) {
      assert(0 && "Failed to commit memory");
    }

    current->cmt = cmt_pst_clamped;
  }

  void *result = NULL;
  if (current->cmt >= pos_post) {
    result = (uint8_t *)current + pos_pre;
    current->pos = pos_post;
  }

  return result;
}

size_t arena_pos(Arena *arena) {
  Arena *current = arena->current;
  return current->pos + current->base_pos;
}

/**
 * Reset Strategy:
 * 1. Clamp the target `pos` to be at least `sizeof(Arena)` (can't reset into
 * header).
 * 2. Iterate backwards through the block list (`arena->current`,
 * `current->prev`, ...).
 * 3. For each block whose *entire* range (`base_pos` to `base_pos + rsv`) is
 *    at or after the target `pos`:
 *    a. Decommit its memory (`mem_decommit`) - Optional but recommended to
 * release physical pages. b. Reset its internal `pos` to `sizeof(Arena)`. c.
 * Add the block to the `arena->free_last` list using `SLLStackPush_N`. d.
 * Update `arena->free_size`.
 * 4. Stop when a block is found where `base_pos < pos`. This block will become
 *    the new `current` block.
 * 5. Update `arena->current` to this block.
 * 6. Calculate the new `pos` *within* this `current` block: `new_pos = pos -
 * current->base_pos`.
 * 7. Set `current->pos = new_pos`.
 */
void arena_reset_to(Arena *arena, size_t pos) {
  size_t big_pos = pos;
  Arena *current = arena->current;

  for (Arena *prev = NULL;
       current != NULL && current->prev != NULL && current->base_pos >= big_pos;
       current = prev) {
    prev = current->prev;
    current->pos = ARENA_HEADER_SIZE;
    arena->free_size += current->rsv;
    SingleListAppend(arena->free_last, current, prev);
  }

  assert(current != NULL);
  arena->current = current;

  size_t new_pos = big_pos - current->base_pos;
  current->pos = ClampBot(ARENA_HEADER_SIZE, new_pos);
}

void arena_clear(Arena *arena) { arena_reset_to(arena, 0); }

void arena_reset(Arena *arena, size_t amt) {
  size_t pos_old = arena_pos(arena);
  size_t pos_new = pos_old;
  if (amt < pos_old) {
    pos_new = pos_old - amt;
  }
  arena_reset_to(arena, pos_new);
}

Scratch scratch_create(Arena *arena) {
  Scratch scratch = {arena, arena_pos(arena)};
  return scratch;
}

void scratch_destroy(Scratch scratch) {
  arena_reset_to(scratch.arena, scratch.pos);
}
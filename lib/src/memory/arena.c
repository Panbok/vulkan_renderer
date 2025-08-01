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
 * - **Alignment:** Allocations are aligned to `MaxAlign()` (implicitly via
 * ALIGN_UP with page size granularity in commit logic, and header size
 * alignment). Commit/Reserve sizes are page-aligned.
 * - **Header:** Each block starts with an `Arena` struct containing metadata.
 *   Allocations happen *after* this header. `ARENA_HEADER_SIZE` needs careful
 * adjustment if the `Arena` struct changes significantly.
 */
#include "arena.h"

Arena *arena_create_internal(uint64_t rsv_size, uint64_t cmt_size,
                             ArenaFlags flags) {
  uint64_t s_rsv_size = AlignPow2(ARENA_HEADER_SIZE + rsv_size, MaxAlign());
  uint64_t s_cmt_size = AlignPow2(ARENA_HEADER_SIZE + cmt_size, MaxAlign());

  uint64_t page_size = 0;
  if (bitset8_is_set(&flags, ARENA_FLAG_LARGE_PAGES)) {
    page_size = platform_get_large_page_size();
  } else {
    page_size = platform_get_page_size();
  }

  // Ensure s_rsv_size is page-aligned for mmap (usually good practice)
  s_rsv_size = AlignPow2(s_rsv_size, page_size);

  // s_cmt_size is the initial commit. It also should be page-aligned.
  // The base address from mmap (mem_block) will be page-aligned.
  // We need to ensure the *size* of the initial commit is page-aligned.
  s_cmt_size = AlignPow2(s_cmt_size, page_size);

  void *mem_block = platform_mem_reserve(s_rsv_size);
  if (mem_block == (void *)-1 || mem_block == NULL) {
    assert(0 && "Failed to reserve memory for arena");
    return NULL;
  }

  Arena *arena = (Arena *)mem_block;
  if (!platform_mem_commit(arena, s_cmt_size)) {
    platform_mem_release(arena, s_rsv_size);
    assert(0 && "Failed to commit memory for arena");
    return NULL;
  }

  memset(arena->tags, 0, sizeof(arena->tags));

  arena->current = arena;
  arena->rsv_size = s_rsv_size;
  arena->cmt_size = s_cmt_size;
  arena->page_size = page_size;
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
    platform_mem_release(current, current->rsv);
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
 *    a. Calculate the result pointer: `(uint8*)current + pos_pre`.
 *    b. Update the current block's position: `current->pos = pos_post`.
 *    c. Return the result pointer.
 * 5. **Failure:** If memory cannot be reserved or committed, the program exits
 * (or could return NULL).
 */
void *arena_alloc(Arena *arena, uint64_t size, ArenaMemoryTag tag) {
  Arena *current = arena->current;

  uint64_t pos_pre = AlignPow2(current->pos, MaxAlign());
  uint64_t pos_post = pos_pre + size;

  if (current->rsv < pos_post) {
    Arena *new_block = NULL;

    Arena *prev_block = NULL;
    for (new_block = arena->free_last, prev_block = NULL; new_block != NULL;
         prev_block = new_block, new_block = new_block->prev) {
      if (new_block->rsv >= AlignPow2(size, MaxAlign())) {
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
      uint64_t s_rsv_size = current->rsv_size;
      uint64_t s_cmt_size = current->cmt_size;
      if (size > s_rsv_size) {
        s_rsv_size = AlignPow2(size, MaxAlign());
        s_cmt_size = AlignPow2(size, MaxAlign());
      }

      // Determine flags based on the original arena's page size
      ArenaFlags new_block_flags = bitset8_create();
      uint64_t base_page_size = platform_get_page_size();
      if (arena->page_size > base_page_size) {
        bitset8_set(&new_block_flags, ARENA_FLAG_LARGE_PAGES);
      }

      new_block =
          arena_create_internal(s_rsv_size, s_cmt_size, new_block_flags);
    }

    new_block->base_pos = current->base_pos + current->rsv;
    SingleListAppend(arena->current, new_block, prev);

    current = new_block;
    pos_pre = AlignPow2(current->pos, MaxAlign());
    pos_post = pos_pre + size;
  }

  if (current->cmt < pos_post) {
    uint64_t page_size = arena->page_size;

    // For large page arenas, we need to commit in large page increments from
    // the block start Calculate the new commit size as a multiple of page_size
    // from the block start
    uint64_t required_cmt_from_block_start = AlignPow2(pos_post, page_size);

    // Clamp to not exceed the reserved size
    if (required_cmt_from_block_start > current->rsv) {
      required_cmt_from_block_start = current->rsv;
    }

    if (required_cmt_from_block_start > current->cmt) {
      uint8_t *commit_start = (uint8_t *)current + current->cmt;
      uint64_t commit_size = required_cmt_from_block_start - current->cmt;

      if (!platform_mem_commit(commit_start, commit_size)) {
        assert(0 && "Failed to commit memory");
      }

      current->cmt = required_cmt_from_block_start;
    }
  }

  void *result = NULL;
  if (current->cmt >= pos_post) {
    result = (uint8_t *)current + pos_pre;
    current->pos = pos_post;

    if (tag < ARENA_MEMORY_TAG_MAX) {
      arena->tags[tag].size += size;
    }
  }

  return result;
}

uint64_t arena_pos(Arena *arena) {
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
void arena_reset_to(Arena *arena, uint64_t pos, ArenaMemoryTag tag) {
  uint64_t pos_before_reset = arena_pos(arena);

  uint64_t big_pos = ClampBot(ARENA_HEADER_SIZE, pos);
  Arena *current_block_iter = arena->current;

  for (Arena *prev = NULL;
       current_block_iter != NULL && current_block_iter->prev != NULL &&
       current_block_iter->base_pos >= big_pos;
       current_block_iter = prev) {
    prev = current_block_iter->prev;
    current_block_iter->pos = ARENA_HEADER_SIZE;
    arena->free_size += current_block_iter->rsv_size;
    SingleListAppend(arena->free_last, current_block_iter, prev);
  }

  assert(current_block_iter != NULL);
  arena->current = current_block_iter;

  uint64_t new_pos_in_block = big_pos - arena->current->base_pos;

  new_pos_in_block = ClampBot(ARENA_HEADER_SIZE, new_pos_in_block);
  new_pos_in_block = ClampTop(new_pos_in_block, arena->current->rsv);
  arena->current->pos = new_pos_in_block;

  uint64_t pos_after_reset = arena_pos(arena);

  if (pos_before_reset > pos_after_reset) {
    uint64_t reclaimed = pos_before_reset - pos_after_reset;
    if (tag < ARENA_MEMORY_TAG_MAX) {
      if (arena->tags[tag].size >= reclaimed) {
        arena->tags[tag].size -= reclaimed;
      } else {
        arena->tags[tag].size = 0;
      }
    }
  }
}

void arena_clear(Arena *arena, ArenaMemoryTag tag) {
  arena_reset_to(arena, 0, tag);
}

void arena_reset(Arena *arena, uint64_t amt, ArenaMemoryTag tag) {
  uint64_t pos_old = arena_pos(arena);
  uint64_t pos_new = pos_old;
  if (amt < pos_old) {
    pos_new = pos_old - amt;
  }
  arena_reset_to(arena, pos_new, tag);
}

Scratch scratch_create(Arena *arena) {
  Scratch scratch = {arena, arena_pos(arena)};
  return scratch;
}

void scratch_destroy(Scratch scratch, ArenaMemoryTag tag) {
  arena_reset_to(scratch.arena, scratch.pos, tag);
}

char *arena_format_statistics(Arena *arena, Arena *str_arena) {
  if (!arena || !str_arena) {
    return NULL;
  }

  char line_buffer[256];
  uint64_t total_len = 0;
  uint32_t num_tags = (uint32_t)ARENA_MEMORY_TAG_MAX;

  for (uint32_t i = 0; i < num_tags; ++i) {
    const char *tag_name =
        (i >= 0 && i < ARENA_MEMORY_TAG_MAX && ArenaMemoryTagNames[i])
            ? ArenaMemoryTagNames[i]
            : "INVALID_TAG_INDEX";
    uint64_t size_stat = arena->tags[i].size;
    uint32_t current_line_len = 0;

    if (size_stat < KB(1)) {
      current_line_len =
          snprintf(line_buffer, sizeof(line_buffer), "%s: %llu Bytes\n",
                   tag_name, (unsigned long long)size_stat);
    } else if (size_stat < MB(1)) {
      current_line_len =
          snprintf(line_buffer, sizeof(line_buffer), "%s: %.2f KB\n", tag_name,
                   (double)size_stat / KB(1));
    } else if (size_stat < GB(1)) {
      current_line_len =
          snprintf(line_buffer, sizeof(line_buffer), "%s: %.2f MB\n", tag_name,
                   (double)size_stat / MB(1));
    } else {
      current_line_len =
          snprintf(line_buffer, sizeof(line_buffer), "%s: %.2f GB\n", tag_name,
                   (double)size_stat / GB(1));
    }

    if (current_line_len > 0) {
      total_len += current_line_len;
    }
  }
  total_len += 1;

  char *result_str =
      (char *)arena_alloc(str_arena, total_len, ARENA_MEMORY_TAG_STRING);
  if (!result_str) {
    return NULL;
  }

  char *current_write_pos = result_str;
  uint64_t remaining_capacity = total_len;

  if (total_len == 1) {
    result_str[0] = '\0';
  } else {
    for (uint32_t i = 0; i < num_tags; ++i) {
      const char *tag_name =
          (i >= 0 && i < ARENA_MEMORY_TAG_MAX && ArenaMemoryTagNames[i])
              ? ArenaMemoryTagNames[i]
              : "INVALID_TAG_INDEX";
      uint64_t size_stat = arena->tags[i].size;
      uint32_t current_line_len = 0;

      if (size_stat < KB(1)) {
        current_line_len =
            snprintf(current_write_pos, remaining_capacity, "%s: %llu Bytes\n",
                     tag_name, (unsigned long long)size_stat);
      } else if (size_stat < MB(1)) {
        current_line_len =
            snprintf(current_write_pos, remaining_capacity, "%s: %.2f KB\n",
                     tag_name, (double)size_stat / KB(1));
      } else if (size_stat < GB(1)) {
        current_line_len =
            snprintf(current_write_pos, remaining_capacity, "%s: %.2f MB\n",
                     tag_name, (double)size_stat / MB(1));
      } else {
        current_line_len =
            snprintf(current_write_pos, remaining_capacity, "%s: %.2f GB\n",
                     tag_name, (double)size_stat / GB(1));
      }

      if (current_line_len > 0 &&
          (uint64_t)current_line_len < remaining_capacity) {
        current_write_pos += current_line_len;
        remaining_capacity -= current_line_len;
      } else {
        *current_write_pos = '\0';
        break;
      }
    }

    if (current_write_pos < result_str + total_len) {
      *current_write_pos = '\0';
    }
  }

  return result_str;
}
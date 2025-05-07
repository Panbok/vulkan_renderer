// clang-format off

/**
 * @file arena.h
 * @brief Defines a region-based memory allocator (arena allocator).
 *
 * An arena allocator manages a large region of virtual memory, committing
 * physical memory pages on demand as allocations are made. This provides
 * fast allocation speed (often just pointer bumping) and efficient bulk
 * deallocation (resetting the entire arena or parts of it).
 *
 * Memory Layout:
 * An arena consists of one or more contiguous blocks of virtual memory.
 * Each block has the following layout:
 *
 * +------------------+ <-- Block Start (Arena*)
 * |   Arena Header   |     (Contains metadata like sizes, links, positions)
 * | (sizeof(Arena))  |
 * +------------------+ <-- Start of allocatable memory ((char*)Arena +sizeof(Arena)) 
 * |                  | 
 * | Committed Memory |     (Memory backed by physical pages, usable for allocations) 
 * | (Arena->cmt)     | 
 * | Arena->pos       | ----> Current allocation high-water mark within this block
 * |                  |
 * +------------------+ <-- End of Committed Region ((char*)Arena + Arena->cmt)
 * |                  |
 * | Reserved but     |     (Virtual address space reserved, but not yet backed by physical pages) 
 * | NOT Committed    | 
 * | (Arena->rsv -    |
 * |     Arena->cmt)  |
 * |                  |
 * +------------------+ <-- End of Reserved Region ((char*)Arena + Arena->rsv)
 *
 * Linked Blocks:
 * If an allocation request exceeds the remaining capacity of the current block,
 * a new block is allocated (or reused from a free list) and linked to the
 * previous one. The `arena->current` pointer always points to the block
 * currently used for allocations.
 *
 *          Arena* arena (points to the *first* block metadata)
 *             |
 *             v
 *    Block 1 (Base: 0)      Block 2 (Base: B1->rsv)   Block N (Base: B_{N-1}->base + B_{N-1}->rsv)
 *    +--------------+        +--------------+          +---------------+ <--
 *    | Header       |        | Header       |          | Header        |
 *    | prev = NULL  |        | prev = B1    |          | prev = B_{N-1}|
 *    +--------------+        +--------------+          +---------------+
 *    | Memory       |        | Memory       |          | Memory        | -->
 *    | [... BN->pos]|        | [... B1->rsv]|          | [... B2->rsv] |
 *    | Allocations  |        |  happen here |          |  here         |
 *    +--------------+    +---|--------------+    +-----|---------------+
 *
 * Scratch Arenas:
 * A `Scratch` provides a mechanism for temporary allocations within an arena.
 * You create a scratch, perform allocations, and then destroy the scratch.
 * Destroying the scratch resets the arena's position back to where it was
 * when the scratch was created, effectively freeing all memory allocated
 * within the scratch's lifetime in O(1) time.
 *
 * Scratch Arenas are useful for:
 * - Short-lived allocations
 * - Temporary buffers
 * - Function-local allocations
 * - Any other case where you need to allocate memory without having to
 *   manually free it later.
 */

// clang-format on
#pragma once

#include "defines.h"
#include "platform.h"

#define ARENA_HEADER_SIZE 128
// Default size for reserved virtual address space per arena block.
#define ARENA_RSV_SIZE MB(64)
// Default size for initially committed memory per arena block. More is
// committed as needed.
#define ARENA_CMT_SIZE                                                         \
  KB(4) // Smaller initial commit can save physical memory if allocations are
        // small.

typedef struct Arena Arena;
/**
 * @brief Represents a block of memory within the arena allocator.
 * Arenas are managed as a singly linked list where `current` points to the
 * head (most recently added block used for allocation).
 */
typedef struct Arena {
  Arena *prev; /**< Pointer to the previous arena block in the linked list. NULL
                  for the first block. */
  Arena *current;    /**< Pointer to the *active* block for allocations (head of
                        the list). Only valid in the *first* block's header, NULL
                        otherwise. */
  uint64_t cmt_size; /**< The default commit chunk size for this block. */
  uint64_t rsv_size; /**< The default reserve size used when *creating*
                      subsequent blocks (if needed). */
  uint64_t base_pos; /**< The starting offset of this block relative to the
                      beginning of the *entire* arena (across all blocks). */
  uint64_t pos; /**< Current allocation offset *within this block*, relative to
                   the start of the block. Includes the header size. */
  uint64_t cmt; /**< Amount of memory committed *in this block*, relative to the
                   start of the block. */
  uint64_t rsv; /**< Total amount of memory reserved *for this block*, relative
                 to the start of the block. */
  uint64_t free_size; /**< Total reserved size of all blocks currently in the
                       free list. Only valid in the *first* block's header. */
  Arena *free_last;   /**< Pointer to the last block added to the free list
                         (LIFO). Only valid in the *first* block's header. */
} Arena;

/**
 * @brief Represents a temporary allocation scope within an arena.
 * Useful for short-lived allocations that can be freed all at once.
 */
typedef struct Scratch {
  Arena *arena; /**< The arena this scratch operates on. */
  uint64_t pos; /**< The absolute position in the arena when the scratch was
                 created. */
} Scratch;

/**
 * @brief Creates a new arena allocator.
 * Reserves `rsv_size` virtual memory and commits `cmt_size` initially.
 * Sizes are aligned up to the system's page size.
 * @param rsv_size The total virtual memory to reserve for the first block.
 * @param cmt_size The initial amount of physical memory to commit for the first
 * block.
 * @return Pointer to the initialized Arena structure (the first block). Exits
 * on failure.
 */
Arena *arena_create_internal(uint64_t rsv_size, uint64_t cmt_size);

/**
 * @brief Macro to create an arena with default or specified sizes.
 * If called with no arguments, uses `ARENA_RSV_SIZE` and `ARENA_CMT_SIZE`.
 * If called with two arguments (rsv_size, cmt_size), uses those values.
 */
#define ARENA_CREATE_RESOLVE(_1, _2, NAME, ...) NAME
#define arena_create(...)                                                      \
  ARENA_CREATE_RESOLVE(__VA_ARGS__, arena_create_internal(__VA_ARGS__),        \
                       arena_create_internal(ARENA_RSV_SIZE, ARENA_CMT_SIZE))

/**
 * @brief Destroys an arena and releases all associated memory blocks.
 * Iterates through all blocks linked from `arena->current` and releases
 * their reserved memory. Also releases memory held in the free list.
 * @param arena Pointer to the first block of the arena to destroy.
 */
void arena_destroy(Arena *arena);

/**
 * @brief Allocates memory from the arena.
 * Attempts to allocate `size` bytes from the `arena->current` block.
 * If the current block lacks sufficient committed space, it commits more.
 * If the current block lacks sufficient reserved space, it tries to reuse a
 * block from the free list or allocates a new block.
 * Allocation address is aligned up to `sizeof(void*)`.
 * @param arena Pointer to the arena.
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or NULL on failure (though failure
 * typically exits).
 */
void *arena_alloc(Arena *arena, uint64_t size);

/**
 * @brief Gets the current absolute position (high-water mark) in the arena.
 * This represents the total size allocated across all blocks so far.
 * @param arena Pointer to the arena.
 * @return The current absolute position offset.
 */
uint64_t arena_pos(Arena *arena);

/**
 * @brief Resets the arena's allocation position back to a specific absolute
 * position. Any allocations made after this position are effectively freed. If
 * the reset position falls within a previous block, subsequent blocks are moved
 * to the free list for potential reuse. Memory is decommitted (but not
 * released) in the freed blocks.
 *
 * @example Resetting moves blocks B2 and B3 to the free list.
 *    Before: arena->current -> B3 -> B2 -> B1
 *            arena->free_list -> NULL
 *
 *    After reset_to(pos_in_B1):
 *            arena->current -> B1
 *            arena->free_list -> B2 -> B3 -> NULL (or B3 -> B2 -> NULL)
 *
 * @param arena Pointer to the arena.
 * @param pos The absolute position to reset to. Clamped to be at least
 * sizeof(Arena).
 */
void arena_reset_to(Arena *arena, uint64_t pos);

/**
 * @brief Resets the arena completely, freeing all allocations.
 * Equivalent to `arena_reset_to(arena, 0)`. All blocks except the first
 * are moved to the free list.
 * @param arena Pointer to the arena.
 */
void arena_clear(Arena *arena);

/**
 * @brief Resets the arena by a relative amount.
 * Moves the current allocation position back by `amt` bytes.
 * Equivalent to `arena_reset_to(arena, arena_pos(arena) - amt)`.
 * @param arena Pointer to the arena.
 * @param amt The number of bytes to "free" from the end.
 */
void arena_reset(Arena *arena, uint64_t amt);

/**
 * @brief Begins a temporary allocation scope (scratch).
 * Records the current arena position.
 * @param arena Pointer to the arena to use for the scratch.
 * @return An initialized Scratch structure.
 */
Scratch scratch_create(Arena *arena);

/**
 * @brief Ends a temporary allocation scope (scratch).
 * Resets the arena back to the position recorded when the scratch was created,
 * effectively freeing all memory allocated since `scratch_create` was called.
 * @param scratch The Scratch structure to end.
 */
void scratch_destroy(Scratch scratch);
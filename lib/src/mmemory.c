#include "mmemory.h"
#include "platform.h"

// Struct to hold slot finding results
typedef struct SlotResult {
  uint64_t slot;    // The slot index if found
  bool32_t success; // Whether the operation was successful
} SlotResult;

static inline uint64_t round_up_to_page_size(uint64_t size,
                                             uint64_t page_size) {
  return ((size + page_size - 1) / page_size) * page_size;
}

static inline SlotResult find_or_grow_slot(MMemory *allocator) {
  assert_log(allocator != NULL, "allocator is NULL");

  SlotResult result = {0};
  result.success = false;

  for (uint64_t i = 0; i < allocator->capacity; i++) {
    if (!allocator->blocks[i].is_used) {
      result.slot = i;
      result.success = true;
      return result;
    }
  }

  uint64_t old_capacity = allocator->capacity;
  uint64_t new_capacity = old_capacity * 2;

  MBlock *new_blocks =
      (MBlock *)realloc(allocator->blocks, new_capacity * sizeof(MBlock));
  if (new_blocks == NULL) {
    return result;
  }

  allocator->blocks = new_blocks;
  allocator->capacity = new_capacity;

  for (uint64_t i = old_capacity; i < new_capacity; i++) {
    allocator->blocks[i].ptr = NULL;
    allocator->blocks[i].usr_size = 0;
    allocator->blocks[i].rsv_size = 0;
    allocator->blocks[i].is_used = false;
  }

  result.slot = old_capacity;
  result.success = true;
  return result;
}

static inline SlotResult find_slot_by_ptr(MMemory *allocator, void *ptr) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(ptr != NULL, "ptr is NULL");

  SlotResult result = {0};
  result.success = false;

  for (uint64_t i = 0; i < allocator->capacity; i++) {
    if (allocator->blocks[i].is_used && allocator->blocks[i].ptr == ptr) {
      result.slot = i;
      result.success = true;
      return result;
    }
  }

  return result;
}

bool32_t mmemory_create(uint64_t capacity, MMemory *out_allocator) {
  assert_log(out_allocator != NULL, "out_allocator is NULL");
  assert_log(capacity > 0, "capacity is not greater than 0");

  out_allocator->page_size = platform_get_page_size();
  if (out_allocator->page_size == 0) {
    return false;
  }

  out_allocator->blocks = (MBlock *)malloc(capacity * sizeof(MBlock));
  if (out_allocator->blocks == NULL) {
    return false;
  }

  out_allocator->capacity = capacity;
  out_allocator->count = 0;

  for (uint64_t i = 0; i < capacity; i++) {
    out_allocator->blocks[i].ptr = NULL;
    out_allocator->blocks[i].usr_size = 0;
    out_allocator->blocks[i].rsv_size = 0;
    out_allocator->blocks[i].is_used = false;
  }

  return true;
}

void mmemory_destroy(MMemory *allocator) {
  assert_log(allocator != NULL, "allocator is NULL");

  for (uint64_t i = 0; i < allocator->capacity; i++) {
    if (allocator->blocks[i].is_used && allocator->blocks[i].ptr != NULL) {
      platform_mem_release(allocator->blocks[i].ptr,
                           allocator->blocks[i].rsv_size);
    }
  }

  free(allocator->blocks);
  allocator->blocks = NULL;
  allocator->capacity = 0;
  allocator->count = 0;
  allocator->page_size = 0;
}

void *mmemory_alloc(MMemory *allocator, uint64_t size) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(size > 0, "size is not greater than 0");

  SlotResult slot_result = find_or_grow_slot(allocator);
  if (!slot_result.success) {
    return NULL;
  }

  uint64_t rsv_size = round_up_to_page_size(size, allocator->page_size);
  void *ptr = platform_mem_reserve(rsv_size);
  if (ptr == NULL) {
    return NULL;
  }

  if (!platform_mem_commit(ptr, size)) {
    platform_mem_release(ptr, rsv_size);
    return NULL;
  }

  MBlock *block = &allocator->blocks[slot_result.slot];
  block->ptr = ptr;
  block->usr_size = size;
  block->rsv_size = rsv_size;
  block->is_used = true;

  allocator->count++;

  return ptr;
}

bool32_t mmemory_free(MMemory *allocator, void *ptr) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(ptr != NULL, "ptr is NULL");

  SlotResult slot_result = find_slot_by_ptr(allocator, ptr);
  if (!slot_result.success) {
    return false;
  }

  MBlock *block = &allocator->blocks[slot_result.slot];

  platform_mem_release(block->ptr, block->rsv_size);

  block->is_used = false;
  block->ptr = NULL;
  block->usr_size = 0;
  block->rsv_size = 0;

  allocator->count--;

  return true;
}

void *mmemory_realloc(MMemory *allocator, void *old_ptr, uint64_t new_size) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(old_ptr != NULL, "old_ptr is NULL");
  assert_log(new_size > 0, "new_size is not greater than 0");

  SlotResult old_slot_result = find_slot_by_ptr(allocator, old_ptr);
  if (!old_slot_result.success) {
    return NULL;
  }

  MBlock *old_block = &allocator->blocks[old_slot_result.slot];
  uint64_t old_size = old_block->usr_size;
  uint64_t old_rsv_size = old_block->rsv_size;

  uint64_t new_rsv_size = round_up_to_page_size(new_size, allocator->page_size);
  if (old_rsv_size >= new_rsv_size) {
    return old_ptr;
  }

  void *new_ptr = mmemory_alloc(allocator, new_size);
  if (new_ptr == NULL) {
    return NULL;
  }

  uint64_t copy_size = old_size < new_size ? old_size : new_size;
  MemCopy(new_ptr, old_ptr, copy_size);

  mmemory_free(allocator, old_ptr);

  return new_ptr;
}

uint64_t mmemory_get_block_size(MMemory *allocator, void *ptr) {
  assert_log(allocator != NULL, "allocator is NULL");
  assert_log(ptr != NULL, "ptr is NULL");

  SlotResult slot_result = find_slot_by_ptr(allocator, ptr);
  if (!slot_result.success) {
    return 0;
  }

  return allocator->blocks[slot_result.slot].rsv_size;
}

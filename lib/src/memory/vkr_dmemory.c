#include "vkr_dmemory.h"
#include "containers/vkr_freelist.h"
#include "core/logger.h"
#include "defines.h"
#include "platform/vkr_platform.h"

vkr_internal INLINE uint64_t vkr_align_to_page(uint64_t size,
                                               uint64_t page_size) {
  return (size + page_size - 1) & ~(page_size - 1);
}

vkr_internal INLINE uint64_t vkr_choose_page_size(uint64_t total_size) {
  uint64_t large_page_size = vkr_platform_get_large_page_size();
  if (total_size >= large_page_size) {
    return large_page_size;
  }
  return vkr_platform_get_page_size();
}

bool8_t vkr_dmemory_create(uint64_t total_size, uint64_t max_reserve_size,
                           VkrDMemory *out_dmemory) {
  assert_log(out_dmemory != NULL, "Output dmemory must not be NULL");
  assert_log(total_size > 0, "Total size must be greater than 0");
  assert_log(max_reserve_size >= total_size,
             "Max reserve size must be >= total size");

  MemZero(out_dmemory, sizeof(VkrDMemory));

  out_dmemory->page_size = vkr_choose_page_size(total_size);

  uint64_t aligned_total_size =
      vkr_align_to_page(total_size, out_dmemory->page_size);
  uint64_t aligned_reserve_size =
      vkr_align_to_page(max_reserve_size, out_dmemory->page_size);

  out_dmemory->total_size = aligned_total_size;
  out_dmemory->reserve_size = aligned_reserve_size;

  void *base_memory = vkr_platform_mem_reserve(aligned_reserve_size);
  if (base_memory == NULL) {
    log_error("Failed to reserve %llu bytes of virtual memory",
              (uint64_t)aligned_reserve_size);
    return false_v;
  }
  out_dmemory->base_memory = base_memory;

  uint64_t freelist_memory_size =
      vkr_freelist_calculate_memory_requirement(aligned_total_size);
  uint64_t aligned_freelist_size =
      vkr_align_to_page(freelist_memory_size, out_dmemory->page_size);
  out_dmemory->freelist_memory_size = aligned_freelist_size;

  void *freelist_memory = vkr_platform_mem_reserve(aligned_freelist_size);
  if (freelist_memory == NULL) {
    log_error("Failed to reserve %llu bytes for freelist",
              (uint64_t)aligned_freelist_size);
    vkr_platform_mem_release(base_memory, aligned_reserve_size);
    return false_v;
  }

  if (!vkr_platform_mem_commit(freelist_memory, aligned_freelist_size)) {
    log_error("Failed to commit %llu bytes for freelist",
              (uint64_t)aligned_freelist_size);
    vkr_platform_mem_release(freelist_memory, aligned_freelist_size);
    vkr_platform_mem_release(base_memory, aligned_reserve_size);
    return false_v;
  }

  out_dmemory->freelist_memory = freelist_memory;

  if (!vkr_platform_mem_commit(base_memory, aligned_total_size)) {
    log_error("Failed to commit %llu bytes for base memory",
              (uint64_t)aligned_total_size);
    vkr_platform_mem_release(freelist_memory, aligned_freelist_size);
    vkr_platform_mem_release(base_memory, aligned_reserve_size);
    return false_v;
  }

  if (!vkr_freelist_create(freelist_memory, aligned_freelist_size,
                           aligned_total_size, &out_dmemory->freelist)) {
    log_error("Failed to create freelist");
    vkr_platform_mem_decommit(base_memory, aligned_total_size);
    vkr_platform_mem_release(freelist_memory, aligned_freelist_size);
    vkr_platform_mem_release(base_memory, aligned_reserve_size);
    return false_v;
  }

  out_dmemory->committed_size = aligned_total_size;

  return true_v;
}

void vkr_dmemory_destroy(VkrDMemory *dmemory) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");

  if (dmemory->base_memory != NULL) {
    if (dmemory->committed_size > 0) {
      vkr_platform_mem_decommit(dmemory->base_memory, dmemory->committed_size);
    }
    vkr_platform_mem_release(dmemory->base_memory, dmemory->reserve_size);
    dmemory->base_memory = NULL;
  }

  if (dmemory->freelist_memory != NULL) {
    vkr_freelist_destroy(&dmemory->freelist);
    vkr_platform_mem_release(dmemory->freelist_memory,
                             dmemory->freelist_memory_size);
    dmemory->freelist_memory = NULL;
  }

  MemZero(dmemory, sizeof(VkrDMemory));
}

void *vkr_dmemory_alloc(VkrDMemory *dmemory, uint64_t size) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");

  if (size > UINT64_MAX) {
    log_error("Allocation size too large: %llu", size);
    return NULL;
  }

  uint64_t offset = 0;
  if (!vkr_freelist_allocate(&dmemory->freelist, size, &offset)) {
    log_error("Failed to allocate %llu bytes from freelist", size);
    return NULL;
  }

  void *ptr = (void *)((uint8_t *)dmemory->base_memory + offset);
  return ptr;
}

bool8_t vkr_dmemory_free(VkrDMemory *dmemory, void *ptr, uint64_t size) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");
  assert_log(ptr != NULL, "Pointer must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");

  uintptr_t base_addr = (uintptr_t)dmemory->base_memory;
  uintptr_t end_addr = base_addr + (uintptr_t)dmemory->total_size;
  uintptr_t target_addr = (uintptr_t)ptr;

  if (target_addr < base_addr || target_addr >= end_addr) {
    log_error("Pointer out of range for this dmemory allocator");
    return false_v;
  }

  uint64_t offset =
      (uint64_t)((uint8_t *)ptr - (uint8_t *)dmemory->base_memory);
  if (offset > UINT32_MAX || size > UINT32_MAX) {
    log_error("Offset or size too large");
    return false_v;
  }

  if (!vkr_freelist_free(&dmemory->freelist, size, offset)) {
    log_error("Failed to free memory at offset %llu", (uint64_t)offset);
    return false_v;
  }

  return true_v;
}

uint64_t vkr_dmemory_get_free_space(VkrDMemory *dmemory) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");
  return vkr_freelist_free_space(&dmemory->freelist);
}

bool8_t vkr_dmemory_resize(VkrDMemory *dmemory, uint64_t new_total_size) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");

  if (new_total_size <= dmemory->total_size) {
    log_error(
        "Cannot resize: new size %llu must be greater than current size %llu",
        (uint64_t)new_total_size, (uint64_t)dmemory->total_size);
    return false_v;
  }

  uint64_t aligned_new_size =
      vkr_align_to_page(new_total_size, dmemory->page_size);

  if (aligned_new_size > dmemory->reserve_size) {
    log_error("Cannot resize: new size %llu exceeds reserved size %llu",
              (uint64_t)aligned_new_size, (uint64_t)dmemory->reserve_size);
    return false_v;
  }

  uint64_t free_space = vkr_dmemory_get_free_space(dmemory);
  uint64_t used_space = dmemory->total_size - free_space;

  if (new_total_size < used_space) {
    log_error(
        "Cannot resize: new size %llu is smaller than allocated space %llu",
        (uint64_t)new_total_size, (uint64_t)used_space);
    return false_v;
  }

  uint64_t old_total_size = dmemory->total_size;
  uint64_t additional_size = aligned_new_size - old_total_size;
  void *additional_start =
      (void *)((uint8_t *)dmemory->base_memory + old_total_size);

  if (!vkr_platform_mem_commit(additional_start, additional_size)) {
    log_error("Failed to commit additional %llu bytes",
              (uint64_t)additional_size);
    return false_v;
  }

  uint64_t new_freelist_memory_size =
      vkr_freelist_calculate_memory_requirement(aligned_new_size);
  uint64_t aligned_new_freelist_size =
      vkr_align_to_page(new_freelist_memory_size, dmemory->page_size);

  if (aligned_new_freelist_size > dmemory->freelist_memory_size) {
    void *new_freelist_memory =
        vkr_platform_mem_reserve(aligned_new_freelist_size);
    if (new_freelist_memory == NULL) {
      log_error("Failed to reserve %llu bytes for new freelist",
                (uint64_t)aligned_new_freelist_size);
      vkr_platform_mem_decommit(additional_start, additional_size);
      return false_v;
    }

    if (!vkr_platform_mem_commit(new_freelist_memory,
                                 aligned_new_freelist_size)) {
      log_error("Failed to commit %llu bytes for new freelist",
                (uint64_t)aligned_new_freelist_size);
      vkr_platform_mem_release(new_freelist_memory, aligned_new_freelist_size);
      vkr_platform_mem_decommit(additional_start, additional_size);
      return false_v;
    }

    void *old_freelist_memory = NULL;
    if (!vkr_freelist_resize(&dmemory->freelist, aligned_new_size,
                             new_freelist_memory, &old_freelist_memory)) {
      log_error("Failed to resize freelist");
      vkr_platform_mem_decommit(new_freelist_memory, aligned_new_freelist_size);
      vkr_platform_mem_release(new_freelist_memory, aligned_new_freelist_size);
      vkr_platform_mem_decommit(additional_start, additional_size);
      return false_v;
    }

    uint64_t old_freelist_size = dmemory->freelist_memory_size;
    vkr_platform_mem_decommit(old_freelist_memory, old_freelist_size);
    vkr_platform_mem_release(old_freelist_memory, old_freelist_size);

    dmemory->freelist_memory = new_freelist_memory;
    dmemory->freelist_memory_size = aligned_new_freelist_size;
  } else {
    dmemory->freelist.total_size = aligned_new_size;
    uint64_t growth_size = aligned_new_size - old_total_size;
    if (!vkr_freelist_free(&dmemory->freelist, growth_size, old_total_size)) {
      log_error("Failed to add new space to freelist after resize");
      vkr_platform_mem_decommit(additional_start, additional_size);
      dmemory->freelist.total_size = old_total_size;
      return false_v;
    }
  }

  dmemory->total_size = aligned_new_size;
  dmemory->committed_size = aligned_new_size;

  return true_v;
}

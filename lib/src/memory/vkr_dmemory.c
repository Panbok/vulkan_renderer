#include "vkr_dmemory.h"
#include "containers/vkr_freelist.h"
#include "core/logger.h"
#include "defines.h"
#include "platform/vkr_platform.h"

typedef struct VkrDMemoryAllocHeader {
  uint64_t offset;       // Offset from base memory where the block starts
  uint64_t request_size; // Total size reserved in the freelist for this block
  uint64_t user_size;    // Size requested by the caller
  uint64_t alignment;    // Effective alignment used for the allocation
} VkrDMemoryAllocHeader;

vkr_internal INLINE uint64_t vkr_dmemory_metadata_size(void) {
  // Ensure header size is aligned to its natural alignment so the header
  // location remains aligned when placed immediately before the user pointer.
  return AlignPow2(sizeof(VkrDMemoryAllocHeader),
                   AlignOf(VkrDMemoryAllocHeader));
}

vkr_internal INLINE uint64_t vkr_dmemory_min_alignment(void) {
  uint64_t min_alignment = AlignOf(void *);
  if (AlignOf(uint64_t) > min_alignment) {
    min_alignment = AlignOf(uint64_t);
  }
  if (AlignOf(VkrDMemoryAllocHeader) > min_alignment) {
    min_alignment = AlignOf(VkrDMemoryAllocHeader);
  }
  return min_alignment;
}

vkr_internal INLINE uint64_t vkr_dmemory_normalize_alignment(
    uint64_t alignment) {
  uint64_t eff_alignment = alignment;
  uint64_t min_alignment = vkr_dmemory_min_alignment();

  if (eff_alignment == 0) {
    eff_alignment = min_alignment;
  }

  assert_log((eff_alignment & (eff_alignment - 1)) == 0,
             "Alignment must be a power of two");

  if (eff_alignment < min_alignment) {
    eff_alignment = min_alignment;
  }

  return eff_alignment;
}

vkr_internal INLINE bool8_t vkr_dmemory_validate_range(VkrDMemory *dmemory,
                                                       void *ptr,
                                                       uint64_t metadata_size) {
  uintptr_t base_addr = (uintptr_t)dmemory->base_memory;
  uintptr_t end_addr = base_addr + (uintptr_t)dmemory->total_size;
  uintptr_t target_addr = (uintptr_t)ptr;

  if (target_addr < base_addr + metadata_size || target_addr >= end_addr) {
    log_error("Pointer out of range for this dmemory allocator");
    return false_v;
  }

  return true_v;
}

vkr_internal INLINE VkrDMemoryAllocHeader *
vkr_dmemory_header_from_ptr(VkrDMemory *dmemory, void *ptr) {
  uint64_t metadata_size = vkr_dmemory_metadata_size();

  if (!vkr_dmemory_validate_range(dmemory, ptr, metadata_size)) {
    return NULL;
  }

  VkrDMemoryAllocHeader *header =
      (VkrDMemoryAllocHeader *)((uint8_t *)ptr - metadata_size);

  if (header->request_size == 0) {
    log_error("Invalid dmemory header: request_size is zero");
    return NULL;
  }

  if (header->offset + header->request_size > dmemory->total_size) {
    log_error("Invalid dmemory header: block exceeds allocator size");
    return NULL;
  }

  return header;
}

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

  uint64_t overhead_slack =
      vkr_dmemory_metadata_size() + vkr_dmemory_min_alignment();
  uint64_t total_with_overhead = total_size + overhead_slack;
  assert_log(total_with_overhead >= total_size,
             "Total size overflow with overhead");

  MemZero(out_dmemory, sizeof(VkrDMemory));

  out_dmemory->page_size = vkr_choose_page_size(total_size);

  uint64_t aligned_total_size =
      vkr_align_to_page(total_with_overhead, out_dmemory->page_size);
  uint64_t reserve_with_overhead = max_reserve_size + overhead_slack;
  if (reserve_with_overhead < max_reserve_size) {
    log_error("Reserve size overflow with overhead");
    return false_v;
  }
  uint64_t aligned_reserve_size =
      vkr_align_to_page(reserve_with_overhead, out_dmemory->page_size);

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

  return vkr_dmemory_alloc_aligned(dmemory, size, 0);
}

void *vkr_dmemory_alloc_aligned(VkrDMemory *dmemory, uint64_t size,
                                uint64_t alignment) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");
  assert_log(size > 0, "Size must be greater than 0");

  uint64_t eff_alignment = vkr_dmemory_normalize_alignment(alignment);

  uint64_t metadata_size = vkr_dmemory_metadata_size();
  uint64_t request_size = size + eff_alignment + metadata_size;
  if (request_size < size) {
    log_error("Overflow when calculating aligned allocation size");
    return NULL;
  }

  uint64_t offset = 0;
  if (!vkr_freelist_allocate(&dmemory->freelist, request_size, &offset)) {
    log_error("Failed to allocate %llu bytes (aligned request size %llu) from "
              "freelist",
              (uint64_t)size, (uint64_t)request_size);
    return NULL;
  }

  uint64_t aligned_offset = AlignPow2(offset + metadata_size, eff_alignment);
  uint64_t aligned_end = aligned_offset + size;
  uint64_t allocation_end = offset + request_size;

  // Sanity check to ensure the aligned region fits in the reserved block.
  if (aligned_end > allocation_end) {
    log_error("Aligned allocation does not fit in reserved block");
    vkr_freelist_free(&dmemory->freelist, request_size, offset);
    return NULL;
  }

  uint8_t *aligned_ptr = (uint8_t *)dmemory->base_memory + aligned_offset;
  VkrDMemoryAllocHeader *header =
      (VkrDMemoryAllocHeader *)(aligned_ptr - metadata_size);

  header->offset = offset;
  header->request_size = request_size;
  header->user_size = size;
  header->alignment = eff_alignment;

  return aligned_ptr;
}

vkr_internal INLINE bool8_t
vkr_dmemory_free_internal(VkrDMemory *dmemory, void *ptr,
                          uint64_t provided_size,
                          uint64_t provided_alignment) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");
  assert_log(ptr != NULL, "Pointer must not be NULL");

  VkrDMemoryAllocHeader *header = vkr_dmemory_header_from_ptr(dmemory, ptr);
  if (header == NULL) {
    return false_v;
  }

  if (provided_size > 0 && provided_size != header->user_size) {
    log_warn("dmemory free size mismatch: provided=%llu, stored=%llu",
             (uint64_t)provided_size, (uint64_t)header->user_size);
  }

  if (provided_alignment > 0 &&
      provided_alignment != header->alignment) {
    log_warn("dmemory free alignment mismatch: provided=%llu, stored=%llu",
             (uint64_t)provided_alignment, (uint64_t)header->alignment);
  }

  if (!vkr_freelist_free(&dmemory->freelist, header->request_size,
                         header->offset)) {
    log_error("Failed to free memory at offset %llu", header->offset);
    return false_v;
  }

  return true_v;
}

bool8_t vkr_dmemory_free(VkrDMemory *dmemory, void *ptr, uint64_t size) {
  return vkr_dmemory_free_internal(dmemory, ptr, size, 0);
}

bool8_t vkr_dmemory_free_aligned(VkrDMemory *dmemory, void *ptr, uint64_t size,
                                 uint64_t alignment) {
  return vkr_dmemory_free_internal(dmemory, ptr, size, alignment);
}

void *vkr_dmemory_realloc(VkrDMemory *dmemory, void *ptr, uint64_t new_size,
                          uint64_t alignment) {
  assert_log(dmemory != NULL, "DMemory must not be NULL");

  if (new_size == 0 && ptr == NULL) {
    return NULL;
  }

  if (ptr == NULL) {
    return vkr_dmemory_alloc_aligned(dmemory, new_size, alignment);
  }

  if (new_size == 0) {
    vkr_dmemory_free_internal(dmemory, ptr, 0, alignment);
    return NULL;
  }

  VkrDMemoryAllocHeader *header = vkr_dmemory_header_from_ptr(dmemory, ptr);
  if (header == NULL) {
    return NULL;
  }

  uint64_t target_alignment = header->alignment;
  if (alignment != 0) {
    target_alignment = vkr_dmemory_normalize_alignment(alignment);
    if (target_alignment < header->alignment) {
      target_alignment = header->alignment;
    }
  }

  void *new_ptr =
      vkr_dmemory_alloc_aligned(dmemory, new_size, target_alignment);
  if (new_ptr == NULL) {
    return NULL;
  }

  uint64_t copy_size = new_size < header->user_size ? new_size : header->user_size;
  MemCopy(new_ptr, ptr, copy_size);

  vkr_dmemory_free_internal(dmemory, ptr, header->user_size,
                            header->alignment);

  return new_ptr;
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

  uint64_t overhead_slack =
      vkr_dmemory_metadata_size() + vkr_dmemory_min_alignment();
  uint64_t total_with_overhead = new_total_size + overhead_slack;
  if (total_with_overhead < new_total_size) {
    log_error("Overflow when calculating new total size with overhead");
    return false_v;
  }

  uint64_t aligned_new_size =
      vkr_align_to_page(total_with_overhead, dmemory->page_size);

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

bool8_t vkr_dmemory_owns_ptr(const VkrDMemory *dmemory, void *ptr) {
  if (dmemory == NULL || ptr == NULL || dmemory->base_memory == NULL) {
    return false_v;
  }

  uintptr_t base = (uintptr_t)dmemory->base_memory;
  uintptr_t end = base + (uintptr_t)dmemory->reserve_size;
  uintptr_t p = (uintptr_t)ptr;
  return p >= base && p < end;
}

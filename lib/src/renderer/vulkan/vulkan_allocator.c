#include "vulkan_allocator.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "memory/arena.h"

vkr_internal INLINE uint64_t
vulkan_allocator_effective_alignment(size_t alignment) {
  uint64_t eff_alignment =
      (alignment == 0) ? (uint64_t)AlignOf(void *) : (uint64_t)alignment;

  // Vulkan guarantees power-of-two alignment, but normalize defensively.
  if ((eff_alignment & (eff_alignment - 1)) != 0) {
    uint64_t highest_bit = 1ULL
                           << (64 - VkrCountLeadingZeros64(eff_alignment - 1));
    eff_alignment = highest_bit;
  }

  return eff_alignment;
}

vkr_internal INLINE VkrAllocator *
vulkan_allocator_select(VulkanAllocator *allocator,
                        VkSystemAllocationScope scope) {
  if (scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) {
    return &allocator->arena_allocator;
  }
  return &allocator->allocator;
}

vkr_internal VKAPI_PTR void *
vulkan_allocator_allocate(void *pUserData, size_t size, size_t alignment,
                          VkSystemAllocationScope scope) {
  VulkanAllocator *allocator = (VulkanAllocator *)pUserData;
  if (allocator == NULL || size == 0) {
    return NULL;
  }

  uint64_t eff_alignment = vulkan_allocator_effective_alignment(alignment);
  VkrAllocator *target = vulkan_allocator_select(allocator, scope);
  void *result = vkr_allocator_alloc_aligned_ts(
      target, (uint64_t)size, eff_alignment, VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
      allocator->mutex);
  if (result != NULL && scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) {
    vkr_atomic_uint32_fetch_add(&allocator->arena_alloc_count, 1,
                                VKR_MEMORY_ORDER_ACQ_REL);
  }
  return result;
}

vkr_internal VKAPI_PTR void *
vulkan_allocator_reallocate(void *pUserData, void *pOriginal, size_t size,
                            size_t alignment, VkSystemAllocationScope scope) {
  VulkanAllocator *allocator = (VulkanAllocator *)pUserData;
  if (allocator == NULL) {
    return NULL;
  }

  // Free-only path when size == 0 as per Vulkan spec.
  if (size == 0) {
    if (pOriginal != NULL) {
      VulkanAllocationSource src =
          vulkan_allocator_source_from_ptr(allocator, pOriginal);
      VkrAllocator *target = (src == VULKAN_ALLOCATION_SOURCE_ARENA)
                                 ? &allocator->arena_allocator
                                 : &allocator->allocator;
      vkr_allocator_free_ts(target, pOriginal, 0,
                            VKR_ALLOCATOR_MEMORY_TAG_VULKAN, allocator->mutex);
      if (src == VULKAN_ALLOCATION_SOURCE_ARENA) {
        // fetch_sub returns previous value; if it was 1, new value is 0.
        uint32_t prev_count = vkr_atomic_uint32_fetch_sub(
            &allocator->arena_alloc_count, 1, VKR_MEMORY_ORDER_ACQ_REL);
        if (prev_count == 1) {
          arena_clear(allocator->arena, ARENA_MEMORY_TAG_VULKAN);
        }
      }
    }
    return NULL;
  }

  uint64_t eff_alignment = vulkan_allocator_effective_alignment(alignment);
  if (pOriginal == NULL) {
    VkrAllocator *target = vulkan_allocator_select(allocator, scope);
    void *result = vkr_allocator_alloc_aligned_ts(
        target, (uint64_t)size, eff_alignment, VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
        allocator->mutex);
    if (result != NULL && scope == VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) {
      vkr_atomic_uint32_fetch_add(&allocator->arena_alloc_count, 1,
                                  VKR_MEMORY_ORDER_ACQ_REL);
    }
    return result;
  }

  VulkanAllocationSource src =
      vulkan_allocator_source_from_ptr(allocator, pOriginal);
  VkrAllocator *target = (src == VULKAN_ALLOCATION_SOURCE_ARENA)
                             ? &allocator->arena_allocator
                             : &allocator->allocator;
  return vkr_allocator_realloc_aligned_ts(
      target, pOriginal, 0, (uint64_t)size, eff_alignment,
      VKR_ALLOCATOR_MEMORY_TAG_VULKAN, allocator->mutex);
}

vkr_internal VKAPI_PTR void vulkan_allocator_free(void *pUserData,
                                                  void *pMemory) {
  VulkanAllocator *allocator = (VulkanAllocator *)pUserData;
  if (allocator == NULL || pMemory == NULL) {
    return;
  }

  VulkanAllocationSource src =
      vulkan_allocator_source_from_ptr(allocator, pMemory);
  VkrAllocator *target = (src == VULKAN_ALLOCATION_SOURCE_ARENA)
                             ? &allocator->arena_allocator
                             : &allocator->allocator;
  vkr_allocator_free_ts(target, pMemory, 0, VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
                        allocator->mutex);
  if (src == VULKAN_ALLOCATION_SOURCE_ARENA) {
    // fetch_sub returns previous value; if it was 1, new value is 0.
    uint32_t prev_count = vkr_atomic_uint32_fetch_sub(
        &allocator->arena_alloc_count, 1, VKR_MEMORY_ORDER_ACQ_REL);
    if (prev_count == 1) {
      arena_clear(allocator->arena, ARENA_MEMORY_TAG_VULKAN);
    }
  }
}

vkr_internal VKAPI_PTR void
vulkan_allocator_internal_allocation(void *pUserData, size_t size,
                                     VkInternalAllocationType type,
                                     VkSystemAllocationScope scope) {
#if VKR_ALLOCATOR_DISABLE_STATS
  (void)pUserData;
  (void)size;
  (void)type;
  (void)scope;
#else
  VulkanAllocator *allocator = (VulkanAllocator *)pUserData;
  (void)type;
  if (allocator == NULL || size == 0) {
    return;
  }

  VkrAllocator *target = vulkan_allocator_select(allocator, scope);
  vkr_allocator_report(target, (uint64_t)size, VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
                       true_v);
#endif // VKR_ALLOCATOR_DISABLE_STATS
}

vkr_internal VKAPI_PTR void
vulkan_allocator_internal_free(void *pUserData, size_t size,
                               VkInternalAllocationType type,
                               VkSystemAllocationScope scope) {
#if VKR_ALLOCATOR_DISABLE_STATS
  (void)pUserData;
  (void)size;
  (void)type;
  (void)scope;
#else
  VulkanAllocator *allocator = (VulkanAllocator *)pUserData;
  (void)type;
  if (allocator == NULL || size == 0) {
    return;
  }

  VkrAllocator *target = vulkan_allocator_select(allocator, scope);
  vkr_allocator_report(target, (uint64_t)size, VKR_ALLOCATOR_MEMORY_TAG_VULKAN,
                       false_v);
#endif // VKR_ALLOCATOR_DISABLE_STATS
}

bool32_t vulkan_allocator_create(VkrAllocator *host_allocator,
                                 VulkanAllocator *out_allocator,
                                 uint64_t commit_size, uint64_t reserve_size) {
  assert_log(host_allocator != NULL, "Host allocator must not be NULL");
  assert_log(out_allocator != NULL, "Out allocator must not be NULL");
  assert_log(commit_size > 0, "Commit size must be greater than 0");
  assert_log(reserve_size >= commit_size,
             "Reserve size must be >= commit size");

  MemZero(out_allocator, sizeof(VulkanAllocator));

  if (!vkr_dmemory_create(commit_size, reserve_size, &out_allocator->dmemory)) {
    log_error("Failed to create Vulkan dmemory allocator (commit=%llu, "
              "reserve=%llu)",
              (unsigned long long)commit_size,
              (unsigned long long)reserve_size);
    return false_v;
  }

  out_allocator->allocator.ctx = &out_allocator->dmemory;
  vkr_dmemory_allocator_create(&out_allocator->allocator);

  out_allocator->arena = arena_create(VKR_VULKAN_ALLOCATOR_ARENA_RESERVE,
                                      VKR_VULKAN_ALLOCATOR_ARENA_COMMIT);
  if (!out_allocator->arena) {
    log_error("Failed to create Vulkan allocator arena");
    vkr_dmemory_destroy(&out_allocator->dmemory);
    return false_v;
  }
  out_allocator->arena_allocator.ctx = out_allocator->arena;
  vkr_allocator_arena(&out_allocator->arena_allocator);

  if (!vkr_mutex_create(host_allocator, &out_allocator->mutex)) {
    log_error("Failed to create mutex for Vulkan allocator");
    arena_destroy(out_allocator->arena);
    vkr_dmemory_destroy(&out_allocator->dmemory);
    MemZero(out_allocator, sizeof(VulkanAllocator));
    return false_v;
  }

  out_allocator->callbacks = (VkAllocationCallbacks){
      .pUserData = out_allocator,
      .pfnAllocation = vulkan_allocator_allocate,
      .pfnReallocation = vulkan_allocator_reallocate,
      .pfnFree = vulkan_allocator_free,
      .pfnInternalAllocation = vulkan_allocator_internal_allocation,
      .pfnInternalFree = vulkan_allocator_internal_free,
  };

  return true_v;
}

void vulkan_allocator_destroy(VkrAllocator *host_allocator,
                              VulkanAllocator *allocator) {
  if (allocator == NULL) {
    return;
  }

  if (allocator->arena != NULL) {
    arena_destroy(allocator->arena);
  }

  if (allocator->allocator.ctx != NULL) {
    vkr_dmemory_allocator_destroy(&allocator->allocator);
  } else if (allocator->dmemory.base_memory != NULL) {
    vkr_dmemory_destroy(&allocator->dmemory);
  }

  if (allocator->mutex != NULL) {
    if (!vkr_mutex_destroy(host_allocator, &allocator->mutex)) {
      log_error("Failed to destroy Vulkan allocator mutex");
    }
  }

  MemZero(allocator, sizeof(VulkanAllocator));
}

VkAllocationCallbacks *vulkan_allocator_callbacks(VulkanAllocator *allocator) {
  if (allocator == NULL) {
    return NULL;
  }
  return &allocator->callbacks;
}

VulkanAllocationSource
vulkan_allocator_source_from_ptr(VulkanAllocator *allocator, void *ptr) {
  if (allocator == NULL || ptr == NULL) {
    return VULKAN_ALLOCATION_SOURCE_UNKNOWN;
  }
  if (vkr_dmemory_owns_ptr(&allocator->dmemory, ptr)) {
    return VULKAN_ALLOCATION_SOURCE_DMEMORY;
  }
  if (allocator->arena && arena_owns_ptr(allocator->arena, ptr)) {
    return VULKAN_ALLOCATION_SOURCE_ARENA;
  }
  return VULKAN_ALLOCATION_SOURCE_UNKNOWN;
}

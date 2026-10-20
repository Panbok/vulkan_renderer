#include "vkr_pipeline_registry.h"

#include "containers/str.h"
#include "memory/vkr_heap.h"

bool8_t vkr_pipeline_registry_create(VkrAllocator *allocator,
                                     VkrPipelineRegistry *out_registry) {
  vkr_assert(allocator != NULL && out_registry != NULL);
  MemZero(out_registry, sizeof(*out_registry));
  out_registry->allocator = allocator;
  return vkr_mutex_create(allocator, &out_registry->lock);
}

void vkr_pipeline_registry_destroy(VkrPipelineRegistry *registry) {
  if (!registry || !registry->allocator) {
    return;
  }
  vkr_heap_free(registry->entries);
  vkr_heap_free(registry->stats);
  if (registry->lock) {
    (void)vkr_mutex_destroy(registry->allocator, &registry->lock);
  }
  MemZero(registry, sizeof(*registry));
}

/* Grows `*block` of `*capacity` items of `size` bytes to hold `needed`,
   doubling, at most to `limit`; false when it cannot. */
vkr_internal bool8_t vkr_pipeline_registry_grow(void **block,
                                                uint32_t *capacity,
                                                uint32_t needed, uint64_t size,
                                                uint32_t limit) {
  if (needed <= *capacity) {
    return true_v;
  }
  if (needed > limit) {
    return false_v;
  }
  uint32_t grown = Max(*capacity * 2u, 64u);
  grown = Min(Max(grown, needed), limit);
  void *moved = vkr_heap_realloc(*block, size * (uint64_t)grown);
  if (!moved) {
    return false_v;
  }
  *block = moved;
  *capacity = grown;
  return true_v;
}

void vkr_pipeline_registry_add(VkrPipelineRegistry *registry,
                               const VkrPipelineInfo *info,
                               const VkrPipelineStat *stats,
                               uint32_t stat_count) {
  if (!registry || !registry->lock || !info) {
    return;
  }
  stat_count = stats ? Min(stat_count, VKR_PIPELINE_STATS_PER_ENTRY) : 0u;
  (void)vkr_mutex_lock(registry->lock);
  if (!vkr_pipeline_registry_grow((void **)&registry->entries,
                                  &registry->capacity, registry->count + 1u,
                                  sizeof(*registry->entries),
                                  VKR_PIPELINE_REGISTRY_MAX)) {
    ++registry->dropped;
    (void)vkr_mutex_unlock(registry->lock);
    return;
  }
  /* Statistics that no longer fit are left out; the entry stays. */
  if (stat_count > 0u &&
      !vkr_pipeline_registry_grow(
          (void **)&registry->stats, &registry->stat_capacity,
          registry->stat_count + stat_count, sizeof(*registry->stats),
          VKR_PIPELINE_REGISTRY_STATS_MAX)) {
    stat_count = 0u;
  }
  VkrPipelineRegistryEntry *entry = &registry->entries[registry->count++];
  entry->info = *info;
  entry->info.name[VKR_PIPELINE_NAME_MAX] = '\0';
  entry->stat_first = registry->stat_count;
  entry->stat_count = stat_count;
  if (stat_count > 0u) {
    MemCopy(registry->stats + registry->stat_count, stats,
            sizeof(*stats) * (uint64_t)stat_count);
    registry->stat_count += stat_count;
  }
  (void)vkr_mutex_unlock(registry->lock);
}

uint32_t vkr_pipeline_registry_visit(VkrPipelineRegistry *registry,
                                     uint32_t first,
                                     VkrPipelineRegistryVisit visit,
                                     void *context) {
  if (!registry || !registry->lock) {
    return 0u;
  }
  (void)vkr_mutex_lock(registry->lock);
  const uint32_t count = registry->count;
  for (uint32_t i = first; visit && i < count; ++i) {
    const VkrPipelineRegistryEntry *entry = &registry->entries[i];
    visit(context, i, &entry->info,
          entry->stat_count ? registry->stats + entry->stat_first : NULL,
          entry->stat_count);
  }
  (void)vkr_mutex_unlock(registry->lock);
  return count;
}

void vkr_pipeline_registry_name(char *out, uint32_t capacity,
                                const char *name) {
  if (!out || capacity == 0u) {
    return;
  }
  string_format(out, capacity, "%s", name ? name : "");
}

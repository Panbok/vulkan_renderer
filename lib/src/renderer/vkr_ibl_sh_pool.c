#include "renderer/vkr_ibl_sh_pool.h"

_Static_assert(VKR_SH_SLOT_BYTES == 112u,
               "SH slot must stay seven float4 vectors");
_Static_assert(VKR_SH_BUFFER_BYTES == 4144u,
               "SH coefficient buffer must stay 37 slots of 112 bytes");
_Static_assert(VKR_SH_LOGICAL_MAX == 2u + VKR_SCENE_REFLECTION_PROBE_MAX,
               "SH logical maximum is the fallback, the scene environment, and "
               "every reflection probe");

void vkr_ibl_sh_pool_init(VkrShSlotPool *pool) {
  MemZero(pool, sizeof(*pool));
  // Push descending so the first reserve hands out slot 1, keeping the sentinel
  // adjacent to the first live generation and making traces readable.
  for (uint32_t slot = VKR_SH_SLOT_CAPACITY; slot > 1u; --slot) {
    pool->free_stack[pool->free_count++] = slot - 1u;
  }
}

vkr_internal bool8_t vkr_ibl_sh_pool_slot_is_reusable(uint32_t slot) {
  return slot > VKR_SH_SLOT_BLACK && slot < VKR_SH_SLOT_CAPACITY;
}

VkrShPoolStatus vkr_ibl_sh_pool_reserve(VkrShSlotPool *pool,
                                        uint32_t *out_slot) {
  if (!pool || !out_slot) {
    return VKR_SH_POOL_STATUS_INVALID_ARGUMENT;
  }
  if (pool->free_count == 0u) {
    pool->metrics.exhaustion_count++;
    return VKR_SH_POOL_STATUS_EXHAUSTED;
  }
  const uint32_t slot = pool->free_stack[--pool->free_count];
  pool->state[slot] = VKR_SH_SLOT_STATE_RESERVED;
  // A reserved slot carries no reader until a frame that references it is
  // actually submitted.
  pool->last_reader_serial[slot] = 0u;
  pool->retire_serial[slot] = 0u;
  *out_slot = slot;
  return VKR_SH_POOL_STATUS_OK;
}

VkrShPoolStatus vkr_ibl_sh_pool_mark_recorded(VkrShSlotPool *pool,
                                              uint32_t slot) {
  if (!pool || !vkr_ibl_sh_pool_slot_is_reusable(slot)) {
    return VKR_SH_POOL_STATUS_INVALID_ARGUMENT;
  }
  if (pool->state[slot] != VKR_SH_SLOT_STATE_RESERVED) {
    return VKR_SH_POOL_STATUS_INVALID_STATE;
  }
  pool->state[slot] = VKR_SH_SLOT_STATE_RECORDED;
  return VKR_SH_POOL_STATUS_OK;
}

VkrShPoolStatus vkr_ibl_sh_pool_abandon(VkrShSlotPool *pool, uint32_t slot) {
  if (!pool || !vkr_ibl_sh_pool_slot_is_reusable(slot)) {
    return VKR_SH_POOL_STATUS_INVALID_ARGUMENT;
  }
  const VkrShSlotState state = pool->state[slot];
  if (state != VKR_SH_SLOT_STATE_RESERVED &&
      state != VKR_SH_SLOT_STATE_RECORDED) {
    return VKR_SH_POOL_STATUS_INVALID_STATE;
  }
  // Abandonment is only reachable before submission, so no reader can exist and
  // the slot is immediately reusable.
  pool->state[slot] = VKR_SH_SLOT_STATE_FREE;
  pool->free_stack[pool->free_count++] = slot;
  pool->metrics.abandon_count++;
  return VKR_SH_POOL_STATUS_OK;
}

VkrShPoolStatus vkr_ibl_sh_pool_publish(VkrShSlotPool *pool, uint32_t slot) {
  if (!pool || !vkr_ibl_sh_pool_slot_is_reusable(slot)) {
    return VKR_SH_POOL_STATUS_INVALID_ARGUMENT;
  }
  if (pool->state[slot] != VKR_SH_SLOT_STATE_RECORDED) {
    return VKR_SH_POOL_STATUS_INVALID_STATE;
  }
  pool->state[slot] = VKR_SH_SLOT_STATE_PUBLISHED;
  pool->metrics.publish_count++;
  return VKR_SH_POOL_STATUS_OK;
}

void vkr_ibl_sh_pool_reference(VkrShSlotPool *pool, uint32_t slot,
                               uint64_t submit_serial) {
  if (!pool || !vkr_ibl_sh_pool_slot_is_reusable(slot)) {
    return;
  }
  const VkrShSlotState state = pool->state[slot];
  // A RECORDED slot can legitimately be read by the same submission that bakes
  // it, so it accrues readers before it is published.
  if (state != VKR_SH_SLOT_STATE_PUBLISHED &&
      state != VKR_SH_SLOT_STATE_RECORDED) {
    return;
  }
  pool->last_reader_serial[slot] =
      Max(pool->last_reader_serial[slot], submit_serial);
}

VkrShPoolStatus vkr_ibl_sh_pool_retire(VkrShSlotPool *pool, uint32_t slot) {
  if (!pool || !vkr_ibl_sh_pool_slot_is_reusable(slot)) {
    return VKR_SH_POOL_STATUS_INVALID_ARGUMENT;
  }
  const VkrShSlotState state = pool->state[slot];
  if (state != VKR_SH_SLOT_STATE_PUBLISHED &&
      state != VKR_SH_SLOT_STATE_RECORDED) {
    return VKR_SH_POOL_STATUS_INVALID_STATE;
  }
  pool->state[slot] = VKR_SH_SLOT_STATE_RETIRED;
  pool->retire_serial[slot] = pool->last_reader_serial[slot];
  return VKR_SH_POOL_STATUS_OK;
}

uint32_t vkr_ibl_sh_pool_collect(VkrShSlotPool *pool,
                                 uint64_t completed_submit_serial) {
  if (!pool) {
    return 0u;
  }
  uint32_t collected = 0u;
  for (uint32_t slot = 1u; slot < VKR_SH_SLOT_CAPACITY; ++slot) {
    if (pool->state[slot] != VKR_SH_SLOT_STATE_RETIRED ||
        pool->retire_serial[slot] > completed_submit_serial) {
      continue;
    }
    pool->state[slot] = VKR_SH_SLOT_STATE_FREE;
    pool->last_reader_serial[slot] = 0u;
    pool->retire_serial[slot] = 0u;
    pool->free_stack[pool->free_count++] = slot;
    collected++;
  }
  pool->metrics.collect_count += collected;
  return collected;
}

void vkr_ibl_sh_pool_get_metrics(const VkrShSlotPool *pool,
                                 VkrShPoolMetrics *out_metrics) {
  *out_metrics = pool->metrics;
  out_metrics->free_count = 0u;
  out_metrics->reserved_count = 0u;
  out_metrics->recorded_count = 0u;
  out_metrics->published_count = 0u;
  out_metrics->retired_count = 0u;
  for (uint32_t slot = 1u; slot < VKR_SH_SLOT_CAPACITY; ++slot) {
    switch (pool->state[slot]) {
    case VKR_SH_SLOT_STATE_FREE:
      out_metrics->free_count++;
      break;
    case VKR_SH_SLOT_STATE_RESERVED:
      out_metrics->reserved_count++;
      break;
    case VKR_SH_SLOT_STATE_RECORDED:
      out_metrics->recorded_count++;
      break;
    case VKR_SH_SLOT_STATE_PUBLISHED:
      out_metrics->published_count++;
      break;
    case VKR_SH_SLOT_STATE_RETIRED:
      out_metrics->retired_count++;
      break;
    }
  }
}

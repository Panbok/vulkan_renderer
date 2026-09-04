#pragma once

#include "renderer/systems/vkr_scene_system.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_packet.h"

/*
 * Bounded copy-on-write pool of L2 coefficient slots (ADR-038).
 *
 * Slot 0 is an immutable zeroed black sentinel: it is never reserved,
 * rewritten, or retired, and it is the valid result for a source with no
 * projection or a failed one. The remaining slots are reused only after their
 * last reader's submit serial is proven complete, so publishing a new
 * generation never overwrites storage a submitted frame may still read.
 *
 * Capacity holds an old and a replacement generation for every logical source
 * that can be live at once. That is a capacity argument, not permission to
 * assume a fixed number of frames in flight — reuse always waits on the actual
 * recorded serial.
 *
 * **Threading: single-threaded by contract**, matching VkrGpuSlotTable. Every
 * entry point mutates plain state and plain counters with no synchronization.
 * All of these are cold-path calls made from the render thread.
 */

typedef enum VkrShSlotState {
  /** Reusable and owned by nobody. */
  VKR_SH_SLOT_STATE_FREE = 0,
  /** Claimed by a projection that has not recorded its dispatch yet. */
  VKR_SH_SLOT_STATE_RESERVED,
  /** Its projection dispatch is in a command buffer that is not submitted. */
  VKR_SH_SLOT_STATE_RECORDED,
  /** Committed as a logical source's current coefficients. */
  VKR_SH_SLOT_STATE_PUBLISHED,
  /** Replaced or released, waiting on its last reader to complete. */
  VKR_SH_SLOT_STATE_RETIRED,
} VkrShSlotState;

typedef enum VkrShPoolStatus {
  VKR_SH_POOL_STATUS_OK = 0,
  VKR_SH_POOL_STATUS_INVALID_ARGUMENT,
  /** No free slot. The caller keeps its prior publication, or black. */
  VKR_SH_POOL_STATUS_EXHAUSTED,
  VKR_SH_POOL_STATUS_INVALID_STATE,
} VkrShPoolStatus;

/** Cold-path counters for the reload gate. Never read in a per-pixel path. */
typedef struct VkrShPoolMetrics {
  uint32_t free_count;
  uint32_t reserved_count;
  uint32_t recorded_count;
  uint32_t published_count;
  uint32_t retired_count;
  uint64_t exhaustion_count;
  uint64_t publish_count;
  uint64_t abandon_count;
  uint64_t collect_count;
} VkrShPoolMetrics;

typedef struct VkrShSlotPool {
  VkrShSlotState state[VKR_SH_SLOT_CAPACITY];
  /** Greatest submit serial that referenced the slot while it was readable. */
  uint64_t last_reader_serial[VKR_SH_SLOT_CAPACITY];
  /** Serial a retired slot must see completed before it returns to FREE. */
  uint64_t retire_serial[VKR_SH_SLOT_CAPACITY];
  uint32_t free_stack[VKR_SH_REUSABLE_SLOTS];
  uint32_t free_count;
  VkrShPoolMetrics metrics;
} VkrShSlotPool;

/** Resets every reusable slot to FREE. Slot 0 stays the black sentinel. */
void vkr_ibl_sh_pool_init(VkrShSlotPool *pool);

/**
 * Claims a FREE slot without waiting. Returns VKR_SH_POOL_STATUS_EXHAUSTED and
 * leaves `out_slot` untouched when none is available; exhaustion is a cold-path
 * error and must never become a wait inside a successful frame.
 */
VkrShPoolStatus vkr_ibl_sh_pool_reserve(VkrShSlotPool *pool,
                                        uint32_t *out_slot);

/** RESERVED -> RECORDED, once the projection dispatch is in a command buffer.
 */
VkrShPoolStatus vkr_ibl_sh_pool_mark_recorded(VkrShSlotPool *pool,
                                              uint32_t slot);

/**
 * Returns a pre-submit slot to FREE after recording or submission failed. Only
 * valid before the recording command buffer is submitted; a submitted slot must
 * go through retirement instead.
 */
VkrShPoolStatus vkr_ibl_sh_pool_abandon(VkrShSlotPool *pool, uint32_t slot);

/** RECORDED -> PUBLISHED, only after the recording submission succeeded. */
VkrShPoolStatus vkr_ibl_sh_pool_publish(VkrShSlotPool *pool, uint32_t slot);

/**
 * Registers that `submit_serial` reads `slot`. Slot 0 and unpublished slots are
 * ignored so callers can pass a packet's slots unconditionally.
 */
void vkr_ibl_sh_pool_reference(VkrShSlotPool *pool, uint32_t slot,
                               uint64_t submit_serial);

/**
 * PUBLISHED or RECORDED -> RETIRED. Does not clear the slot's bytes: a
 * submitted frame may still be reading them.
 */
VkrShPoolStatus vkr_ibl_sh_pool_retire(VkrShSlotPool *pool, uint32_t slot);

/** Returns retired slots whose last reader serial has completed to FREE. */
uint32_t vkr_ibl_sh_pool_collect(VkrShSlotPool *pool,
                                 uint64_t completed_submit_serial);

void vkr_ibl_sh_pool_get_metrics(const VkrShSlotPool *pool,
                                 VkrShPoolMetrics *out_metrics);

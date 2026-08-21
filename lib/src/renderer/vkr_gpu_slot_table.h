#pragma once

#include "defines.h"

/**
 * Fixed-capacity generation-checked slot table over a caller-owned, GPU-visible
 * row array. Backs both descriptor heaps and material tables on either backend.
 *
 * **Threading: single-threaded by contract.** Every entry point mutates plain
 * table state and plain metric counters with no synchronization. The renderer
 * satisfies this by publishing only from the render thread — asset publication
 * is pumped from inside `prepare_frame` (see `renderer_frontend.c`), never from
 * a loader or job-system worker. Calling any of these from a second thread is a
 * data race. If that ever changes, convert the slot state, generation, and
 * metric counters to `core/vkr_atomic.h` types; a bare fence is not sufficient
 * and none is used here.
 *
 * Publication order: a virgin table hands out slot 0 first, then 1, 2, ... The
 * sentinel contract in the GPU slot-table design depends on that. Slots freed
 * by
 * `collect` are reused in LIFO order, so index reuse after a retirement
 * is not ascending — no caller may depend on which free index is returned.
 */
typedef struct VkrGpuSlotTable VkrGpuSlotTable;

typedef enum VkrGpuSlotStatus {
  VKR_GPU_SLOT_STATUS_OK = 0,
  VKR_GPU_SLOT_STATUS_INVALID_ARGUMENT,
  VKR_GPU_SLOT_STATUS_CAPACITY_EXHAUSTED,
  VKR_GPU_SLOT_STATUS_RETIREMENT_CAPACITY_EXHAUSTED,
  VKR_GPU_SLOT_STATUS_STALE_HANDLE,
} VkrGpuSlotStatus;

typedef struct VkrGpuSlotHandle {
  uint32_t index;
  uint32_t generation;
} VkrGpuSlotHandle;

typedef struct VkrGpuSlotTableConfig {
  uint32_t max_slots;
  uint32_t max_retirements;
  uint32_t row_size;
} VkrGpuSlotTableConfig;

typedef struct VkrGpuSlotTableMetrics {
  uint64_t slots_live;
  uint64_t slots_retired;
  uint64_t slots_retirements;
  uint64_t slots_peak;
  uint64_t slots_capacity;
  uint64_t slots_published;
  uint64_t slots_replaced;
  uint64_t slots_collected;
  uint64_t capacity_failures;
  uint64_t retirement_capacity_failures;
  uint64_t stale_handle_failures;
} VkrGpuSlotTableMetrics;

uint64_t
vkr_gpu_slot_table_storage_requirement(const VkrGpuSlotTableConfig *config);

VkrGpuSlotStatus vkr_gpu_slot_table_create(const VkrGpuSlotTableConfig *config,
                                           void *storage, uint64_t storage_size,
                                           void *mapped_rows,
                                           VkrGpuSlotTable **out_table);

VkrGpuSlotStatus vkr_gpu_slot_table_publish(VkrGpuSlotTable *table,
                                            const void *row,
                                            VkrGpuSlotHandle *out_handle);
VkrGpuSlotStatus vkr_gpu_slot_table_replace(VkrGpuSlotTable *table,
                                            VkrGpuSlotHandle old_handle,
                                            const void *new_row,
                                            uint64_t old_last_use_submit_value,
                                            VkrGpuSlotHandle *out_new_handle);
/**
 * Checks whether a live handle can be retired without mutating table state or
 * failure metrics. Use this to preflight a logical resource whose publication
 * spans multiple slot tables before retiring any constituent slot.
 */
VkrGpuSlotStatus vkr_gpu_slot_table_can_retire(const VkrGpuSlotTable *table,
                                               VkrGpuSlotHandle handle);
VkrGpuSlotStatus vkr_gpu_slot_table_retire(VkrGpuSlotTable *table,
                                           VkrGpuSlotHandle handle,
                                           uint64_t last_use_submit_value);
VkrGpuSlotStatus vkr_gpu_slot_table_resolve(VkrGpuSlotTable *table,
                                            VkrGpuSlotHandle handle,
                                            uint32_t *out_slot_index);
VkrGpuSlotStatus vkr_gpu_slot_table_collect(VkrGpuSlotTable *table,
                                            uint64_t completed_submit_value,
                                            uint32_t *out_collected_count);
void vkr_gpu_slot_table_get_metrics(const VkrGpuSlotTable *table,
                                    VkrGpuSlotTableMetrics *out_metrics);

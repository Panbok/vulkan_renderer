#pragma once

#include "core/vkr_atomic.h"

/* Progress and cancellation of a long depot step (snapshot, upload, fetch,
 * checkout, clone), shared between the thread that runs the step and the
 * thread that shows it. The step writes the counters; any thread reads them
 * with vkr_depot_progress_read and may ask the step to stop with
 * vkr_depot_progress_cancel. A step checks the flag between files and
 * objects, stops with false and leaves the working copy valid: a later run
 * of the same step completes.
 *
 * Totals can grow while a step runs: a fetch learns what it lacks as trees
 * and file manifests arrive. A total of zero means unknown. */

typedef enum VkrDepotPhase {
  VKR_DEPOT_PHASE_IDLE = 0,
  VKR_DEPOT_PHASE_CONNECTING,
  /* Hashing changed files: files and bytes to hash. */
  VKR_DEPOT_PHASE_SNAPSHOT,
  /* Objects the server lacks and their stored bytes. */
  VKR_DEPOT_PHASE_UPLOAD,
  /* Objects this store lacks and the decoded bytes of their chunks. */
  VKR_DEPOT_PHASE_FETCH,
  /* Files to write and their sizes. */
  VKR_DEPOT_PHASE_CHECKOUT,
  VKR_DEPOT_PHASE_DONE,
} VkrDepotPhase;

typedef struct VkrDepotProgress {
  VkrAtomicUint32 phase; /**< VkrDepotPhase. */
  VkrAtomicUint64 files_done;
  VkrAtomicUint64 files_total;
  VkrAtomicUint64 bytes_done;
  VkrAtomicUint64 bytes_total;
  VkrAtomicBool cancel;
} VkrDepotProgress;

/* A plain copy of the counters. */
typedef struct VkrDepotProgressView {
  VkrDepotPhase phase;
  uint64_t files_done;
  uint64_t files_total;
  uint64_t bytes_done;
  uint64_t bytes_total;
  bool8_t canceled;
} VkrDepotProgressView;

/* Clears the counters and the cancel flag; call before starting a step. */
static inline void vkr_depot_progress_reset(VkrDepotProgress *progress) {
  vkr_atomic_uint32_store(&progress->phase, VKR_DEPOT_PHASE_IDLE,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->files_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->files_total, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->bytes_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->bytes_total, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_bool_store(&progress->cancel, false_v, VKR_MEMORY_ORDER_RELEASE);
}

static inline void vkr_depot_progress_cancel(VkrDepotProgress *progress) {
  vkr_atomic_bool_store(&progress->cancel, true_v, VKR_MEMORY_ORDER_RELEASE);
}

/* False for a NULL progress. */
static inline bool8_t
vkr_depot_progress_canceled(const VkrDepotProgress *progress) {
  return progress &&
         vkr_atomic_bool_load(&progress->cancel, VKR_MEMORY_ORDER_ACQUIRE);
}

static inline VkrDepotProgressView
vkr_depot_progress_read(const VkrDepotProgress *progress) {
  return (VkrDepotProgressView){
      .phase = (VkrDepotPhase)vkr_atomic_uint32_load(&progress->phase,
                                                     VKR_MEMORY_ORDER_RELAXED),
      .files_done = vkr_atomic_uint64_load(&progress->files_done,
                                           VKR_MEMORY_ORDER_RELAXED),
      .files_total = vkr_atomic_uint64_load(&progress->files_total,
                                            VKR_MEMORY_ORDER_RELAXED),
      .bytes_done = vkr_atomic_uint64_load(&progress->bytes_done,
                                           VKR_MEMORY_ORDER_RELAXED),
      .bytes_total = vkr_atomic_uint64_load(&progress->bytes_total,
                                            VKR_MEMORY_ORDER_RELAXED),
      .canceled = vkr_depot_progress_canceled(progress),
  };
}

/* Starts `phase` with its totals and zero done; a NULL progress ignores it. */
static inline void vkr_depot_progress_phase(VkrDepotProgress *progress,
                                            VkrDepotPhase phase,
                                            uint64_t files_total,
                                            uint64_t bytes_total) {
  if (!progress) {
    return;
  }
  vkr_atomic_uint64_store(&progress->files_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->bytes_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->files_total, files_total,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&progress->bytes_total, bytes_total,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&progress->phase, (uint32_t)phase,
                          VKR_MEMORY_ORDER_RELEASE);
}

/* Adds to the done counters; a NULL progress ignores it. */
static inline void vkr_depot_progress_add(VkrDepotProgress *progress,
                                          uint64_t files, uint64_t bytes) {
  if (!progress) {
    return;
  }
  (void)vkr_atomic_uint64_fetch_add(&progress->files_done, files,
                                    VKR_MEMORY_ORDER_RELAXED);
  (void)vkr_atomic_uint64_fetch_add(&progress->bytes_done, bytes,
                                    VKR_MEMORY_ORDER_RELAXED);
}

/* Adds to the totals; a NULL progress ignores it. */
static inline void vkr_depot_progress_add_total(VkrDepotProgress *progress,
                                                uint64_t files,
                                                uint64_t bytes) {
  if (!progress) {
    return;
  }
  (void)vkr_atomic_uint64_fetch_add(&progress->files_total, files,
                                    VKR_MEMORY_ORDER_RELAXED);
  (void)vkr_atomic_uint64_fetch_add(&progress->bytes_total, bytes,
                                    VKR_MEMORY_ORDER_RELAXED);
}

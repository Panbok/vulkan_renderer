/**
 * @file vkr_job_system.h
 * @brief Lightweight prioritized job system with type-masked workers.
 *
 * Features:
 * - Priorities (LOW/NORMAL/HIGH) to balance latency-sensitive work.
 * - Job type masks (Bitset8) so workers can opt into GENERAL/RESOURCE/GPU jobs.
 * - Chained/dependent jobs via dependency tracking.
 * - Per-job success/failure callbacks executed on the worker thread.
 * - Worker-local arenas/scratch to avoid allocator contention.
 *
 * The API is intentionally minimal; integration into loaders can submit decode
 * jobs as RESOURCE and enqueue GPU follow-ups as GPU jobs.
 */
#pragma once

#include "containers/array.h"
#include "containers/bitset.h"
#include "containers/queue.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"

/**
 * @brief Priority classes used for scheduling.
 */
typedef enum VkrJobPriority {
  VKR_JOB_PRIORITY_LOW = 0,
  VKR_JOB_PRIORITY_NORMAL = 1,
  VKR_JOB_PRIORITY_HIGH = 2,
  VKR_JOB_PRIORITY_MAX = 3
} VkrJobPriority;

/**
 * @brief Bitset flags describing job categories workers may accept.
 */
typedef enum JobType {
  VKR_JOB_TYPE_GENERAL = 1 << 0,
  VKR_JOB_TYPE_RESOURCE = 1 << 1,
  VKR_JOB_TYPE_GPU = 1 << 2
} VkrJobType;

/**
 * @brief Opaque handle to a job entry (slot id + generation for safety).
 */
typedef struct VkrJobHandle {
  uint32_t id;
  uint32_t generation;
} VkrJobHandle;

Queue(VkrJobHandle);
Array(VkrJobHandle);

/**
 * @brief Per-job context available to run/callback functions.
 */
typedef struct VkrJobContext {
  struct VkrJobSystem *system;
  uint32_t worker_index;
  VkrThreadId thread_id;
  VkrAllocator *allocator; /**< Temporary allocator for the job */
  VkrAllocatorScope scope; /**< Scope controlling temp allocations */
} VkrJobContext;

typedef bool8_t (*VkrJobRunFn)(VkrJobContext *ctx, void *payload);
typedef void (*VkrJobCallbackFn)(VkrJobContext *ctx, void *payload);

/**
 * @brief Description used when submitting a job.
 */
typedef struct VkrJobDesc {
  VkrJobPriority priority;
  Bitset8 type_mask;
  VkrJobRunFn run;
  VkrJobCallbackFn on_success;
  VkrJobCallbackFn on_failure;
  const void *payload;
  uint32_t payload_size;
  const VkrJobHandle *dependencies;
  uint32_t dependency_count;
  bool8_t defer_enqueue;
} VkrJobDesc;

/**
 * @brief Configuration for initializing the job system.
 */
typedef struct VkrJobSystemConfig {
  uint32_t worker_count;
  uint32_t max_jobs;
  uint32_t queue_capacity;
  uint64_t arena_rsv_size;
  uint64_t arena_cmt_size;
  Bitset8 worker_type_mask_default;
} VkrJobSystemConfig;

typedef struct VkrJobSystemMetrics {
  uint32_t queue_depth;
  uint32_t busy_workers;
  uint32_t worker_count;
  uint64_t jobs_completed_total;
} VkrJobSystemMetrics;

typedef void (*VkrJobForFn)(void *context, uint32_t index);

/* Parallel loops (vkr_job_for_begin) running at once, each holding one
   block until its last worker lets go. */
#define VKR_JOB_FOR_MAX 8u

/* One parallel loop's shared progress: the next index to take and those
   finished. The loop's caller and each worker sent to help hold a
   reference; the block is reused only once all have let go. */
typedef struct VkrJobFor {
  VkrAtomicUint32 references;
  VkrAtomicUint32 next;
  VkrAtomicUint32 finished;
  uint32_t count;
  VkrJobForFn run;
  void *context;
} VkrJobFor;

/**
 * @brief Job system state.
 */
typedef struct VkrJobSystem {
  Arena *arena;
  VkrAllocator allocator;
  VkrMutex mutex;
  /* Idle workers wait here for queued work. */
  VkrCondVar cond;
  /* vkr_job_wait callers wait here for a slot to be recycled; completions
     broadcast it only while `done_waiters` is nonzero. */
  VkrCondVar done_cond;
  uint32_t done_waiters;
  VkrCondVar slots_avail;
  bool32_t running;

  uint32_t worker_count;
  struct VkrJobWorker *workers;

  Queue_VkrJobHandle queues[VKR_JOB_PRIORITY_MAX];

  struct VkrJobSlot *slots;
  uint32_t max_jobs;

  uint32_t *free_stack;
  uint32_t free_top;

  VkrJobFor fors[VKR_JOB_FOR_MAX];

  VkrAtomicUint32 metrics_queue_depth;
  VkrAtomicUint32 metrics_busy_workers;
  VkrAtomicUint64 metrics_jobs_completed;
} VkrJobSystem;

/**
 * @brief Build a default configuration.
 * @return The default configuration.
 */
VkrJobSystemConfig vkr_job_system_config_default();

/**
 * @brief Initialize the job system with the provided configuration.
 * @param config The configuration to use.
 * @param out_system The job system to initialize.
 * @return True if the job system was initialized successfully, false otherwise.
 */
bool8_t vkr_job_system_init(const VkrJobSystemConfig *config,
                            VkrJobSystem *out_system);

/**
 * @brief Shutdown the job system and free associated resources.
 * @param system The job system to shutdown.
 */
void vkr_job_system_shutdown(VkrJobSystem *system);
void vkr_job_system_get_metrics(const VkrJobSystem *system,
                                VkrJobSystemMetrics *out_metrics);

/**
 * @brief Submit a job for execution.
 *
 * This call can block while waiting for a free job slot.
 * @param system The job system to submit the job to.
 * @param desc The description of the job to submit.
 * @param out_handle The handle to the submitted job.
 * @return True if the job was submitted successfully, false otherwise.
 */
bool8_t vkr_job_submit(VkrJobSystem *system, const VkrJobDesc *desc,
                       VkrJobHandle *out_handle);

/**
 * @brief Attempt to submit a job without waiting for a free slot.
 *
 * Returns immediately when no slot is available or submission fails.
 * @param system The job system to submit the job to.
 * @param desc The description of the job to submit.
 * @param out_handle The handle to the submitted job (valid only on success).
 * @return True if the job was submitted successfully, false otherwise.
 */
bool8_t vkr_job_try_submit(VkrJobSystem *system, const VkrJobDesc *desc,
                           VkrJobHandle *out_handle);

/**
 * @brief Mark a pending job as ready for execution. Needed when submission was
 * deferred via VkrJobDesc::defer_enqueue.
 * @return True if the job was queued or already queued, false otherwise.
 */
bool8_t vkr_job_mark_ready(VkrJobSystem *system, VkrJobHandle handle);

/**
 * @brief Block until the given job completes.
 * @param system The job system to wait for the job in.
 * @param handle The handle to the job to wait for.
 * @return True if the job completed successfully, false otherwise.
 */
bool8_t vkr_job_wait(VkrJobSystem *system, VkrJobHandle handle);

/* Starts running `run(context, index)` for every index below `count` on
   idle workers and returns at once. Without workers or a free block it runs
   them all here first and returns NULL, which counts as done. `run` must
   allow different indices at once, and `context` must live until
   vkr_job_for_end. */
VkrJobFor *vkr_job_for_begin(VkrJobSystem *system, uint32_t count,
                             VkrJobForFn run, void *context);

/* Whether every index of `loop` has run. */
bool8_t vkr_job_for_done(const VkrJobFor *loop);

/* Runs here every index of `loop` no worker took, waits for those workers
   are running, and lets go of the loop. A worker that starts later finds no
   index left, so a busy queue costs no more than running them here. */
void vkr_job_for_end(VkrJobFor *loop);

vkr_internal INLINE Bitset8 vkr_job_type_mask_general_and_resource(void) {
  Bitset8 mask = bitset8_create();
  bitset8_set(&mask, VKR_JOB_TYPE_GENERAL);
  bitset8_set(&mask, VKR_JOB_TYPE_RESOURCE);
  return mask;
}

vkr_internal INLINE Bitset8 vkr_job_type_mask_all(void) {
  Bitset8 mask = bitset8_create();
  bitset8_set(&mask, VKR_JOB_TYPE_GENERAL);
  bitset8_set(&mask, VKR_JOB_TYPE_RESOURCE);
  bitset8_set(&mask, VKR_JOB_TYPE_GPU);
  return mask;
}

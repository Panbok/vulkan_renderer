#include "core/vkr_job_system.h"
#include "core/logger.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"

typedef enum JobState {
  JOB_STATE_FREE = 0,
  JOB_STATE_PENDING,
  JOB_STATE_QUEUED,
  JOB_STATE_RUNNING,
  JOB_STATE_COMPLETED
} JobState;

typedef struct VkrJobSlot {
  VkrJobHandle handle;
  JobState state;
  VkrJobPriority priority;
  Bitset8 type_mask;
  VkrJobRunFn run;
  VkrJobCallbackFn on_success;
  VkrJobCallbackFn on_failure;
  void *payload;
  uint32_t payload_size;
  uint32_t payload_capacity;
  uint32_t remaining_dependencies;
  bool8_t defer_enqueue;
  Vector_VkrJobHandle dependents;
  bool8_t success;
} VkrJobSlot;

typedef struct VkrJobWorker {
  struct VkrJobSystem *system;
  VkrThread thread;
  Arena *arena;
  VkrAllocator allocator;
  Bitset8 type_mask;
  uint32_t index;
} VkrJobWorker;

vkr_internal INLINE bool8_t job_handle_is_valid(VkrJobHandle handle) {
  return handle.id != 0 && handle.generation != 0;
}

vkr_internal VkrJobSlot *job_system_get_slot(VkrJobSystem *system,
                                             VkrJobHandle handle) {
  if (!system || !job_handle_is_valid(handle)) {
    return NULL;
  }

  uint32_t idx = handle.id - 1;
  if (idx >= system->max_jobs) {
    return NULL;
  }

  VkrJobSlot *slot = &system->slots[idx];
  if (slot->handle.generation != handle.generation) {
    return NULL;
  }

  return slot;
}

vkr_internal bool8_t job_system_enqueue_locked(VkrJobSystem *system,
                                               VkrJobSlot *slot) {
  assert_log(system != NULL, "VkrJobSystem is NULL");
  assert_log(slot != NULL, "VkrJobSlot is NULL");

  if (slot->state == JOB_STATE_RUNNING || slot->state == JOB_STATE_COMPLETED) {
    return false_v;
  }

  Queue_VkrJobHandle *queue = &system->queues[slot->priority];
  if (queue_is_full_VkrJobHandle(queue)) {
    return false_v;
  }

  VkrJobHandle handle = slot->handle;
  slot->state = JOB_STATE_QUEUED;
  return queue_enqueue_VkrJobHandle(queue, handle);
}

vkr_internal bool8_t job_system_register_dependency_locked(
    VkrJobSystem *system, VkrJobSlot *child, VkrJobHandle dependency) {
  assert_log(system != NULL, "VkrJobSystem is NULL");

  if (child == NULL) {
    return false_v;
  }

  if (!job_handle_is_valid(dependency)) {
    return false_v;
  }

  uint32_t idx = dependency.id - 1;
  if (idx >= system->max_jobs) {
    return false_v;
  }

  VkrJobSlot *parent = &system->slots[idx];
  if (parent->handle.generation > dependency.generation) {
    return true_v;
  }

  if (parent->handle.generation != dependency.generation) {
    return false_v;
  }

  if (parent == child) {
    return false_v;
  }

  // Already satisfied.
  if (parent->state == JOB_STATE_COMPLETED) {
    return true_v;
  }

  child->remaining_dependencies++;
  vector_push_VkrJobHandle(&parent->dependents, child->handle);
  return true_v;
}

vkr_internal bool8_t job_system_try_dequeue_locked(VkrJobSystem *system,
                                                   Bitset8 worker_mask,
                                                   VkrJobHandle *out_handle) {
  assert_log(system != NULL, "VkrJobSystem is NULL");
  assert_log(out_handle != NULL, "VkrJobHandle out pointer is NULL");

  for (int32_t p = VKR_JOB_PRIORITY_HIGH; p >= VKR_JOB_PRIORITY_LOW; p--) {
    Queue_VkrJobHandle *queue = &system->queues[p];
    uint64_t attempts = queue->size;
    for (uint64_t i = 0; i < attempts; i++) {
      VkrJobHandle handle;
      if (!queue_dequeue_VkrJobHandle(queue, &handle)) {
        break;
      }

      VkrJobSlot *slot = job_system_get_slot(system, handle);
      if (!slot || slot->state != JOB_STATE_QUEUED) {
        continue;
      }

      if (slot->remaining_dependencies > 0) {
        queue_enqueue_VkrJobHandle(queue, handle);
        continue;
      }

      if ((bitset8_get_value(&slot->type_mask) &
           bitset8_get_value(&worker_mask)) == 0) {
        // Not compatible; rotate to back.
        queue_enqueue_VkrJobHandle(queue, handle);
        continue;
      }

      slot->state = JOB_STATE_RUNNING;
      *out_handle = handle;
      return true_v;
    }
  }

  return false_v;
}

vkr_internal void job_slot_reset(VkrJobSystem *system, VkrJobSlot *slot) {
  assert_log(system != NULL, "VkrJobSystem is NULL");

  if (slot == NULL) {
    return;
  }

  slot->state = JOB_STATE_FREE;
  slot->run = NULL;
  slot->on_success = NULL;
  slot->on_failure = NULL;
  slot->priority = VKR_JOB_PRIORITY_NORMAL;
  slot->type_mask = bitset8_create();
  slot->payload_size = 0;
  slot->remaining_dependencies = 0;
  slot->defer_enqueue = false_v;
  slot->success = false_v;

  if (slot->dependents.data != NULL) {
    slot->dependents.length = 0;
  }
}

vkr_internal void job_worker_complete(VkrJobSystem *system, VkrJobSlot *slot,
                                      VkrJobHandle handle, VkrJobContext *ctx,
                                      bool8_t success) {
  assert_log(system != NULL, "VkrJobSystem is NULL");
  assert_log(slot != NULL, "VkrJobSlot is NULL");
  assert_log(job_handle_is_valid(handle), "VkrJobHandle is invalid");
  assert_log(ctx != NULL, "VkrJobContext is NULL");

  VkrJobCallbackFn callback = success ? slot->on_success : slot->on_failure;
  void *payload = slot->payload;

  vkr_mutex_lock(system->mutex);
  slot->state = JOB_STATE_COMPLETED;
  slot->success = success;

  // Wake waiters for this job.
  vkr_cond_broadcast(system->cond);

  // Release dependents
  if (slot->dependents.data != NULL) {
    for (uint64_t i = 0; i < slot->dependents.length; i++) {
      VkrJobHandle child_handle = slot->dependents.data[i];
      VkrJobSlot *child = job_system_get_slot(system, child_handle);
      if (!child || child->state == JOB_STATE_FREE ||
          child->state == JOB_STATE_COMPLETED) {
        continue;
      }
      if (child->remaining_dependencies > 0) {
        child->remaining_dependencies--;
        if (child->remaining_dependencies == 0 &&
            child->state == JOB_STATE_PENDING) {
          if (!job_system_enqueue_locked(system, child)) {
            log_warn("Job failed to enqueue dependency child job");
          }
        }
      }
    }
  }

  vkr_cond_broadcast(system->cond);
  vkr_mutex_unlock(system->mutex);

  // Run callback outside the lock to avoid blocking other workers.
  if (callback) {
    callback(ctx, payload);
  }

  vkr_mutex_lock(system->mutex);
  // Recycle slot
  slot->handle.generation++;
  system->free_stack[system->free_top++] = handle.id - 1;
  job_slot_reset(system, slot);
  // Signal waiting submitters that a slot is available
  vkr_cond_signal(system->slots_avail);
  // Wake any waiters so they see the generation has changed (job fully done)
  vkr_cond_broadcast(system->cond);
  vkr_mutex_unlock(system->mutex);
}

vkr_internal void *job_worker_thread(void *param) {
  assert_log(param != NULL, "VkrJobWorker is NULL");

  VkrJobWorker *worker = (VkrJobWorker *)param;
  VkrJobSystem *system = worker->system;

  while (true) {
    vkr_mutex_lock(system->mutex);
    VkrJobHandle handle = {0};
    while (system->running &&
           !job_system_try_dequeue_locked(system, worker->type_mask, &handle)) {
      vkr_cond_wait(system->cond, system->mutex);
    }

    if (!system->running) {
      vkr_mutex_unlock(system->mutex);
      break;
    }

    VkrJobSlot *slot = job_system_get_slot(system, handle);
    if (!slot || slot->state != JOB_STATE_RUNNING) {
      vkr_mutex_unlock(system->mutex);
      continue;
    }
    vkr_mutex_unlock(system->mutex);

    VkrAllocator *scratch_alloc = &worker->allocator;
    VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_alloc);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      job_worker_complete(system, slot, handle, &(VkrJobContext){0}, false_v);
      continue;
    }
    VkrJobContext ctx = {.system = system,
                         .worker_index = worker->index,
                         .thread_id = vkr_thread_current_id(),
                         .allocator = scratch_alloc,
                         .scope = scope};

    bool8_t success = false_v;
    if (slot->run) {
      success = slot->run(&ctx, slot->payload);
    }

    job_worker_complete(system, slot, handle, &ctx, success);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }

  return NULL;
}

VkrJobSystemConfig vkr_job_system_config_default() {
  VkrJobSystemConfig cfg = {0};
  cfg.worker_count = vkr_platform_get_logical_core_count();
  if (cfg.worker_count > 0) {
    cfg.worker_count -= 1; // leave one for main thread
  }

  if (cfg.worker_count == 0) {
    cfg.worker_count = 1;
  }

  // With texture deduplication in place, we no longer need to limit jobs
  // to avoid sampler overflow. Use higher values for better parallelism.
  cfg.max_jobs = 4096;
  cfg.queue_capacity = 4096;
  cfg.arena_rsv_size = MB(8);
  cfg.arena_cmt_size = MB(2);
  cfg.worker_type_mask_default = vkr_job_type_mask_all();
  return cfg;
}

bool8_t vkr_job_system_init(const VkrJobSystemConfig *config,
                            VkrJobSystem *out_system) {
  assert_log(out_system != NULL, "JobSystem out pointer is NULL");
  assert_log(config != NULL, "JobSystem config is NULL");
  assert_log(config->worker_count > 0, "worker_count must be > 0");
  assert_log(config->max_jobs > 0, "max_jobs must be > 0");
  assert_log(config->queue_capacity > 0, "queue_capacity must be > 0");

  MemZero(out_system, sizeof(VkrJobSystem));

  out_system->arena =
      arena_create(config->arena_rsv_size, config->arena_cmt_size);
  if (!out_system->arena) {
    return false_v;
  }

  out_system->allocator = (VkrAllocator){.ctx = out_system->arena};
  if (!vkr_allocator_arena(&out_system->allocator)) {
    log_error("Failed to initialize job system allocator");
    arena_destroy(out_system->arena);
    MemZero(out_system, sizeof(VkrJobSystem));
    return false_v;
  }

  out_system->max_jobs = config->max_jobs;
  out_system->slots = vkr_allocator_alloc(&out_system->allocator,
                                          sizeof(VkrJobSlot) * config->max_jobs,
                                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  out_system->free_stack = vkr_allocator_alloc(
      &out_system->allocator, sizeof(uint32_t) * config->max_jobs,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!out_system->slots || !out_system->free_stack) {
    log_error("Failed to allocate job slots or free stack");
    return false_v;
  }

  for (uint32_t i = 0; i < config->max_jobs; i++) {
    out_system->slots[i] = (VkrJobSlot){
        .handle = {.id = i + 1, .generation = 1},
        .state = JOB_STATE_FREE,
        .priority = VKR_JOB_PRIORITY_NORMAL,
        .type_mask = bitset8_create(),
        .payload = NULL,
        .payload_size = 0,
        .payload_capacity = 0,
        .remaining_dependencies = 0,
        .defer_enqueue = false_v,
        .dependents = vector_create_VkrJobHandle(&out_system->allocator),
    };
    out_system->free_stack[i] = config->max_jobs - i - 1;
  }
  out_system->free_top = config->max_jobs;

  for (uint32_t p = 0; p < VKR_JOB_PRIORITY_MAX; p++) {
    out_system->queues[p] = queue_create_VkrJobHandle(&out_system->allocator,
                                                      config->queue_capacity);
  }

  out_system->running = true_v;

  if (!vkr_mutex_create(&out_system->allocator, &out_system->mutex) ||
      !vkr_cond_create(&out_system->allocator, &out_system->cond) ||
      !vkr_cond_create(&out_system->allocator, &out_system->slots_avail)) {
    log_error("Failed to create job system synchronization primitives");
    return false_v;
  }

  out_system->worker_count = config->worker_count;
  out_system->workers = vkr_allocator_alloc(
      &out_system->allocator, sizeof(VkrJobWorker) * config->worker_count,
      VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!out_system->workers) {
    log_error("Failed to allocate job workers");
    return false_v;
  }

  for (uint32_t i = 0; i < config->worker_count; i++) {
    VkrJobWorker *worker = &out_system->workers[i];
    worker->system = out_system;
    worker->index = i;
    worker->type_mask = config->worker_type_mask_default;
    worker->arena = arena_create(MB(32), MB(32));
    worker->allocator = (VkrAllocator){.ctx = worker->arena};
    vkr_allocator_arena(&worker->allocator);

    if (!vkr_thread_create(&out_system->allocator, &worker->thread,
                           job_worker_thread, worker)) {
      log_error("Failed to create job worker thread %u", i);
      worker->thread = NULL;
      return false_v;
    }
  }

  log_debug("Job system initialized with %u workers", config->worker_count);

  return true_v;
}

void vkr_job_system_shutdown(VkrJobSystem *system) {
  if (!system) {
    return;
  }

  vkr_mutex_lock(system->mutex);
  system->running = false_v;
  vkr_mutex_unlock(system->mutex);
  vkr_cond_broadcast(system->cond);
  vkr_cond_broadcast(system->slots_avail); // Wake up any waiting submitters

  for (uint32_t i = 0; i < system->worker_count; i++) {
    VkrJobWorker *worker = &system->workers[i];
    if (worker->thread) {
      vkr_thread_join(worker->thread);
      vkr_thread_destroy(&system->allocator, &worker->thread);
    }
    if (worker->arena) {
      arena_destroy(worker->arena);
    }
  }

  if (system->workers) {
    vkr_allocator_free(&system->allocator, system->workers,
                       sizeof(VkrJobWorker) * system->worker_count,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }

  if (system->slots_avail) {
    vkr_cond_destroy(&system->allocator, &system->slots_avail);
  }
  if (system->cond) {
    vkr_cond_destroy(&system->allocator, &system->cond);
  }
  if (system->mutex) {
    vkr_mutex_destroy(&system->allocator, &system->mutex);
  }

  if (system->slots) {
    for (uint32_t i = 0; i < system->max_jobs; i++) {
      VkrJobSlot *slot = &system->slots[i];
      if (slot->payload && slot->payload_capacity > 0) {
        vkr_allocator_free(&system->allocator, slot->payload,
                           slot->payload_capacity,
                           VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      }
    }
    vkr_allocator_free(&system->allocator, system->slots,
                       sizeof(VkrJobSlot) * system->max_jobs,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  if (system->free_stack) {
    vkr_allocator_free(&system->allocator, system->free_stack,
                       sizeof(uint32_t) * system->max_jobs,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (system->arena) {
    arena_destroy(system->arena);
  }

  MemZero(system, sizeof(VkrJobSystem));
}

vkr_internal bool8_t vkr_job_submit_internal(VkrJobSystem *system,
                                             const VkrJobDesc *desc,
                                             VkrJobHandle *out_handle,
                                             bool8_t wait_for_slot) {
  assert_log(system != NULL, "JobSystem is NULL");
  assert_log(desc != NULL, "JobDesc is NULL");

  vkr_mutex_lock(system->mutex);

  while (wait_for_slot && system->free_top == 0 && system->running) {
    vkr_cond_wait(system->slots_avail, system->mutex);
  }

  if (!wait_for_slot && system->free_top == 0) {
    vkr_mutex_unlock(system->mutex);
    return false_v;
  }

  if (!system->running) {
    vkr_mutex_unlock(system->mutex);
    return false_v;
  }

  uint32_t slot_index = system->free_stack[--system->free_top];
  VkrJobSlot *slot = &system->slots[slot_index];

  slot->priority = desc->priority;
  slot->type_mask = desc->type_mask;
  slot->run = desc->run;
  slot->on_success = desc->on_success;
  slot->on_failure = desc->on_failure;
  slot->remaining_dependencies = 0;
  slot->state = JOB_STATE_PENDING;
  slot->success = false_v;
  slot->defer_enqueue = desc->defer_enqueue;

  // Register dependencies up front to avoid races.
  if (desc->dependencies && desc->dependency_count > 0) {
    for (uint32_t i = 0; i < desc->dependency_count; i++) {
      VkrJobHandle dep = desc->dependencies[i];
      if (!job_handle_is_valid(dep)) {
        log_warn("Job dependency handle invalid for job %u", slot->handle.id);
        continue;
      }
      if (!job_system_register_dependency_locked(system, slot, dep)) {
        log_error("Job failed to register dependency for job %u",
                  slot->handle.id);
        slot->state = JOB_STATE_FREE;
        slot->handle.generation++;
        system->free_stack[system->free_top++] = slot_index;
        vkr_mutex_unlock(system->mutex);
        return false_v;
      }
    }
  }

  if (desc->payload && desc->payload_size > 0) {
    if (slot->payload_capacity < desc->payload_size) {
      if (slot->payload && slot->payload_capacity > 0) {
        vkr_allocator_free(&system->allocator, slot->payload,
                           slot->payload_capacity,
                           VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      }
      slot->payload =
          vkr_allocator_alloc(&system->allocator, desc->payload_size,
                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
      slot->payload_capacity = desc->payload_size;
    }
    if (slot->payload) {
      MemCopy(slot->payload, desc->payload, desc->payload_size);
      slot->payload_size = desc->payload_size;
    } else {
      vkr_mutex_unlock(system->mutex);
      log_error("Job failed to allocate payload buffer");
      return false_v;
    }
  } else {
    slot->payload_size = 0;
  }

  bool8_t should_enqueue =
      !slot->defer_enqueue && slot->remaining_dependencies == 0;
  if (should_enqueue) {
    if (!job_system_enqueue_locked(system, slot)) {
      log_warn("Job queue full for priority %d", slot->priority);
      slot->state = JOB_STATE_FREE;
      slot->handle.generation++;
      system->free_stack[system->free_top++] = slot_index;
      vkr_mutex_unlock(system->mutex);
      return false_v;
    }
  }
  VkrJobHandle handle = slot->handle;
  if (out_handle) {
    *out_handle = handle;
  }

  if (should_enqueue) {
    vkr_cond_signal(system->cond);
  }

  vkr_mutex_unlock(system->mutex);
  return true_v;
}

bool8_t vkr_job_submit(VkrJobSystem *system, const VkrJobDesc *desc,
                       VkrJobHandle *out_handle) {
  return vkr_job_submit_internal(system, desc, out_handle, true_v);
}

bool8_t vkr_job_try_submit(VkrJobSystem *system, const VkrJobDesc *desc,
                           VkrJobHandle *out_handle) {
  return vkr_job_submit_internal(system, desc, out_handle, false_v);
}

bool8_t vkr_job_add_dependency(VkrJobSystem *system, VkrJobHandle job,
                               VkrJobHandle dependency) {
  assert_log(system != NULL, "JobSystem is NULL");

  if (!job_handle_is_valid(job) || !job_handle_is_valid(dependency)) {
    return false_v;
  }

  vkr_mutex_lock(system->mutex);
  VkrJobSlot *child = job_system_get_slot(system, job);
  if (!child || child->state != JOB_STATE_PENDING) {
    vkr_mutex_unlock(system->mutex);
    log_warn("job_add_dependency: child not pending or missing");
    return false_v;
  }

  bool8_t ok = job_system_register_dependency_locked(system, child, dependency);
  vkr_mutex_unlock(system->mutex);
  return ok;
}

bool8_t vkr_job_mark_ready(VkrJobSystem *system, VkrJobHandle handle) {
  assert_log(system != NULL, "VkrJobSystem is NULL");

  if (!job_handle_is_valid(handle)) {
    return false_v;
  }

  vkr_mutex_lock(system->mutex);
  VkrJobSlot *slot = job_system_get_slot(system, handle);
  if (!slot || slot->state != JOB_STATE_PENDING) {
    vkr_mutex_unlock(system->mutex);
    return false_v;
  }

  if (slot->remaining_dependencies > 0) {
    vkr_mutex_unlock(system->mutex);
    return false_v;
  }

  if (!job_system_enqueue_locked(system, slot)) {
    vkr_mutex_unlock(system->mutex);
    log_warn("Queue for jobs is full for priority %d", slot->priority);
    return false_v;
  }

  vkr_cond_signal(system->cond);
  vkr_mutex_unlock(system->mutex);
  return true_v;
}

bool8_t vkr_job_wait(VkrJobSystem *system, VkrJobHandle handle) {
  assert_log(system != NULL, "VkrJobSystem is NULL");

  if (!job_handle_is_valid(handle)) {
    return false_v;
  }

  vkr_mutex_lock(system->mutex);
  uint32_t idx = handle.id - 1;
  if (idx >= system->max_jobs) {
    vkr_mutex_unlock(system->mutex);
    return false_v;
  }

  VkrJobSlot *slot = &system->slots[idx];
  if (slot->handle.generation != handle.generation) {
    // Slot already recycled; treat as completed.
    vkr_mutex_unlock(system->mutex);
    return true_v;
  }

  // Wait for the slot to be recycled (generation changes after callbacks run).
  // This ensures the job AND its callbacks have fully completed.
  while (slot->handle.generation == handle.generation) {
    vkr_cond_wait(system->cond, system->mutex);
  }

  vkr_mutex_unlock(system->mutex);
  return true_v;
}

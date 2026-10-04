#include "job_system_test.h"

#include <stdatomic.h>

static VkrJobSystemConfig make_small_config(void) {
  VkrJobSystemConfig cfg = vkr_job_system_config_default();
  cfg.worker_count = vkr_min_u32(2, vkr_platform_get_logical_core_count());
  if (cfg.worker_count == 0) {
    cfg.worker_count = 1;
  }
  cfg.max_jobs = 16;
  cfg.queue_capacity = 16;
  return cfg;
}

typedef struct SimpleJobPayload {
  atomic_int *runs;
  atomic_int *callbacks;
} SimpleJobPayload;

static bool8_t simple_job_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  SimpleJobPayload *p = (SimpleJobPayload *)payload;
  atomic_fetch_add_explicit(p->runs, 1, memory_order_relaxed);
  return true_v;
}

static void simple_job_on_success(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  SimpleJobPayload *p = (SimpleJobPayload *)payload;
  atomic_fetch_add_explicit(p->callbacks, 1, memory_order_relaxed);
}

static void test_single_job(void) {
  printf("  Running test_single_job...\n");
  VkrJobSystem system;
  VkrJobSystemConfig cfg = make_small_config();
  assert(vkr_job_system_init(&cfg, &system) && "Job system init failed");

  atomic_int runs = 0;
  atomic_int callbacks = 0;
  SimpleJobPayload payload = {.runs = &runs, .callbacks = &callbacks};

  VkrJobDesc desc = {0};
  desc.priority = VKR_JOB_PRIORITY_NORMAL;
  desc.type_mask = vkr_job_type_mask_all();
  desc.run = simple_job_run;
  desc.on_success = simple_job_on_success;
  desc.payload = &payload;
  desc.payload_size = sizeof(payload);

  VkrJobHandle handle = {0};
  assert(vkr_job_submit(&system, &desc, &handle) && "submit failed");
  assert(vkr_job_wait(&system, handle) && "wait failed");
  assert(atomic_load_explicit(&runs, memory_order_relaxed) == 1 &&
         "run count mismatch");
  assert(atomic_load_explicit(&callbacks, memory_order_relaxed) == 1 &&
         "callback count mismatch");

  vkr_job_system_shutdown(&system);
  printf("  test_single_job PASSED\n");
}

typedef struct DepJobPayload {
  atomic_bool *parent_done;
  atomic_int *child_runs;
} DepJobPayload;

typedef struct ParentPayload {
  atomic_bool *parent_done;
} ParentPayload;

static bool8_t dep_parent_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  ParentPayload *p = (ParentPayload *)payload;
  atomic_store_explicit(p->parent_done, true, memory_order_release);
  return true_v;
}

static bool8_t dep_child_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  DepJobPayload *p = (DepJobPayload *)payload;
  if (!atomic_load_explicit(p->parent_done, memory_order_acquire)) {
    return false_v;
  }
  atomic_fetch_add_explicit(p->child_runs, 1, memory_order_relaxed);
  return true_v;
}

static void test_dependency_ordering(void) {
  printf("  Running test_dependency_ordering...\n");
  VkrJobSystem system;
  VkrJobSystemConfig cfg = make_small_config();
  assert(vkr_job_system_init(&cfg, &system) && "Job system init failed");

  atomic_bool parent_done = ATOMIC_VAR_INIT(false);
  ParentPayload parent_payload = {.parent_done = &parent_done};
  VkrJobDesc parent_desc = {0};
  parent_desc.priority = VKR_JOB_PRIORITY_NORMAL;
  parent_desc.type_mask = vkr_job_type_mask_all();
  parent_desc.run = dep_parent_run;
  parent_desc.payload = &parent_payload;
  parent_desc.payload_size = sizeof(parent_payload);

  VkrJobHandle parent_handle = {0};
  assert(vkr_job_submit(&system, &parent_desc, &parent_handle) &&
         "parent submit failed");

  atomic_int child_runs = 0;
  DepJobPayload child_payload = {.parent_done = &parent_done,
                                 .child_runs = &child_runs};

  VkrJobHandle deps[1] = {parent_handle};
  VkrJobDesc child_desc = {0};
  child_desc.priority = VKR_JOB_PRIORITY_NORMAL;
  child_desc.type_mask = vkr_job_type_mask_all();
  child_desc.run = dep_child_run;
  child_desc.payload = &child_payload;
  child_desc.payload_size = sizeof(child_payload);
  child_desc.dependencies = deps;
  child_desc.dependency_count = 1;

  VkrJobHandle child_handle = {0};
  assert(vkr_job_submit(&system, &child_desc, &child_handle) &&
         "child submit failed");

  assert(vkr_job_wait(&system, parent_handle) && "parent wait failed");
  assert(vkr_job_wait(&system, child_handle) && "child wait failed");

  assert(atomic_load_explicit(&parent_done, memory_order_acquire) == true &&
         "parent did not run");
  assert(atomic_load_explicit(&child_runs, memory_order_relaxed) == 1 &&
         "child run count mismatch");

  vkr_job_system_shutdown(&system);
  printf("  test_dependency_ordering PASSED\n");
}

typedef struct DeferredPayload {
  atomic_int *runs;
} DeferredPayload;

static bool8_t deferred_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  DeferredPayload *p = (DeferredPayload *)payload;
  atomic_fetch_add_explicit(p->runs, 1, memory_order_relaxed);
  return true_v;
}

static void test_deferred_ready(void) {
  printf("  Running test_deferred_ready...\n");
  VkrJobSystem system;
  VkrJobSystemConfig cfg = make_small_config();
  assert(vkr_job_system_init(&cfg, &system) && "Job system init failed");

  atomic_int runs = 0;
  DeferredPayload payload = {.runs = &runs};

  VkrJobDesc desc = {0};
  desc.priority = VKR_JOB_PRIORITY_NORMAL;
  desc.type_mask = vkr_job_type_mask_all();
  desc.run = deferred_run;
  desc.payload = &payload;
  desc.payload_size = sizeof(payload);
  desc.defer_enqueue = true_v;

  VkrJobHandle handle = {0};
  assert(vkr_job_submit(&system, &desc, &handle) && "deferred submit failed");

  // Give workers a moment; job should not run while deferred.
  vkr_platform_sleep(5);
  assert(atomic_load_explicit(&runs, memory_order_relaxed) == 0 &&
         "deferred job ran before mark_ready");

  assert(vkr_job_mark_ready(&system, handle) && "mark_ready failed");
  assert(vkr_job_wait(&system, handle) && "wait failed");
  assert(atomic_load_explicit(&runs, memory_order_relaxed) == 1 &&
         "deferred job did not run after mark_ready");

  vkr_job_system_shutdown(&system);
  printf("  test_deferred_ready PASSED\n");
}

static void *(*job_test_original_alloc)(void *, uint64_t,
                                        VkrAllocatorMemoryTag);

static void *fail_job_payload_allocation(void *ctx, uint64_t size,
                                         VkrAllocatorMemoryTag tag) {
  return tag == VKR_ALLOCATOR_MEMORY_TAG_STRUCT
             ? NULL
             : job_test_original_alloc(ctx, size, tag);
}

static void test_failed_submission_returns_slot(void) {
  VkrJobSystem system;
  VkrJobSystemConfig cfg = make_small_config();
  assert(vkr_job_system_init(&cfg, &system));
  atomic_int runs = 0;
  DeferredPayload payload = {.runs = &runs};
  VkrJobDesc parent_desc = {.priority = VKR_JOB_PRIORITY_NORMAL,
                            .type_mask = vkr_job_type_mask_all(),
                            .run = deferred_run,
                            .payload = &payload,
                            .payload_size = sizeof(payload),
                            .defer_enqueue = true_v};
  VkrJobHandle parent = {0};
  assert(vkr_job_submit(&system, &parent_desc, &parent));
  VkrJobDesc child_desc = parent_desc;
  child_desc.defer_enqueue = false_v;
  child_desc.dependencies = &parent;
  child_desc.dependency_count = 1;
  const uint32_t available_slots = system.free_top;
  job_test_original_alloc = system.allocator.alloc;
  system.allocator.alloc = fail_job_payload_allocation;
  VkrJobHandle child = {0};
  assert(!vkr_job_try_submit(&system, &child_desc, &child));
  system.allocator.alloc = job_test_original_alloc;
  assert(system.free_top == available_slots);
  assert(vkr_job_try_submit(&system, &child_desc, &child));
  assert(vkr_job_mark_ready(&system, parent));
  assert(vkr_job_wait(&system, child));
  assert(atomic_load_explicit(&runs, memory_order_relaxed) == 2);
  vkr_job_system_shutdown(&system);
}

typedef struct ForCounts {
  atomic_int runs[256];
} ForCounts;

static void for_count(void *context, uint32_t index) {
  ForCounts *counts = context;
  atomic_fetch_add_explicit(&counts->runs[index], 1, memory_order_relaxed);
}

static bool8_t for_counts_are(ForCounts *counts, uint32_t count, int runs) {
  for (uint32_t i = 0; i < count; ++i) {
    if (atomic_load_explicit(&counts->runs[i], memory_order_relaxed) != runs) {
      return false_v;
    }
  }
  return true_v;
}

typedef struct HoldPayload {
  atomic_int *held;
  atomic_int *release;
} HoldPayload;

static bool8_t hold_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  HoldPayload *p = (HoldPayload *)payload;
  atomic_fetch_add_explicit(p->held, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(p->release, memory_order_acquire)) {
    vkr_thread_sleep(1);
  }
  return true_v;
}

/* A parallel loop runs every index once. With every worker held, ending it
   runs them all on the calling thread; the workers sent to help start
   later, find none left and let go of the loop's block. */
static void test_job_for(void) {
  VkrJobSystem system;
  VkrJobSystemConfig cfg = make_small_config();
  assert(vkr_job_system_init(&cfg, &system));
  static ForCounts counts;
  MemZero(&counts, sizeof(counts));
  VkrJobFor *loop = vkr_job_for_begin(&system, 256u, for_count, &counts);
  while (!vkr_job_for_done(loop)) {
    vkr_thread_sleep(1);
  }
  assert(for_counts_are(&counts, 256u, 1));
  vkr_job_for_end(loop);
  assert(!vkr_job_for_begin(NULL, 256u, for_count, &counts));
  assert(for_counts_are(&counts, 256u, 2));

  atomic_int held = 0;
  atomic_int release = 0;
  HoldPayload payload = {.held = &held, .release = &release};
  VkrJobHandle holds[16];
  const uint32_t workers = system.worker_count;
  for (uint32_t i = 0; i < workers; ++i) {
    const VkrJobDesc desc = {.priority = VKR_JOB_PRIORITY_HIGH,
                             .type_mask = vkr_job_type_mask_all(),
                             .run = hold_run,
                             .payload = &payload,
                             .payload_size = sizeof(payload)};
    assert(vkr_job_submit(&system, &desc, &holds[i]));
  }
  while (atomic_load_explicit(&held, memory_order_acquire) < (int)workers) {
    vkr_thread_sleep(1);
  }
  /* More loops than blocks: the last ones run on this thread alone. */
  for (uint32_t loop_count = 0; loop_count < VKR_JOB_FOR_MAX + 1u;
       ++loop_count) {
    MemZero(&counts, sizeof(counts));
    loop = vkr_job_for_begin(&system, 8u, for_count, &counts);
    assert(loop ? !vkr_job_for_done(loop) : loop_count >= VKR_JOB_FOR_MAX);
    vkr_job_for_end(loop);
    assert(for_counts_are(&counts, 8u, 1));
  }
  atomic_store_explicit(&release, 1, memory_order_release);
  for (uint32_t i = 0; i < workers; ++i) {
    assert(vkr_job_wait(&system, holds[i]));
  }
  for (uint32_t i = 0; i < VKR_JOB_FOR_MAX; ++i) {
    for (uint32_t waited = 0;
         vkr_atomic_uint32_load(&system.fors[i].references,
                                VKR_MEMORY_ORDER_ACQUIRE) != 0u;
         ++waited) {
      assert(waited < 5000u);
      vkr_thread_sleep(1);
    }
  }
  assert(for_counts_are(&counts, 8u, 1));
  vkr_job_system_shutdown(&system);
}

bool32_t run_job_system_tests(void) {
  printf("--- Running JobSystem tests... ---\n");
  test_single_job();
  test_dependency_ordering();
  test_deferred_ready();
  test_failed_submission_returns_slot();
  test_job_for();
  printf("--- JobSystem tests completed. ---\n");
  return true_v;
}

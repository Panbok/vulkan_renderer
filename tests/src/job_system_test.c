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

bool32_t run_job_system_tests(void) {
  printf("--- Running JobSystem tests... ---\n");
  test_single_job();
  test_dependency_ordering();
  test_deferred_ready();
  printf("--- JobSystem tests completed. ---\n");
  return true_v;
}

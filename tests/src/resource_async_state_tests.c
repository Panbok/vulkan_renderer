#include "resource_async_state_tests.h"

#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_resource_system.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ResourceAsyncMockBackendState {
  uint64_t submit_serial;
  uint64_t completed_submit_serial;
} ResourceAsyncMockBackendState;

typedef struct ResourceAsyncMockLoaderContext {
  atomic_uint load_calls;
  atomic_uint unload_calls;
  atomic_uint token_counter;
} ResourceAsyncMockLoaderContext;

typedef struct ResourceAsyncDependencyContext {
  atomic_uint dep_prepare_calls;
  atomic_uint dep_finalize_calls;
  atomic_uint dep_release_calls;
  atomic_uint dep_unload_calls;
  atomic_uint root_prepare_calls;
  atomic_uint root_finalize_calls;
  atomic_uint root_blocked_calls;
  atomic_uint root_release_calls;
  atomic_uint root_unload_calls;
  atomic_uint token_counter;
} ResourceAsyncDependencyContext;

typedef struct ResourceAsyncDepPayload {
  uint32_t token;
  bool8_t should_fail;
} ResourceAsyncDepPayload;

typedef struct ResourceAsyncRootPayload {
  char dep_path_storage[96];
  String8 dep_path;
  VkrResourceHandleInfo dep_request;
} ResourceAsyncRootPayload;

typedef struct ResourceAsyncBudgetContext {
  atomic_uint prepare_calls;
  atomic_uint finalize_calls;
  atomic_uint release_calls;
  uint32_t finalize_ops;
  uint64_t finalize_bytes;
  atomic_uint token_counter;
} ResourceAsyncBudgetContext;

typedef struct ResourceAsyncBudgetPayload {
  uint32_t token;
} ResourceAsyncBudgetPayload;

typedef struct ResourceAsyncSceneContext {
  atomic_uint prepare_calls;
  atomic_uint finalize_calls;
  atomic_uint release_calls;
  atomic_uint unload_calls;
  atomic_uint token_counter;
  uint32_t prepare_delay_ms;
} ResourceAsyncSceneContext;

typedef struct ResourceAsyncScenePayload {
  uint32_t token;
} ResourceAsyncScenePayload;

static uint64_t resource_async_mock_get_submit_serial(void *backend_state) {
  ResourceAsyncMockBackendState *state =
      (ResourceAsyncMockBackendState *)backend_state;
  return state ? state->submit_serial : 0;
}

static uint64_t
resource_async_mock_get_completed_submit_serial(void *backend_state) {
  ResourceAsyncMockBackendState *state =
      (ResourceAsyncMockBackendState *)backend_state;
  return state ? state->completed_submit_serial : 0;
}

static void resource_async_mock_init_renderer(
    RendererFrontend *renderer, ResourceAsyncMockBackendState *backend_state) {
  MemZero(renderer, sizeof(*renderer));
  MemZero(backend_state, sizeof(*backend_state));
  backend_state->submit_serial = 1;
  backend_state->completed_submit_serial = 2;
  renderer->backend_state = backend_state;
  renderer->backend.get_submit_serial = resource_async_mock_get_submit_serial;
  renderer->backend.get_completed_submit_serial =
      resource_async_mock_get_completed_submit_serial;
  renderer->frame_active = false_v;
}

static VkrJobSystemConfig resource_async_make_job_config(void) {
  VkrJobSystemConfig cfg = vkr_job_system_config_default();
  cfg.worker_count = 1;
  cfg.max_jobs = 64;
  cfg.queue_capacity = 64;
  return cfg;
}

static bool8_t resource_async_mock_can_load(VkrResourceLoader *self,
                                            String8 name) {
  (void)self;
  return name.str != NULL && name.length > 0;
}

static bool8_t resource_async_mock_load(VkrResourceLoader *self, String8 name,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_handle,
                                        VkrRendererError *out_error) {
  (void)temp_alloc;
  assert(self != NULL);
  assert(out_handle != NULL);
  assert(out_error != NULL);

  ResourceAsyncMockLoaderContext *ctx =
      (ResourceAsyncMockLoaderContext *)self->resource_system;
  assert(ctx != NULL);

  atomic_fetch_add_explicit(&ctx->load_calls, 1u, memory_order_relaxed);

  if (string8_contains_cstr(&name, "slow_cancel")) {
    vkr_platform_sleep(25);
  }

  if (string8_contains_cstr(&name, "fail")) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  uint32_t token =
      atomic_fetch_add_explicit(&ctx->token_counter, 1u, memory_order_relaxed) +
      1u;

  out_handle->type = VKR_RESOURCE_TYPE_TEXTURE;
  out_handle->as.texture = (VkrTextureHandle){
      .id = token,
      .generation = token + 100u,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static void resource_async_mock_unload(VkrResourceLoader *self,
                                       const VkrResourceHandleInfo *handle,
                                       String8 name) {
  (void)handle;
  (void)name;
  assert(self != NULL);
  ResourceAsyncMockLoaderContext *ctx =
      (ResourceAsyncMockLoaderContext *)self->resource_system;
  assert(ctx != NULL);
  atomic_fetch_add_explicit(&ctx->unload_calls, 1u, memory_order_relaxed);
}

static bool8_t resource_async_noop_job_run(VkrJobContext *ctx, void *payload) {
  (void)ctx;
  (void)payload;
  return true_v;
}

static bool8_t resource_async_dep_can_load(VkrResourceLoader *self,
                                           String8 name) {
  (void)self;
  return name.str != NULL && name.length > 0;
}

static bool8_t resource_async_dep_prepare(VkrResourceLoader *self, String8 name,
                                          VkrAllocator *temp_alloc,
                                          void **out_payload,
                                          VkrRendererError *out_error) {
  (void)temp_alloc;
  assert(self != NULL);
  assert(out_payload != NULL);
  assert(out_error != NULL);

  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  assert(ctx != NULL);

  ResourceAsyncDepPayload *payload =
      (ResourceAsyncDepPayload *)malloc(sizeof(*payload));
  assert(payload != NULL);

  payload->token =
      atomic_fetch_add_explicit(&ctx->token_counter, 1u, memory_order_relaxed) +
      1u;
  payload->should_fail =
      string8_contains_cstr(&name, "dep_fail") ? true_v : false_v;

  atomic_fetch_add_explicit(&ctx->dep_prepare_calls, 1u, memory_order_relaxed);
  vkr_platform_sleep(12);
  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t resource_async_dep_finalize(VkrResourceLoader *self,
                                           String8 name, void *payload,
                                           VkrResourceHandleInfo *out_handle,
                                           VkrRendererError *out_error) {
  (void)name;
  assert(self != NULL);
  assert(payload != NULL);
  assert(out_handle != NULL);
  assert(out_error != NULL);

  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  ResourceAsyncDepPayload *dep_payload = (ResourceAsyncDepPayload *)payload;

  atomic_fetch_add_explicit(&ctx->dep_finalize_calls, 1u, memory_order_relaxed);

  if (dep_payload->should_fail) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_MATERIAL;
  out_handle->as.material = (VkrMaterialHandle){
      .id = dep_payload->token,
      .generation = dep_payload->token + 100u,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static void resource_async_dep_release_payload(VkrResourceLoader *self,
                                               void *payload) {
  assert(self != NULL);
  if (!payload) {
    return;
  }
  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->dep_release_calls, 1u, memory_order_relaxed);
  free(payload);
}

static void resource_async_dep_unload(VkrResourceLoader *self,
                                      const VkrResourceHandleInfo *handle,
                                      String8 name) {
  (void)handle;
  (void)name;
  assert(self != NULL);
  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->dep_unload_calls, 1u, memory_order_relaxed);
}

static bool8_t resource_async_root_can_load(VkrResourceLoader *self,
                                            String8 name) {
  (void)self;
  return name.str != NULL && name.length > 0;
}

static bool8_t resource_async_root_prepare(VkrResourceLoader *self,
                                           String8 name,
                                           VkrAllocator *temp_alloc,
                                           void **out_payload,
                                           VkrRendererError *out_error) {
  assert(self != NULL);
  assert(temp_alloc != NULL);
  assert(out_payload != NULL);
  assert(out_error != NULL);

  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  assert(ctx != NULL);

  ResourceAsyncRootPayload *payload =
      (ResourceAsyncRootPayload *)malloc(sizeof(*payload));
  assert(payload != NULL);
  MemZero(payload, sizeof(*payload));

  String8 dep_path = string8_contains_cstr(&name, "fail")
                         ? string8_lit("tests/assets/dep_fail.mock")
                         : string8_lit("tests/assets/dep_ok.mock");

  assert(dep_path.length < sizeof(payload->dep_path_storage));
  MemCopy(payload->dep_path_storage, dep_path.str, dep_path.length);
  payload->dep_path_storage[dep_path.length] = '\0';
  payload->dep_path =
      string8_create((uint8_t *)payload->dep_path_storage, dep_path.length);

  payload->dep_request = (VkrResourceHandleInfo){0};
  payload->dep_request.type = VKR_RESOURCE_TYPE_MATERIAL;
  payload->dep_request.loader_id = VKR_INVALID_ID;
  payload->dep_request.load_state = VKR_RESOURCE_LOAD_STATE_INVALID;

  VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
  (void)vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL, payload->dep_path,
                                 temp_alloc, &payload->dep_request,
                                 &dependency_error);

  atomic_fetch_add_explicit(&ctx->root_prepare_calls, 1u, memory_order_relaxed);
  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t resource_async_root_finalize(VkrResourceLoader *self,
                                            String8 name, void *payload,
                                            VkrResourceHandleInfo *out_handle,
                                            VkrRendererError *out_error) {
  (void)name;
  assert(self != NULL);
  assert(payload != NULL);
  assert(out_handle != NULL);
  assert(out_error != NULL);

  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  ResourceAsyncRootPayload *root_payload = (ResourceAsyncRootPayload *)payload;

  atomic_fetch_add_explicit(&ctx->root_finalize_calls, 1u,
                            memory_order_relaxed);

  VkrRendererError dep_state_error = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState dep_state = vkr_resource_system_get_state(
      &root_payload->dep_request, &dep_state_error);
  if (dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
      dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
      dep_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
    atomic_fetch_add_explicit(&ctx->root_blocked_calls, 1u,
                              memory_order_relaxed);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (dep_state != VKR_RESOURCE_LOAD_STATE_READY) {
    *out_error = dep_state_error != VKR_RENDERER_ERROR_NONE
                     ? dep_state_error
                     : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  VkrResourceHandleInfo resolved_dep = {0};
  if (!vkr_resource_system_try_get_resolved(&root_payload->dep_request,
                                            &resolved_dep)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->as.mesh =
      (VkrMeshLoaderResult *)(uintptr_t)resolved_dep.as.material.id;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static void resource_async_root_release_payload(VkrResourceLoader *self,
                                                void *payload) {
  assert(self != NULL);
  if (!payload) {
    return;
  }

  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  ResourceAsyncRootPayload *root_payload = (ResourceAsyncRootPayload *)payload;
  if (root_payload->dep_request.request_id != 0) {
    vkr_resource_system_unload(&root_payload->dep_request,
                               root_payload->dep_path);
  }
  atomic_fetch_add_explicit(&ctx->root_release_calls, 1u, memory_order_relaxed);
  free(payload);
}

static void resource_async_root_unload(VkrResourceLoader *self,
                                       const VkrResourceHandleInfo *handle,
                                       String8 name) {
  (void)handle;
  (void)name;
  assert(self != NULL);
  ResourceAsyncDependencyContext *ctx =
      (ResourceAsyncDependencyContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->root_unload_calls, 1u, memory_order_relaxed);
}

static bool8_t resource_async_budget_can_load(VkrResourceLoader *self,
                                              String8 name) {
  (void)self;
  if (!name.str || name.length == 0) {
    return false_v;
  }
  return string8_contains_cstr(&name, ".budget.mock");
}

static bool8_t resource_async_budget_prepare(VkrResourceLoader *self,
                                             String8 name,
                                             VkrAllocator *temp_alloc,
                                             void **out_payload,
                                             VkrRendererError *out_error) {
  (void)name;
  (void)temp_alloc;
  assert(self != NULL);
  assert(out_payload != NULL);
  assert(out_error != NULL);

  ResourceAsyncBudgetContext *ctx =
      (ResourceAsyncBudgetContext *)self->resource_system;
  assert(ctx != NULL);

  ResourceAsyncBudgetPayload *payload =
      (ResourceAsyncBudgetPayload *)malloc(sizeof(*payload));
  assert(payload != NULL);
  payload->token =
      atomic_fetch_add_explicit(&ctx->token_counter, 1u, memory_order_relaxed) +
      1u;

  atomic_fetch_add_explicit(&ctx->prepare_calls, 1u, memory_order_relaxed);
  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t resource_async_budget_finalize(VkrResourceLoader *self,
                                              String8 name, void *payload,
                                              VkrResourceHandleInfo *out_handle,
                                              VkrRendererError *out_error) {
  (void)name;
  assert(self != NULL);
  assert(payload != NULL);
  assert(out_handle != NULL);
  assert(out_error != NULL);

  ResourceAsyncBudgetContext *ctx =
      (ResourceAsyncBudgetContext *)self->resource_system;
  ResourceAsyncBudgetPayload *budget_payload =
      (ResourceAsyncBudgetPayload *)payload;
  atomic_fetch_add_explicit(&ctx->finalize_calls, 1u, memory_order_relaxed);

  out_handle->type = VKR_RESOURCE_TYPE_SCENE;
  out_handle->as.scene = (VkrSceneHandle)(uintptr_t)budget_payload->token;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t
resource_async_budget_estimate_cost(VkrResourceLoader *self, String8 name,
                                    void *payload,
                                    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)name;
  (void)payload;
  assert(self != NULL);
  assert(out_cost != NULL);

  ResourceAsyncBudgetContext *ctx =
      (ResourceAsyncBudgetContext *)self->resource_system;
  out_cost->gpu_upload_ops = ctx->finalize_ops;
  out_cost->gpu_upload_bytes = ctx->finalize_bytes;
  return true_v;
}

static void resource_async_budget_release_payload(VkrResourceLoader *self,
                                                  void *payload) {
  assert(self != NULL);
  if (!payload) {
    return;
  }
  ResourceAsyncBudgetContext *ctx =
      (ResourceAsyncBudgetContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->release_calls, 1u, memory_order_relaxed);
  free(payload);
}

static void resource_async_budget_unload(VkrResourceLoader *self,
                                         const VkrResourceHandleInfo *handle,
                                         String8 name) {
  (void)self;
  (void)handle;
  (void)name;
}

static bool8_t resource_async_scene_can_load(VkrResourceLoader *self,
                                             String8 name) {
  (void)self;
  if (!name.str || name.length == 0) {
    return false_v;
  }
  return string8_contains_cstr(&name, ".scene.mock");
}

static bool8_t resource_async_scene_prepare(VkrResourceLoader *self,
                                            String8 name,
                                            VkrAllocator *temp_alloc,
                                            void **out_payload,
                                            VkrRendererError *out_error) {
  (void)name;
  (void)temp_alloc;
  assert(self != NULL);
  assert(out_payload != NULL);
  assert(out_error != NULL);

  ResourceAsyncSceneContext *ctx =
      (ResourceAsyncSceneContext *)self->resource_system;
  assert(ctx != NULL);

  ResourceAsyncScenePayload *payload =
      (ResourceAsyncScenePayload *)malloc(sizeof(*payload));
  assert(payload != NULL);
  payload->token =
      atomic_fetch_add_explicit(&ctx->token_counter, 1u, memory_order_relaxed) +
      1u;

  atomic_fetch_add_explicit(&ctx->prepare_calls, 1u, memory_order_relaxed);
  if (ctx->prepare_delay_ms > 0) {
    vkr_platform_sleep(ctx->prepare_delay_ms);
  }

  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t resource_async_scene_finalize(VkrResourceLoader *self,
                                             String8 name, void *payload,
                                             VkrResourceHandleInfo *out_handle,
                                             VkrRendererError *out_error) {
  assert(self != NULL);
  assert(name.str != NULL);
  assert(payload != NULL);
  assert(out_handle != NULL);
  assert(out_error != NULL);

  ResourceAsyncSceneContext *ctx =
      (ResourceAsyncSceneContext *)self->resource_system;
  ResourceAsyncScenePayload *scene_payload =
      (ResourceAsyncScenePayload *)payload;
  atomic_fetch_add_explicit(&ctx->finalize_calls, 1u, memory_order_relaxed);

  if (string8_contains_cstr(&name, "scene_fail")) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_SCENE;
  out_handle->as.scene = (VkrSceneHandle)(uintptr_t)scene_payload->token;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

static bool8_t
resource_async_scene_estimate_cost(VkrResourceLoader *self, String8 name,
                                   void *payload,
                                   VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  (void)payload;
  assert(out_cost != NULL);
  out_cost->gpu_upload_ops = 1u;
  out_cost->gpu_upload_bytes = 1024u;
  return true_v;
}

static void resource_async_scene_release_payload(VkrResourceLoader *self,
                                                 void *payload) {
  assert(self != NULL);
  if (!payload) {
    return;
  }
  ResourceAsyncSceneContext *ctx =
      (ResourceAsyncSceneContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->release_calls, 1u, memory_order_relaxed);
  free(payload);
}

static void resource_async_scene_unload(VkrResourceLoader *self,
                                        const VkrResourceHandleInfo *handle,
                                        String8 name) {
  (void)handle;
  (void)name;
  assert(self != NULL);
  ResourceAsyncSceneContext *ctx =
      (ResourceAsyncSceneContext *)self->resource_system;
  atomic_fetch_add_explicit(&ctx->unload_calls, 1u, memory_order_relaxed);
}

static bool8_t
resource_async_wait_for_state(const VkrResourceHandleInfo *handle,
                              VkrResourceLoadState expected,
                              VkrRendererError *out_error) {
  for (uint32_t i = 0; i < 300; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state = vkr_resource_system_get_state(handle, &err);
    if (state == expected) {
      if (out_error) {
        *out_error = err;
      }
      return true_v;
    }
    vkr_platform_sleep(2);
  }

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_UNKNOWN;
  }
  return false_v;
}

static void
test_resource_async_dedupe_and_ready(VkrAllocator *allocator,
                                     ResourceAsyncMockLoaderContext *ctx) {
  printf("  Running test_resource_async_dedupe_and_ready...\n");

  String8 path = string8_lit("tests/assets/dedupe.mock");
  VkrResourceHandleInfo h0 = {0};
  VkrResourceHandleInfo h1 = {0};
  VkrRendererError e0 = VKR_RENDERER_ERROR_NONE;
  VkrRendererError e1 = VKR_RENDERER_ERROR_NONE;

  bool8_t accepted0 = vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                               allocator, &h0, &e0);
  bool8_t accepted1 = vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                               allocator, &h1, &e1);

  assert(accepted0 == true_v);
  assert(accepted1 == true_v);
  assert(e0 == VKR_RENDERER_ERROR_NONE);
  assert(e1 == VKR_RENDERER_ERROR_NONE);
  assert(h0.request_id != 0);
  assert(h0.request_id == h1.request_id);

  VkrRendererError state_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&h0, VKR_RESOURCE_LOAD_STATE_READY,
                                       &state_error) == true_v);
  assert(state_error == VKR_RENDERER_ERROR_NONE);
  assert(vkr_resource_system_is_ready(&h1) == true_v);
  assert(atomic_load_explicit(&ctx->load_calls, memory_order_relaxed) == 1u);

  vkr_resource_system_unload(&h0, path);
  vkr_resource_system_unload(&h1, path);
  assert(atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed) == 1u);

  printf("  test_resource_async_dedupe_and_ready PASSED\n");
}

static void test_resource_async_submit_saturation_recovers(
    VkrAllocator *allocator, VkrJobSystem *job_system,
    ResourceAsyncMockLoaderContext *ctx) {
  printf("  Running test_resource_async_submit_saturation_recovers...\n");

  assert(job_system != NULL);
  assert(job_system->max_jobs > 1u);

  uint32_t blocker_count = job_system->max_jobs;
  VkrJobHandle *blockers =
      (VkrJobHandle *)malloc(sizeof(VkrJobHandle) * blocker_count);
  assert(blockers != NULL);

  Bitset8 general_mask = bitset8_create();
  bitset8_set(&general_mask, VKR_JOB_TYPE_GENERAL);

  VkrJobDesc blocker_desc = {
      .priority = VKR_JOB_PRIORITY_NORMAL,
      .type_mask = general_mask,
      .run = resource_async_noop_job_run,
      .on_success = NULL,
      .on_failure = NULL,
      .payload = NULL,
      .payload_size = 0,
      .dependencies = NULL,
      .dependency_count = 0,
      .defer_enqueue = true_v,
  };

  for (uint32_t i = 0; i < blocker_count; ++i) {
    assert(vkr_job_submit(job_system, &blocker_desc, &blockers[i]) == true_v);
  }

  const uint32_t load_before =
      atomic_load_explicit(&ctx->load_calls, memory_order_relaxed);

  String8 path = string8_lit("tests/assets/submit_saturation.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                              allocator, &handle, &load_error);

  assert(accepted == true_v);
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.request_id != 0);

  VkrRendererError pending_error = VKR_RENDERER_ERROR_UNKNOWN;
  VkrResourceLoadState pending_state =
      vkr_resource_system_get_state(&handle, &pending_error);
  assert(pending_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU);
  assert(pending_error == VKR_RENDERER_ERROR_NONE);
  assert(atomic_load_explicit(&ctx->load_calls, memory_order_relaxed) ==
         load_before);

  for (uint32_t i = 0; i < blocker_count; ++i) {
    assert(vkr_job_mark_ready(job_system, blockers[i]) == true_v);
  }
  for (uint32_t i = 0; i < blocker_count; ++i) {
    assert(vkr_job_wait(job_system, blockers[i]) == true_v);
  }

  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&handle, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);
  assert(atomic_load_explicit(&ctx->load_calls, memory_order_relaxed) ==
         load_before + 1u);

  vkr_resource_system_unload(&handle, path);
  free(blockers);

  printf("  test_resource_async_submit_saturation_recovers PASSED\n");
}

static void test_resource_async_failure_state(VkrAllocator *allocator) {
  printf("  Running test_resource_async_failure_state...\n");

  String8 path = string8_lit("tests/assets/fail.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(handle.request_id != 0);

  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  assert(resource_async_wait_for_state(&handle, VKR_RESOURCE_LOAD_STATE_FAILED,
                                       &state_error) == true_v);
  assert(state_error == VKR_RENDERER_ERROR_FILE_NOT_FOUND);

  vkr_resource_system_unload(&handle, path);

  printf("  test_resource_async_failure_state PASSED\n");
}

static void
test_resource_async_batch_accept_count(VkrAllocator *allocator,
                                       ResourceAsyncMockLoaderContext *ctx) {
  printf("  Running test_resource_async_batch_accept_count...\n");

  String8 paths[4] = {
      string8_lit("tests/assets/batch_dedupe.mock"),
      string8_lit("tests/assets/batch_dedupe.mock"),
      string8_lit("tests/assets/batch_fail.mock"),
      (String8){0},
  };
  VkrResourceHandleInfo handles[4] = {0};
  VkrRendererError errors[4] = {0};

  const uint32_t load_before =
      atomic_load_explicit(&ctx->load_calls, memory_order_relaxed);
  const uint32_t unload_before =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);

  uint32_t accepted = vkr_resource_system_load_batch(
      VKR_RESOURCE_TYPE_TEXTURE, paths, 4, allocator, handles, errors);
  assert(accepted == 3u);

  assert(handles[0].request_id != 0);
  assert(handles[1].request_id == handles[0].request_id);
  assert(handles[2].request_id != 0);
  assert(handles[3].request_id == 0);
  assert(handles[3].load_state == VKR_RESOURCE_LOAD_STATE_FAILED);
  assert(errors[3] == VKR_RENDERER_ERROR_INVALID_PARAMETER);

  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  VkrRendererError failed_error = VKR_RENDERER_ERROR_NONE;
  assert(resource_async_wait_for_state(&handles[0],
                                       VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(resource_async_wait_for_state(&handles[2],
                                       VKR_RESOURCE_LOAD_STATE_FAILED,
                                       &failed_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);
  assert(failed_error == VKR_RENDERER_ERROR_FILE_NOT_FOUND);

  vkr_resource_system_unload(&handles[0], paths[0]);
  vkr_resource_system_unload(&handles[1], paths[1]);
  vkr_resource_system_unload(&handles[2], paths[2]);

  const uint32_t load_after =
      atomic_load_explicit(&ctx->load_calls, memory_order_relaxed);
  const uint32_t unload_after =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);
  assert(load_after >= load_before + 2u);
  assert(unload_after >= unload_before + 1u);

  printf("  test_resource_async_batch_accept_count PASSED\n");
}

static void test_resource_async_cancel_cleans_loaded_result(
    VkrAllocator *allocator, ResourceAsyncMockLoaderContext *ctx) {
  printf("  Running test_resource_async_cancel_cleans_loaded_result...\n");

  String8 path = string8_lit("tests/assets/slow_cancel.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  const uint32_t unload_before =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(handle.request_id != 0);

  vkr_resource_system_unload(&handle, path);

  bool8_t reached_terminal = false_v;
  for (uint32_t i = 0; i < 400; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state = vkr_resource_system_get_state(&handle, &err);
    uint32_t unload_now =
        atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);
    if (state == VKR_RESOURCE_LOAD_STATE_INVALID &&
        unload_now >= unload_before + 1u) {
      reached_terminal = true_v;
      break;
    }
    vkr_platform_sleep(2);
  }
  assert(reached_terminal == true_v);

  const uint32_t unload_after =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);
  assert(unload_after == unload_before + 1u);

  printf("  test_resource_async_cancel_cleans_loaded_result PASSED\n");
}

static void test_resource_async_cancel_then_reload_same_path(
    VkrAllocator *allocator, ResourceAsyncMockLoaderContext *ctx) {
  printf("  Running test_resource_async_cancel_then_reload_same_path...\n");

  String8 path = string8_lit("tests/assets/slow_cancel_reload.mock");
  VkrResourceHandleInfo first = {0};
  VkrRendererError first_error = VKR_RENDERER_ERROR_NONE;

  const uint32_t load_before =
      atomic_load_explicit(&ctx->load_calls, memory_order_relaxed);
  const uint32_t unload_before =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);

  bool8_t first_accepted = vkr_resource_system_load(
      VKR_RESOURCE_TYPE_TEXTURE, path, allocator, &first, &first_error);
  assert(first_accepted == true_v);
  assert(first_error == VKR_RENDERER_ERROR_NONE);
  assert(first.request_id != 0);

  uint64_t first_request_id = first.request_id;
  vkr_resource_system_unload(&first, path);

  // Immediate reload while cancelation is still in-flight should report the
  // canceled request state and not spawn duplicate work.
  VkrResourceHandleInfo canceled_view = {0};
  VkrRendererError canceled_error = VKR_RENDERER_ERROR_NONE;
  bool8_t canceled_accepted =
      vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path, allocator,
                               &canceled_view, &canceled_error);
  assert(canceled_accepted == false_v);
  assert(canceled_view.request_id == first_request_id);
  assert(canceled_view.load_state == VKR_RESOURCE_LOAD_STATE_CANCELED);
  assert(canceled_error == VKR_RENDERER_ERROR_NONE);
  vkr_resource_system_unload(&canceled_view, path);

  bool8_t reached_terminal = false_v;
  for (uint32_t i = 0; i < 300; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError first_state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState first_state =
        vkr_resource_system_get_state(&first, &first_state_error);
    if (first_state == VKR_RESOURCE_LOAD_STATE_INVALID) {
      reached_terminal = true_v;
      break;
    }
    vkr_platform_sleep(2);
  }
  assert(reached_terminal == true_v);

  VkrResourceHandleInfo reloaded = {0};
  VkrRendererError reload_error = VKR_RENDERER_ERROR_NONE;
  bool8_t reload_accepted = vkr_resource_system_load(
      VKR_RESOURCE_TYPE_TEXTURE, path, allocator, &reloaded, &reload_error);
  assert(reload_accepted == true_v);
  assert(reload_error == VKR_RENDERER_ERROR_NONE);
  assert(reloaded.request_id != 0);
  assert(reloaded.request_id != first_request_id);

  VkrRendererError state_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&reloaded, VKR_RESOURCE_LOAD_STATE_READY,
                                       &state_error) == true_v);
  assert(state_error == VKR_RENDERER_ERROR_NONE);
  assert(vkr_resource_system_is_ready(&reloaded) == true_v);

  vkr_resource_system_unload(&reloaded, path);

  const uint32_t load_after =
      atomic_load_explicit(&ctx->load_calls, memory_order_relaxed);
  const uint32_t unload_after =
      atomic_load_explicit(&ctx->unload_calls, memory_order_relaxed);
  assert(load_after >= load_before + 2u);
  assert(unload_after >= unload_before + 2u);

  printf("  test_resource_async_cancel_then_reload_same_path PASSED\n");
}

static void test_resource_async_dependency_waits_then_ready(
    VkrAllocator *allocator, RendererFrontend *renderer,
    ResourceAsyncDependencyContext *ctx) {
  printf("  Running test_resource_async_dependency_waits_then_ready...\n");

  String8 path = string8_lit("tests/assets/mesh_dep_ok.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  renderer->frame_active = true_v;

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.request_id != 0);

  bool8_t saw_pending_dependencies = false_v;
  bool8_t reached_ready = false_v;
  for (uint32_t i = 0; i < 600; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state = vkr_resource_system_get_state(&handle, &err);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES) {
      saw_pending_dependencies = true_v;
    }
    if (state == VKR_RESOURCE_LOAD_STATE_READY) {
      reached_ready = true_v;
      break;
    }
    assert(state != VKR_RESOURCE_LOAD_STATE_FAILED);
    vkr_platform_sleep(2);
  }
  assert(reached_ready == true_v);
  assert(saw_pending_dependencies == true_v);

  VkrResourceHandleInfo resolved = {0};
  assert(vkr_resource_system_try_get_resolved(&handle, &resolved) == true_v);
  assert(resolved.type == VKR_RESOURCE_TYPE_MESH);
  assert(resolved.as.mesh != NULL);

  vkr_resource_system_unload(&handle, path);
  renderer->frame_active = false_v;

  assert(atomic_load_explicit(&ctx->root_blocked_calls, memory_order_relaxed) >=
         1u);
  assert(atomic_load_explicit(&ctx->dep_prepare_calls, memory_order_relaxed) >=
         1u);
  assert(atomic_load_explicit(&ctx->root_release_calls, memory_order_relaxed) >=
         1u);

  printf("  test_resource_async_dependency_waits_then_ready PASSED\n");
}

static void test_resource_async_dependency_failure_propagates(
    VkrAllocator *allocator, RendererFrontend *renderer,
    ResourceAsyncDependencyContext *ctx) {
  printf("  Running test_resource_async_dependency_failure_propagates...\n");

  String8 path = string8_lit("tests/assets/mesh_dep_fail.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  renderer->frame_active = true_v;

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(handle.request_id != 0);

  bool8_t reached_failed = false_v;
  VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
  for (uint32_t i = 0; i < 600; ++i) {
    vkr_resource_system_pump(NULL);
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&handle, &state_error);
    if (state == VKR_RESOURCE_LOAD_STATE_FAILED) {
      reached_failed = true_v;
      break;
    }
    if (state == VKR_RESOURCE_LOAD_STATE_READY) {
      break;
    }
    vkr_platform_sleep(2);
  }

  assert(reached_failed == true_v);
  assert(state_error == VKR_RENDERER_ERROR_FILE_NOT_FOUND);

  vkr_resource_system_unload(&handle, path);
  renderer->frame_active = false_v;

  assert(atomic_load_explicit(&ctx->dep_finalize_calls, memory_order_relaxed) >=
         1u);
  assert(atomic_load_explicit(&ctx->root_finalize_calls,
                              memory_order_relaxed) >= 1u);

  printf("  test_resource_async_dependency_failure_propagates PASSED\n");
}

static void test_resource_async_pending_gpu_waits_for_submit_completion(
    VkrAllocator *allocator, RendererFrontend *renderer,
    ResourceAsyncMockBackendState *backend_state) {
  printf("  Running "
         "test_resource_async_pending_gpu_waits_for_submit_completion...\n");

  String8 path = string8_lit("tests/assets/dep_submit_serial.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  backend_state->submit_serial = 7;
  backend_state->completed_submit_serial = 7;
  renderer->frame_active = true_v;

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.request_id != 0);

  bool8_t reached_pending_gpu = false_v;
  for (uint32_t i = 0; i < 400; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&handle, &state_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      reached_pending_gpu = true_v;
      break;
    }
    assert(state != VKR_RESOURCE_LOAD_STATE_READY);
    assert(state != VKR_RESOURCE_LOAD_STATE_FAILED);
    vkr_platform_sleep(2);
  }
  assert(reached_pending_gpu == true_v);

  backend_state->completed_submit_serial = backend_state->submit_serial + 1u;
  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&handle, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);

  vkr_resource_system_unload(&handle, path);
  renderer->frame_active = false_v;

  printf(
      "  test_resource_async_pending_gpu_waits_for_submit_completion PASSED\n");
}

static void test_resource_async_finalize_requires_active_frame(
    VkrAllocator *allocator, RendererFrontend *renderer,
    ResourceAsyncDependencyContext *ctx) {
  printf("  Running test_resource_async_finalize_requires_active_frame...\n");

  String8 path = string8_lit("tests/assets/dep_frame_gate.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  renderer->frame_active = false_v;
  uint32_t finalize_before =
      atomic_load_explicit(&ctx->dep_finalize_calls, memory_order_relaxed);

  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL, path,
                                              allocator, &handle, &load_error);
  assert(accepted == true_v);
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.request_id != 0);

  for (uint32_t i = 0; i < 200; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&handle, &state_error);
    assert(state != VKR_RESOURCE_LOAD_STATE_READY);
    assert(state != VKR_RESOURCE_LOAD_STATE_FAILED);
    vkr_platform_sleep(2);
  }

  uint32_t finalize_inactive =
      atomic_load_explicit(&ctx->dep_finalize_calls, memory_order_relaxed);
  assert(finalize_inactive == finalize_before);

  renderer->frame_active = true_v;
  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&handle, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);
  assert(atomic_load_explicit(&ctx->dep_finalize_calls, memory_order_relaxed) >
         finalize_before);

  vkr_resource_system_unload(&handle, path);
  renderer->frame_active = false_v;

  printf("  test_resource_async_finalize_requires_active_frame PASSED\n");
}

static void test_resource_async_gpu_budget_throttles_finalize(
    VkrAllocator *allocator, RendererFrontend *renderer,
    ResourceAsyncMockBackendState *backend_state,
    ResourceAsyncBudgetContext *ctx) {
  printf("  Running test_resource_async_gpu_budget_throttles_finalize...\n");

  String8 path_a = string8_lit("tests/assets/a.budget.mock");
  String8 path_b = string8_lit("tests/assets/b.budget.mock");
  VkrResourceHandleInfo handle_a = {0};
  VkrResourceHandleInfo handle_b = {0};
  VkrRendererError error_a = VKR_RENDERER_ERROR_NONE;
  VkrRendererError error_b = VKR_RENDERER_ERROR_NONE;

  renderer->frame_active = false_v;
  bool8_t accepted_a = vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, path_a,
                                                allocator, &handle_a, &error_a);
  bool8_t accepted_b = vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, path_b,
                                                allocator, &handle_b, &error_b);
  assert(accepted_a == true_v);
  assert(accepted_b == true_v);
  assert(error_a == VKR_RENDERER_ERROR_NONE);
  assert(error_b == VKR_RENDERER_ERROR_NONE);

  bool8_t both_pending_gpu = false_v;
  for (uint32_t i = 0; i < 400; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError state_error_a = VKR_RENDERER_ERROR_NONE;
    VkrRendererError state_error_b = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state_a =
        vkr_resource_system_get_state(&handle_a, &state_error_a);
    VkrResourceLoadState state_b =
        vkr_resource_system_get_state(&handle_b, &state_error_b);
    assert(state_a != VKR_RESOURCE_LOAD_STATE_FAILED);
    assert(state_b != VKR_RESOURCE_LOAD_STATE_FAILED);
    if (state_a == VKR_RESOURCE_LOAD_STATE_PENDING_GPU &&
        state_b == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      both_pending_gpu = true_v;
      break;
    }
    vkr_platform_sleep(2);
  }
  assert(both_pending_gpu == true_v);

  uint32_t finalize_calls_before =
      atomic_load_explicit(&ctx->finalize_calls, memory_order_relaxed);

  renderer->frame_active = true_v;
  backend_state->submit_serial = 80;
  backend_state->completed_submit_serial = 128;

  VkrResourceAsyncBudget throttle_budget = {
      .max_finalize_requests = 8,
      .max_gpu_upload_ops = 1,
      .max_gpu_upload_bytes = 1024u,
  };
  vkr_resource_system_pump(&throttle_budget);

  VkrRendererError state_error_a = VKR_RENDERER_ERROR_NONE;
  VkrRendererError state_error_b = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState state_a =
      vkr_resource_system_get_state(&handle_a, &state_error_a);
  VkrResourceLoadState state_b =
      vkr_resource_system_get_state(&handle_b, &state_error_b);
  bool8_t one_ready_one_pending =
      (state_a == VKR_RESOURCE_LOAD_STATE_READY &&
       state_b == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) ||
      (state_b == VKR_RESOURCE_LOAD_STATE_READY &&
       state_a == VKR_RESOURCE_LOAD_STATE_PENDING_GPU);
  assert(one_ready_one_pending == true_v);

  vkr_resource_system_pump(&throttle_budget);
  VkrRendererError ready_error_a = VKR_RENDERER_ERROR_UNKNOWN;
  VkrRendererError ready_error_b = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&handle_a, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error_a) == true_v);
  assert(resource_async_wait_for_state(&handle_b, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error_b) == true_v);
  assert(ready_error_a == VKR_RENDERER_ERROR_NONE);
  assert(ready_error_b == VKR_RENDERER_ERROR_NONE);

  uint32_t finalize_calls_after =
      atomic_load_explicit(&ctx->finalize_calls, memory_order_relaxed);
  assert(finalize_calls_after >= finalize_calls_before + 2u);

  vkr_resource_system_unload(&handle_a, path_a);
  vkr_resource_system_unload(&handle_b, path_b);
  renderer->frame_active = false_v;

  printf("  test_resource_async_gpu_budget_throttles_finalize PASSED\n");
}

static void
test_scene_async_load_smoke(VkrAllocator *allocator, RendererFrontend *renderer,
                            ResourceAsyncMockBackendState *backend_state,
                            ResourceAsyncSceneContext *ctx) {
  printf("  Running test_scene_async_load_smoke...\n");

  String8 path = string8_lit("tests/assets/smoke.scene.mock");
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  renderer->frame_active = false_v;
  backend_state->submit_serial = 100u;
  backend_state->completed_submit_serial = 99u;

  float64_t start = vkr_platform_get_absolute_time();
  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, path,
                                              allocator, &handle, &load_error);
  float64_t elapsed = vkr_platform_get_absolute_time() - start;
  assert(accepted == true_v);
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.request_id != 0);
  // Async API should return quickly and leave work pending.
  assert(elapsed < 0.05);

  VkrRendererError initial_state_error = VKR_RENDERER_ERROR_NONE;
  VkrResourceLoadState initial_state =
      vkr_resource_system_get_state(&handle, &initial_state_error);
  assert(initial_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
         initial_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
         initial_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU);

  bool8_t reached_pending_gpu = false_v;
  for (uint32_t i = 0; i < 400; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&handle, &state_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      reached_pending_gpu = true_v;
      break;
    }
    assert(state != VKR_RESOURCE_LOAD_STATE_READY);
    assert(state != VKR_RESOURCE_LOAD_STATE_FAILED);
    vkr_platform_sleep(2);
  }
  assert(reached_pending_gpu == true_v);

  renderer->frame_active = true_v;
  backend_state->completed_submit_serial = backend_state->submit_serial + 1u;

  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&handle, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);
  assert(atomic_load_explicit(&ctx->prepare_calls, memory_order_relaxed) >= 1u);
  assert(atomic_load_explicit(&ctx->finalize_calls, memory_order_relaxed) >=
         1u);

  vkr_resource_system_unload(&handle, path);
  renderer->frame_active = false_v;

  printf("  test_scene_async_load_smoke PASSED\n");
}

static void
test_scene_reload_async_cancel(VkrAllocator *allocator,
                               RendererFrontend *renderer,
                               ResourceAsyncMockBackendState *backend_state,
                               ResourceAsyncSceneContext *ctx) {
  printf("  Running test_scene_reload_async_cancel...\n");

  String8 path = string8_lit("tests/assets/reload.scene.mock");
  VkrResourceHandleInfo first = {0};
  VkrRendererError first_error = VKR_RENDERER_ERROR_NONE;

  const uint32_t release_before =
      atomic_load_explicit(&ctx->release_calls, memory_order_relaxed);

  renderer->frame_active = false_v;
  bool8_t accepted = vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, path,
                                              allocator, &first, &first_error);
  assert(accepted == true_v);
  assert(first_error == VKR_RENDERER_ERROR_NONE);
  assert(first.request_id != 0);
  uint64_t first_request_id = first.request_id;

  vkr_resource_system_unload(&first, path);

  VkrResourceHandleInfo canceled_view = {0};
  VkrRendererError canceled_error = VKR_RENDERER_ERROR_NONE;
  bool8_t canceled_accepted =
      vkr_resource_system_load(VKR_RESOURCE_TYPE_SCENE, path, allocator,
                               &canceled_view, &canceled_error);
  assert(canceled_accepted == false_v);
  assert(canceled_view.request_id == first_request_id);
  assert(canceled_view.load_state == VKR_RESOURCE_LOAD_STATE_CANCELED);
  assert(canceled_error == VKR_RENDERER_ERROR_NONE);
  vkr_resource_system_unload(&canceled_view, path);

  bool8_t reached_terminal = false_v;
  for (uint32_t i = 0; i < 400; ++i) {
    vkr_resource_system_pump(NULL);
    VkrRendererError state_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state =
        vkr_resource_system_get_state(&first, &state_error);
    if (state == VKR_RESOURCE_LOAD_STATE_INVALID) {
      reached_terminal = true_v;
      break;
    }
    vkr_platform_sleep(2);
  }
  assert(reached_terminal == true_v);

  VkrResourceHandleInfo reloaded = {0};
  VkrRendererError reload_error = VKR_RENDERER_ERROR_NONE;
  bool8_t reload_accepted = vkr_resource_system_load(
      VKR_RESOURCE_TYPE_SCENE, path, allocator, &reloaded, &reload_error);
  assert(reload_accepted == true_v);
  assert(reload_error == VKR_RENDERER_ERROR_NONE);
  assert(reloaded.request_id != 0);
  assert(reloaded.request_id != first_request_id);

  renderer->frame_active = true_v;
  backend_state->submit_serial = 200u;
  backend_state->completed_submit_serial = 201u;
  VkrRendererError ready_error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(resource_async_wait_for_state(&reloaded, VKR_RESOURCE_LOAD_STATE_READY,
                                       &ready_error) == true_v);
  assert(ready_error == VKR_RENDERER_ERROR_NONE);

  vkr_resource_system_unload(&reloaded, path);
  renderer->frame_active = false_v;

  const uint32_t release_after =
      atomic_load_explicit(&ctx->release_calls, memory_order_relaxed);
  assert(release_after >= release_before + 2u);

  printf("  test_scene_reload_async_cancel PASSED\n");
}

bool32_t run_resource_async_state_tests(void) {
  printf("--- Running Resource Async State tests... ---\n");

  Arena *arena = arena_create(MB(4), MB(4));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrJobSystem job_system = {0};
  VkrJobSystemConfig cfg = resource_async_make_job_config();
  assert(vkr_job_system_init(&cfg, &job_system) == true_v);

  RendererFrontend renderer = {0};
  ResourceAsyncMockBackendState backend_state = {0};
  resource_async_mock_init_renderer(&renderer, &backend_state);

  assert(vkr_resource_system_init(&allocator, &renderer, &job_system) ==
         true_v);

  ResourceAsyncMockLoaderContext loader_ctx = {0};
  VkrResourceLoader loader = {
      .type = VKR_RESOURCE_TYPE_TEXTURE,
      .can_load = resource_async_mock_can_load,
      .load = resource_async_mock_load,
      .unload = resource_async_mock_unload,
  };
  assert(vkr_resource_system_register_loader(&loader_ctx, loader) == true_v);

  ResourceAsyncDependencyContext dependency_ctx = {0};
  VkrResourceLoader dependency_loader = {
      .type = VKR_RESOURCE_TYPE_MATERIAL,
      .can_load = resource_async_dep_can_load,
      .prepare_async = resource_async_dep_prepare,
      .finalize_async = resource_async_dep_finalize,
      .release_async_payload = resource_async_dep_release_payload,
      .unload = resource_async_dep_unload,
  };
  assert(vkr_resource_system_register_loader(&dependency_ctx,
                                             dependency_loader) == true_v);

  VkrResourceLoader root_loader = {
      .type = VKR_RESOURCE_TYPE_MESH,
      .can_load = resource_async_root_can_load,
      .prepare_async = resource_async_root_prepare,
      .finalize_async = resource_async_root_finalize,
      .release_async_payload = resource_async_root_release_payload,
      .unload = resource_async_root_unload,
  };
  assert(vkr_resource_system_register_loader(&dependency_ctx, root_loader) ==
         true_v);

  ResourceAsyncBudgetContext budget_ctx = {
      .prepare_calls = 0,
      .finalize_calls = 0,
      .release_calls = 0,
      .finalize_ops = 1,
      .finalize_bytes = 2048u,
      .token_counter = 0,
  };
  VkrResourceLoader budget_loader = {
      .type = VKR_RESOURCE_TYPE_SCENE,
      .can_load = resource_async_budget_can_load,
      .prepare_async = resource_async_budget_prepare,
      .finalize_async = resource_async_budget_finalize,
      .estimate_async_finalize_cost = resource_async_budget_estimate_cost,
      .release_async_payload = resource_async_budget_release_payload,
      .unload = resource_async_budget_unload,
  };
  assert(vkr_resource_system_register_loader(&budget_ctx, budget_loader) ==
         true_v);

  ResourceAsyncSceneContext scene_ctx = {
      .prepare_calls = 0,
      .finalize_calls = 0,
      .release_calls = 0,
      .unload_calls = 0,
      .token_counter = 0,
      .prepare_delay_ms = 20u,
  };
  VkrResourceLoader scene_loader = {
      .type = VKR_RESOURCE_TYPE_SCENE,
      .can_load = resource_async_scene_can_load,
      .prepare_async = resource_async_scene_prepare,
      .finalize_async = resource_async_scene_finalize,
      .estimate_async_finalize_cost = resource_async_scene_estimate_cost,
      .release_async_payload = resource_async_scene_release_payload,
      .unload = resource_async_scene_unload,
  };
  assert(vkr_resource_system_register_loader(&scene_ctx, scene_loader) ==
         true_v);

  test_resource_async_dedupe_and_ready(&allocator, &loader_ctx);
  test_resource_async_submit_saturation_recovers(&allocator, &job_system,
                                                 &loader_ctx);
  test_resource_async_failure_state(&allocator);
  test_resource_async_batch_accept_count(&allocator, &loader_ctx);
  test_resource_async_cancel_cleans_loaded_result(&allocator, &loader_ctx);
  test_resource_async_cancel_then_reload_same_path(&allocator, &loader_ctx);
  test_resource_async_dependency_waits_then_ready(&allocator, &renderer,
                                                  &dependency_ctx);
  test_resource_async_dependency_failure_propagates(&allocator, &renderer,
                                                    &dependency_ctx);
  test_resource_async_finalize_requires_active_frame(&allocator, &renderer,
                                                     &dependency_ctx);
  test_resource_async_pending_gpu_waits_for_submit_completion(
      &allocator, &renderer, &backend_state);
  test_resource_async_gpu_budget_throttles_finalize(
      &allocator, &renderer, &backend_state, &budget_ctx);
  test_scene_async_load_smoke(&allocator, &renderer, &backend_state,
                              &scene_ctx);
  test_scene_reload_async_cancel(&allocator, &renderer, &backend_state,
                                 &scene_ctx);

  vkr_job_system_shutdown(&job_system);
  arena_destroy(arena);

  printf("--- Resource Async State tests completed. ---\n");
  return true_v;
}

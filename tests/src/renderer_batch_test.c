#include "renderer_batch_test.h"

#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct RendererBatchMockState {
  bool8_t use_scripted_batch;
  uint32_t scripted_count;
  uint32_t scripted_created;
  VkrBackendResourceHandle scripted_handles[16];
  VkrRendererError scripted_errors[16];

  uint32_t batch_call_count;
  uint32_t fail_on_batch_call;
  uint32_t fail_request_index;
  VkrRendererError fail_error;

  uint32_t create_call_count;
  uint32_t upload_call_count;
  uint32_t upload_fail_call;
  VkrRendererError upload_fail_error;
  uint32_t destroy_call_count;
  uint32_t upload_wait_stats_call_count;
  VkrRendererUploadWaitStats upload_wait_stats;

  uintptr_t next_handle_token;
} RendererBatchMockState;

static VkrBackendResourceHandle renderer_batch_mock_make_handle(
    RendererBatchMockState *state) {
  state->next_handle_token++;
  return (VkrBackendResourceHandle){
      .ptr = (void *)((state->next_handle_token << 4u) | 1u)};
}

static uint32_t renderer_batch_mock_buffer_create_batch(
    void *backend_state, const VkrBufferBatchCreateRequest *requests,
    uint32_t count, VkrBackendResourceHandle *out_handles,
    VkrRendererError *out_errors) {
  (void)requests;
  RendererBatchMockState *state = backend_state;
  state->batch_call_count++;

  if (state->use_scripted_batch) {
    assert(state->scripted_count == count);
    for (uint32_t i = 0; i < count; ++i) {
      out_handles[i] = state->scripted_handles[i];
      out_errors[i] = state->scripted_errors[i];
    }
    return state->scripted_created;
  }

  uint32_t created = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (state->fail_on_batch_call == state->batch_call_count &&
        i == state->fail_request_index) {
      out_handles[i] = (VkrBackendResourceHandle){0};
      out_errors[i] = state->fail_error;
      continue;
    }

    out_handles[i] = renderer_batch_mock_make_handle(state);
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    created++;
  }

  return created;
}

static uint32_t renderer_batch_mock_texture_create_with_payload_batch(
    void *backend_state, const VkrTextureBatchCreateRequest *requests,
    uint32_t count, VkrBackendResourceHandle *out_handles,
    VkrRendererError *out_errors) {
  (void)requests;
  RendererBatchMockState *state = backend_state;
  state->batch_call_count++;
  assert(state->scripted_count == count);

  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i] = state->scripted_handles[i];
    out_errors[i] = state->scripted_errors[i];
  }

  return state->scripted_created;
}

static VkrBackendResourceHandle renderer_batch_mock_buffer_create(
    void *backend_state, const VkrBufferDescription *desc,
    const void *initial_data) {
  (void)desc;
  (void)initial_data;
  RendererBatchMockState *state = backend_state;
  state->create_call_count++;
  return renderer_batch_mock_make_handle(state);
}

static VkrRendererError renderer_batch_mock_buffer_upload(
    void *backend_state, VkrBackendResourceHandle handle, uint64_t offset,
    uint64_t size, const void *data) {
  (void)handle;
  (void)offset;
  (void)size;
  (void)data;

  RendererBatchMockState *state = backend_state;
  state->upload_call_count++;
  if (state->upload_fail_call > 0 &&
      state->upload_call_count == state->upload_fail_call) {
    return state->upload_fail_error;
  }

  return VKR_RENDERER_ERROR_NONE;
}

static void renderer_batch_mock_buffer_destroy(void *backend_state,
                                               VkrBackendResourceHandle handle) {
  (void)handle;
  RendererBatchMockState *state = backend_state;
  state->destroy_call_count++;
}

static bool8_t renderer_batch_mock_get_and_reset_upload_wait_stats(
    void *backend_state, VkrRendererUploadWaitStats *out_stats) {
  RendererBatchMockState *state = backend_state;
  assert(state != NULL);
  assert(out_stats != NULL);

  state->upload_wait_stats_call_count++;
  *out_stats = state->upload_wait_stats;
  state->upload_wait_stats = (VkrRendererUploadWaitStats){0};
  return true_v;
}

static void renderer_batch_test_init_frontend(RendererFrontend *renderer,
                                              RendererBatchMockState *state) {
  MemZero(renderer, sizeof(*renderer));
  MemZero(state, sizeof(*state));

  renderer->arena = arena_create(MB(2), MB(2));
  assert(renderer->arena != NULL);
  renderer->allocator = (VkrAllocator){.ctx = renderer->arena};
  assert(vkr_allocator_arena(&renderer->allocator));

  renderer->scratch_arena = arena_create(MB(2), MB(2));
  assert(renderer->scratch_arena != NULL);
  renderer->scratch_allocator = (VkrAllocator){.ctx = renderer->scratch_arena};
  assert(vkr_allocator_arena(&renderer->scratch_allocator));

  renderer->backend_state = state;
  state->next_handle_token = 0x100u;
}

static void renderer_batch_test_shutdown_frontend(RendererFrontend *renderer) {
  if (renderer->scratch_arena) {
    arena_destroy(renderer->scratch_arena);
    renderer->scratch_arena = NULL;
  }
  if (renderer->arena) {
    arena_destroy(renderer->arena);
    renderer->arena = NULL;
  }
}

static void test_renderer_buffer_batch_fallback_cleanup(void) {
  printf("  Running test_renderer_buffer_batch_fallback_cleanup...\n");

  RendererFrontend renderer = {0};
  RendererBatchMockState state = {0};
  renderer_batch_test_init_frontend(&renderer, &state);
  renderer.backend.buffer_create = renderer_batch_mock_buffer_create;
  renderer.backend.buffer_upload = renderer_batch_mock_buffer_upload;
  renderer.backend.buffer_destroy = renderer_batch_mock_buffer_destroy;

  uint8_t payload_bytes[16] = {0};
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  VkrBufferDescription descs[3] = {
      {.size = 16,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_VERTEX_BUFFER),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type,
       .bind_on_create = true_v},
      {.size = 16,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type,
       .bind_on_create = true_v},
      {.size = 8,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type,
       .bind_on_create = true_v},
  };
  VkrBufferUploadPayload uploads[3] = {
      {.data = payload_bytes, .size = 16, .offset = 0},
      {.data = payload_bytes, .size = 4, .offset = 4},
      {.data = payload_bytes, .size = 9, .offset = 0},
  };
  VkrBufferBatchCreateRequest requests[3] = {
      {.description = &descs[0], .upload = &uploads[0]},
      {.description = &descs[1], .upload = &uploads[1]},
      {.description = &descs[2], .upload = &uploads[2]},
  };

  state.upload_fail_call = 1;
  state.upload_fail_error = VKR_RENDERER_ERROR_DEVICE_ERROR;

  VkrBufferHandle out_handles[3] = {0};
  VkrRendererError out_errors[3] = {0};
  uint32_t created = vkr_renderer_create_buffer_batch(&renderer, requests, 3,
                                                      out_handles, out_errors);

  assert(created == 1);
  assert(out_handles[0] != NULL);
  assert(out_errors[0] == VKR_RENDERER_ERROR_NONE);

  assert(out_handles[1] == NULL);
  assert(out_errors[1] == VKR_RENDERER_ERROR_DEVICE_ERROR);

  assert(out_handles[2] == NULL);
  assert(out_errors[2] == VKR_RENDERER_ERROR_INVALID_PARAMETER);

  assert(state.create_call_count == 2);
  assert(state.upload_call_count == 1);
  assert(state.destroy_call_count == 1);

  if (out_handles[0]) {
    vkr_renderer_destroy_buffer(&renderer, out_handles[0]);
  }
  renderer_batch_test_shutdown_frontend(&renderer);

  printf("  test_renderer_buffer_batch_fallback_cleanup PASSED\n");
}

static void test_renderer_buffer_batch_backend_mapping(void) {
  printf("  Running test_renderer_buffer_batch_backend_mapping...\n");

  RendererFrontend renderer = {0};
  RendererBatchMockState state = {0};
  renderer_batch_test_init_frontend(&renderer, &state);
  renderer.backend.buffer_create_batch = renderer_batch_mock_buffer_create_batch;

  state.use_scripted_batch = true_v;
  state.scripted_count = 3;
  state.scripted_created = 2;
  state.scripted_handles[0] = (VkrBackendResourceHandle){.ptr = (void *)0x1011};
  state.scripted_errors[0] = VKR_RENDERER_ERROR_UNKNOWN;
  state.scripted_handles[1] = (VkrBackendResourceHandle){0};
  state.scripted_errors[1] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  state.scripted_handles[2] = (VkrBackendResourceHandle){.ptr = (void *)0x1031};
  state.scripted_errors[2] = VKR_RENDERER_ERROR_NONE;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  VkrBufferDescription descs[3] = {
      {.size = 4,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_DST),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type},
      {.size = 4,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_DST),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type},
      {.size = 4,
       .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_DST),
       .memory_properties = vkr_memory_property_flags_from_bits(
           VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
       .buffer_type = buffer_type},
  };
  VkrBufferBatchCreateRequest requests[3] = {
      {.description = &descs[0]},
      {.description = &descs[1]},
      {.description = &descs[2]},
  };

  VkrBufferHandle out_handles[3] = {0};
  VkrRendererError out_errors[3] = {0};
  uint32_t created = vkr_renderer_create_buffer_batch(&renderer, requests, 3,
                                                      out_handles, out_errors);

  assert(created == 2);
  assert(state.batch_call_count == 1);
  assert(out_handles[0] != NULL);
  assert(out_errors[0] == VKR_RENDERER_ERROR_NONE);
  assert(out_handles[1] == NULL);
  assert(out_errors[1] == VKR_RENDERER_ERROR_INVALID_PARAMETER);
  assert(out_handles[2] != NULL);
  assert(out_errors[2] == VKR_RENDERER_ERROR_NONE);

  renderer_batch_test_shutdown_frontend(&renderer);

  printf("  test_renderer_buffer_batch_backend_mapping PASSED\n");
}

static void test_renderer_texture_batch_backend_mapping(void) {
  printf("  Running test_renderer_texture_batch_backend_mapping...\n");

  RendererFrontend renderer = {0};
  RendererBatchMockState state = {0};
  renderer_batch_test_init_frontend(&renderer, &state);
  renderer.backend.texture_create_with_payload_batch =
      renderer_batch_mock_texture_create_with_payload_batch;

  state.scripted_count = 3;
  state.scripted_created = 2;
  state.scripted_handles[0] = (VkrBackendResourceHandle){.ptr = (void *)0x2011};
  state.scripted_errors[0] = VKR_RENDERER_ERROR_UNKNOWN;
  state.scripted_handles[1] = (VkrBackendResourceHandle){0};
  state.scripted_errors[1] = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  state.scripted_handles[2] = (VkrBackendResourceHandle){.ptr = (void *)0x2031};
  state.scripted_errors[2] = VKR_RENDERER_ERROR_NONE;

  VkrTextureDescription descs[3] = {0};
  VkrTextureUploadPayload payloads[3] = {0};
  VkrTextureBatchCreateRequest requests[3] = {
      {.description = &descs[0], .payload = &payloads[0]},
      {.description = &descs[1], .payload = &payloads[1]},
      {.description = &descs[2], .payload = &payloads[2]},
  };

  VkrTextureOpaqueHandle out_handles[3] = {0};
  VkrRendererError out_errors[3] = {0};
  uint32_t created = vkr_renderer_create_texture_with_payload_batch(
      &renderer, requests, 3, out_handles, out_errors);

  assert(created == 2);
  assert(state.batch_call_count == 1);
  assert(out_handles[0] != NULL);
  assert(out_errors[0] == VKR_RENDERER_ERROR_NONE);
  assert(out_handles[1] == NULL);
  assert(out_errors[1] == VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
  assert(out_handles[2] != NULL);
  assert(out_errors[2] == VKR_RENDERER_ERROR_NONE);

  renderer_batch_test_shutdown_frontend(&renderer);

  printf("  test_renderer_texture_batch_backend_mapping PASSED\n");
}

static void test_geometry_system_batch_failure_rolls_back_buffers(void) {
  printf("  Running test_geometry_system_batch_failure_rolls_back_buffers...\n");

  RendererFrontend renderer = {0};
  RendererBatchMockState state = {0};
  renderer_batch_test_init_frontend(&renderer, &state);
  renderer.backend.buffer_create_batch = renderer_batch_mock_buffer_create_batch;
  renderer.backend.buffer_destroy = renderer_batch_mock_buffer_destroy;

  VkrGeometrySystem geometry_system = {0};
  VkrGeometrySystemConfig config = {.max_geometries = 32};
  VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(vkr_geometry_system_init(&geometry_system, &renderer, &config, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);

  const uint32_t batch_calls_after_init = state.batch_call_count;
  const uint32_t destroy_calls_after_init = state.destroy_call_count;
  state.fail_on_batch_call = batch_calls_after_init + 1;
  state.fail_request_index = 3;
  state.fail_error = VKR_RENDERER_ERROR_DEVICE_ERROR;

  VkrVertex3d vertices_a[3] = {0};
  uint32_t indices_a[3] = {0, 1, 2};
  VkrVertex3d vertices_b[3] = {0};
  uint32_t indices_b[3] = {0, 1, 2};
  VkrGeometryConfig create_configs[2] = {
      {.vertex_size = sizeof(VkrVertex3d),
       .vertex_count = 3,
       .vertices = vertices_a,
       .index_size = sizeof(uint32_t),
       .index_count = 3,
       .indices = indices_a,
       .name = "batch_geom_a"},
      {.vertex_size = sizeof(VkrVertex3d),
       .vertex_count = 3,
       .vertices = vertices_b,
       .index_size = sizeof(uint32_t),
       .index_count = 3,
       .indices = indices_b,
       .name = "batch_geom_b"},
  };

  VkrGeometryHandle out_handles[2] = {0};
  VkrRendererError out_errors[2] = {0};
  uint32_t created = vkr_geometry_system_create_batch(
      &geometry_system, create_configs, 2, false_v, out_handles, out_errors);

  assert(created == 1);
  assert(out_handles[0].id != 0);
  assert(out_errors[0] == VKR_RENDERER_ERROR_NONE);

  assert(out_handles[1].id == 0);
  assert(out_errors[1] == VKR_RENDERER_ERROR_DEVICE_ERROR);

  assert(state.batch_call_count == batch_calls_after_init + 1);
  assert(state.destroy_call_count == destroy_calls_after_init + 1);

  vkr_geometry_system_shutdown(&geometry_system);
  renderer_batch_test_shutdown_frontend(&renderer);

  printf("  test_geometry_system_batch_failure_rolls_back_buffers PASSED\n");
}

static void test_renderer_upload_wait_stats_mapping(void) {
  printf("  Running test_renderer_upload_wait_stats_mapping...\n");

  RendererFrontend renderer = {0};
  RendererBatchMockState state = {0};
  renderer_batch_test_init_frontend(&renderer, &state);
  renderer.backend.get_and_reset_upload_wait_stats =
      renderer_batch_mock_get_and_reset_upload_wait_stats;

  state.upload_wait_stats = (VkrRendererUploadWaitStats){
      .fence_wait_count = 3,
      .queue_wait_idle_count = 2,
      .device_wait_idle_count = 1,
  };

  VkrRendererUploadWaitStats stats = {0};
  assert(vkr_renderer_get_and_reset_upload_wait_stats(&renderer, &stats) ==
         true_v);
  assert(stats.fence_wait_count == 3);
  assert(stats.queue_wait_idle_count == 2);
  assert(stats.device_wait_idle_count == 1);
  assert(state.upload_wait_stats_call_count == 1);

  stats = (VkrRendererUploadWaitStats){0};
  assert(vkr_renderer_get_and_reset_upload_wait_stats(&renderer, &stats) ==
         true_v);
  assert(stats.fence_wait_count == 0);
  assert(stats.queue_wait_idle_count == 0);
  assert(stats.device_wait_idle_count == 0);
  assert(state.upload_wait_stats_call_count == 2);

  renderer_batch_test_shutdown_frontend(&renderer);
  printf("  test_renderer_upload_wait_stats_mapping PASSED\n");
}

bool32_t run_renderer_batch_tests(void) {
  printf("--- Running Renderer Batch tests... ---\n");
  test_renderer_buffer_batch_fallback_cleanup();
  test_renderer_buffer_batch_backend_mapping();
  test_renderer_texture_batch_backend_mapping();
  test_geometry_system_batch_failure_rolls_back_buffers();
  test_renderer_upload_wait_stats_mapping();
  printf("--- Renderer Batch tests completed. ---\n");
  return true_v;
}

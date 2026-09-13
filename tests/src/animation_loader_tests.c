#include "animation_loader_tests.h"

#include "assets/vkr_animation_encode.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/animation_loader.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void animation_loader_test_write(const char *path, const uint8_t *bytes,
                                        uint64_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(bytes, 1u, (size_t)size, file) == size);
  assert(fclose(file) == 0);
}

/* Exercise the actual CPU worker fallback and retained-request ownership. */
static void animation_loader_test_async(String8 name, VkrAllocator *scratch) {
  Arena *arena = arena_create(MB(16), MB(1));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrJobSystem jobs = {0};
  VkrJobSystemConfig config = vkr_job_system_config_default();
  config.worker_count = 1u;
  config.max_jobs = 32u;
  config.queue_capacity = 32u;
  assert(vkr_job_system_init(&config, &jobs));
  assert(vkr_resource_system_init(&allocator, &jobs, NULL));
  assert(vkr_resource_system_register_loader(&allocator,
                                             vkr_animation_loader_create()));
  VkrResourceHandleInfo first = {0};
  VkrResourceHandleInfo second = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION, name, scratch,
                                  &first, &error));
  assert(first.request_id != 0u);
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION, name, scratch,
                                  &second, &error));
  assert(first.request_id == second.request_id);
  VkrResourceSubmissionState submission = {.submit_serial = 1u,
                                           .completed_submit_serial = 2u};
  VkrResourceHandleInfo resolved = {0};
  for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
    vkr_resource_system_pump(submission, NULL);
    if (vkr_resource_system_try_get_resolved(&second, &resolved)) {
      break;
    }
    vkr_platform_sleep(2u);
  }
  assert(resolved.type == VKR_RESOURCE_TYPE_ANIMATION && resolved.as.animation);
  vkr_resource_system_unload(&first, name);
  assert(vkr_resource_system_try_get_resolved(&second, &resolved));
  assert(resolved.as.animation->asset.source_fingerprint == 123u);
  vkr_resource_system_unload(&second, name);
  /* A canceled request retains its key until the worker completion drains.
   * Wait for that request to expire before reloading the same path. */
  uint64_t previous_request = second.request_id;
  for (uint32_t i = 0u; i < 8u; ++i) {
    VkrResourceHandleInfo cancelled = {0};
    assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION, name, scratch,
                                    &cancelled, &error));
    assert(cancelled.request_id != 0u &&
           cancelled.request_id != previous_request);
    previous_request = cancelled.request_id;
    vkr_resource_system_unload(&cancelled, name);
    VkrResourceLoadState state = VKR_RESOURCE_LOAD_STATE_CANCELED;
    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
      vkr_resource_system_pump(submission, NULL);
      state = vkr_resource_system_get_state(&cancelled, &error);
      if (state == VKR_RESOURCE_LOAD_STATE_INVALID) {
        break;
      }
      assert(state == VKR_RESOURCE_LOAD_STATE_CANCELED);
      vkr_platform_sleep(2u);
    }
    assert(state == VKR_RESOURCE_LOAD_STATE_INVALID);
  }
  /* Also leave one canceled worker request for the shutdown drain. */
  VkrResourceHandleInfo shutdown_cancelled = {0};
  assert(vkr_resource_system_load(VKR_RESOURCE_TYPE_ANIMATION, name, scratch,
                                  &shutdown_cancelled, &error));
  vkr_resource_system_unload(&shutdown_cancelled, name);
  vkr_job_system_shutdown(&jobs);
  vkr_resource_system_shutdown();
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
}

bool32_t run_animation_loader_tests(void) {
  char directory[1024];
  snprintf(directory, sizeof(directory), "%stests/tmp", PROJECT_SOURCE_DIR);
#if defined(_WIN32)
  const int32_t directory_result = _mkdir(directory);
#else
  const int32_t directory_result = mkdir(directory, 0755);
#endif
  assert(directory_result == 0 || errno == EEXIST);
  char path[1200];
  snprintf(path, sizeof(path), "%s/animation_loader_%u.vka", directory,
           vkr_platform_get_process_id());
  const String8 name = {.str = (uint8_t *)path, .length = strlen(path)};
  Arena *arena = arena_create(MB(16), MB(1));
  assert(arena);
  VkrAllocator scratch = {.ctx = arena};
  assert(vkr_allocator_arena(&scratch));
  VkrAnimationNode node = {
      .name = string8_lit("root"),
      .parent = VKR_ANIMATION_NO_NODE,
      .local = mat4_identity(),
      .rest = {.rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}},
  };
  uint32_t order = 0u;
  VkrAnimationAsset asset = {.source_fingerprint = 123u,
                             .node_count = 1u,
                             .nodes = &node,
                             .node_order = &order};
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  const char *error = NULL;
  assert(vkr_animation_cooked_encode(&scratch, &asset, &bytes, &size, &error));
  animation_loader_test_write(path, bytes, size);
  VkrResourceLoader loader = vkr_animation_loader_create();
  loader.id = 23u;
  assert(loader.can_load(&loader, name));
  assert(!loader.can_load(&loader, string8_lit("model.gltf")));
  assert(!loader.can_load(&loader, string8_lit("model.vka.tmp")));
  const VkrAllocatorStatistics baseline = vkr_allocator_get_global_statistics();
  const uint64_t scratch_bytes = scratch.stats.total_allocated;
  for (uint32_t iteration = 0; iteration < 32u; ++iteration) {
    VkrResourceHandleInfo first = {0};
    VkrResourceHandleInfo second = {0};
    VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
    assert(loader.load(&loader, name, &scratch, &first, &load_error));
    assert(loader.load(&loader, name, &scratch, &second, &load_error));
    assert(load_error == VKR_RENDERER_ERROR_NONE);
    assert(first.type == VKR_RESOURCE_TYPE_ANIMATION && first.loader_id == 23u);
    assert(first.as.animation != second.as.animation);
    assert(first.as.animation->asset.nodes != second.as.animation->asset.nodes);
    assert(first.as.animation->asset.source_fingerprint == 123u);
    assert(first.as.animation->source_path.str != name.str);
    assert(first.as.animation->source_path.length == name.length);
    assert(MemCompare(first.as.animation->source_path.str, name.str,
                      name.length) == 0);
    assert(scratch.stats.total_allocated == scratch_bytes);
    loader.unload(&loader, &first, name);
    assert(second.as.animation->asset.node_count == 1u);
    assert(second.as.animation->asset.nodes[0].name.length == 4u);
    assert(MemCompare(second.as.animation->asset.nodes[0].name.str, "root",
                      4u) == 0);
    loader.unload(&loader, &second, name);
    const VkrAllocatorStatistics after = vkr_allocator_get_global_statistics();
    assert(after.total_allocated == baseline.total_allocated);
    for (uint32_t tag = 0u; tag < VKR_ALLOCATOR_MEMORY_TAG_MAX; ++tag) {
      assert(after.tagged_allocs[tag] == baseline.tagged_allocs[tag]);
    }
  }
  /* Truncation after the header reaches decode cleanup; an undersized header
   * exercises cleanup before result-arena creation. */
  const uint64_t bad_sizes[] = {12u, size - 1u};
  for (uint32_t i = 0; i < ArrayCount(bad_sizes); ++i) {
    animation_loader_test_write(path, bytes, bad_sizes[i]);
    VkrResourceHandleInfo handle;
    MemSet(&handle, 0xa5, sizeof(handle));
    VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
    assert(!loader.load(&loader, name, &scratch, &handle, &load_error));
    assert(load_error != VKR_RENDERER_ERROR_NONE);
    assert(handle.type == VKR_RESOURCE_TYPE_UNKNOWN && !handle.as.animation);
    assert(scratch.stats.total_allocated == scratch_bytes);
    assert(vkr_allocator_get_global_statistics().total_allocated ==
           baseline.total_allocated);
  }
  animation_loader_test_write(path, bytes, size);
  animation_loader_test_async(name, &scratch);
  assert(remove(path) == 0);
  const uint64_t missing_baseline =
      vkr_allocator_get_global_statistics().total_allocated;
  VkrResourceHandleInfo missing = {0};
  VkrRendererError missing_error = VKR_RENDERER_ERROR_NONE;
  assert(!loader.load(&loader, name, &scratch, &missing, &missing_error));
  assert(missing_error == VKR_RENDERER_ERROR_FILE_NOT_FOUND);
  assert(vkr_allocator_get_global_statistics().total_allocated ==
         missing_baseline);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(arena);
  return true_v;
}

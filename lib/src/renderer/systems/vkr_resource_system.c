#include "renderer/systems/vkr_resource_system.h"
#include "containers/vkr_hashtable.h"
#include "memory/vkr_allocator.h"

typedef struct VkrResourceAsyncRequest {
  bool8_t in_use;
  uint64_t request_id;
  char *key;
  String8 path;
  VkrResourceType type;
  uint32_t loader_id;
  VkrResourceLoadState load_state;
  VkrRendererError last_error;
  uint32_t ref_count;
  bool8_t cancel_requested;
  bool8_t cpu_job_in_flight;
  uint64_t gpu_submit_serial;
  void *async_payload;
  VkrResourceHandleInfo loaded_info;
} VkrResourceAsyncRequest;

typedef struct VkrResourceAsyncCompletion {
  uint64_t request_id;
  uint32_t loader_id;
  bool8_t has_async_payload;
  void *async_payload;
  bool8_t loaded;
  VkrResourceHandleInfo loaded_info;
  VkrRendererError load_error;
  String8 path;
} VkrResourceAsyncCompletion;

typedef struct VkrResourceAsyncJobPayload {
  struct VkrResourceSystem *system;
  uint64_t request_id;
  uint32_t loader_id;
  VkrResourceType type;
  String8 path;
} VkrResourceAsyncJobPayload;

struct VkrResourceSystem {
  VkrAllocator *allocator;
  VkrRendererFrontendHandle renderer;
  VkrJobSystem *job_system;

  // Registered loaders
  VkrResourceLoader *loaders;
  uint32_t loader_count;
  uint32_t loader_capacity;

  VkrMutex mutex;
  VkrHashTable_uint32_t request_by_key;
  VkrResourceAsyncRequest *requests;
  uint32_t request_capacity;
  uint64_t next_request_id;

  // Worker -> render-thread completion queue.
  VkrResourceAsyncCompletion *completions;
  uint32_t completion_capacity;
  uint32_t completion_head;
  uint32_t completion_tail;
  uint32_t completion_count;
};

/**
 * @brief Global resource system instance.
 */
vkr_global VkrResourceSystem *vkr_resource_system = NULL;

vkr_internal _Thread_local bool8_t g_resource_system_force_sync = false_v;

#define VKR_RESOURCE_COMPLETION_QUEUE_INITIAL_CAPACITY 512u

vkr_internal const VkrResourceAsyncBudget vkr_resource_async_budget_default = {
    .max_finalize_requests = 32,
    .max_gpu_upload_ops = 64,
    .max_gpu_upload_bytes = 32ull * 1024ull * 1024ull,
};

vkr_internal bool8_t vkr_resource_system_async_load_job_run(VkrJobContext *ctx,
                                                            void *payload);

/**
 * @brief Strip accidental "<type>|" request-key prefixes from resource paths.
 *
 * Some async race/corruption paths can surface an internal dedupe key as a
 * loader path (for example `1|assets/textures/foo.png`). Loaders expect raw
 * paths, so normalize this here to keep the pipeline resilient.
 */
vkr_internal String8 vkr_resource_system_normalize_path(VkrResourceType type,
                                                        String8 path) {
  if (!path.str || path.length < 3) {
    return path;
  }

  String8 normalized = path;
  for (uint32_t pass = 0; pass < 4; ++pass) {
    uint64_t prefix_start = UINT64_MAX;
    uint64_t prefix_pipe = UINT64_MAX;
    uint64_t parsed_type = UINT64_MAX;

    for (uint64_t segment_start = 0; segment_start < normalized.length;) {
      if (segment_start > 0) {
        uint8_t prev = normalized.str[segment_start - 1];
        if (prev != '/' && prev != '\\') {
          segment_start++;
          continue;
        }
      }

      uint64_t index = segment_start;
      uint64_t parsed = 0;
      while (index < normalized.length && normalized.str[index] >= '0' &&
             normalized.str[index] <= '9') {
        parsed = parsed * 10ull + (uint64_t)(normalized.str[index] - '0');
        index++;
      }

      if (index > segment_start && index < normalized.length &&
          normalized.str[index] == '|') {
        prefix_start = segment_start;
        prefix_pipe = index;
        parsed_type = parsed;
        break;
      }

      while (segment_start < normalized.length &&
             normalized.str[segment_start] != '/' &&
             normalized.str[segment_start] != '\\') {
        segment_start++;
      }
      if (segment_start < normalized.length) {
        segment_start++;
      }
    }

    if (prefix_pipe == UINT64_MAX) {
      break;
    }

    String8 stripped =
        string8_substring(&normalized, prefix_pipe + 1, normalized.length);
    if (!stripped.str || stripped.length == 0) {
      break;
    }

    if (type != VKR_RESOURCE_TYPE_UNKNOWN && parsed_type != (uint64_t)type) {
      log_warn("Resource path '%.*s' carried mismatched key prefix (%llu) for "
               "type %u; "
               "stripping prefix",
               (int32_t)normalized.length, normalized.str,
               (unsigned long long)parsed_type, (uint32_t)type);
    } else if (prefix_start > 0) {
      log_warn("Resource path '%.*s' carried embedded request-key prefix; "
               "stripping to '%.*s'",
               (int32_t)normalized.length, normalized.str,
               (int32_t)stripped.length, stripped.str);
    }

    normalized = stripped;
  }

  return normalized;
}

vkr_internal void
vkr_resource_system_reset_handle_info(VkrResourceHandleInfo *out_info) {
  if (!out_info) {
    return;
  }

  MemZero(out_info, sizeof(*out_info));
  out_info->type = VKR_RESOURCE_TYPE_UNKNOWN;
  out_info->loader_id = VKR_INVALID_ID;
  out_info->load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
  out_info->last_error = VKR_RENDERER_ERROR_NONE;
  out_info->request_id = 0;
}

vkr_internal void
vkr_resource_system_completion_reset(VkrResourceAsyncCompletion *completion) {
  if (!completion) {
    return;
  }
  MemZero(completion, sizeof(*completion));
}

vkr_internal void vkr_resource_system_completion_release_path(
    VkrResourceSystem *system, VkrResourceAsyncCompletion *completion) {
  if (!system || !completion || !completion->path.str) {
    return;
  }

  vkr_allocator_free(system->allocator, completion->path.str,
                     completion->path.length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  completion->path = (String8){0};
}

vkr_internal bool8_t
vkr_resource_system_is_async_default_type(VkrResourceType type) {
  switch (type) {
  case VKR_RESOURCE_TYPE_TEXTURE:
  case VKR_RESOURCE_TYPE_MATERIAL:
  case VKR_RESOURCE_TYPE_MESH:
  case VKR_RESOURCE_TYPE_SCENE:
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_resource_system_loader_type_matches(
    VkrResourceType requested, const VkrResourceLoader *loader) {
  if (!loader) {
    return false_v;
  }
  return requested == VKR_RESOURCE_TYPE_UNKNOWN || loader->type == requested;
}

vkr_internal VkrResourceLoader *
vkr_resource_system_find_loader_for_path(VkrResourceType type, String8 path) {
  if (!vkr_resource_system || !path.str || path.length == 0) {
    return NULL;
  }

  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       ++loader_idx) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (!vkr_resource_system_loader_type_matches(type, loader)) {
      continue;
    }
    if (loader->can_load && !loader->can_load(loader, path)) {
      continue;
    }
    return loader;
  }

  return NULL;
}

vkr_internal void vkr_resource_system_release_async_payload(uint32_t loader_id,
                                                            void *payload) {
  if (!vkr_resource_system || !payload || loader_id == VKR_INVALID_ID ||
      loader_id >= vkr_resource_system->loader_count) {
    return;
  }

  VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_id];
  if (loader->release_async_payload) {
    loader->release_async_payload(loader, payload);
  }
}

vkr_internal bool8_t vkr_resource_system_load_sync_internal(
    VkrResourceType type, String8 path, VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_info, VkrRendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  path = vkr_resource_system_normalize_path(type, path);

  if (!out_error) {
    return false_v;
  }

  vkr_resource_system_reset_handle_info(out_info);
  *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;

  bool8_t previous_force_sync = g_resource_system_force_sync;
  g_resource_system_force_sync = true_v;

  bool8_t loaded = false_v;
  VkrResourceLoader *loader =
      vkr_resource_system_find_loader_for_path(type, path);
  if (loader && loader->load) {
    VkrResourceHandleInfo loaded_info = {0};
    VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
    if (loader->load(loader, path, temp_alloc, &loaded_info, &load_error)) {
      loaded_info.loader_id = loader->id;
      loaded_info.load_state = VKR_RESOURCE_LOAD_STATE_READY;
      loaded_info.last_error = VKR_RENDERER_ERROR_NONE;
      loaded_info.request_id = 0;
      *out_info = loaded_info;
      *out_error = VKR_RENDERER_ERROR_NONE;
      loaded = true_v;
    } else {
      *out_error = load_error;
    }
  }

  g_resource_system_force_sync = previous_force_sync;

  if (!loaded) {
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
  }

  return loaded;
}

vkr_internal void
vkr_resource_system_unload_sync_internal(const VkrResourceHandleInfo *info,
                                         String8 name) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(info != NULL, "Info is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  if (info->loader_id != VKR_INVALID_ID &&
      info->loader_id < vkr_resource_system->loader_count) {
    VkrResourceLoader *by_id = &vkr_resource_system->loaders[info->loader_id];
    if (by_id->unload && by_id->type == info->type) {
      by_id->unload(by_id, info, name);
      return;
    }
  }

  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == info->type && loader->unload) {
      loader->unload(loader, info, name);
      return;
    }
  }

  log_warn("Resource system: no unloader for type=%u name='%s'",
           (unsigned)info->type, string8_cstr(&name));
}

vkr_internal bool8_t vkr_resource_system_request_slot_ensure_capacity(
    VkrResourceSystem *system, uint32_t required_capacity) {
  assert_log(system != NULL, "System is NULL");

  if (required_capacity <= system->request_capacity) {
    return true_v;
  }

  uint32_t new_capacity = system->request_capacity > 0
                              ? system->request_capacity
                              : VKR_HASH_TABLE_INITIAL_CAPACITY;
  while (new_capacity < required_capacity) {
    new_capacity *= 2;
  }

  VkrResourceAsyncRequest *new_requests = vkr_allocator_alloc(
      system->allocator, sizeof(VkrResourceAsyncRequest) * new_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!new_requests) {
    return false_v;
  }

  MemZero(new_requests, sizeof(VkrResourceAsyncRequest) * new_capacity);
  if (system->requests && system->request_capacity > 0) {
    MemCopy(new_requests, system->requests,
            sizeof(VkrResourceAsyncRequest) * system->request_capacity);
    vkr_allocator_free(system->allocator, system->requests,
                       sizeof(VkrResourceAsyncRequest) *
                           system->request_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  system->requests = new_requests;
  system->request_capacity = new_capacity;
  return true_v;
}

vkr_internal int32_t
vkr_resource_system_request_find_free_slot_locked(VkrResourceSystem *system) {
  assert_log(system != NULL, "System is NULL");

  for (uint32_t i = 0; i < system->request_capacity; ++i) {
    if (!system->requests[i].in_use) {
      return (int32_t)i;
    }
  }
  return -1;
}

vkr_internal int32_t vkr_resource_system_request_find_by_id_locked(
    VkrResourceSystem *system, uint64_t request_id) {
  if (!system || request_id == 0 || !system->requests) {
    return -1;
  }
  for (uint32_t i = 0; i < system->request_capacity; ++i) {
    if (!system->requests[i].in_use) {
      continue;
    }
    if (system->requests[i].request_id == request_id) {
      return (int32_t)i;
    }
  }
  return -1;
}

vkr_internal bool8_t vkr_resource_system_allocate_string8_copy(
    VkrResourceSystem *system, String8 source, char **out_cstr,
    String8 *out_string8) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_cstr != NULL, "Out C string pointer is NULL");
  assert_log(out_string8 != NULL, "Out String8 pointer is NULL");

  *out_cstr = NULL;
  *out_string8 = (String8){0};

  if (!source.str || source.length == 0) {
    return false_v;
  }

  char *storage = vkr_allocator_alloc(system->allocator, source.length + 1,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!storage) {
    return false_v;
  }

  MemCopy(storage, source.str, (size_t)source.length);
  storage[source.length] = '\0';

  *out_cstr = storage;
  *out_string8 =
      string8_create_from_cstr((const uint8_t *)storage, source.length);
  return true_v;
}

vkr_internal char *
vkr_resource_system_make_request_key(VkrResourceSystem *system,
                                     VkrResourceType type, String8 path) {
  assert_log(system != NULL, "System is NULL");
  assert_log(path.str != NULL, "Path is NULL");

  uint64_t type_value = (uint64_t)type;
  uint64_t type_digits = 1;
  while (type_value >= 10) {
    type_value /= 10;
    type_digits++;
  }

  uint64_t required_size = type_digits + 1 + path.length + 1;
  char *key = vkr_allocator_alloc(system->allocator, required_size,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!key) {
    return NULL;
  }

  int32_t written = string_format(key, required_size, "%u|%.*s", (uint32_t)type,
                                  (int32_t)path.length, path.str);
  if (written <= 0 || ((uint64_t)written + 1) != required_size) {
    vkr_allocator_free(system->allocator, key, required_size,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return NULL;
  }

  return key;
}

vkr_internal void
vkr_resource_system_request_release_locked(VkrResourceSystem *system,
                                           int32_t request_index,
                                           uint32_t *out_async_loader_id,
                                           void **out_async_payload) {
  assert_log(system != NULL, "System is NULL");
  assert_log(request_index >= 0, "Request index is invalid");
  assert_log((uint32_t)request_index < system->request_capacity,
             "Request index is out of bounds");

  VkrResourceAsyncRequest *request = &system->requests[request_index];
  if (!request->in_use) {
    return;
  }

  if (out_async_loader_id) {
    *out_async_loader_id = VKR_INVALID_ID;
  }
  if (out_async_payload) {
    *out_async_payload = NULL;
  }

  if (request->key) {
    vkr_hash_table_remove_uint32_t(&system->request_by_key, request->key);
    uint64_t key_len = string_length(request->key);
    vkr_allocator_free(system->allocator, request->key, key_len + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (request->path.str) {
    vkr_allocator_free(system->allocator, request->path.str,
                       request->path.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (request->async_payload) {
    if (out_async_loader_id) {
      *out_async_loader_id = request->loader_id;
    }
    if (out_async_payload) {
      *out_async_payload = request->async_payload;
    }
    request->async_payload = NULL;
  }

  MemZero(request, sizeof(*request));
}

vkr_internal void vkr_resource_system_fill_info_from_request(
    const VkrResourceAsyncRequest *request, VkrResourceHandleInfo *out_info) {
  assert_log(request != NULL, "Request is NULL");
  assert_log(out_info != NULL, "Out info is NULL");

  vkr_resource_system_reset_handle_info(out_info);
  out_info->type = request->type;
  out_info->loader_id = request->loader_id;
  out_info->load_state = request->load_state;
  out_info->last_error = request->last_error;
  out_info->request_id = request->request_id;

  if (request->load_state == VKR_RESOURCE_LOAD_STATE_READY) {
    out_info->as = request->loaded_info.as;
    out_info->type = request->loaded_info.type;
    out_info->loader_id = request->loaded_info.loader_id;
  }
}

/**
 * @brief Try to enqueue the CPU prepare job for a pending async request.
 *
 * Must be called with `system->mutex` held. This helper is non-blocking and
 * leaves the request in `PENDING_CPU` when the job system is saturated.
 */
vkr_internal bool8_t vkr_resource_system_try_submit_cpu_job_locked(
    VkrResourceSystem *system, VkrResourceAsyncRequest *request) {
  assert_log(system != NULL, "System is NULL");
  assert_log(request != NULL, "Request is NULL");

  if (!system->job_system || !request->in_use || request->cancel_requested ||
      request->cpu_job_in_flight ||
      request->load_state != VKR_RESOURCE_LOAD_STATE_PENDING_CPU) {
    return false_v;
  }

  VkrResourceAsyncJobPayload payload = {
      .system = system,
      .request_id = request->request_id,
      .loader_id = request->loader_id,
      .type = request->type,
      .path = request->path,
  };

  Bitset8 type_mask = bitset8_create();
  bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);
  VkrJobDesc job_desc = {
      .priority = VKR_JOB_PRIORITY_NORMAL,
      .type_mask = type_mask,
      .run = vkr_resource_system_async_load_job_run,
      .on_success = NULL,
      .on_failure = NULL,
      .payload = &payload,
      .payload_size = sizeof(payload),
      .dependencies = NULL,
      .dependency_count = 0,
      .defer_enqueue = false_v,
  };

  VkrJobHandle job_handle = {0};
  if (!vkr_job_try_submit(system->job_system, &job_desc, &job_handle)) {
    return false_v;
  }

  request->cpu_job_in_flight = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_resource_system_completion_enqueue_locked(
    VkrResourceSystem *system, const VkrResourceAsyncCompletion *completion) {
  assert_log(system != NULL, "System is NULL");
  assert_log(completion != NULL, "Completion is NULL");

  if (!system->completions || system->completion_capacity == 0) {
    return false_v;
  }
  if (system->completion_count >= system->completion_capacity) {
    return false_v;
  }

  system->completions[system->completion_tail] = *completion;
  system->completion_tail =
      (system->completion_tail + 1) % system->completion_capacity;
  system->completion_count++;
  return true_v;
}

vkr_internal bool8_t vkr_resource_system_completion_dequeue_locked(
    VkrResourceSystem *system, VkrResourceAsyncCompletion *out_completion) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_completion != NULL, "Out completion is NULL");

  if (!system->completions || system->completion_capacity == 0 ||
      system->completion_count == 0) {
    return false_v;
  }

  *out_completion = system->completions[system->completion_head];
  vkr_resource_system_completion_reset(
      &system->completions[system->completion_head]);
  system->completion_head =
      (system->completion_head + 1) % system->completion_capacity;
  system->completion_count--;
  return true_v;
}

vkr_internal bool8_t vkr_resource_system_async_load_job_run(VkrJobContext *ctx,
                                                            void *payload) {
  assert_log(payload != NULL, "Payload is NULL");

  VkrResourceAsyncJobPayload *job = (VkrResourceAsyncJobPayload *)payload;
  VkrResourceSystem *system = job->system;
  if (!system || !job->path.str || job->path.length == 0) {
    return false_v;
  }

  VkrAllocator *temp_alloc =
      (ctx && ctx->allocator) ? ctx->allocator : system->allocator;

  VkrResourceLoader *loader = NULL;
  if (job->loader_id != VKR_INVALID_ID &&
      job->loader_id < system->loader_count) {
    loader = &system->loaders[job->loader_id];
  } else {
    loader = vkr_resource_system_find_loader_for_path(job->type, job->path);
  }

  VkrResourceAsyncCompletion completion = {0};
  completion.request_id = job->request_id;
  completion.loader_id = loader ? loader->id : VKR_INVALID_ID;
  completion.load_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;

  if (loader && loader->prepare_async && loader->finalize_async) {
    void *async_payload = NULL;
    VkrRendererError prepare_error = VKR_RENDERER_ERROR_NONE;
    if (loader->prepare_async(loader, job->path, temp_alloc, &async_payload,
                              &prepare_error) &&
        async_payload) {
      completion.has_async_payload = true_v;
      completion.async_payload = async_payload;
      completion.load_error = VKR_RENDERER_ERROR_NONE;
    } else {
      completion.has_async_payload = false_v;
      completion.async_payload = NULL;
      completion.load_error = prepare_error;
      if (async_payload && loader->release_async_payload) {
        loader->release_async_payload(loader, async_payload);
      }
    }
  } else {
    VkrResourceHandleInfo loaded_info = {0};
    VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
    bool8_t loaded = false_v;

    bool8_t previous_force_sync = g_resource_system_force_sync;
    g_resource_system_force_sync = true_v;
    if (loader && loader->load) {
      loaded = loader->load(loader, job->path, temp_alloc, &loaded_info,
                            &load_error);
      if (loaded) {
        loaded_info.loader_id = loader->id;
      }
    } else {
      loaded = vkr_resource_system_load_sync_internal(
          job->type, job->path, temp_alloc, &loaded_info, &load_error);
    }
    g_resource_system_force_sync = previous_force_sync;

    completion.loaded = loaded;
    completion.loaded_info = loaded_info;
    completion.load_error = loaded ? VKR_RENDERER_ERROR_NONE : load_error;
    if (loaded) {
      completion.loader_id = loaded_info.loader_id;
    }

    char *path_cstr = NULL;
    String8 path_copy = {0};
    if (loaded && !vkr_resource_system_allocate_string8_copy(
                      system, job->path, &path_cstr, &path_copy)) {
      vkr_resource_system_unload_sync_internal(&loaded_info, job->path);
      vkr_resource_system_reset_handle_info(&completion.loaded_info);
      completion.loaded = false_v;
      completion.load_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    } else if (loaded) {
      completion.path = path_copy;
    }
  }

  if (!vkr_mutex_lock(system->mutex)) {
    if (completion.loaded) {
      vkr_resource_system_unload_sync_internal(&completion.loaded_info,
                                               completion.path);
    }
    if (completion.has_async_payload) {
      vkr_resource_system_release_async_payload(completion.loader_id,
                                                completion.async_payload);
      completion.has_async_payload = false_v;
      completion.async_payload = NULL;
    }
    vkr_resource_system_completion_release_path(system, &completion);
    return false_v;
  }

  if (!vkr_resource_system_completion_enqueue_locked(system, &completion)) {
    int32_t request_index =
        vkr_resource_system_request_find_by_id_locked(system, job->request_id);
    if (request_index >= 0) {
      VkrResourceAsyncRequest *request = &system->requests[request_index];
      if (request->in_use) {
        request->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
        request->last_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    vkr_mutex_unlock(system->mutex);

    if (completion.loaded) {
      vkr_resource_system_unload_sync_internal(&completion.loaded_info,
                                               completion.path);
    }
    if (completion.has_async_payload) {
      vkr_resource_system_release_async_payload(completion.loader_id,
                                                completion.async_payload);
      completion.has_async_payload = false_v;
      completion.async_payload = NULL;
    }
    vkr_resource_system_completion_release_path(system, &completion);
    return false_v;
  }

  vkr_mutex_unlock(system->mutex);
  return true_v;
}

bool8_t vkr_resource_system_init(VkrAllocator *allocator,
                                 VkrRendererFrontendHandle renderer,
                                 VkrJobSystem *job_system) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");

  if (vkr_resource_system) {
    if (vkr_resource_system->renderer != renderer) {
      log_error(
          "Resource system already initialized with different parameters");
      return false_v;
    }

    log_debug("Resource system already initialized with same parameters");
    return true_v;
  }

  vkr_resource_system = vkr_allocator_alloc(
      allocator, sizeof(VkrResourceSystem), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!vkr_resource_system) {
    log_fatal("Failed to allocate resource system");
    return false_v;
  }

  MemZero(vkr_resource_system, sizeof(*vkr_resource_system));

  vkr_resource_system->renderer = renderer;
  vkr_resource_system->allocator = allocator;
  vkr_resource_system->job_system = job_system;
  vkr_resource_system->loader_capacity = 16;
  vkr_resource_system->request_capacity = 64;
  vkr_resource_system->completion_capacity =
      VKR_RESOURCE_COMPLETION_QUEUE_INITIAL_CAPACITY;
  vkr_resource_system->next_request_id = 1;

  vkr_resource_system->loaders = vkr_allocator_alloc(
      vkr_resource_system->allocator,
      sizeof(VkrResourceLoader) * vkr_resource_system->loader_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  assert_log(vkr_resource_system->loaders != NULL,
             "Failed to allocate loaders array");
  vkr_resource_system->loader_count = 0;

  for (uint32_t loader = 0; loader < vkr_resource_system->loader_capacity;
       loader++) {
    vkr_resource_system->loaders[loader].id = VKR_INVALID_ID;
  }

  if (!vkr_mutex_create(vkr_resource_system->allocator,
                        &vkr_resource_system->mutex)) {
    log_fatal("Failed to create resource system mutex");
    return false_v;
  }

  vkr_resource_system->request_by_key =
      vkr_hash_table_create_uint32_t(vkr_resource_system->allocator, 128);

  vkr_resource_system->requests = vkr_allocator_alloc(
      vkr_resource_system->allocator,
      sizeof(VkrResourceAsyncRequest) * vkr_resource_system->request_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!vkr_resource_system->requests) {
    log_fatal("Failed to allocate async request table");
    return false_v;
  }
  MemZero(vkr_resource_system->requests,
          sizeof(VkrResourceAsyncRequest) *
              vkr_resource_system->request_capacity);

  vkr_resource_system->completions =
      vkr_allocator_alloc(vkr_resource_system->allocator,
                          sizeof(VkrResourceAsyncCompletion) *
                              vkr_resource_system->completion_capacity,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!vkr_resource_system->completions) {
    log_fatal("Failed to allocate async completion queue");
    return false_v;
  }
  MemZero(vkr_resource_system->completions,
          sizeof(VkrResourceAsyncCompletion) *
              vkr_resource_system->completion_capacity);
  vkr_resource_system->completion_head = 0;
  vkr_resource_system->completion_tail = 0;
  vkr_resource_system->completion_count = 0;

  return true_v;
}

bool8_t vkr_resource_system_register_loader(void *resource_system,
                                            VkrResourceLoader loader) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(resource_system != NULL, "Loader resource system context is NULL");

  if (vkr_resource_system->loader_count >=
      vkr_resource_system->loader_capacity) {
    uint32_t new_capacity = vkr_resource_system->loader_capacity * 2;
    VkrResourceLoader *new_mem =
        vkr_allocator_alloc(vkr_resource_system->allocator,
                            sizeof(VkrResourceLoader) * new_capacity,
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    assert_log(new_mem != NULL, "Failed to grow loaders array");
    MemCopy(new_mem, vkr_resource_system->loaders,
            sizeof(VkrResourceLoader) * vkr_resource_system->loader_count);
    vkr_allocator_free(
        vkr_resource_system->allocator, vkr_resource_system->loaders,
        sizeof(VkrResourceLoader) * vkr_resource_system->loader_capacity,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    vkr_resource_system->loaders = new_mem;
    vkr_resource_system->loader_capacity = new_capacity;
  }

  uint32_t id = vkr_resource_system->loader_count++;
  vkr_resource_system->loaders[id] = loader;

  VkrResourceLoader *dst = &vkr_resource_system->loaders[id];
  dst->id = id;
  dst->resource_system = resource_system;
  dst->renderer = vkr_resource_system->renderer;

  return true_v;
}

bool8_t vkr_resource_system_load_sync(VkrResourceType type, String8 path,
                                      VkrAllocator *temp_alloc,
                                      VkrResourceHandleInfo *out_info,
                                      VkrRendererError *out_error) {
  return vkr_resource_system_load_sync_internal(type, path, temp_alloc,
                                                out_info, out_error);
}

bool8_t vkr_resource_system_load(VkrResourceType type, String8 path,
                                 VkrAllocator *temp_alloc,
                                 VkrResourceHandleInfo *out_info,
                                 VkrRendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  path = vkr_resource_system_normalize_path(type, path);

  vkr_resource_system_reset_handle_info(out_info);
  *out_error = VKR_RENDERER_ERROR_NONE;

  if (g_resource_system_force_sync ||
      !vkr_resource_system_is_async_default_type(type) ||
      !vkr_resource_system->job_system) {
    return vkr_resource_system_load_sync_internal(type, path, temp_alloc,
                                                  out_info, out_error);
  }

  VkrResourceLoader *selected_loader =
      vkr_resource_system_find_loader_for_path(type, path);
  if (!selected_loader) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    *out_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  /*
   * Use the request-table mutex to serialize key/path allocations. The backing
   * allocator is shared with worker threads and does not guarantee lock-free
   * thread safety.
   */
  char *request_key =
      vkr_resource_system_make_request_key(vkr_resource_system, type, path);
  if (!request_key) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  uint32_t *existing_index = vkr_hash_table_get_uint32_t(
      &vkr_resource_system->request_by_key, request_key);
  if (existing_index &&
      *existing_index < vkr_resource_system->request_capacity) {
    VkrResourceAsyncRequest *existing =
        &vkr_resource_system->requests[*existing_index];
    if (existing->in_use) {
      existing->ref_count++;
      if (existing->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU &&
          !existing->cpu_job_in_flight && !existing->cancel_requested) {
        (void)vkr_resource_system_try_submit_cpu_job_locked(vkr_resource_system,
                                                            existing);
      }
      vkr_resource_system_fill_info_from_request(existing, out_info);
      *out_error = existing->last_error;
      vkr_mutex_unlock(vkr_resource_system->mutex);

      uint64_t key_len = string_length(request_key);
      vkr_allocator_free(vkr_resource_system->allocator, request_key,
                         key_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);

      return existing->load_state != VKR_RESOURCE_LOAD_STATE_FAILED &&
             existing->load_state != VKR_RESOURCE_LOAD_STATE_CANCELED;
    }
  }

  int32_t request_index =
      vkr_resource_system_request_find_free_slot_locked(vkr_resource_system);
  if (request_index < 0) {
    if (!vkr_resource_system_request_slot_ensure_capacity(
            vkr_resource_system, vkr_resource_system->request_capacity + 1)) {
      vkr_mutex_unlock(vkr_resource_system->mutex);
      uint64_t key_len = string_length(request_key);
      vkr_allocator_free(vkr_resource_system->allocator, request_key,
                         key_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
      out_info->last_error = *out_error;
      return false_v;
    }
    request_index =
        vkr_resource_system_request_find_free_slot_locked(vkr_resource_system);
  }

  if (request_index < 0) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    uint64_t key_len = string_length(request_key);
    vkr_allocator_free(vkr_resource_system->allocator, request_key, key_len + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  char *path_cstr = NULL;
  String8 path_copy = {0};
  if (!vkr_resource_system_allocate_string8_copy(vkr_resource_system, path,
                                                 &path_cstr, &path_copy)) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    uint64_t key_len = string_length(request_key);
    vkr_allocator_free(vkr_resource_system->allocator, request_key, key_len + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  VkrResourceAsyncRequest *request =
      &vkr_resource_system->requests[request_index];
  MemZero(request, sizeof(*request));
  request->in_use = true_v;
  request->request_id = vkr_resource_system->next_request_id++;
  request->key = request_key;
  request->path = path_copy;
  request->type = type;
  request->loader_id = selected_loader->id;
  request->load_state = VKR_RESOURCE_LOAD_STATE_PENDING_CPU;
  request->last_error = VKR_RENDERER_ERROR_NONE;
  request->ref_count = 1;
  request->cancel_requested = false_v;
  request->cpu_job_in_flight = false_v;
  request->gpu_submit_serial = 0;
  request->async_payload = NULL;
  vkr_resource_system_reset_handle_info(&request->loaded_info);

  if (!vkr_hash_table_insert_uint32_t(&vkr_resource_system->request_by_key,
                                      request->key, (uint32_t)request_index)) {
    vkr_resource_system_request_release_locked(vkr_resource_system,
                                               request_index, NULL, NULL);
    vkr_mutex_unlock(vkr_resource_system->mutex);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
    return false_v;
  }

  (void)vkr_resource_system_try_submit_cpu_job_locked(vkr_resource_system,
                                                      request);

  vkr_resource_system_fill_info_from_request(request, out_info);
  *out_error = request->last_error;
  bool8_t success = request->load_state != VKR_RESOURCE_LOAD_STATE_FAILED &&
                    request->load_state != VKR_RESOURCE_LOAD_STATE_CANCELED;

  vkr_mutex_unlock(vkr_resource_system->mutex);
  return success;
}

bool8_t vkr_resource_system_load_custom(String8 custom_type, String8 path,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_info,
                                        VkrRendererError *out_error) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(custom_type.str != NULL, "Custom type is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_info != NULL, "Out info is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  vkr_resource_system_reset_handle_info(out_info);
  *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;

  bool8_t previous_force_sync = g_resource_system_force_sync;
  g_resource_system_force_sync = true_v;

  bool8_t loaded = false_v;
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (!loader->custom_type.str) {
      continue;
    }
    if (!string8_equalsi(&loader->custom_type, &custom_type)) {
      continue;
    }
    if (loader->can_load && !loader->can_load(loader, path)) {
      continue;
    }

    VkrResourceHandleInfo loaded_info = {0};
    VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
    if (loader->load &&
        loader->load(loader, path, temp_alloc, &loaded_info, &load_error)) {
      loaded_info.loader_id = loader->id;
      loaded_info.load_state = VKR_RESOURCE_LOAD_STATE_READY;
      loaded_info.last_error = VKR_RENDERER_ERROR_NONE;
      loaded_info.request_id = 0;
      *out_info = loaded_info;
      *out_error = VKR_RENDERER_ERROR_NONE;
      loaded = true_v;
      break;
    }

    *out_error = load_error;
  }

  g_resource_system_force_sync = previous_force_sync;

  if (!loaded) {
    out_info->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
    out_info->last_error = *out_error;
  }

  return loaded;
}

void vkr_resource_system_unload(const VkrResourceHandleInfo *info,
                                String8 name) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(info != NULL, "Info is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  name = vkr_resource_system_normalize_path(info->type, name);

  if (info->request_id == 0) {
    vkr_resource_system_unload_sync_internal(info, name);
    return;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    return;
  }

  int32_t request_index = vkr_resource_system_request_find_by_id_locked(
      vkr_resource_system, info->request_id);
  if (request_index < 0) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    return;
  }

  VkrResourceAsyncRequest *request =
      &vkr_resource_system->requests[request_index];
  if (!request->in_use) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    return;
  }

  if (request->ref_count > 0) {
    request->ref_count--;
  }

  if (request->ref_count > 0) {
    vkr_mutex_unlock(vkr_resource_system->mutex);
    return;
  }

  VkrResourceHandleInfo ready_info = {0};
  bool8_t unload_ready_resource = false_v;
  String8 unload_name = name;
  bool8_t unload_name_owned = false_v;
  uint32_t release_async_loader_id = VKR_INVALID_ID;
  void *release_async_payload = NULL;

  if (info->type != VKR_RESOURCE_TYPE_UNKNOWN && info->type != request->type) {
    log_warn("Resource unload type mismatch for request %llu: info type=%u, "
             "tracked type=%u. Using tracked unload metadata.",
             (unsigned long long)request->request_id, (uint32_t)info->type,
             (uint32_t)request->type);
  }

  switch (request->load_state) {
  case VKR_RESOURCE_LOAD_STATE_PENDING_CPU:
  case VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES:
  case VKR_RESOURCE_LOAD_STATE_PENDING_GPU:
    request->cancel_requested = true_v;
    request->load_state = VKR_RESOURCE_LOAD_STATE_CANCELED;
    vkr_mutex_unlock(vkr_resource_system->mutex);
    return;
  case VKR_RESOURCE_LOAD_STATE_READY:
    ready_info = request->loaded_info;
    unload_ready_resource = true_v;

    /*
     * Use the tracked canonical request path for unload to keep key matching
     * stable even when callers pass aliases or stale names.
     */
    if (request->path.str && request->path.length > 0) {
      uint8_t *path_copy = vkr_allocator_alloc(vkr_resource_system->allocator,
                                               request->path.length + 1,
                                               VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (path_copy) {
        MemCopy(path_copy, request->path.str, request->path.length);
        path_copy[request->path.length] = '\0';
        unload_name =
            (String8){.str = path_copy, .length = request->path.length};
        unload_name_owned = true_v;
      }
    }

    vkr_resource_system_request_release_locked(
        vkr_resource_system, request_index, &release_async_loader_id,
        &release_async_payload);
    break;
  case VKR_RESOURCE_LOAD_STATE_FAILED:
  case VKR_RESOURCE_LOAD_STATE_CANCELED:
  case VKR_RESOURCE_LOAD_STATE_INVALID:
  default:
    if (request->cpu_job_in_flight) {
      /*
       * Keep canceled/failed requests alive until the worker posts completion.
       * Releasing here would free request-owned path/key storage that job
       * payloads still reference.
       */
      request->cancel_requested = true_v;
      vkr_mutex_unlock(vkr_resource_system->mutex);
      return;
    }
    vkr_resource_system_request_release_locked(
        vkr_resource_system, request_index, &release_async_loader_id,
        &release_async_payload);
    break;
  }

  vkr_mutex_unlock(vkr_resource_system->mutex);

  if (release_async_payload) {
    vkr_resource_system_release_async_payload(release_async_loader_id,
                                              release_async_payload);
  }

  if (unload_ready_resource) {
    vkr_resource_system_unload_sync_internal(&ready_info, unload_name);
  }

  if (unload_name_owned && unload_name.str) {
    vkr_allocator_free(vkr_resource_system->allocator, unload_name.str,
                       unload_name.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
}

uint32_t vkr_resource_system_get_loader_id(VkrResourceType type, String8 name) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == type) {
      return loader->id;
    }
  }

  return VKR_INVALID_ID;
}

VkrJobSystem *vkr_resource_system_get_job_system(void) {
  if (!vkr_resource_system) {
    return NULL;
  }
  return vkr_resource_system->job_system;
}

uint32_t vkr_resource_system_load_batch_sync(VkrResourceType type,
                                             const String8 *paths,
                                             uint32_t count,
                                             VkrAllocator *temp_alloc,
                                             VkrResourceHandleInfo *out_handles,
                                             VkrRendererError *out_errors) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    vkr_resource_system_reset_handle_info(&out_handles[i]);
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  VkrResourceLoader *batch_loader = NULL;
  for (uint32_t loader_idx = 0; loader_idx < vkr_resource_system->loader_count;
       loader_idx++) {
    VkrResourceLoader *loader = &vkr_resource_system->loaders[loader_idx];
    if (loader->type == type && loader->batch_load) {
      batch_loader = loader;
      break;
    }
  }

  if (batch_loader) {
    uint32_t loaded = batch_loader->batch_load(
        batch_loader, paths, count, temp_alloc, out_handles, out_errors);
    for (uint32_t i = 0; i < count; i++) {
      if (out_handles[i].type != VKR_RESOURCE_TYPE_UNKNOWN) {
        out_handles[i].loader_id = batch_loader->id;
        out_handles[i].load_state = VKR_RESOURCE_LOAD_STATE_READY;
        out_handles[i].last_error = VKR_RENDERER_ERROR_NONE;
      } else {
        out_handles[i].load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
        out_handles[i].last_error = out_errors[i];
      }
      out_handles[i].request_id = 0;
    }
    return loaded;
  }

  uint32_t loaded_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!paths[i].str || paths[i].length == 0) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      out_handles[i].load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
      out_handles[i].last_error = out_errors[i];
      continue;
    }

    if (vkr_resource_system_load_sync_internal(
            type, paths[i], temp_alloc, &out_handles[i], &out_errors[i])) {
      loaded_count++;
    }
  }

  return loaded_count;
}

uint32_t vkr_resource_system_load_batch(VkrResourceType type,
                                        const String8 *paths, uint32_t count,
                                        VkrAllocator *temp_alloc,
                                        VkrResourceHandleInfo *out_handles,
                                        VkrRendererError *out_errors) {
  assert_log(vkr_resource_system != NULL, "Resource system is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  if (g_resource_system_force_sync ||
      !vkr_resource_system_is_async_default_type(type) ||
      !vkr_resource_system->job_system) {
    return vkr_resource_system_load_batch_sync(type, paths, count, temp_alloc,
                                               out_handles, out_errors);
  }

  uint32_t accepted_count = 0;
  for (uint32_t i = 0; i < count; ++i) {
    vkr_resource_system_reset_handle_info(&out_handles[i]);
    out_errors[i] = VKR_RENDERER_ERROR_NONE;

    if (!paths[i].str || paths[i].length == 0) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      out_handles[i].load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
      out_handles[i].last_error = out_errors[i];
      continue;
    }

    if (vkr_resource_system_load(type, paths[i], temp_alloc, &out_handles[i],
                                 &out_errors[i])) {
      accepted_count++;
    }
  }

  return accepted_count;
}

VkrResourceLoadState
vkr_resource_system_get_state(const VkrResourceHandleInfo *info,
                              VkrRendererError *out_error) {
  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }
  if (!info) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return VKR_RESOURCE_LOAD_STATE_INVALID;
  }

  if (info->request_id == 0 || !vkr_resource_system) {
    if (out_error) {
      *out_error = info->last_error;
    }
    return info->load_state;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
    return VKR_RESOURCE_LOAD_STATE_INVALID;
  }

  VkrResourceLoadState state = VKR_RESOURCE_LOAD_STATE_INVALID;
  VkrRendererError error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;

  int32_t request_index = vkr_resource_system_request_find_by_id_locked(
      vkr_resource_system, info->request_id);
  if (request_index >= 0) {
    VkrResourceAsyncRequest *request =
        &vkr_resource_system->requests[request_index];
    state = request->load_state;
    error = request->last_error;
  }

  vkr_mutex_unlock(vkr_resource_system->mutex);

  if (out_error) {
    *out_error = error;
  }

  return state;
}

bool8_t vkr_resource_system_is_ready(const VkrResourceHandleInfo *info) {
  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  return vkr_resource_system_get_state(info, &err) ==
         VKR_RESOURCE_LOAD_STATE_READY;
}

bool8_t
vkr_resource_system_try_get_resolved(const VkrResourceHandleInfo *tracked_info,
                                     VkrResourceHandleInfo *out_info) {
  if (!tracked_info || !out_info) {
    return false_v;
  }

  vkr_resource_system_reset_handle_info(out_info);

  if (tracked_info->request_id == 0 || !vkr_resource_system) {
    if (tracked_info->load_state == VKR_RESOURCE_LOAD_STATE_READY) {
      *out_info = *tracked_info;
      return true_v;
    }
    return false_v;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    return false_v;
  }

  bool8_t resolved = false_v;
  int32_t request_index = vkr_resource_system_request_find_by_id_locked(
      vkr_resource_system, tracked_info->request_id);
  if (request_index >= 0) {
    VkrResourceAsyncRequest *request =
        &vkr_resource_system->requests[request_index];
    if (request->in_use &&
        request->load_state == VKR_RESOURCE_LOAD_STATE_READY) {
      vkr_resource_system_fill_info_from_request(request, out_info);
      resolved = true_v;
    }
  }

  vkr_mutex_unlock(vkr_resource_system->mutex);
  return resolved;
}

vkr_internal bool8_t vkr_resource_system_gpu_cost_fits_budget(
    const VkrResourceAsyncFinalizeCost *cost, uint32_t used_gpu_ops,
    uint64_t used_gpu_bytes, const VkrResourceAsyncBudget *budget) {
  assert_log(cost != NULL, "Cost is NULL");
  assert_log(budget != NULL, "Budget is NULL");

  if (cost->gpu_upload_ops > 0) {
    if (budget->max_gpu_upload_ops == 0) {
      return false_v;
    }
    bool8_t fits_ops =
        used_gpu_ops <= budget->max_gpu_upload_ops &&
        cost->gpu_upload_ops <= (budget->max_gpu_upload_ops - used_gpu_ops);
    bool8_t allow_oversized_first_op =
        used_gpu_ops == 0 && cost->gpu_upload_ops > budget->max_gpu_upload_ops;
    if (!fits_ops && !allow_oversized_first_op) {
      return false_v;
    }
  }

  if (cost->gpu_upload_bytes > 0) {
    if (budget->max_gpu_upload_bytes == 0) {
      return false_v;
    }
    bool8_t fits_bytes = used_gpu_bytes <= budget->max_gpu_upload_bytes &&
                         cost->gpu_upload_bytes <=
                             (budget->max_gpu_upload_bytes - used_gpu_bytes);
    bool8_t allow_oversized_first_upload =
        used_gpu_bytes == 0 &&
        cost->gpu_upload_bytes > budget->max_gpu_upload_bytes;
    if (!fits_bytes && !allow_oversized_first_upload) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal void vkr_resource_system_gpu_cost_consume(
    const VkrResourceAsyncFinalizeCost *cost, uint32_t *used_gpu_ops,
    uint64_t *used_gpu_bytes, const VkrResourceAsyncBudget *budget) {
  assert_log(cost != NULL, "Cost is NULL");
  assert_log(used_gpu_ops != NULL, "Used GPU ops pointer is NULL");
  assert_log(used_gpu_bytes != NULL, "Used GPU bytes pointer is NULL");
  assert_log(budget != NULL, "Budget is NULL");

  if (cost->gpu_upload_ops > 0 && budget->max_gpu_upload_ops > 0) {
    if (cost->gpu_upload_ops >= budget->max_gpu_upload_ops - *used_gpu_ops) {
      *used_gpu_ops = budget->max_gpu_upload_ops;
    } else {
      *used_gpu_ops += cost->gpu_upload_ops;
    }
  }

  if (cost->gpu_upload_bytes > 0 && budget->max_gpu_upload_bytes > 0) {
    if (cost->gpu_upload_bytes >=
        budget->max_gpu_upload_bytes - *used_gpu_bytes) {
      *used_gpu_bytes = budget->max_gpu_upload_bytes;
    } else {
      *used_gpu_bytes += cost->gpu_upload_bytes;
    }
  }
}

vkr_internal void vkr_resource_system_estimate_finalize_cost(
    VkrResourceLoader *loader, String8 path, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  assert_log(out_cost != NULL, "Out cost is NULL");

  // A single finalize callback may mutate renderer state and record GPU work.
  // Use one op as a safe default when a loader does not provide an estimate.
  out_cost->gpu_upload_ops = 1;
  out_cost->gpu_upload_bytes = 0;

  if (!loader || !payload) {
    return;
  }

  if (!loader->estimate_async_finalize_cost) {
    return;
  }

  VkrResourceAsyncFinalizeCost estimated = {0};
  if (loader->estimate_async_finalize_cost(loader, path, payload, &estimated)) {
    *out_cost = estimated;
  }
}

void vkr_resource_system_pump(const VkrResourceAsyncBudget *budget) {
  if (!vkr_resource_system) {
    return;
  }

  const VkrResourceAsyncBudget *effective_budget =
      budget ? budget : &vkr_resource_async_budget_default;

  uint64_t completed_submit_serial =
      vkr_renderer_get_completed_submit_serial(vkr_resource_system->renderer);
  bool8_t frame_active =
      vkr_renderer_is_frame_active(vkr_resource_system->renderer);
  uint64_t submit_serial =
      vkr_renderer_get_submit_serial(vkr_resource_system->renderer);
  if (frame_active && submit_serial < UINT64_MAX) {
    submit_serial += 1u;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    return;
  }

  uint32_t finalize_budget = effective_budget->max_finalize_requests;
  uint32_t used_gpu_upload_ops = 0;
  uint64_t used_gpu_upload_bytes = 0;
  while (finalize_budget > 0) {
    VkrResourceAsyncCompletion completion = {0};
    if (!vkr_resource_system_completion_dequeue_locked(vkr_resource_system,
                                                       &completion)) {
      break;
    }

    bool8_t unload_loaded_resource = false_v;
    VkrResourceHandleInfo unload_info = {0};
    bool8_t release_async_payload = false_v;
    uint32_t release_async_loader_id = VKR_INVALID_ID;
    void *release_async_ptr = NULL;

    int32_t request_index = vkr_resource_system_request_find_by_id_locked(
        vkr_resource_system, completion.request_id);
    if (request_index < 0) {
      unload_loaded_resource = completion.loaded;
      unload_info = completion.loaded_info;
      if (completion.has_async_payload) {
        release_async_payload = true_v;
        release_async_loader_id = completion.loader_id;
        release_async_ptr = completion.async_payload;
      }
    } else {
      VkrResourceAsyncRequest *request =
          &vkr_resource_system->requests[request_index];
      request->cpu_job_in_flight = false_v;
      if (!request->in_use) {
        unload_loaded_resource = completion.loaded;
        unload_info = completion.loaded_info;
        if (completion.has_async_payload) {
          release_async_payload = true_v;
          release_async_loader_id = completion.loader_id;
          release_async_ptr = completion.async_payload;
        }
      } else if (request->cancel_requested && request->ref_count == 0) {
        unload_loaded_resource = completion.loaded;
        unload_info = completion.loaded_info;
        if (completion.has_async_payload) {
          release_async_payload = true_v;
          release_async_loader_id = completion.loader_id;
          release_async_ptr = completion.async_payload;
        }
        request->load_state = VKR_RESOURCE_LOAD_STATE_CANCELED;
        request->last_error = VKR_RENDERER_ERROR_NONE;
        uint32_t detached_loader_id = VKR_INVALID_ID;
        void *detached_payload = NULL;
        vkr_resource_system_request_release_locked(vkr_resource_system,
                                                   request_index,
                                                   &detached_loader_id,
                                                   &detached_payload);
        if (detached_payload) {
          release_async_payload = true_v;
          release_async_loader_id = detached_loader_id;
          release_async_ptr = detached_payload;
        }
      } else if (completion.has_async_payload) {
        request->loader_id = completion.loader_id;
        request->async_payload = completion.async_payload;
        request->gpu_submit_serial = 0;
        request->last_error = VKR_RENDERER_ERROR_NONE;
        request->load_state = VKR_RESOURCE_LOAD_STATE_PENDING_GPU;
      } else if (completion.loaded) {
        request->loaded_info = completion.loaded_info;
        request->loader_id = completion.loaded_info.loader_id;
        request->last_error = VKR_RENDERER_ERROR_NONE;
        // Submit serial assignment and READY transition are render-thread
        // responsibilities.
        request->gpu_submit_serial = 0;
        request->load_state = VKR_RESOURCE_LOAD_STATE_PENDING_GPU;
      } else {
        request->last_error = completion.load_error;
        request->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
      }
    }

    if (unload_loaded_resource) {
      vkr_mutex_unlock(vkr_resource_system->mutex);
      vkr_resource_system_unload_sync_internal(&unload_info, completion.path);
      if (release_async_payload) {
        vkr_resource_system_release_async_payload(release_async_loader_id,
                                                  release_async_ptr);
      }
      vkr_resource_system_completion_release_path(vkr_resource_system,
                                                  &completion);
      if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
        return;
      }
    } else {
      if (release_async_payload) {
        vkr_mutex_unlock(vkr_resource_system->mutex);
        vkr_resource_system_release_async_payload(release_async_loader_id,
                                                  release_async_ptr);
        if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
          return;
        }
      }
      vkr_resource_system_completion_release_path(vkr_resource_system,
                                                  &completion);
    }

    finalize_budget--;
  }

  for (uint32_t i = 0;
       i < vkr_resource_system->request_capacity && finalize_budget > 0; ++i) {
    VkrResourceAsyncRequest *request = &vkr_resource_system->requests[i];
    if (!request->in_use) {
      continue;
    }

    if (request->load_state == VKR_RESOURCE_LOAD_STATE_CANCELED &&
        request->ref_count == 0 && !request->cpu_job_in_flight) {
      uint32_t detached_loader_id = VKR_INVALID_ID;
      void *detached_payload = NULL;
      vkr_resource_system_request_release_locked(vkr_resource_system, (int32_t)i,
                                                 &detached_loader_id,
                                                 &detached_payload);
      if (detached_payload) {
        vkr_mutex_unlock(vkr_resource_system->mutex);
        vkr_resource_system_release_async_payload(detached_loader_id,
                                                  detached_payload);
        if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
          return;
        }
      }
      finalize_budget--;
      continue;
    }

    if (request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU) {
      if (request->cancel_requested && request->ref_count == 0 &&
          !request->cpu_job_in_flight) {
        uint32_t detached_loader_id = VKR_INVALID_ID;
        void *detached_payload = NULL;
        vkr_resource_system_request_release_locked(vkr_resource_system,
                                                   (int32_t)i,
                                                   &detached_loader_id,
                                                   &detached_payload);
        if (detached_payload) {
          vkr_mutex_unlock(vkr_resource_system->mutex);
          vkr_resource_system_release_async_payload(detached_loader_id,
                                                    detached_payload);
          if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
            return;
          }
        }
        finalize_budget--;
        continue;
      }

      if (!request->cpu_job_in_flight && !request->cancel_requested) {
        (void)vkr_resource_system_try_submit_cpu_job_locked(vkr_resource_system,
                                                            request);
      }
      continue;
    }

    if (request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU ||
        request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES) {
      if (request->cancel_requested && request->ref_count == 0) {
        request->load_state = VKR_RESOURCE_LOAD_STATE_CANCELED;
        request->last_error = VKR_RENDERER_ERROR_NONE;
        if (!request->cpu_job_in_flight) {
          uint32_t detached_loader_id = VKR_INVALID_ID;
          void *detached_payload = NULL;
          vkr_resource_system_request_release_locked(vkr_resource_system,
                                                     (int32_t)i,
                                                     &detached_loader_id,
                                                     &detached_payload);
          if (detached_payload) {
            vkr_mutex_unlock(vkr_resource_system->mutex);
            vkr_resource_system_release_async_payload(detached_loader_id,
                                                      detached_payload);
            if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
              return;
            }
          }
          finalize_budget--;
        }
        continue;
      }

      if (request->async_payload && request->gpu_submit_serial == 0) {
        // Async finalize can issue Vulkan mutations/uploads and must run only
        // while recording an active frame command buffer on the render thread.
        if (!frame_active) {
          continue;
        }

        VkrResourceLoader *loader = NULL;
        if (request->loader_id != VKR_INVALID_ID &&
            request->loader_id < vkr_resource_system->loader_count) {
          loader = &vkr_resource_system->loaders[request->loader_id];
        }

        if (!loader || !loader->finalize_async) {
          void *payload_ptr = request->async_payload;
          uint32_t payload_loader_id = request->loader_id;
          request->async_payload = NULL;
          request->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
          request->last_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
          vkr_mutex_unlock(vkr_resource_system->mutex);
          vkr_resource_system_release_async_payload(payload_loader_id,
                                                    payload_ptr);
          if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
            return;
          }
          finalize_budget--;
          continue;
        }

        void *payload_ptr = request->async_payload;
        VkrResourceAsyncFinalizeCost finalize_cost = {0};
        vkr_resource_system_estimate_finalize_cost(loader, request->path,
                                                   payload_ptr, &finalize_cost);
        if (!vkr_resource_system_gpu_cost_fits_budget(
                &finalize_cost, used_gpu_upload_ops, used_gpu_upload_bytes,
                effective_budget)) {
          continue;
        }

        VkrResourceHandleInfo finalized_info = {0};
        VkrRendererError finalize_error = VKR_RENDERER_ERROR_NONE;
        uint64_t finalize_request_id = request->request_id;
        String8 finalize_path = request->path;

        /*
         * Finalize callbacks may submit nested async dependencies through the
         * resource system. They must run without holding the request-table
         * mutex to avoid re-entrant mutex deadlocks on the render thread.
         */
        vkr_mutex_unlock(vkr_resource_system->mutex);
        bool8_t finalized =
            loader->finalize_async(loader, finalize_path, payload_ptr,
                                   &finalized_info, &finalize_error);
        if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
          return;
        }

        int32_t refreshed_index = vkr_resource_system_request_find_by_id_locked(
            vkr_resource_system, finalize_request_id);
        if (refreshed_index < 0) {
          /*
           * Request vanished while finalize ran. Release the payload to avoid a
           * CPU-side leak; any finalized GPU handle is orphaned and should not
           * happen because pending requests are not released until terminal.
           */
          vkr_mutex_unlock(vkr_resource_system->mutex);
          vkr_resource_system_release_async_payload(loader->id, payload_ptr);
          if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
            return;
          }
          finalize_budget--;
          continue;
        }

        request = &vkr_resource_system->requests[refreshed_index];
        if (!request->in_use) {
          vkr_mutex_unlock(vkr_resource_system->mutex);
          vkr_resource_system_release_async_payload(loader->id, payload_ptr);
          if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
            return;
          }
          finalize_budget--;
          continue;
        }

        if (!finalized) {
          if (finalize_error == VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED) {
            request->load_state = VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES;
            request->last_error = VKR_RENDERER_ERROR_NONE;
            continue;
          }
          uint32_t payload_loader_id = request->loader_id;
          request->async_payload = NULL;
          request->load_state = VKR_RESOURCE_LOAD_STATE_FAILED;
          request->last_error = finalize_error;
          vkr_mutex_unlock(vkr_resource_system->mutex);
          vkr_resource_system_release_async_payload(payload_loader_id,
                                                    payload_ptr);
          if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
            return;
          }
          finalize_budget--;
          continue;
        }

        uint32_t payload_loader_id = request->loader_id;
        request->async_payload = NULL;

        finalized_info.loader_id = loader->id;
        finalized_info.load_state = VKR_RESOURCE_LOAD_STATE_READY;
        finalized_info.last_error = VKR_RENDERER_ERROR_NONE;
        finalized_info.request_id = request->request_id;
        request->loaded_info = finalized_info;
        vkr_resource_system_gpu_cost_consume(
            &finalize_cost, &used_gpu_upload_ops, &used_gpu_upload_bytes,
            effective_budget);

        vkr_mutex_unlock(vkr_resource_system->mutex);
        vkr_resource_system_release_async_payload(payload_loader_id,
                                                  payload_ptr);
        if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
          return;
        }
      }

      if (request->gpu_submit_serial == 0) {
        request->gpu_submit_serial = submit_serial;
      }

      if (request->gpu_submit_serial == 0 ||
          completed_submit_serial >= request->gpu_submit_serial) {
        request->load_state = VKR_RESOURCE_LOAD_STATE_READY;
        request->last_error = VKR_RENDERER_ERROR_NONE;
        finalize_budget--;
      }
    }
  }

  vkr_mutex_unlock(vkr_resource_system->mutex);
}

void vkr_resource_system_cancel(const VkrResourceHandleInfo *info) {
  if (!vkr_resource_system || !info || info->request_id == 0) {
    return;
  }

  if (!vkr_mutex_lock(vkr_resource_system->mutex)) {
    return;
  }

  int32_t request_index = vkr_resource_system_request_find_by_id_locked(
      vkr_resource_system, info->request_id);
  if (request_index >= 0) {
    VkrResourceAsyncRequest *request =
        &vkr_resource_system->requests[request_index];
    if (request->in_use) {
      request->cancel_requested = true_v;
      if (request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
          request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
          request->load_state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
        request->load_state = VKR_RESOURCE_LOAD_STATE_CANCELED;
      }
    }
  }

  vkr_mutex_unlock(vkr_resource_system->mutex);
}

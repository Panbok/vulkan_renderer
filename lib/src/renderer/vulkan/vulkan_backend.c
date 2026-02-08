#include "vulkan_backend.h"
#include "containers/str.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_pool_allocator.h"
#include "vulkan_buffer.h"
#include "vulkan_command.h"
#include "vulkan_device.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_image.h"
#include "vulkan_instance.h"
#include "vulkan_pipeline.h"
#include "vulkan_renderpass.h"
#include "vulkan_shaders.h"
#include "vulkan_swapchain.h"

#ifndef NDEBUG
#include "vulkan_debug.h"
#endif

// todo: make these configurable
#define VKR_MAX_TEXTURE_HANDLES 4096
#define VKR_MAX_BUFFER_HANDLES 8192
#define VKR_MAX_RENDER_TARGET_HANDLES 256

// Assign texture generation for descriptor invalidation and debug liveness
#ifndef NDEBUG
#define ASSIGN_TEXTURE_GENERATION(state, texture)                              \
  do {                                                                         \
    uint32_t g = ++(state)->texture_generation_counter;                        \
    (texture)->generation = g;                                                 \
    (texture)->description.generation = g;                                     \
  } while (0)
#else
#define ASSIGN_TEXTURE_GENERATION(state, texture)                              \
  ((texture)->description.generation = ++(state)->texture_generation_counter)
#endif

// Forward declarations for interface functions defined at end of file
VkrAllocator *renderer_vulkan_get_allocator(void *backend_state);
void renderer_vulkan_set_default_2d_texture(void *backend_state,
                                            VkrTextureOpaqueHandle texture);
void renderer_vulkan_set_depth_bias(void *backend_state,
                                    float32_t constant_factor, float32_t clamp,
                                    float32_t slope_factor);
bool8_t renderer_vulkan_domain_renderpass_set(void *backend_state,
                                               VkrPipelineDomain domain,
                                               VkrRenderPassHandle pass_handle,
                                               VkrDomainOverridePolicy policy,
                                               VkrRendererError *out_error);
VkrRenderPassHandle renderer_vulkan_renderpass_create_desc(
    void *backend_state, const VkrRenderPassDesc *desc,
    VkrRendererError *out_error);
VkrRenderTargetHandle renderer_vulkan_render_target_create(
    void *backend_state, const VkrRenderTargetDesc *desc,
    VkrRenderPassHandle pass_handle, VkrRendererError *out_error);
VkrBackendResourceHandle renderer_vulkan_create_render_target_texture_msaa(
    void *backend_state, uint32_t width, uint32_t height, VkrTextureFormat format,
    VkrSampleCount samples);
VkrRendererError renderer_vulkan_buffer_barrier(
    void *backend_state, VkrBackendResourceHandle handle,
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access);
VkrTextureFormat renderer_vulkan_swapchain_format_get(void *backend_state);
VkrTextureFormat renderer_vulkan_shadow_depth_format_get(void *backend_state);
bool8_t renderer_vulkan_pipeline_get_shader_runtime_layout(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrShaderRuntimeLayout *out_layout);

// Local helpers defined later in this file.
vkr_internal VkrTextureFormat vulkan_vk_format_to_vkr(VkFormat format);
vkr_internal VkrSampleCount
vulkan_vk_samples_to_vkr(VkSampleCountFlagBits samples);
vkr_internal VkImageAspectFlags
vulkan_aspect_flags_from_texture_format(VkrTextureFormat format);

static void vulkan_rg_timing_fetch_results(VulkanBackendState *state);

/**
 * @brief Resolve the depth format used by sampled shadow resources.
 *
 * Some devices cannot expose a dedicated sampled shadow format. In that case
 * this falls back to the primary depth format so shadow pipelines, passes, and
 * runtime shadow images remain format-compatible.
 */
vkr_internal VkFormat
vulkan_shadow_depth_vk_format_get(const VulkanBackendState *state) {
  if (!state) {
    return VK_FORMAT_UNDEFINED;
  }
  if (state->device.shadow_depth_format != VK_FORMAT_UNDEFINED) {
    return state->device.shadow_depth_format;
  }
  return state->device.depth_format;
}

vkr_internal VkrTextureFormat
vulkan_shadow_depth_vkr_format_get(const VulkanBackendState *state) {
  VkFormat shadow_format = vulkan_shadow_depth_vk_format_get(state);
  if (shadow_format == VK_FORMAT_UNDEFINED) {
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  }
  return vulkan_vk_format_to_vkr(shadow_format);
}

// todo: we are having issues with image ghosting when camera moves
// too fast, need to figure out why (clues VSync/present mode issues)

vkr_internal uint32_t vulkan_calculate_mip_levels(uint32_t width,
                                                  uint32_t height) {
  uint32_t mip_levels = 1;
  uint32_t max_dim = Max(width, height);
  while (max_dim > 1) {
    max_dim >>= 1;
    mip_levels++;
  }
  return mip_levels;
}

/**
 * @brief Classifies a raw filesystem path string as absolute/relative.
 *
 * Pipeline cache paths may come from environment overrides and must preserve
 * caller intent. This helper avoids `file_path_create()` because that helper
 * rewrites relative paths through `PROJECT_SOURCE_DIR`, which is not desired
 * for explicit cache location overrides.
 */
vkr_internal FilePathType
vulkan_path_type_from_string8(const String8 *path) {
  if (!path || !path->str || path->length == 0) {
    return FILE_PATH_TYPE_RELATIVE;
  }

#if defined(PLATFORM_WINDOWS)
  if (path->str[0] == '/' || path->str[0] == '\\') {
    return FILE_PATH_TYPE_ABSOLUTE;
  }
  if (path->length >= 2 && path->str[1] == ':') {
    return FILE_PATH_TYPE_ABSOLUTE;
  }
  return FILE_PATH_TYPE_RELATIVE;
#else
  return (path->str[0] == '/') ? FILE_PATH_TYPE_ABSOLUTE
                               : FILE_PATH_TYPE_RELATIVE;
#endif
}

vkr_internal INLINE FilePath
vulkan_file_path_from_string8(String8 path) {
  return (FilePath){
      .path = path,
      .type = vulkan_path_type_from_string8(&path),
  };
}

vkr_internal void vulkan_pipeline_cache_log_file_error(
    const char *operation, const String8 *path, FileError error) {
  String8 err = file_get_error_string(error);
  log_warn("Failed to %s pipeline cache '%s': %s", operation,
           path ? string8_cstr(path) : "", (const char *)err.str);
}

vkr_internal bool8_t vulkan_pipeline_cache_try_load_initial_data(
    VulkanBackendState *state, VkPipelineCacheCreateInfo *io_create_info) {
  if (!state || !io_create_info || !state->pipeline_cache_path.str ||
      state->pipeline_cache_path.length == 0) {
    return false_v;
  }

  const FilePath cache_file =
      vulkan_file_path_from_string8(state->pipeline_cache_path);
  if (!file_exists(&cache_file)) {
    return false_v;
  }

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle handle = {0};
  const FileError open_error = file_open(&cache_file, mode, &handle);
  if (open_error != FILE_ERROR_NONE) {
    vulkan_pipeline_cache_log_file_error(
        "open", &state->pipeline_cache_path, open_error);
    return false_v;
  }

  uint8_t *cache_data = NULL;
  uint64_t cache_size = 0;
  const FileError read_error =
      file_read_all(&handle, &state->temp_scope, &cache_data, &cache_size);
  file_close(&handle);
  if (read_error != FILE_ERROR_NONE) {
    vulkan_pipeline_cache_log_file_error(
        "read", &state->pipeline_cache_path, read_error);
    return false_v;
  }

  if (!cache_data || cache_size == 0) {
    return false_v;
  }

  io_create_info->initialDataSize = (size_t)cache_size;
  io_create_info->pInitialData = cache_data;
  log_info("Loaded pipeline cache data: %llu bytes",
           (unsigned long long)cache_size);
  return true_v;
}

vkr_internal VkResult vulkan_pipeline_cache_create_with_fallback(
    VulkanBackendState *state, VkPipelineCacheCreateInfo *create_info) {
  VkResult cache_result =
      vkCreatePipelineCache(state->device.logical_device, create_info,
                            state->allocator, &state->pipeline_cache);
  if (cache_result == VK_SUCCESS || create_info->initialDataSize == 0) {
    return cache_result;
  }

  log_warn("Pipeline cache '%s' is incompatible/corrupt (VkResult=%d); "
           "recreating empty cache",
           string8_cstr(&state->pipeline_cache_path), cache_result);
  create_info->initialDataSize = 0;
  create_info->pInitialData = NULL;
  return vkCreatePipelineCache(state->device.logical_device, create_info,
                               state->allocator, &state->pipeline_cache);
}

vkr_internal bool8_t vulkan_file_promote_replace(const char *temp_path,
                                                 const char *final_path) {
  if (!temp_path || !final_path) {
    return false_v;
  }
  if (rename(temp_path, final_path) == 0) {
    return true_v;
  }
  remove(final_path);
  return rename(temp_path, final_path) == 0;
}

vkr_internal String8
vulkan_pipeline_cache_resolve_path(VulkanBackendState *state) {
  assert_log(state != NULL, "state is NULL");

  const char *override_path = getenv("VKR_PIPELINE_CACHE_PATH");
  if (override_path && override_path[0] != '\0') {
    return vkr_string8_duplicate_cstr(&state->alloc, override_path);
  }

#if defined(PLATFORM_APPLE)
  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    return string8_create_formatted(
        &state->alloc,
        "%s/Library/Caches/VulkanRenderer/pipeline_cache_v1.bin", home);
  }
#elif defined(PLATFORM_WINDOWS)
  const char *local_app_data = getenv("LOCALAPPDATA");
  if (local_app_data && local_app_data[0] != '\0') {
    return string8_create_formatted(
        &state->alloc, "%s\\VulkanRenderer\\pipeline_cache_v1.bin",
        local_app_data);
  }
#else
  const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
  if (xdg_cache_home && xdg_cache_home[0] != '\0') {
    return string8_create_formatted(
        &state->alloc, "%s/vulkan_renderer/pipeline_cache_v1.bin",
        xdg_cache_home);
  }

  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    return string8_create_formatted(
        &state->alloc, "%s/.cache/vulkan_renderer/pipeline_cache_v1.bin",
        home);
  }
#endif

  // Last-resort fallback keeps cache enabled when platform env vars are absent.
  return vkr_string8_duplicate_cstr(&state->alloc, "pipeline_cache_v1.bin");
}

vkr_internal bool8_t
vulkan_pipeline_cache_initialize(VulkanBackendState *state) {
  assert_log(state != NULL, "state is NULL");

  state->pipeline_cache = VK_NULL_HANDLE;
  state->pipeline_cache_path = vulkan_pipeline_cache_resolve_path(state);
  log_info("Pipeline cache path: %s", string8_cstr(&state->pipeline_cache_path));

  VkPipelineCacheCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = 0,
      .pInitialData = NULL,
  };

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  bool8_t scope_valid = vkr_allocator_scope_is_valid(&scope);
  if (scope_valid) {
    vulkan_pipeline_cache_try_load_initial_data(state, &create_info);
  }

  const bool8_t used_persisted_data = create_info.initialDataSize > 0;
  VkResult cache_result =
      vulkan_pipeline_cache_create_with_fallback(state, &create_info);

  if (scope_valid) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  }

  if (cache_result != VK_SUCCESS) {
    log_warn("Failed to create Vulkan pipeline cache (VkResult=%d); continuing "
             "without persistent cache",
             cache_result);
    state->pipeline_cache = VK_NULL_HANDLE;
    return false_v;
  }

  if (used_persisted_data) {
    log_info("Initialized Vulkan pipeline cache with persisted data");
  } else {
    log_info("Initialized Vulkan pipeline cache with empty data");
  }

  return true_v;
}

vkr_internal bool8_t vulkan_pipeline_cache_save(VulkanBackendState *state) {
  assert_log(state != NULL, "state is NULL");

  if (state->pipeline_cache == VK_NULL_HANDLE) {
    return false_v;
  }
  if (!state->pipeline_cache_path.str || state->pipeline_cache_path.length == 0) {
    log_warn("Skipping pipeline cache save: cache path is empty");
    return false_v;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_warn("Skipping pipeline cache save: failed to create temp scope");
    return false_v;
  }

  size_t cache_size = 0;
  VkResult query_result = vkGetPipelineCacheData(state->device.logical_device,
                                                 state->pipeline_cache,
                                                 &cache_size, NULL);
  if (query_result != VK_SUCCESS || cache_size == 0) {
    if (query_result != VK_SUCCESS) {
      log_warn("Failed to query pipeline cache data size (VkResult=%d)",
               query_result);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  uint8_t *cache_data = vkr_allocator_alloc(&state->temp_scope, cache_size,
                                            VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!cache_data) {
    log_warn("Skipping pipeline cache save: failed to allocate %zu bytes",
             cache_size);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  VkResult read_result = vkGetPipelineCacheData(state->device.logical_device,
                                                state->pipeline_cache,
                                                &cache_size, cache_data);
  if (read_result != VK_SUCCESS || cache_size == 0) {
    log_warn("Failed to read pipeline cache data (VkResult=%d)", read_result);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  String8 cache_directory =
      file_path_get_directory(&state->temp_scope, state->pipeline_cache_path);
  if (cache_directory.length > 0 &&
      !file_ensure_directory(&state->temp_scope, &cache_directory)) {
    log_warn("Failed to create pipeline cache directory '%.*s'",
             (int32_t)cache_directory.length, cache_directory.str);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  String8 temp_path = string8_create_formatted(
      &state->temp_scope, "%s.tmp", string8_cstr(&state->pipeline_cache_path));
  const FilePath temp_file = vulkan_file_path_from_string8(temp_path);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle handle = {0};
  FileError open_error = file_open(&temp_file, mode, &handle);
  if (open_error != FILE_ERROR_NONE) {
    String8 err = file_get_error_string(open_error);
    log_warn("Failed to open pipeline cache temp file '%s': %s",
             string8_cstr(&temp_path), (const char *)err.str);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  uint64_t bytes_written = 0;
  FileError write_error =
      file_write(&handle, (uint64_t)cache_size, cache_data, &bytes_written);
  file_close(&handle);

  if (write_error != FILE_ERROR_NONE || bytes_written != (uint64_t)cache_size) {
    String8 err = file_get_error_string(write_error);
    log_warn("Failed to write pipeline cache temp file '%s': %s",
             string8_cstr(&temp_path), (const char *)err.str);
    remove(string8_cstr(&temp_path));
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  const char *temp_cstr = string8_cstr(&temp_path);
  const char *final_cstr = string8_cstr(&state->pipeline_cache_path);
  if (!vulkan_file_promote_replace(temp_cstr, final_cstr)) {
    int32_t rename_error = errno;
    log_warn("Failed to promote pipeline cache temp file '%s' -> '%s': %s",
             temp_cstr, final_cstr, strerror(rename_error));
    remove(temp_cstr);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    return false_v;
  }

  log_info("Saved pipeline cache data: %zu bytes -> %s", cache_size, final_cstr);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return true_v;
}

vkr_internal void vulkan_pipeline_cache_shutdown(VulkanBackendState *state) {
  assert_log(state != NULL, "state is NULL");

  if (state->pipeline_cache != VK_NULL_HANDLE) {
    vulkan_pipeline_cache_save(state);
    vkDestroyPipelineCache(state->device.logical_device, state->pipeline_cache,
                           state->allocator);
    state->pipeline_cache = VK_NULL_HANDLE;
  }
}

vkr_internal void framebuffer_cache_invalidate(VulkanBackendState *state) {
  VkrFramebufferCache *cache = &state->framebuffer_cache;
  for (uint32_t i = 0; i < cache->entry_count; ++i) {
    if (cache->entries[i].in_use &&
        cache->entries[i].framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(state->device.logical_device,
                           cache->entries[i].framebuffer, state->allocator);
      cache->entries[i].framebuffer = VK_NULL_HANDLE;
      cache->entries[i].in_use = false_v;
    }
  }
  cache->entry_count = 0;
}

// ============================================================================
// Deferred Destruction Queue
// ============================================================================

/**
 * @brief Enqueue a resource for deferred destruction.
 *
 * Resources are not destroyed immediately but queued for destruction once
 * the GPU is guaranteed to have finished using them (after BUFFERING_FRAMES frames).
 *
 * @param state Vulkan backend state
 * @param kind Type of resource to destroy
 * @param payload Union containing the resource handle
 * @param memory Optional device memory to free (VK_NULL_HANDLE if none)
 * @param pool_alloc Optional allocator for wrapper pool return
 * @param wrapper_size Size of wrapper struct if pool_alloc is set
 * @return true if enqueued successfully, false if queue is full (immediate destroy needed)
 */
vkr_internal bool8_t
vulkan_deferred_destroy_enqueue(VulkanBackendState *state,
                                VkrDeferredDestroyKind kind, void *handle,
                                VkDeviceMemory memory, VkrAllocator *pool_alloc,
                                uint64_t wrapper_size) {
  VkrDeferredDestroyQueue *queue = &state->deferred_destroy_queue;

  if (queue->count >= VKR_DEFERRED_DESTROY_QUEUE_SIZE) {
    log_warn("Deferred destroy queue full, immediate destruction required");
    return false_v;
  }

  VkrDeferredDestroyEntry *entry = &queue->entries[queue->tail];
  entry->kind = kind;
  entry->submit_serial = state->submit_serial;
  entry->payload.wrapper = handle; // All handles are pointer-sized
  entry->memory = memory;
  entry->pool_alloc = pool_alloc;
  entry->wrapper_size = wrapper_size;

  queue->tail = (queue->tail + 1) % VKR_DEFERRED_DESTROY_QUEUE_SIZE;
  queue->count++;

  return true_v;
}

/**
 * @brief Process the deferred destruction queue, destroying retired resources.
 *
 * Called at the start of each frame after fence wait. Destroys all resources
 * whose submit_serial is old enough that the GPU is guaranteed to be done with them.
 *
 * @param state Vulkan backend state
 */
vkr_internal void vulkan_deferred_destroy_process(VulkanBackendState *state) {
  VkrDeferredDestroyQueue *queue = &state->deferred_destroy_queue;

  // Resources are safe to destroy when submit_serial <= current - BUFFERING_FRAMES
  uint64_t safe_serial =
      state->submit_serial >= BUFFERING_FRAMES
          ? state->submit_serial - BUFFERING_FRAMES
          : 0;

  while (queue->count > 0) {
    VkrDeferredDestroyEntry *entry = &queue->entries[queue->head];

    // Stop if we reach an entry that's not safe to destroy yet
    if (entry->submit_serial > safe_serial) {
      break;
    }

    // Destroy the resource based on its kind
    switch (entry->kind) {
    case VKR_DEFERRED_DESTROY_FRAMEBUFFER:
      if (entry->payload.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(state->device.logical_device,
                             entry->payload.framebuffer, state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_RENDERPASS:
      if (entry->payload.renderpass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(state->device.logical_device,
                            entry->payload.renderpass, state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_IMAGE:
      if (entry->payload.image != VK_NULL_HANDLE) {
        vkDestroyImage(state->device.logical_device, entry->payload.image,
                       state->allocator);
      }
      if (entry->memory != VK_NULL_HANDLE) {
        vkFreeMemory(state->device.logical_device, entry->memory,
                     state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_IMAGE_VIEW:
      if (entry->payload.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(state->device.logical_device,
                           entry->payload.image_view, state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_SAMPLER:
      if (entry->payload.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(state->device.logical_device, entry->payload.sampler,
                         state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_BUFFER:
      if (entry->payload.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(state->device.logical_device, entry->payload.buffer,
                        state->allocator);
      }
      if (entry->memory != VK_NULL_HANDLE) {
        vkFreeMemory(state->device.logical_device, entry->memory,
                     state->allocator);
      }
      break;
    case VKR_DEFERRED_DESTROY_TEXTURE_WRAPPER:
    case VKR_DEFERRED_DESTROY_BUFFER_WRAPPER:
    case VKR_DEFERRED_DESTROY_RENDER_TARGET_WRAPPER:
      // Free wrapper back to pool if allocator provided
      if (entry->pool_alloc && entry->payload.wrapper) {
        vkr_allocator_free(entry->pool_alloc, entry->payload.wrapper,
                           entry->wrapper_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      }
      break;
    }

    // Advance head and decrement count
    queue->head = (queue->head + 1) % VKR_DEFERRED_DESTROY_QUEUE_SIZE;
    queue->count--;
  }
}

/**
 * @brief Flush the entire deferred destruction queue, destroying all entries.
 *
 * Called during shutdown to ensure all resources are destroyed.
 *
 * @param state Vulkan backend state
 */
vkr_internal void vulkan_deferred_destroy_flush(VulkanBackendState *state) {
  // Process all entries regardless of serial by setting safe_serial high
  state->submit_serial = UINT64_MAX;
  vulkan_deferred_destroy_process(state);
  state->submit_serial = 0;

  // Reset queue state
  state->deferred_destroy_queue.head = 0;
  state->deferred_destroy_queue.tail = 0;
  state->deferred_destroy_queue.count = 0;
}

vkr_internal void vulkan_select_filter_modes(
    const VkrTextureDescription *desc, bool32_t anisotropy_supported,
    uint32_t mip_levels, VkFilter *out_min_filter, VkFilter *out_mag_filter,
    VkSamplerMipmapMode *out_mipmap_mode, VkBool32 *out_anisotropy_enable,
    float32_t *out_max_lod) {
  VkFilter min_filter = (desc->min_filter == VKR_FILTER_LINEAR)
                            ? VK_FILTER_LINEAR
                            : VK_FILTER_NEAREST;
  VkFilter mag_filter = (desc->mag_filter == VKR_FILTER_LINEAR)
                            ? VK_FILTER_LINEAR
                            : VK_FILTER_NEAREST;

  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  float32_t max_lod = (mip_levels > 0) ? (float32_t)(mip_levels - 1) : 0.0f;
  switch (desc->mip_filter) {
  case VKR_MIP_FILTER_NONE:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    max_lod = 0.0f;
    break;
  case VKR_MIP_FILTER_NEAREST:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    break;
  case VKR_MIP_FILTER_LINEAR:
  default:
    mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    break;
  }

  VkBool32 anisotropy_enable =
      (desc->anisotropy_enable && anisotropy_supported) ? VK_TRUE : VK_FALSE;

  if (out_min_filter)
    *out_min_filter = min_filter;
  if (out_mag_filter)
    *out_mag_filter = mag_filter;
  if (out_mipmap_mode)
    *out_mipmap_mode = mipmap_mode;
  if (out_anisotropy_enable)
    *out_anisotropy_enable = anisotropy_enable;
  if (out_max_lod)
    *out_max_lod = max_lod;
}

/**
 * @brief Select sampler filtering for sampled shadow depth images.
 *
 * Shadow depth attachments are created with optimal tiling, so we query only
 * `optimalTilingFeatures` and enable linear filtering when supported.
 */
vkr_internal void vulkan_select_shadow_sampler_filter_modes(
    const VulkanBackendState *state, VkFormat depth_format, VkFilter *out_filter,
    VkSamplerMipmapMode *out_mipmap_mode) {
  VkFilter filter = VK_FILTER_NEAREST;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

  if (state && depth_format != VK_FORMAT_UNDEFINED) {
    VkFormatProperties props = {0};
    vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                        depth_format, &props);
    if (props.optimalTilingFeatures &
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) {
      filter = VK_FILTER_LINEAR;
      mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
  }

  if (out_filter) {
    *out_filter = filter;
  }
  if (out_mipmap_mode) {
    *out_mipmap_mode = mipmap_mode;
  }
}

vkr_internal bool8_t vulkan_texture_format_is_depth(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_D16_UNORM:
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vulkan_texture_format_is_integer(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R32_UINT:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t
vulkan_texture_format_is_compressed(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return true_v;
  default:
    return false_v;
  }
}

/**
 * @brief Rejects runtime mutation APIs for compressed textures in rollout 1.
 *
 * Compressed uploads currently require full mip/layer payload creation, so
 * write/resize entrypoints are intentionally blocked to prevent partial updates.
 */
vkr_internal VkrRendererError vulkan_texture_reject_compressed_mutation(
    VkrTextureFormat format, const char *operation_name) {
  if (!vulkan_texture_format_is_compressed(format)) {
    return VKR_RENDERER_ERROR_NONE;
  }

  log_error("Texture operation '%s' is unsupported for compressed formats in "
            "this rollout",
            operation_name ? operation_name : "unknown");
  return VKR_RENDERER_ERROR_INVALID_PARAMETER;
}

vkr_internal uint32_t
vulkan_texture_format_channel_count(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return 4;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return 0;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return 2;
  case VKR_TEXTURE_FORMAT_R8_UNORM:
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
  case VKR_TEXTURE_FORMAT_R32_UINT:
  case VKR_TEXTURE_FORMAT_D16_UNORM:
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return 1;
  default:
    return 1;
  }
}

vkr_internal uint32_t
vulkan_texture_format_block_width(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return 4;
  default:
    return 1;
  }
}

vkr_internal uint32_t
vulkan_texture_format_block_height(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return 4;
  default:
    return 1;
  }
}

vkr_internal uint32_t
vulkan_texture_format_block_size_bytes(VkrTextureFormat format,
                                       uint32_t channels) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return 16;
  default:
    return channels;
  }
}

vkr_internal uint32_t vulkan_texture_mip_extent(uint32_t base,
                                                uint32_t mip_level) {
  return Max(1u, base >> mip_level);
}

vkr_internal uint64_t
vulkan_texture_expected_region_size_bytes(VkrTextureFormat format,
                                          uint32_t channels, uint32_t width,
                                          uint32_t height) {
  const uint32_t block_width = vulkan_texture_format_block_width(format);
  const uint32_t block_height = vulkan_texture_format_block_height(format);
  const uint32_t block_size =
      vulkan_texture_format_block_size_bytes(format, channels);
  if (block_size == 0) {
    return 0;
  }

  const uint64_t blocks_x = (width + (uint64_t)block_width - 1) / block_width;
  const uint64_t blocks_y =
      (height + (uint64_t)block_height - 1) / block_height;
  return blocks_x * blocks_y * (uint64_t)block_size;
}

vkr_internal VkImageLayout
vulkan_texture_layout_to_vk(VkrTextureLayout layout) {
  switch (layout) {
  case VKR_TEXTURE_LAYOUT_UNDEFINED:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  case VKR_TEXTURE_LAYOUT_GENERAL:
    return VK_IMAGE_LAYOUT_GENERAL;
  case VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR:
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  default:
    log_error("Unsupported texture layout: %d", layout);
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

vkr_internal bool32_t create_command_buffers(VulkanBackendState *state) {
  state->graphics_command_buffers = array_create_VulkanCommandBuffer(
      &state->alloc, state->swapchain.images.length);
  for (uint32_t i = 0; i < state->swapchain.images.length; i++) {
    VulkanCommandBuffer *command_buffer =
        array_get_VulkanCommandBuffer(&state->graphics_command_buffers, i);
    if (!vulkan_command_buffer_allocate(state, command_buffer)) {
      array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);
      log_fatal("Failed to create Vulkan command buffer");
      return false;
    }
  }
  return true;
}

vkr_internal bool32_t create_domain_render_passes(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");

  VkrTextureFormat swapchain_format =
      vulkan_vk_format_to_vkr(state->swapchain.format);
  VkrTextureFormat depth_format =
      vulkan_vk_format_to_vkr(state->device.depth_format);
  VkrTextureFormat shadow_depth_format =
      vulkan_shadow_depth_vkr_format_get(state);
  VkrClearValue clear_world = {.color_f32 = {0.1f, 0.1f, 0.2f, 1.0f}};
  VkrClearValue clear_black = {.color_f32 = {0.0f, 0.0f, 0.0f, 1.0f}};
  VkrClearValue clear_transparent = {.color_f32 = {0.0f, 0.0f, 0.0f, 0.0f}};
  VkrClearValue clear_depth = {.depth_stencil = {1.0f, 0}};
  VkrClearValue clear_picking = {.color_u32 = {0u, 0u, 0u, 0u}};

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (state->domain_initialized[domain]) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT ||
        domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_COMPUTE) {
      continue;
    }

    state->domain_render_passes[domain] =
        vkr_allocator_alloc(&state->alloc, sizeof(VulkanRenderPass),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!state->domain_render_passes[domain]) {
      log_fatal("Failed to allocate domain render pass for domain %u", domain);
      return false;
    }

    MemZero(state->domain_render_passes[domain], sizeof(VulkanRenderPass));

    VkrRenderPassAttachmentDesc color_attachment = {0};
    VkrRenderPassAttachmentDesc depth_attachment = {0};
    VkrRenderPassDesc desc = {0};

    switch (domain) {
    case VKR_PIPELINE_DOMAIN_WORLD:
    case VKR_PIPELINE_DOMAIN_SKYBOX:
    case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
    case VKR_PIPELINE_DOMAIN_WORLD_OVERLAY:
    {
      VkrClearValue color_clear =
          (domain == VKR_PIPELINE_DOMAIN_SKYBOX) ? clear_black : clear_world;
      color_attachment = (VkrRenderPassAttachmentDesc){
          .format = swapchain_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .clear_value = color_clear,
      };
      depth_attachment = (VkrRenderPassAttachmentDesc){
          .format = depth_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          .clear_value = clear_depth,
      };
      desc = (VkrRenderPassDesc){
          .name = (String8){0},
          .domain = (VkrPipelineDomain)domain,
          .color_attachment_count = 1,
          .color_attachments = &color_attachment,
          .depth_stencil_attachment = &depth_attachment,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      break;
    }
    case VKR_PIPELINE_DOMAIN_UI:
      color_attachment = (VkrRenderPassAttachmentDesc){
          .format = swapchain_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .final_layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
          .clear_value = clear_transparent,
      };
      desc = (VkrRenderPassDesc){
          .name = (String8){0},
          .domain = VKR_PIPELINE_DOMAIN_UI,
          .color_attachment_count = 1,
          .color_attachments = &color_attachment,
          .depth_stencil_attachment = NULL,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      break;
    case VKR_PIPELINE_DOMAIN_SHADOW:
      depth_attachment = (VkrRenderPassAttachmentDesc){
          .format = shadow_depth_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
          .clear_value = clear_depth,
      };
      desc = (VkrRenderPassDesc){
          .name = (String8){0},
          .domain = VKR_PIPELINE_DOMAIN_SHADOW,
          .color_attachment_count = 0,
          .color_attachments = NULL,
          .depth_stencil_attachment = &depth_attachment,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      break;
    case VKR_PIPELINE_DOMAIN_POST:
      color_attachment = (VkrRenderPassAttachmentDesc){
          .format = swapchain_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
          .clear_value = clear_black,
      };
      desc = (VkrRenderPassDesc){
          .name = (String8){0},
          .domain = VKR_PIPELINE_DOMAIN_POST,
          .color_attachment_count = 1,
          .color_attachments = &color_attachment,
          .depth_stencil_attachment = NULL,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      break;
    case VKR_PIPELINE_DOMAIN_PICKING:
    case VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT:
    case VKR_PIPELINE_DOMAIN_PICKING_OVERLAY:
      color_attachment = (VkrRenderPassAttachmentDesc){
          .format = VKR_TEXTURE_FORMAT_R32_UINT,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .clear_value = clear_picking,
      };
      depth_attachment = (VkrRenderPassAttachmentDesc){
          .format = depth_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          .clear_value = clear_depth,
      };
      desc = (VkrRenderPassDesc){
          .name = (String8){0},
          .domain = (VkrPipelineDomain)domain,
          .color_attachment_count = 1,
          .color_attachments = &color_attachment,
          .depth_stencil_attachment = &depth_attachment,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      break;
    default:
      log_fatal("Unknown pipeline domain: %u", domain);
      return false;
    }

    if (!vulkan_renderpass_create_from_desc(
            state, &desc, state->domain_render_passes[domain])) {
      log_fatal("Failed to create domain render pass for domain %u", domain);
      return false;
    }

    state->domain_initialized[domain] = true;
    // log_debug("Created domain render pass for domain %u", domain);
  }

  if (state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD]) {
    if (!state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT]) {
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] =
          state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD];
      state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] = true;
      // log_debug("Linked WORLD_TRANSPARENT to WORLD render pass");
    }
    if (!state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY]) {
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] =
          state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD];
      state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] = true;
      // log_debug("Linked WORLD_OVERLAY to WORLD render pass");
    }
  }

  return true;
}

vkr_internal VkrTextureFormat vulkan_vk_format_to_vkr(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
    return VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB;
  case VK_FORMAT_B8G8R8A8_UNORM:
    return VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM;
  case VK_FORMAT_R8G8B8A8_SRGB:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  case VK_FORMAT_R8G8B8A8_UNORM:
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  case VK_FORMAT_BC7_UNORM_BLOCK:
    return VKR_TEXTURE_FORMAT_BC7_UNORM;
  case VK_FORMAT_BC7_SRGB_BLOCK:
    return VKR_TEXTURE_FORMAT_BC7_SRGB;
  case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    return VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM;
  case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
    return VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB;
  case VK_FORMAT_R32_UINT:
    return VKR_TEXTURE_FORMAT_R32_UINT;
  case VK_FORMAT_D16_UNORM:
    return VKR_TEXTURE_FORMAT_D16_UNORM;
  case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
    return VKR_TEXTURE_FORMAT_D32_SFLOAT;
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT;
  default:
    log_warn("Unmapped VkFormat %d, defaulting to R8G8B8A8_UNORM", format);
    return VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
  }
}

vkr_internal VkrSampleCount
vulkan_vk_samples_to_vkr(VkSampleCountFlagBits samples) {
  switch (samples) {
  case VK_SAMPLE_COUNT_1_BIT:
    return VKR_SAMPLE_COUNT_1;
  case VK_SAMPLE_COUNT_2_BIT:
    return VKR_SAMPLE_COUNT_2;
  case VK_SAMPLE_COUNT_4_BIT:
    return VKR_SAMPLE_COUNT_4;
  case VK_SAMPLE_COUNT_8_BIT:
    return VKR_SAMPLE_COUNT_8;
  case VK_SAMPLE_COUNT_16_BIT:
    return VKR_SAMPLE_COUNT_16;
  case VK_SAMPLE_COUNT_32_BIT:
    return VKR_SAMPLE_COUNT_32;
  case VK_SAMPLE_COUNT_64_BIT:
    return VKR_SAMPLE_COUNT_64;
  default:
    return VKR_SAMPLE_COUNT_1;
  }
}

vkr_internal VkImageAspectFlags
vulkan_aspect_flags_from_texture_format(VkrTextureFormat format) {
  VkFormat vk_format = vulkan_image_format_from_texture_format(format);
  switch (vk_format) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_D32_SFLOAT:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

vkr_internal void
vulkan_backend_destroy_attachment_wrappers(VulkanBackendState *state,
                                           uint32_t image_count) {
  if (!state) {
    return;
  }

  if (image_count == 0) {
    image_count = state->swapchain.image_count;
  }

  if (state->swapchain_image_textures) {
    for (uint32_t i = 0; i < image_count; ++i) {
      struct s_TextureHandle *wrapper = state->swapchain_image_textures[i];
      if (wrapper) {
        vkr_allocator_free(&state->swapchain_alloc, wrapper,
                           sizeof(struct s_TextureHandle),
                           VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
      }
    }

    vkr_allocator_free(&state->swapchain_alloc, state->swapchain_image_textures,
                       sizeof(struct s_TextureHandle *) * image_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    state->swapchain_image_textures = NULL;
  }

  if (state->depth_texture) {
    vkr_allocator_free(&state->swapchain_alloc, state->depth_texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    state->depth_texture = NULL;
  }
}

vkr_internal bool32_t
vulkan_backend_create_attachment_wrappers(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.image_count > 0, "Swapchain image count is 0");

  uint32_t image_count = state->swapchain.image_count;

  state->swapchain_image_textures = vkr_allocator_alloc(
      &state->swapchain_alloc, sizeof(struct s_TextureHandle *) * image_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!state->swapchain_image_textures) {
    log_fatal("Failed to allocate swapchain image texture wrappers");
    return false;
  }

  for (uint32_t i = 0; i < image_count; ++i) {
    struct s_TextureHandle *wrapper = vkr_allocator_alloc(
        &state->swapchain_alloc, sizeof(struct s_TextureHandle),
        VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    if (!wrapper) {
      log_fatal("Failed to allocate swapchain image wrapper");
      return false;
    }
    MemZero(wrapper, sizeof(struct s_TextureHandle));

    wrapper->texture.image.handle =
        *array_get_VkImage(&state->swapchain.images, i);
    wrapper->texture.image.view =
        *array_get_VkImageView(&state->swapchain.image_views, i);
    wrapper->texture.image.width = state->swapchain.extent.width;
    wrapper->texture.image.height = state->swapchain.extent.height;
    wrapper->texture.image.mip_levels = 1;
    wrapper->texture.image.array_layers = 1;
    wrapper->texture.image.samples = VK_SAMPLE_COUNT_1_BIT;
    wrapper->texture.sampler = VK_NULL_HANDLE;

    wrapper->description.width = state->swapchain.extent.width;
    wrapper->description.height = state->swapchain.extent.height;
    wrapper->description.channels = 4;
    wrapper->description.format =
        vulkan_vk_format_to_vkr(state->swapchain.format);
    wrapper->description.sample_count = VKR_SAMPLE_COUNT_1;

    state->swapchain_image_textures[i] = wrapper;
  }

  struct s_TextureHandle *depth_wrapper = vkr_allocator_alloc(
      &state->swapchain_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!depth_wrapper) {
    log_fatal("Failed to allocate depth attachment wrapper");
    return false;
  }
  MemZero(depth_wrapper, sizeof(struct s_TextureHandle));
  depth_wrapper->texture.image = state->swapchain.depth_attachment;
  depth_wrapper->texture.image.samples = VK_SAMPLE_COUNT_1_BIT;
  depth_wrapper->texture.sampler = VK_NULL_HANDLE;
  depth_wrapper->description.width = state->swapchain.extent.width;
  depth_wrapper->description.height = state->swapchain.extent.height;
  depth_wrapper->description.channels = 1;
  depth_wrapper->description.format =
      vulkan_vk_format_to_vkr(state->device.depth_format);
  depth_wrapper->description.sample_count = VKR_SAMPLE_COUNT_1;

  state->depth_texture = depth_wrapper;

  return true;
}

vkr_internal struct s_RenderPass *
vulkan_backend_renderpass_lookup(VulkanBackendState *state, String8 name) {
  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (!entry->pass || !entry->pass->vk ||
        entry->pass->vk->handle == VK_NULL_HANDLE) {
      continue;
    }
    if (entry->name.length == 0 || entry->name.str == NULL) {
      continue;
    }
    if (string8_equalsi(&entry->name, &name)) {
      return entry->pass;
    }
  }
  return NULL;
}

vkr_internal bool32_t vulkan_backend_renderpass_register(
    VulkanBackendState *state, struct s_RenderPass *pass) {
  assert_log(state != NULL, "State not initialized");
  assert_log(pass != NULL, "Pass is NULL");

  if (array_is_null_VkrRenderPassEntry(&state->render_pass_registry)) {
    state->render_pass_registry =
        array_create_VkrRenderPassEntry(&state->alloc, 4);
    state->render_pass_count = 0;
  }

  uint32_t slot = state->render_pass_count;
  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (!entry->pass || !entry->pass->vk ||
        entry->pass->vk->handle == VK_NULL_HANDLE) {
      slot = i;
      break;
    }
  }

  if (slot >= state->render_pass_registry.length) {
    uint64_t old_length = state->render_pass_registry.length;
    uint64_t min_length = (uint64_t)slot + 1;
    uint64_t new_length = Max(old_length * 2, min_length);
    Array_VkrRenderPassEntry new_registry =
        array_create_VkrRenderPassEntry(&state->alloc, new_length);
    MemZero(new_registry.data,
            sizeof(VkrRenderPassEntry) * (uint64_t)new_registry.length);
    for (uint64_t i = 0; i < old_length; ++i) {
      new_registry.data[i] = state->render_pass_registry.data[i];
    }
    array_destroy_VkrRenderPassEntry(&state->render_pass_registry);
    state->render_pass_registry = new_registry;
  }

  VkrRenderPassEntry entry = {.name = pass->name, .pass = pass};
  array_set_VkrRenderPassEntry(&state->render_pass_registry, slot, entry);
  if (slot == state->render_pass_count) {
    state->render_pass_count++;
  }
  return true;
}

/**
 * @brief Internal helper to create a render pass from VkrRenderPassDesc.
 */
vkr_internal struct s_RenderPass *
vulkan_backend_renderpass_create_from_desc_internal(VulkanBackendState *state,
                                                    const VkrRenderPassDesc *desc) {
  assert_log(state != NULL, "State not initialized");
  assert_log(desc != NULL, "Render pass descriptor is NULL");

  if (desc->color_attachment_count > 0 && !desc->color_attachments) {
    log_error("Render pass descriptor missing color attachments");
    return NULL;
  }
  if (desc->resolve_attachment_count > 0 && !desc->resolve_attachments) {
    log_error("Render pass descriptor missing resolve attachments");
    return NULL;
  }
  if (desc->color_attachment_count > VKR_MAX_COLOR_ATTACHMENTS) {
    log_error("Render pass color attachment count %u exceeds max %u",
              desc->color_attachment_count, VKR_MAX_COLOR_ATTACHMENTS);
    return NULL;
  }
  if (desc->resolve_attachment_count > VKR_MAX_COLOR_ATTACHMENTS) {
    log_error("Render pass resolve attachment count %u exceeds max %u",
              desc->resolve_attachment_count, VKR_MAX_COLOR_ATTACHMENTS);
    return NULL;
  }
  uint8_t total_attachments =
      (uint8_t)(desc->color_attachment_count +
                (desc->depth_stencil_attachment ? 1u : 0u) +
                desc->resolve_attachment_count);
  if (total_attachments > VKR_RENDER_TARGET_MAX_ATTACHMENTS) {
    log_error("Render pass attachment count %u exceeds max %u",
              total_attachments, VKR_RENDER_TARGET_MAX_ATTACHMENTS);
    return NULL;
  }

  struct s_RenderPass *pass = vkr_allocator_alloc(
      &state->alloc, sizeof(*pass), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pass) {
    log_fatal("Failed to allocate render pass wrapper");
    return NULL;
  }
  MemZero(pass, sizeof(*pass));

  // Store name and descriptor-derived metadata
  pass->name = string8_duplicate(&state->alloc, &desc->name);
  pass->attachment_count = (uint8_t)(desc->color_attachment_count +
                                     (desc->depth_stencil_attachment ? 1u : 0u) +
                                     desc->resolve_attachment_count);
  pass->resolve_attachment_count = desc->resolve_attachment_count;
  for (uint8_t i = 0; i < desc->resolve_attachment_count; ++i) {
    pass->resolve_attachments[i] = desc->resolve_attachments[i];
  }
  pass->ends_in_present = false_v;

  uint8_t attachment_index = 0;
  for (uint8_t i = 0; i < desc->color_attachment_count; ++i) {
    const VkrRenderPassAttachmentDesc *att = &desc->color_attachments[i];
    VkClearValue clear = {0};
    if (vulkan_texture_format_is_integer(att->format)) {
      clear.color.uint32[0] = att->clear_value.color_u32.r;
      clear.color.uint32[1] = att->clear_value.color_u32.g;
      clear.color.uint32[2] = att->clear_value.color_u32.b;
      clear.color.uint32[3] = att->clear_value.color_u32.a;
    } else {
      clear.color.float32[0] = att->clear_value.color_f32.r;
      clear.color.float32[1] = att->clear_value.color_f32.g;
      clear.color.float32[2] = att->clear_value.color_f32.b;
      clear.color.float32[3] = att->clear_value.color_f32.a;
    }
    pass->clear_values[attachment_index++] = clear;
    if (att->final_layout == VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR) {
      pass->ends_in_present = true_v;
    }
  }

  if (desc->depth_stencil_attachment) {
    VkClearValue clear = {0};
    clear.depthStencil.depth =
        desc->depth_stencil_attachment->clear_value.depth_stencil.depth;
    clear.depthStencil.stencil =
        desc->depth_stencil_attachment->clear_value.depth_stencil.stencil;
    pass->clear_values[attachment_index++] = clear;
  }

  for (uint8_t i = 0; i < desc->resolve_attachment_count; ++i) {
    VkClearValue clear = {0};
    pass->clear_values[attachment_index++] = clear;
  }

  pass->vk = vkr_allocator_alloc(&state->alloc, sizeof(VulkanRenderPass),
                                 VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pass->vk) {
    log_fatal("Failed to allocate Vulkan render pass");
    goto cleanup;
  }
  MemZero(pass->vk, sizeof(VulkanRenderPass));

  if (!vulkan_renderpass_create_from_desc(state, desc, pass->vk)) {
    log_error("Failed to create Vulkan render pass from descriptor");
    goto cleanup;
  }

  if (desc->name.length > 0 && desc->name.str != NULL) {
    if (!vulkan_backend_renderpass_register(state, pass)) {
      goto cleanup;
    }
  }

  return pass;

cleanup:
  if (pass->vk) {
    vulkan_renderpass_destroy(state, pass->vk);
    vkr_allocator_free(&state->alloc, pass->vk, sizeof(VulkanRenderPass),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    pass->vk = NULL;
  }
  vkr_allocator_free(&state->alloc, pass, sizeof(*pass),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  return NULL;
}

vkr_internal bool32_t vulkan_backend_create_builtin_passes(
    VulkanBackendState *state, const VkrRendererBackendConfig *backend_config) {
  assert_log(state != NULL, "State not initialized");
  assert_log(backend_config != NULL, "Backend config is NULL");

  uint16_t desc_count =
      backend_config ? backend_config->renderpass_desc_count : (uint16_t)0;
  const VkrRenderPassDesc *descs =
      backend_config ? backend_config->pass_descs : NULL;

  if (!array_is_null_VkrRenderPassEntry(&state->render_pass_registry)) {
    state->render_pass_count = 0;
  } else {
    uint16_t capacity = (uint16_t)Max(4u, (uint32_t)desc_count + 4u);
    state->render_pass_registry =
        array_create_VkrRenderPassEntry(&state->alloc, capacity);
    state->render_pass_count = 0;
  }

  if (descs && desc_count > 0) {
    for (uint16_t i = 0; i < desc_count; ++i) {
      struct s_RenderPass *created =
          vulkan_backend_renderpass_create_from_desc_internal(state, &descs[i]);
      if (!created) {
        return false;
      }

      if (vkr_string8_equals_cstr_i(&descs[i].name,
                                    "renderpass.builtin.world")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] = true;
      } else if (vkr_string8_equals_cstr_i(&descs[i].name,
                                           "renderpass.builtin.ui")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_UI] = true;
      } else if (vkr_string8_equals_cstr_i(&descs[i].name,
                                           "renderpass.builtin.skybox")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_SKYBOX] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_SKYBOX] = true;
      } else if (vkr_string8_equals_cstr_i(&descs[i].name,
                                           "renderpass.builtin.picking")) {
        state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING] = created->vk;
        state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING] = true;
      }
    }
  }

  VkrTextureFormat swapchain_format =
      vulkan_vk_format_to_vkr(state->swapchain.format);
  VkrTextureFormat depth_format =
      vulkan_vk_format_to_vkr(state->device.depth_format);
  VkrClearValue clear_black = {.color_f32 = {0.0f, 0.0f, 0.0f, 1.0f}};
  VkrClearValue clear_world = {.color_f32 = {0.1f, 0.1f, 0.2f, 1.0f}};
  VkrClearValue clear_transparent = {.color_f32 = {0.0f, 0.0f, 0.0f, 0.0f}};
  VkrClearValue clear_depth = {.depth_stencil = {1.0f, 0}};
  VkrClearValue clear_picking = {.color_u32 = {0u, 0u, 0u, 0u}};

  if (!vulkan_backend_renderpass_lookup(
          state, string8_lit("Renderpass.Builtin.Skybox"))) {
    VkrRenderPassAttachmentDesc skybox_color = {
        .format = swapchain_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = clear_black,
    };
    VkrRenderPassAttachmentDesc skybox_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc skybox_desc = {
        .name = string8_lit("Renderpass.Builtin.Skybox"),
        .domain = VKR_PIPELINE_DOMAIN_SKYBOX,
        .color_attachment_count = 1,
        .color_attachments = &skybox_color,
        .depth_stencil_attachment = &skybox_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    struct s_RenderPass *skybox =
        vulkan_backend_renderpass_create_from_desc_internal(state, &skybox_desc);
    if (!skybox) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_SKYBOX] = skybox->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_SKYBOX] = true;
  }

  if (!vulkan_backend_renderpass_lookup(
          state, string8_lit("Renderpass.Builtin.World"))) {
    VkrRenderPassAttachmentDesc world_color = {
        .format = swapchain_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = clear_world,
    };
    VkrRenderPassAttachmentDesc world_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc world_desc = {
        .name = string8_lit("Renderpass.Builtin.World"),
        .domain = VKR_PIPELINE_DOMAIN_WORLD,
        .color_attachment_count = 1,
        .color_attachments = &world_color,
        .depth_stencil_attachment = &world_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    struct s_RenderPass *world =
        vulkan_backend_renderpass_create_from_desc_internal(state, &world_desc);
    if (!world) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD] = world->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD] = true;
  }

  if (!vulkan_backend_renderpass_lookup(
          state, string8_lit("Renderpass.Builtin.UI"))) {
    VkrRenderPassAttachmentDesc ui_color = {
        .format = swapchain_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
        .clear_value = clear_transparent,
    };
    VkrRenderPassDesc ui_desc = {
        .name = string8_lit("Renderpass.Builtin.UI"),
        .domain = VKR_PIPELINE_DOMAIN_UI,
        .color_attachment_count = 1,
        .color_attachments = &ui_color,
        .depth_stencil_attachment = NULL,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    struct s_RenderPass *ui =
        vulkan_backend_renderpass_create_from_desc_internal(state, &ui_desc);
    if (!ui) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_UI] = ui->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_UI] = true;
  }

  if (!vulkan_backend_renderpass_lookup(
          state, string8_lit("Renderpass.Builtin.Picking"))) {
    VkrRenderPassAttachmentDesc picking_color = {
        .format = VKR_TEXTURE_FORMAT_R32_UINT,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .clear_value = clear_picking,
    };
    VkrRenderPassAttachmentDesc picking_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc picking_desc = {
        .name = string8_lit("Renderpass.Builtin.Picking"),
        .domain = VKR_PIPELINE_DOMAIN_PICKING,
        .color_attachment_count = 1,
        .color_attachments = &picking_color,
        .depth_stencil_attachment = &picking_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    struct s_RenderPass *picking =
        vulkan_backend_renderpass_create_from_desc_internal(state, &picking_desc);
    if (!picking) {
      return false;
    }
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING] = picking->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING] = true;
  }

  return true;
}

bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  if (state->is_swapchain_recreation_requested) {
    // log_debug("Swapchain recreation was already requested");
    return false;
  }

  state->is_swapchain_recreation_requested = true;

  // Store old image count BEFORE recreation for proper cleanup
  uint32_t old_image_count = state->swapchain.image_count;

  // Wait for GPU to finish all pending work
  vkQueueWaitIdle(state->device.graphics_queue);

  // Attempt swapchain recreation FIRST
  // If this fails (e.g., window minimized), we don't destroy anything
  // and the old swapchain remains valid
  if (!vulkan_swapchain_recreate(state)) {
    log_warn("Swapchain recreation skipped or failed, keeping old swapchain");
    state->is_swapchain_recreation_requested = false;
    return false;
  }

  // Swapchain recreation succeeded - now clean up old resources and create new
  // ones

  // Invalidate framebuffer cache - all cached framebuffers reference old
  // swapchain images that are now invalid
  framebuffer_cache_invalidate(state);

  vulkan_backend_destroy_attachment_wrappers(state, old_image_count);

  // Clear images_in_flight using OLD count
  for (uint32_t i = 0; i < old_image_count; ++i) {
    array_set_VulkanFencePtr(&state->images_in_flight, i, NULL);
  }

  // Free command buffers and framebuffers using OLD count
  for (uint32_t i = 0; i < old_image_count; ++i) {
    vulkan_command_buffer_free(state, array_get_VulkanCommandBuffer(
                                          &state->graphics_command_buffers, i));
  }
  if (state->graphics_command_buffers.data) {
    array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);
  }

  uint32_t old_framebuffer_count = state->swapchain.framebuffers.length;
  for (uint32_t i = 0; i < old_framebuffer_count; ++i) {
    vulkan_framebuffer_destroy(
        state, array_get_VulkanFramebuffer(&state->swapchain.framebuffers, i));
  }
  if (state->swapchain.framebuffers.data && old_framebuffer_count > 0) {
    array_destroy_VulkanFramebuffer(&state->swapchain.framebuffers);
  }

  // Destroy old sync objects (counts may change with new swapchain)
  // Ensure nothing is using them anymore.
  vkDeviceWaitIdle(state->device.logical_device);

  for (uint32_t i = 0; i < state->image_available_semaphores.length; ++i) {
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->image_available_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->queue_complete_semaphores.length; ++i) {
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->queue_complete_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->in_flight_fences.length; ++i) {
    vulkan_fence_destroy(state,
                         array_get_VulkanFence(&state->in_flight_fences, i));
  }

  array_destroy_VkSemaphore(&state->image_available_semaphores);
  array_destroy_VkSemaphore(&state->queue_complete_semaphores);
  array_destroy_VulkanFence(&state->in_flight_fences);

  // Recreate sync objects with new sizes
  state->image_available_semaphores = array_create_VkSemaphore(
      &state->alloc, state->swapchain.max_in_flight_frames);

  state->queue_complete_semaphores =
      array_create_VkSemaphore(&state->alloc, state->swapchain.image_count);

  state->in_flight_fences = array_create_VulkanFence(
      &state->alloc, state->swapchain.max_in_flight_frames);

  for (uint32_t i = 0; i < state->swapchain.max_in_flight_frames; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(
            state->device.logical_device, &semaphore_info, state->allocator,
            array_get_VkSemaphore(&state->image_available_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create image available semaphore during resize");
      return false;
    }

    // Create signaled fence so first frame can wait safely.
    vulkan_fence_create(state, true_v,
                        array_get_VulkanFence(&state->in_flight_fences, i));
  }

  for (uint32_t i = 0; i < state->swapchain.image_count; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(
            state->device.logical_device, &semaphore_info, state->allocator,
            array_get_VkSemaphore(&state->queue_complete_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create queue complete semaphore during resize");
      return false;
    }
  }

  // Resize images_in_flight array if needed for new image count
  if (state->swapchain.image_count != old_image_count) {
    if (state->images_in_flight.data) {
      array_destroy_VulkanFencePtr(&state->images_in_flight);
    }
    // Recreate the images_in_flight array with the new size
    state->images_in_flight = array_create_VulkanFencePtr(
        &state->alloc, state->swapchain.image_count);
    for (uint32_t i = 0; i < state->swapchain.image_count; ++i) {
      array_set_VulkanFencePtr(&state->images_in_flight, i, NULL);
    }
  }


  if (!create_command_buffers(state)) {
    log_error("Failed to create Vulkan command buffers");
    return false;
  }

  if (!vulkan_backend_create_attachment_wrappers(state)) {
    log_error("Failed to recreate swapchain attachment wrappers");
    return false;
  }

  state->swapchain.framebuffers = array_create_VulkanFramebuffer(
      &state->swapchain_alloc, state->swapchain.images.length);
  for (uint32_t i = 0; i < state->swapchain.framebuffers.length; ++i) {
    array_set_VulkanFramebuffer(&state->swapchain.framebuffers, i,
                                (VulkanFramebuffer){
                                    .handle = VK_NULL_HANDLE,
                                    .attachments = {0},
                                    .renderpass = VK_NULL_HANDLE,
                                });
  }

  if (state->on_render_target_refresh_required) {
    state->on_render_target_refresh_required();
  }

  // Ensure current_frame is within bounds of new max_in_flight_frames
  if (state->current_frame >= state->swapchain.max_in_flight_frames) {
    state->current_frame = 0;
  }

  state->active_named_render_pass = NULL;
  state->is_swapchain_recreation_requested = false;

  log_debug("Swapchain recreation complete: %u images, %u in-flight frames",
            state->swapchain.image_count,
            state->swapchain.max_in_flight_frames);

  return true;
}

VkrRendererBackendInterface renderer_vulkan_get_interface() {
  return (VkrRendererBackendInterface){
      .initialize = renderer_vulkan_initialize,
      .shutdown = renderer_vulkan_shutdown,
      .on_resize = renderer_vulkan_on_resize,
      .get_device_information = renderer_vulkan_get_device_information,
      .wait_idle = renderer_vulkan_wait_idle,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .renderpass_create_desc = renderer_vulkan_renderpass_create_desc,
      .renderpass_destroy = renderer_vulkan_renderpass_destroy,
      .renderpass_get = renderer_vulkan_renderpass_get,
      .domain_renderpass_set = renderer_vulkan_domain_renderpass_set,
      .render_target_create = renderer_vulkan_render_target_create,
      .render_target_destroy = renderer_vulkan_render_target_destroy,
      .begin_render_pass = renderer_vulkan_begin_render_pass,
      .end_render_pass = renderer_vulkan_end_render_pass,
      .window_attachment_get = renderer_vulkan_window_attachment_get,
      .depth_attachment_get = renderer_vulkan_depth_attachment_get,
      .window_attachment_count_get = renderer_vulkan_window_attachment_count,
      .window_attachment_index_get = renderer_vulkan_window_attachment_index,
      .swapchain_format_get = renderer_vulkan_swapchain_format_get,
      .shadow_depth_format_get = renderer_vulkan_shadow_depth_format_get,
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .buffer_upload = renderer_vulkan_upload_buffer,
      .buffer_get_mapped_ptr = renderer_vulkan_buffer_get_mapped_ptr,
      .buffer_flush = renderer_vulkan_flush_buffer,
      .buffer_barrier = renderer_vulkan_buffer_barrier,
      .texture_create = renderer_vulkan_create_texture,
      .texture_create_with_payload = renderer_vulkan_create_texture_with_payload,
      .render_target_texture_create =
          renderer_vulkan_create_render_target_texture,
      .depth_attachment_create = renderer_vulkan_create_depth_attachment,
      .sampled_depth_attachment_create =
          renderer_vulkan_create_sampled_depth_attachment,
      .sampled_depth_attachment_array_create =
          renderer_vulkan_create_sampled_depth_attachment_array,
      .render_target_texture_msaa_create =
          renderer_vulkan_create_render_target_texture_msaa,
      .texture_transition_layout = renderer_vulkan_transition_texture_layout,
      .texture_update = renderer_vulkan_update_texture,
      .texture_write = renderer_vulkan_write_texture,
      .texture_resize = renderer_vulkan_resize_texture,
      .texture_destroy = renderer_vulkan_destroy_texture,
      .graphics_pipeline_create = renderer_vulkan_create_graphics_pipeline,
      .pipeline_get_shader_runtime_layout =
          renderer_vulkan_pipeline_get_shader_runtime_layout,
      .pipeline_update_state = renderer_vulkan_update_pipeline_state,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .instance_state_acquire = renderer_vulkan_instance_state_acquire,
      .instance_state_release = renderer_vulkan_instance_state_release,
      .bind_buffer = renderer_vulkan_bind_buffer,
      .set_viewport = renderer_vulkan_set_viewport,
      .set_scissor = renderer_vulkan_set_scissor,
      .set_depth_bias = renderer_vulkan_set_depth_bias,
      .draw = renderer_vulkan_draw,
      .draw_indexed = renderer_vulkan_draw_indexed,
      .draw_indexed_indirect = renderer_vulkan_draw_indexed_indirect,
      .set_instance_buffer = renderer_vulkan_set_instance_buffer,
      .get_and_reset_descriptor_writes_avoided =
          renderer_vulkan_get_and_reset_descriptor_writes_avoided,
      .rg_timing_begin_frame = renderer_vulkan_rg_timing_begin_frame,
      .rg_timing_begin_pass = renderer_vulkan_rg_timing_begin_pass,
      .rg_timing_end_pass = renderer_vulkan_rg_timing_end_pass,
      .rg_timing_get_results = renderer_vulkan_rg_timing_get_results,
      .readback_ring_init = renderer_vulkan_readback_ring_init,
      .readback_ring_shutdown = renderer_vulkan_readback_ring_shutdown,
      .request_pixel_readback = renderer_vulkan_request_pixel_readback,
      .get_pixel_readback_result = renderer_vulkan_get_pixel_readback_result,
      .update_readback_ring = renderer_vulkan_update_readback_ring,
      .get_allocator = renderer_vulkan_get_allocator,
      .set_default_2d_texture = renderer_vulkan_set_default_2d_texture,
  };
}
uint64_t
renderer_vulkan_get_and_reset_descriptor_writes_avoided(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  uint64_t value = state->descriptor_writes_avoided;
  state->descriptor_writes_avoided = 0;
  return value;
}

static void vulkan_rg_timing_destroy(VulkanBackendState *state) {
  if (!state) {
    return;
  }

  for (uint32_t i = 0; i < BUFFERING_FRAMES; ++i) {
    if (state->rg_timing.query_pools[i] != VK_NULL_HANDLE) {
      vkDestroyQueryPool(state->device.logical_device,
                         state->rg_timing.query_pools[i], state->allocator);
      state->rg_timing.query_pools[i] = VK_NULL_HANDLE;
    }
    state->rg_timing.frame_pass_counts[i] = 0;
  }

  if (state->rg_timing.query_results) {
    vkr_allocator_free(&state->alloc, state->rg_timing.query_results,
                       sizeof(uint64_t) *
                           (uint64_t)state->rg_timing.query_results_capacity *
                           2,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    state->rg_timing.query_results = NULL;
  }

  if (state->rg_timing.last_pass_ms) {
    vkr_allocator_free(&state->alloc, state->rg_timing.last_pass_ms,
                       sizeof(float64_t) *
                           (uint64_t)state->rg_timing.last_pass_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    state->rg_timing.last_pass_ms = NULL;
  }

  if (state->rg_timing.last_pass_valid) {
    vkr_allocator_free(&state->alloc, state->rg_timing.last_pass_valid,
                       sizeof(bool8_t) *
                           (uint64_t)state->rg_timing.last_pass_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    state->rg_timing.last_pass_valid = NULL;
  }

  state->rg_timing.query_capacity = 0;
  state->rg_timing.query_results_capacity = 0;
  state->rg_timing.last_pass_capacity = 0;
  state->rg_timing.last_pass_count = 0;
}

static bool32_t vulkan_rg_timing_create_pools(VulkanBackendState *state,
                                              uint32_t query_capacity) {
  if (!state || query_capacity == 0) {
    return false_v;
  }

  vulkan_rg_timing_destroy(state);

  VkQueryPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = query_capacity,
  };

  for (uint32_t i = 0; i < BUFFERING_FRAMES; ++i) {
    if (vkCreateQueryPool(state->device.logical_device, &pool_info,
                          state->allocator,
                          &state->rg_timing.query_pools[i]) != VK_SUCCESS) {
      log_warn("Failed to create Vulkan RG timing query pool");
      vulkan_rg_timing_destroy(state);
      return false_v;
    }
  }

  state->rg_timing.query_capacity = query_capacity;
  state->rg_timing.query_results_capacity = query_capacity;
  state->rg_timing.query_results = vkr_allocator_alloc(
      &state->alloc, sizeof(uint64_t) * (uint64_t)query_capacity * 2,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!state->rg_timing.query_results) {
    log_warn("Failed to allocate RG timing query result buffer");
    vulkan_rg_timing_destroy(state);
    return false_v;
  }

  return true_v;
}

static bool32_t vulkan_rg_timing_ensure_capacity(VulkanBackendState *state,
                                                 uint32_t pass_count) {
  if (!state || !state->rg_timing.supported) {
    return false_v;
  }

  uint32_t required = pass_count * 2;
  if (required == 0) {
    return false_v;
  }

  if (required <= state->rg_timing.query_capacity) {
    return true_v;
  }

  vkDeviceWaitIdle(state->device.logical_device);
  return vulkan_rg_timing_create_pools(state, required);
}

static void vulkan_rg_timing_fetch_results(VulkanBackendState *state) {
  if (!state || !state->rg_timing.supported) {
    return;
  }

  uint32_t frame_index = state->current_frame;
  uint32_t pass_count = state->rg_timing.frame_pass_counts[frame_index];
  state->rg_timing.last_pass_count = 0;

  if (pass_count == 0 || state->rg_timing.query_capacity == 0) {
    state->rg_timing.frame_pass_counts[frame_index] = 0;
    return;
  }

  uint32_t query_count = pass_count * 2;
  if (query_count > state->rg_timing.query_capacity) {
    query_count = state->rg_timing.query_capacity;
    pass_count = query_count / 2;
  }

  if (state->rg_timing.query_results_capacity < query_count) {
    if (state->rg_timing.query_results) {
      vkr_allocator_free(&state->alloc, state->rg_timing.query_results,
                         sizeof(uint64_t) *
                             (uint64_t)state->rg_timing.query_results_capacity *
                             2,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      state->rg_timing.query_results = NULL;
    }
    state->rg_timing.query_results_capacity = query_count;
    state->rg_timing.query_results = vkr_allocator_alloc(
        &state->alloc, sizeof(uint64_t) * (uint64_t)query_count * 2,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!state->rg_timing.query_results) {
      log_warn("Failed to resize RG timing query result buffer");
      state->rg_timing.query_results_capacity = 0;
      state->rg_timing.frame_pass_counts[frame_index] = 0;
      return;
    }
  }

  if (state->rg_timing.last_pass_capacity < pass_count) {
    if (state->rg_timing.last_pass_ms) {
      vkr_allocator_free(&state->alloc, state->rg_timing.last_pass_ms,
                         sizeof(float64_t) *
                             (uint64_t)state->rg_timing.last_pass_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      state->rg_timing.last_pass_ms = NULL;
    }
    if (state->rg_timing.last_pass_valid) {
      vkr_allocator_free(&state->alloc, state->rg_timing.last_pass_valid,
                         sizeof(bool8_t) *
                             (uint64_t)state->rg_timing.last_pass_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      state->rg_timing.last_pass_valid = NULL;
    }
    state->rg_timing.last_pass_capacity = pass_count;
    state->rg_timing.last_pass_ms = vkr_allocator_alloc(
        &state->alloc, sizeof(float64_t) * (uint64_t)pass_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    state->rg_timing.last_pass_valid = vkr_allocator_alloc(
        &state->alloc, sizeof(bool8_t) * (uint64_t)pass_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!state->rg_timing.last_pass_ms || !state->rg_timing.last_pass_valid) {
      log_warn("Failed to allocate RG timing results");
      state->rg_timing.last_pass_count = 0;
      state->rg_timing.frame_pass_counts[frame_index] = 0;
      return;
    }
  }

  VkQueryPool pool = state->rg_timing.query_pools[frame_index];
  if (pool == VK_NULL_HANDLE) {
    state->rg_timing.frame_pass_counts[frame_index] = 0;
    return;
  }

  VkResult result = vkGetQueryPoolResults(
      state->device.logical_device, pool, 0, query_count,
      sizeof(uint64_t) * (uint64_t)query_count * 2,
      state->rg_timing.query_results, sizeof(uint64_t) * 2,
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
  if (result != VK_SUCCESS && result != VK_NOT_READY) {
    log_warn("Failed to read RG timing query results");
    state->rg_timing.frame_pass_counts[frame_index] = 0;
    return;
  }

  float64_t period = (float64_t)state->device.properties.limits.timestampPeriod;
  for (uint32_t i = 0; i < pass_count; ++i) {
    uint32_t start_query = i * 2;
    uint32_t end_query = start_query + 1;
    uint64_t start_ts =
        state->rg_timing.query_results[start_query * 2 + 0];
    uint64_t start_avail =
        state->rg_timing.query_results[start_query * 2 + 1];
    uint64_t end_ts = state->rg_timing.query_results[end_query * 2 + 0];
    uint64_t end_avail = state->rg_timing.query_results[end_query * 2 + 1];

    bool8_t valid = (start_avail != 0 && end_avail != 0 && end_ts >= start_ts);
    state->rg_timing.last_pass_valid[i] = valid;
    if (valid) {
      state->rg_timing.last_pass_ms[i] =
          ((float64_t)(end_ts - start_ts) * period) / 1000000.0;
    } else {
      state->rg_timing.last_pass_ms[i] = 0.0;
    }
  }

  state->rg_timing.last_pass_count = pass_count;
  state->rg_timing.frame_pass_counts[frame_index] = 0;
}

bool8_t renderer_vulkan_rg_timing_begin_frame(void *backend_state,
                                              uint32_t pass_count) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !state->rg_timing.supported || pass_count == 0) {
    return false_v;
  }

  if (!vulkan_rg_timing_ensure_capacity(state, pass_count)) {
    return false_v;
  }

  VkQueryPool pool = state->rg_timing.query_pools[state->current_frame];
  if (pool == VK_NULL_HANDLE) {
    return false_v;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  if (!command_buffer) {
    return false_v;
  }

  vkCmdResetQueryPool(command_buffer->handle, pool, 0,
                      state->rg_timing.query_capacity);
  state->rg_timing.frame_pass_counts[state->current_frame] = pass_count;
  return true_v;
}

void renderer_vulkan_rg_timing_begin_pass(void *backend_state,
                                          uint32_t pass_index) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !state->rg_timing.supported ||
      state->rg_timing.query_capacity == 0) {
    return;
  }

  uint32_t query_index = pass_index * 2;
  if (query_index >= state->rg_timing.query_capacity) {
    return;
  }

  VkQueryPool pool = state->rg_timing.query_pools[state->current_frame];
  if (pool == VK_NULL_HANDLE) {
    return;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  if (!command_buffer) {
    return;
  }

  vkCmdWriteTimestamp(command_buffer->handle, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      pool, query_index);
}

void renderer_vulkan_rg_timing_end_pass(void *backend_state,
                                        uint32_t pass_index) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !state->rg_timing.supported ||
      state->rg_timing.query_capacity == 0) {
    return;
  }

  uint32_t query_index = pass_index * 2 + 1;
  if (query_index >= state->rg_timing.query_capacity) {
    return;
  }

  VkQueryPool pool = state->rg_timing.query_pools[state->current_frame];
  if (pool == VK_NULL_HANDLE) {
    return;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  if (!command_buffer) {
    return;
  }

  vkCmdWriteTimestamp(command_buffer->handle,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool,
                      query_index);
}

bool8_t renderer_vulkan_rg_timing_get_results(void *backend_state,
                                              uint32_t *out_pass_count,
                                              const float64_t **out_pass_ms,
                                              const bool8_t **out_pass_valid) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (out_pass_count) {
    *out_pass_count = 0;
  }
  if (out_pass_ms) {
    *out_pass_ms = NULL;
  }
  if (out_pass_valid) {
    *out_pass_valid = NULL;
  }

  if (!state || !state->rg_timing.supported ||
      state->rg_timing.last_pass_count == 0) {
    return false_v;
  }

  if (out_pass_count) {
    *out_pass_count = state->rg_timing.last_pass_count;
  }
  if (out_pass_ms) {
    *out_pass_ms = state->rg_timing.last_pass_ms;
  }
  if (out_pass_valid) {
    *out_pass_valid = state->rg_timing.last_pass_valid;
  }
  return true_v;
}

// todo: set up event manager for window stuff and maybe other events
bool32_t
renderer_vulkan_initialize(void **out_backend_state,
                           VkrRendererBackendType type, VkrWindow *window,
                           uint32_t initial_width, uint32_t initial_height,
                           VkrDeviceRequirements *device_requirements,
                           const VkrRendererBackendConfig *backend_config) {
  assert_log(out_backend_state != NULL, "Out backend state is NULL");
  assert_log(type == VKR_RENDERER_BACKEND_TYPE_VULKAN,
             "Vulkan backend type is required");
  assert_log(window != NULL, "Window is NULL");
  assert_log(initial_width > 0, "Initial width is 0");
  assert_log(initial_height > 0, "Initial height is 0");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  // log_debug("Initializing Vulkan backend");

  ArenaFlags temp_arena_flags = bitset8_create();
  Arena *temp_arena = arena_create(MB(4), KB(64), temp_arena_flags);
  if (!temp_arena) {
    log_fatal("Failed to create temporary arena");
    return false;
  }

  VkrAllocator temp_scope = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_scope);

  ArenaFlags swapchain_arena_flags = bitset8_create();
  Arena *swapchain_arena = arena_create(KB(64), KB(64), swapchain_arena_flags);
  if (!swapchain_arena) {
    log_fatal("Failed to create swapchain arena");
    arena_destroy(temp_arena);
    return false;
  }

  VkrAllocator swapchain_alloc = {.ctx = swapchain_arena};
  vkr_allocator_arena(&swapchain_alloc);

  ArenaFlags arena_flags = bitset8_create();
  Arena *arena = arena_create(MB(1), MB(1), arena_flags);
  if (!arena) {
    log_fatal("Failed to create arena");
    return false;
  }

  VkrAllocator alloc = {.ctx = arena};
  vkr_allocator_arena(&alloc);

  VulkanBackendState *backend_state = vkr_allocator_alloc(
      &alloc, sizeof(VulkanBackendState), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!backend_state) {
    log_fatal("Failed to allocate backend state");
    arena_destroy(arena);
    arena_destroy(temp_arena);
    return false;
  }

  MemZero(backend_state, sizeof(VulkanBackendState));
  backend_state->arena = arena;
  backend_state->alloc = alloc;
  backend_state->temp_arena = temp_arena;
  backend_state->temp_scope = temp_scope;
  backend_state->swapchain_arena = swapchain_arena;
  backend_state->swapchain_alloc = swapchain_alloc;
  backend_state->window = window;
  backend_state->device_requirements = device_requirements;
  backend_state->descriptor_writes_avoided = 0;
  backend_state->render_pass_registry = (Array_VkrRenderPassEntry){0};
  backend_state->render_pass_count = 0;
  backend_state->swapchain_image_textures = NULL;
  backend_state->depth_texture = NULL;
  backend_state->on_render_target_refresh_required =
      backend_config ? backend_config->on_render_target_refresh_required : NULL;

  backend_state->current_render_pass_domain =
      VKR_PIPELINE_DOMAIN_COUNT; // Invalid domain
  backend_state->active_named_render_pass = NULL;
  backend_state->render_pass_active = false;
  backend_state->active_image_index = 0;

  for (uint32_t i = 0; i < VKR_PIPELINE_DOMAIN_COUNT; i++) {
    backend_state->domain_render_passes[i] = NULL;
    backend_state->domain_initialized[i] = false;
  }

  *out_backend_state = backend_state;
  if (!vulkan_allocator_create(&backend_state->alloc,
                               &backend_state->vk_allocator,
                               VKR_VULKAN_ALLOCATOR_COMMIT_SIZE,
                               VKR_VULKAN_ALLOCATOR_RESERVE_SIZE)) {
    log_fatal("Failed to create Vulkan allocator");
    return false;
  }
  backend_state->allocator =
      vulkan_allocator_callbacks(&backend_state->vk_allocator);

  if (!vulkan_instance_create(backend_state, window)) {
    log_fatal("Failed to create Vulkan instance");
    return false;
  }

#ifndef NDEBUG
  if (!vulkan_debug_create_debug_messenger(backend_state)) {
    log_fatal("Failed to create Vulkan debug messenger");
    return false;
  }
#endif

  if (!vulkan_platform_create_surface(backend_state)) {
    log_fatal("Failed to create Vulkan surface");
    return false;
  }

  if (!vulkan_device_pick_physical_device(backend_state)) {
    log_fatal("Failed to create Vulkan physical device");
    return false;
  }

  if (!vulkan_device_create_logical_device(backend_state)) {
    log_fatal("Failed to create Vulkan logical device");
    return false;
  }

  vulkan_pipeline_cache_initialize(backend_state);

  backend_state->rg_timing.supported =
      backend_state->device.properties.limits.timestampComputeAndGraphics != 0;
  if (!backend_state->rg_timing.supported ||
      backend_state->device.properties.limits.timestampPeriod <= 0.0f) {
    backend_state->rg_timing.supported = false_v;
    log_warn("Vulkan GPU timestamps not supported; RG GPU timings disabled");
  }

  if (!vulkan_swapchain_create(backend_state)) {
    log_fatal("Failed to create Vulkan swapchain");
    return false;
  }

  if (!vulkan_backend_create_builtin_passes(backend_state, backend_config)) {
    log_fatal("Failed to create built-in render passes");
    return false;
  }

  if (!create_domain_render_passes(backend_state)) {
    log_fatal("Failed to create Vulkan domain render passes");
    return false;
  }


  if (!vulkan_backend_create_attachment_wrappers(backend_state)) {
    log_fatal("Failed to create swapchain attachment wrappers");
    return false;
  }

  backend_state->swapchain.framebuffers = array_create_VulkanFramebuffer(
      &backend_state->swapchain_alloc, backend_state->swapchain.images.length);
  for (uint32_t i = 0; i < backend_state->swapchain.images.length; i++) {
    array_set_VulkanFramebuffer(&backend_state->swapchain.framebuffers, i,
                                (VulkanFramebuffer){
                                    .handle = VK_NULL_HANDLE,
                                    .attachments = {0},
                                    .renderpass = VK_NULL_HANDLE,
                                });
  }

  if (!create_command_buffers(backend_state)) {
    log_fatal("Failed to create Vulkan command buffers");
    return false;
  }
  backend_state->image_available_semaphores = array_create_VkSemaphore(
      &backend_state->alloc, backend_state->swapchain.max_in_flight_frames);
  backend_state->queue_complete_semaphores = array_create_VkSemaphore(
      &backend_state->alloc, backend_state->swapchain.image_count);
  backend_state->in_flight_fences = array_create_VulkanFence(
      &backend_state->alloc, backend_state->swapchain.max_in_flight_frames);
  for (uint32_t i = 0; i < backend_state->swapchain.max_in_flight_frames; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    if (vkCreateSemaphore(backend_state->device.logical_device, &semaphore_info,
                          backend_state->allocator,
                          array_get_VkSemaphore(
                              &backend_state->image_available_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create Vulkan image available semaphore");
      return false;
    }

    // fence is created with is_signaled set to true, because we want to wait
    // on the fence until the previous frame is finished
    vulkan_fence_create(
        backend_state, true_v,
        array_get_VulkanFence(&backend_state->in_flight_fences, i));
  }

  // Create queue complete semaphores for each swapchain image
  for (uint32_t i = 0; i < backend_state->swapchain.image_count; i++) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    if (vkCreateSemaphore(backend_state->device.logical_device, &semaphore_info,
                          backend_state->allocator,
                          array_get_VkSemaphore(
                              &backend_state->queue_complete_semaphores, i)) !=
        VK_SUCCESS) {
      log_fatal("Failed to create Vulkan queue complete semaphore");
      return false;
    }
  }

  backend_state->images_in_flight = array_create_VulkanFencePtr(
      &backend_state->alloc, backend_state->swapchain.image_count);
  for (uint32_t i = 0; i < backend_state->swapchain.image_count; i++) {
    array_set_VulkanFencePtr(&backend_state->images_in_flight, i, NULL);
  }

  // Create resource handle pools for textures and buffers.
  // Pool allocation allows proper free on resource destroy (arena frees are
  // no-ops). Each pool is wrapped with a VkrAllocator for statistics tracking.

  if (!vkr_pool_create(sizeof(struct s_TextureHandle), VKR_MAX_TEXTURE_HANDLES,
                       &backend_state->texture_handle_pool)) {
    log_fatal("Failed to create texture handle pool");
    return false;
  }
  backend_state->texture_pool_alloc.ctx = &backend_state->texture_handle_pool;
  vkr_pool_allocator_create(&backend_state->texture_pool_alloc);

  if (!vkr_pool_create(sizeof(struct s_BufferHandle), VKR_MAX_BUFFER_HANDLES,
                       &backend_state->buffer_handle_pool)) {
    log_fatal("Failed to create buffer handle pool");
    vkr_pool_allocator_destroy(&backend_state->texture_pool_alloc);
    return false;
  }
  backend_state->buffer_pool_alloc.ctx = &backend_state->buffer_handle_pool;
  vkr_pool_allocator_create(&backend_state->buffer_pool_alloc);

  if (!vkr_pool_create(sizeof(struct s_RenderTarget), VKR_MAX_RENDER_TARGET_HANDLES,
                       &backend_state->render_target_pool)) {
    log_fatal("Failed to create render target handle pool");
    vkr_pool_allocator_destroy(&backend_state->buffer_pool_alloc);
    vkr_pool_allocator_destroy(&backend_state->texture_pool_alloc);
    return false;
  }
  backend_state->render_target_alloc.ctx = &backend_state->render_target_pool;
  vkr_pool_allocator_create(&backend_state->render_target_alloc);

  return true;
}

void renderer_vulkan_get_device_information(
    void *backend_state, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  vulkan_device_get_information(state, device_information, temp_arena);
}

void renderer_vulkan_shutdown(void *backend_state) {
  // log_debug("Shutting down Vulkan backend");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Ensure all GPU work is complete before destroying any resources
  vkDeviceWaitIdle(state->device.logical_device);
  vulkan_pipeline_cache_shutdown(state);

  // Flush deferred destruction queue - destroy all pending resources
  vulkan_deferred_destroy_flush(state);

  // Invalidate framebuffer cache - destroy all cached framebuffers
  framebuffer_cache_invalidate(state);

  // Ensure pixel readback ring resources are destroyed before device teardown.
  renderer_vulkan_readback_ring_shutdown(state);
  vulkan_rg_timing_destroy(state);

  // Free command buffers first to release references to pipelines
  for (uint32_t i = 0; i < state->graphics_command_buffers.length; i++) {
    vulkan_command_buffer_free(state, array_get_VulkanCommandBuffer(
                                          &state->graphics_command_buffers, i));
  }
  array_destroy_VulkanCommandBuffer(&state->graphics_command_buffers);

  // Wait again to ensure command buffer cleanup is complete
  vkDeviceWaitIdle(state->device.logical_device);

  for (uint32_t i = 0; i < state->swapchain.max_in_flight_frames; i++) {
    vulkan_fence_destroy(state,
                         array_get_VulkanFence(&state->in_flight_fences, i));
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->image_available_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->swapchain.image_count; i++) {
    vkDestroySemaphore(
        state->device.logical_device,
        *array_get_VkSemaphore(&state->queue_complete_semaphores, i),
        state->allocator);
  }
  for (uint32_t i = 0; i < state->swapchain.framebuffers.length; i++) {
    VulkanFramebuffer *framebuffer =
        array_get_VulkanFramebuffer(&state->swapchain.framebuffers, i);
    vulkan_framebuffer_destroy(state, framebuffer);
  }
  array_destroy_VulkanFramebuffer(&state->swapchain.framebuffers);

  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry && entry->pass && entry->pass->vk) {
      vulkan_renderpass_destroy(state, entry->pass->vk);
    }
  }

  for (uint32_t domain = 0; domain < VKR_PIPELINE_DOMAIN_COUNT; domain++) {
    if (!state->domain_initialized[domain]) {
      continue;
    }

    if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT ||
        domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY) {
      state->domain_render_passes[domain] = NULL;
      continue;
    }

    VulkanRenderPass *domain_pass = state->domain_render_passes[domain];
    if (!domain_pass) {
      continue;
    }

    bool skip_destroy = false;
    for (uint32_t i = 0; i < state->render_pass_count; ++i) {
      VkrRenderPassEntry *entry =
          array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
      if (entry && entry->pass && entry->pass->vk == domain_pass) {
        skip_destroy = true;
        break;
      }
    }

    if (!skip_destroy) {
      vulkan_renderpass_destroy(state, domain_pass);
    }

    state->domain_render_passes[domain] = NULL;
  }
  vulkan_backend_destroy_attachment_wrappers(state,
                                             state->swapchain.image_count);
  vulkan_swapchain_destroy(state);
  vulkan_device_destroy_logical_device(state);
  vulkan_device_release_physical_device(state);
  vulkan_platform_destroy_surface(state);
#ifndef NDEBUG
  vulkan_debug_destroy_debug_messenger(state);
#endif
  vulkan_instance_destroy(state);
  vulkan_allocator_destroy(&state->alloc, &state->vk_allocator);
  state->allocator = NULL;

  // Destroy resource handle pool allocators (also destroys underlying pools)
  vkr_pool_allocator_destroy(&state->texture_pool_alloc);
  vkr_pool_allocator_destroy(&state->buffer_pool_alloc);
  vkr_pool_allocator_destroy(&state->render_target_alloc);

  arena_destroy(state->swapchain_arena);
  arena_destroy(state->temp_arena);
  arena_destroy(state->arena);
  return;
}

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height) {
  // log_debug("Resizing Vulkan backend to %d x %d", new_width, new_height);

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  if (state->is_swapchain_recreation_requested) {
    // log_debug("Swapchain recreation was already requested");
    return;
  }

  state->swapchain.extent.width = new_width;
  state->swapchain.extent.height = new_height;

  if (!vulkan_backend_recreate_swapchain(state)) {
    log_error("Failed to recreate swapchain");
    return;
  }

  return;
}

VkrRendererError renderer_vulkan_wait_idle(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkResult result = vkDeviceWaitIdle(state->device.logical_device);
  if (result != VK_SUCCESS) {
    log_warn("Failed to wait for Vulkan device to be idle");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

/**
 * @brief Begin a new rendering frame
 *
 * RENDER PASS MANAGEMENT:
 * This function deliberately does NOT start any render pass. Render passes are
 * started explicitly via vkr_renderer_begin_render_pass() and ended via
 * vkr_renderer_end_render_pass().
 *
 * FRAME LIFECYCLE:
 * 1. Wait for previous frame fence (GPU finished previous frame)
 * 2. Acquire next swapchain image
 * 3. Reset and begin command buffer recording
 * 4. Set initial viewport and scissor (may be overridden by render pass
 * switches)
 * 5. Mark render pass as inactive (render_pass_active = false)
 * 6. Set domain to invalid (current_render_pass_domain = COUNT)
 *
 * RENDER PASS STATE:
 * - render_pass_active = false: No render pass is active at frame start
 * - current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT: Invalid domain
 * - swapchain_image_is_present_ready = false: Image not yet transitioned to
 * PRESENT
 *
 * NEXT STEPS:
 * After begin_frame, the application should:
 * 1. Update global uniforms (view/projection matrices)
 * 2. Begin a render pass (vkr_renderer_begin_render_pass)
 * 3. Bind pipelines and draw geometry
 * 4. End render passes and call end_frame
 *
 * @param backend_state Vulkan backend state
 * @param delta_time Frame delta time in seconds
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError renderer_vulkan_begin_frame(void *backend_state,
                                             float64_t delta_time) {
  // log_debug("Beginning Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  state->frame_delta = delta_time;
  state->swapchain_image_is_present_ready = false;

  // Wait for the current frame's fence to be signaled (previous frame
  // finished)
  if (!vulkan_fence_wait(state, UINT64_MAX,
                         array_get_VulkanFence(&state->in_flight_fences,
                                               state->current_frame))) {
    log_warn("Vulkan fence timed out");
    return VKR_RENDERER_ERROR_NONE;
  }

  vulkan_rg_timing_fetch_results(state);

  // Process deferred destruction queue after fence wait
  // (safe to destroy resources from BUFFERING_FRAMES ago)
  vulkan_deferred_destroy_process(state);

  // Acquire the next image from the swapchain
  if (!vulkan_swapchain_acquire_next_image(
          state, UINT64_MAX,
          *array_get_VkSemaphore(&state->image_available_semaphores,
                                 state->current_frame),
          VK_NULL_HANDLE, // Don't use fence with acquire - it conflicts with
                          // queue submit
          &state->image_index)) {
    log_warn("Failed to acquire next image");
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);
  vulkan_command_buffer_reset(command_buffer);

  if (!vulkan_command_buffer_begin(command_buffer)) {
    log_fatal("Failed to begin Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }
  state->frame_active = true_v;

  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)state->swapchain.extent.width,
      .height = (float32_t)state->swapchain.extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  VkRect2D scissor = {
      .offset = {0, 0},
      .extent = state->swapchain.extent,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle, 0, 1, &scissor);

  state->render_pass_active = false;
  state->current_render_pass_domain =
      VKR_PIPELINE_DOMAIN_COUNT; // Invalid domain (no pass active)
  state->active_named_render_pass = NULL;

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(vertex_count > 0, "Vertex count is 0");
  assert_log(instance_count > 0, "Instance count is 0");
  assert_log(first_vertex < vertex_count, "First vertex is out of bounds");
  assert_log(first_instance < instance_count,
             "First instance is out of bounds");

  // log_debug("Drawing Vulkan vertices");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdDraw(command_buffer->handle, vertex_count, instance_count, first_vertex,
            first_instance);

  return;
}

/**
 * @brief End the current rendering frame and submit to GPU
 *
 * IMAGE LAYOUT TRANSITIONS:
 * The function handles a critical layout transition case:
 * - If WORLD domain was last active: Image is in COLOR_ATTACHMENT_OPTIMAL
 * - Image must be transitioned to PRESENT_SRC_KHR for presentation
 * - If UI/POST domain was last: Image is already in PRESENT_SRC_KHR (no-op)
 *
 * This is tracked via swapchain_image_is_present_ready flag:
 * - Set by UI/POST render passes (finalLayout = PRESENT_SRC_KHR)
 * - If false: Manual transition required (WORLD was last)
 * - If true: No transition needed (UI/POST was last)
 *
 * FRAME SUBMISSION FLOW:
 * 1. End any active render pass
 * 2. Transition image to PRESENT layout if needed
 * 3. End command buffer recording
 * 4. Wait for previous frame using this image (fence)
 * 5. Submit command buffer to GPU queue
 * 6. Present image to swapchain
 * 7. Advance frame counter for triple buffering
 *
 * SYNCHRONIZATION:
 * - Image available semaphore: Signals when image is acquired from swapchain
 * - Queue complete semaphore: Signals when GPU finishes rendering
 * - In-flight fence: Ensures previous frame using this image has completed
 *
 * @param backend_state Vulkan backend state
 * @param delta_time Frame delta time in seconds
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError renderer_vulkan_end_frame(void *backend_state,
                                           float64_t delta_time) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(delta_time > 0, "Delta time is 0");

  // log_debug("Ending Vulkan frame");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  if (state->render_pass_active) {
    VkrRendererError end_err = renderer_vulkan_end_render_pass(state);
    if (end_err != VKR_RENDERER_ERROR_NONE) {
      log_fatal("Failed to end active render pass");
      return end_err;
    }
  }

  // ============================================================================
  // CRITICAL IMAGE LAYOUT TRANSITION
  // ============================================================================
  // Handle the case where WORLD domain was the last (or only) pass active:
  //
  // WORLD render pass: finalLayout = COLOR_ATTACHMENT_OPTIMAL
  //   → Image is left in attachment-optimal layout for efficient UI chaining
  //   → If no UI pass runs, we must transition to PRESENT_SRC_KHR here
  //
  // UI render pass: finalLayout = PRESENT_SRC_KHR
  //   → Image is already in present layout, no transition needed
  //   → swapchain_image_is_present_ready = true (set by UI pass)
  //
  // POST render pass: finalLayout = PRESENT_SRC_KHR
  //   → Image is already in present layout, no transition needed
  //   → swapchain_image_is_present_ready = true (set by POST pass)
  //
  // This design allows efficient WORLD→UI chaining without extra transitions,
  // while still supporting WORLD-only frames via manual transition here.
  // ============================================================================
  if (!state->swapchain_image_is_present_ready) {
    VkImageMemoryBarrier present_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image =
            *array_get_VkImage(&state->swapchain.images, state->image_index),
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };

    vkCmdPipelineBarrier(command_buffer->handle,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &present_barrier);
  }

  if (!vulkan_command_buffer_end(command_buffer)) {
    log_fatal("Failed to end Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  state->frame_active = false_v;

  // Make sure the previous frame is not using this image (i.e. its fence is
  // being waited on)
  VulkanFencePtr *image_fence =
      array_get_VulkanFencePtr(&state->images_in_flight, state->image_index);
  if (*image_fence != NULL) { // was frame
    if (!vulkan_fence_wait(state, UINT64_MAX, *image_fence)) {
      log_warn("Failed to wait for Vulkan fence");
      return VKR_RENDERER_ERROR_NONE;
    }
  }

  // Mark the image fence as in-use by this frame.
  *image_fence =
      array_get_VulkanFence(&state->in_flight_fences, state->current_frame);

  // Reset the fence for use on the next frame
  vulkan_fence_reset(state, array_get_VulkanFence(&state->in_flight_fences,
                                                  state->current_frame));

  VkPipelineStageFlags flags[1] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer->handle,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = array_get_VkSemaphore(
          &state->queue_complete_semaphores, state->image_index),
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = array_get_VkSemaphore(
          &state->image_available_semaphores, state->current_frame),
      .pWaitDstStageMask = flags,
  };

  VkResult result = vkQueueSubmit(
      state->device.graphics_queue, 1, &submit_info,
      array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
          ->handle);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to submit Vulkan command buffer");
    return VKR_RENDERER_ERROR_NONE;
  }

  vulkan_command_buffer_update_submitted(command_buffer);

  // Monotonic submit counter used for async readback submission tracking.
  state->submit_serial++;

  // Advance frame counter for triple-buffering synchronization.
  // Must happen after queue submit so readback fence checks can detect
  // completion.
  state->current_frame =
      (state->current_frame + 1) % state->swapchain.max_in_flight_frames;

  if (!vulkan_swapchain_present(
          state,
          *array_get_VkSemaphore(&state->queue_complete_semaphores,
                                 state->image_index),
          state->image_index)) {
    log_warn("Failed to present Vulkan image");
    return VKR_RENDERER_ERROR_NONE;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_draw_indexed(void *backend_state, uint32_t index_count,
                                  uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset,
                                  uint32_t first_instance) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(index_count > 0, "Index count is 0");
  assert_log(instance_count > 0, "Instance count is 0");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdDrawIndexed(command_buffer->handle, index_count, instance_count,
                   first_index, vertex_offset, first_instance);

  return;
}

void renderer_vulkan_draw_indexed_indirect(
    void *backend_state, VkrBackendResourceHandle indirect_buffer,
    uint64_t offset, uint32_t draw_count, uint32_t stride) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(indirect_buffer.ptr != NULL, "Indirect buffer is NULL");
  assert_log(draw_count > 0, "Draw count is 0");
  assert_log(stride > 0, "Stride is 0");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer =
      (struct s_BufferHandle *)indirect_buffer.ptr;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  if (!state->device.features.multiDrawIndirect && draw_count > 1) {
    for (uint32_t i = 0; i < draw_count; ++i) {
      uint64_t draw_offset = offset + (uint64_t)i * stride;
      vkCmdDrawIndexedIndirect(command_buffer->handle, buffer->buffer.handle,
                               draw_offset, 1, stride);
    }
    return;
  }

  vkCmdDrawIndexedIndirect(command_buffer->handle, buffer->buffer.handle,
                           offset, draw_count, stride);
}

VkrBackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const VkrBufferDescription *desc,
                              const void *initial_data) {
  // log_debug("Creating Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_BufferHandle *buffer = vkr_allocator_alloc(
      &state->buffer_pool_alloc, sizeof(struct s_BufferHandle),
      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!buffer) {
    log_fatal("Failed to allocate buffer (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(buffer, sizeof(struct s_BufferHandle));

  // Copy the description so we can access usage flags later
  buffer->description = *desc;

  if (!vulkan_buffer_create(state, desc, buffer)) {
    vkr_allocator_free(&state->buffer_pool_alloc, buffer,
                       sizeof(struct s_BufferHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    log_fatal("Failed to create Vulkan buffer");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // If initial data is provided, load it into the buffer
  if (initial_data && desc->size > 0) {
    if (renderer_vulkan_upload_buffer(
            backend_state, (VkrBackendResourceHandle){.ptr = buffer}, 0,
            desc->size, initial_data) != VKR_RENDERER_ERROR_NONE) {
      vulkan_buffer_destroy(state, &buffer->buffer);
      vkr_allocator_free(&state->buffer_pool_alloc, buffer,
                         sizeof(struct s_BufferHandle),
                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
      log_error("Failed to upload initial data into buffer");
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  }

  return (VkrBackendResourceHandle){.ptr = buffer};
}

VkrRendererError renderer_vulkan_update_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data) {
  // log_debug("Updating Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  if (!vulkan_buffer_load_data(state, &buffer->buffer, offset, size, 0, data)) {
    log_fatal("Failed to update Vulkan buffer");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void *renderer_vulkan_buffer_get_mapped_ptr(void *backend_state,
                                            VkrBackendResourceHandle handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !handle.ptr) {
    return NULL;
  }
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  return buffer->buffer.mapped_ptr;
}

VkrRendererError renderer_vulkan_flush_buffer(void *backend_state,
                                              VkrBackendResourceHandle handle,
                                              uint64_t offset, uint64_t size) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !handle.ptr) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  vulkan_buffer_flush(state, &buffer->buffer, offset, size);
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkAccessFlags
vulkan_buffer_access_to_vk(VkrBufferAccessFlags access) {
  VkAccessFlags flags = 0;
  if (access & VKR_BUFFER_ACCESS_VERTEX) {
    flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_INDEX) {
    flags |= VK_ACCESS_INDEX_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_UNIFORM) {
    flags |= VK_ACCESS_UNIFORM_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_STORAGE_READ) {
    flags |= VK_ACCESS_SHADER_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_STORAGE_WRITE) {
    flags |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_SRC) {
    flags |= VK_ACCESS_TRANSFER_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_DST) {
    flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  return flags;
}

vkr_internal VkPipelineStageFlags
vulkan_buffer_stage_for_access(VkrBufferAccessFlags access, bool8_t is_src) {
  if (access == VKR_BUFFER_ACCESS_NONE) {
    return is_src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  }

  VkPipelineStageFlags flags = 0;
  if (access & (VKR_BUFFER_ACCESS_VERTEX | VKR_BUFFER_ACCESS_INDEX)) {
    flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
  }
  if (access & (VKR_BUFFER_ACCESS_UNIFORM | VKR_BUFFER_ACCESS_STORAGE_READ |
                VKR_BUFFER_ACCESS_STORAGE_WRITE)) {
    flags |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  if (access & (VKR_BUFFER_ACCESS_TRANSFER_SRC |
                VKR_BUFFER_ACCESS_TRANSFER_DST)) {
    flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
  }

  if (flags == 0) {
    flags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  }

  return flags;
}

VkrRendererError renderer_vulkan_buffer_barrier(
    void *backend_state, VkrBackendResourceHandle handle,
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Buffer handle is NULL");

  if (src_access == dst_access) {
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  if (!buffer || buffer->buffer.handle == VK_NULL_HANDLE) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkBufferMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = vulkan_buffer_access_to_vk(src_access),
      .dstAccessMask = vulkan_buffer_access_to_vk(dst_access),
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = buffer->buffer.handle,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
  };

  VkPipelineStageFlags src_stage =
      vulkan_buffer_stage_for_access(src_access, true_v);
  VkPipelineStageFlags dst_stage =
      vulkan_buffer_stage_for_access(dst_access, false_v);

  if (state->frame_active) {
    if (state->render_pass_active) {
      log_error("Cannot apply buffer barrier during active render pass");
      return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    }
    if (state->image_index >= state->graphics_command_buffers.length) {
      return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    }
    VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
        &state->graphics_command_buffers, state->image_index);
    vkCmdPipelineBarrier(command_buffer->handle, src_stage, dst_stage, 0, 0,
                         NULL, 1, &barrier, 0, NULL);
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
  }

  vkCmdPipelineBarrier(temp_command_buffer.handle, src_stage, dst_stage, 0, 0,
                       NULL, 1, &barrier, 0, NULL);

  if (!vulkan_command_buffer_end_single_use(state, &temp_command_buffer,
                                            state->device.graphics_queue,
                                            VK_NULL_HANDLE)) {
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_upload_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data) {
  // log_debug("Uploading Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;

  // Create a host-visible staging buffer to upload to. Mark it as the source
  // of the transfer.
  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription staging_buffer_desc = {
      .size = size,
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };
  struct s_BufferHandle *staging_buffer = vkr_allocator_alloc(
      &state->buffer_pool_alloc, sizeof(struct s_BufferHandle),
      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!staging_buffer) {
    log_fatal("Failed to allocate staging buffer (pool exhausted)");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  MemZero(staging_buffer, sizeof(struct s_BufferHandle));

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    vkr_allocator_free(&state->buffer_pool_alloc, staging_buffer,
                       sizeof(struct s_BufferHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    log_fatal("Failed to create staging buffer");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, size, 0,
                               data)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_free(&state->buffer_pool_alloc, staging_buffer,
                       sizeof(struct s_BufferHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    log_fatal("Failed to load data into staging buffer");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_buffer_copy_to(state, &staging_buffer->buffer,
                             staging_buffer->buffer.handle, 0,
                             buffer->buffer.handle, offset, size)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_free(&state->buffer_pool_alloc, staging_buffer,
                       sizeof(struct s_BufferHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    log_fatal("Failed to copy Vulkan buffer");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  vkr_allocator_free(&state->buffer_pool_alloc, staging_buffer,
                     sizeof(struct s_BufferHandle),
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_set_instance_buffer(void *backend_state,
                                         VkrBackendResourceHandle handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return;
  }
  state->instance_buffer = (struct s_BufferHandle *)handle.ptr;
}

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    VkrBackendResourceHandle handle) {
  // log_debug("Destroying Vulkan buffer");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)handle.ptr;
  vulkan_buffer_destroy(state, &buffer->buffer);

  // Return handle struct to pool
  vkr_allocator_free(&state->buffer_pool_alloc, buffer,
                     sizeof(struct s_BufferHandle),
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  return;
}

VkrBackendResourceHandle renderer_vulkan_create_render_target_texture(
    void *backend_state, const VkrRenderTargetTextureDesc *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Render target texture desc is NULL");

  if (desc->width == 0 || desc->height == 0) {
    log_error("Render target texture dimensions must be greater than zero");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (vulkan_texture_format_is_depth(desc->format)) {
    log_error("Render target texture format must be a color format");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkFormat image_format = vulkan_image_format_from_texture_format(desc->format);

  VkrTextureUsageFlags usage_flags = desc->usage;
  if (bitset8_get_value(&usage_flags) == 0) {
    usage_flags = vkr_texture_usage_flags_from_bits(
        VKR_TEXTURE_USAGE_COLOR_ATTACHMENT | VKR_TEXTURE_USAGE_SAMPLED);
  }
  if (bitset8_is_set(&usage_flags,
                     VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)) {
    log_error("Render target texture usage includes depth/stencil attachment");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VkImageUsageFlags usage = vulkan_image_usage_from_texture_usage(usage_flags);
  if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
    log_warn("Render target texture missing COLOR_ATTACHMENT usage; adding it");
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate render target texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, desc->width, desc->height,
                           image_format, VK_IMAGE_TILING_OPTIMAL, usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
                           VK_IMAGE_ASPECT_COLOR_BIT, &texture->texture.image)) {
    log_fatal("Failed to create render target image");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  texture->texture.sampler = VK_NULL_HANDLE;
  if (bitset8_is_set(&usage_flags, VKR_TEXTURE_USAGE_SAMPLED)) {
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    if (vkCreateSampler(state->device.logical_device, &sampler_info,
                        state->allocator,
                        &texture->texture.sampler) != VK_SUCCESS) {
      log_fatal("Failed to create render target sampler");
      vulkan_image_destroy(state, &texture->texture.image);
      return (VkrBackendResourceHandle){.ptr = NULL};
    }
  }

  texture->description = (VkrTextureDescription){
      .width = desc->width,
      .height = desc->height,
      .channels = vulkan_texture_format_channel_count(desc->format),
      .type = VKR_TEXTURE_TYPE_2D,
      .format = desc->format,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = 1,
  };

  // Only set transparency for non-integer color formats
  if (texture->description.channels == 4 &&
      desc->format != VKR_TEXTURE_FORMAT_R8G8B8A8_UINT &&
      desc->format != VKR_TEXTURE_FORMAT_R8G8B8A8_SINT) {
    bitset8_set(&texture->description.properties,
                VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
  }

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrBackendResourceHandle
renderer_vulkan_create_depth_attachment(void *backend_state, uint32_t width,
                                        uint32_t height) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  if (width == 0 || height == 0) {
    log_error("Depth attachment dimensions must be greater than zero");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkFormat depth_format = state->device.depth_format;
  VkrTextureFormat vkr_format = vulkan_vk_format_to_vkr(depth_format);
  if (!vulkan_texture_format_is_depth(vkr_format)) {
    log_error("Unsupported depth format for depth attachment");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate depth attachment texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, width, height, depth_format,
          VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
          &texture->texture.image)) {
    log_fatal("Failed to create depth attachment image");
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  texture->texture.sampler = VK_NULL_HANDLE;
  texture->description = (VkrTextureDescription){
      .width = width,
      .height = height,
      .channels = 1,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = vkr_format,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = 1,
  };

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrBackendResourceHandle renderer_vulkan_create_sampled_depth_attachment(
    void *backend_state, uint32_t width, uint32_t height) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  if (width == 0 || height == 0) {
    log_error("Sampled depth attachment dimensions must be greater than zero");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkFormat depth_format = vulkan_shadow_depth_vk_format_get(state);
  if (depth_format == VK_FORMAT_UNDEFINED) {
    log_error("No valid depth format available for sampled depth attachment");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }
  VkrTextureFormat vkr_format = vulkan_vk_format_to_vkr(depth_format);
  if (!vulkan_texture_format_is_depth(vkr_format)) {
    log_error("Unsupported depth format for sampled depth attachment");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal(
        "Failed to allocate sampled depth attachment texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, width, height, depth_format,
                           VK_IMAGE_TILING_OPTIMAL, usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
                           VK_IMAGE_ASPECT_DEPTH_BIT, &texture->texture.image)) {
    log_fatal("Failed to create sampled depth attachment image");
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VkFilter shadow_filter = VK_FILTER_NEAREST;
  VkSamplerMipmapMode shadow_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  vulkan_select_shadow_sampler_filter_modes(state, depth_format, &shadow_filter,
                                            &shadow_mip);

  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      // Use comparison sampling for shadow maps. When the depth format supports
      // linear filtering, enable it to get hardware PCF-like smoothing.
      .magFilter = shadow_filter,
      .minFilter = shadow_filter,
      .mipmapMode = shadow_mip,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_FALSE,
      .maxAnisotropy = 1.0f,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create sampled depth attachment sampler");
    vulkan_image_destroy(state, &texture->texture.image);
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  texture->description = (VkrTextureDescription){
      .width = width,
      .height = height,
      .channels = 1,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = vkr_format,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .min_filter = (shadow_filter == VK_FILTER_LINEAR) ? VKR_FILTER_LINEAR
                                                        : VKR_FILTER_NEAREST,
      .mag_filter = (shadow_filter == VK_FILTER_LINEAR) ? VKR_FILTER_LINEAR
                                                        : VKR_FILTER_NEAREST,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = 1,
  };

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrBackendResourceHandle renderer_vulkan_create_sampled_depth_attachment_array(
    void *backend_state, uint32_t width, uint32_t height, uint32_t layers) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  if (width == 0 || height == 0 || layers == 0) {
    log_error("Sampled depth attachment array dimensions must be > 0");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkFormat depth_format = vulkan_shadow_depth_vk_format_get(state);
  if (depth_format == VK_FORMAT_UNDEFINED) {
    log_error(
        "No valid depth format available for sampled depth attachment array");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }
  VkrTextureFormat vkr_format = vulkan_vk_format_to_vkr(depth_format);
  if (!vulkan_texture_format_is_depth(vkr_format)) {
    log_error("Unsupported depth format for sampled depth attachment array");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate sampled depth attachment array texture");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, width, height, depth_format,
          VK_IMAGE_TILING_OPTIMAL, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1,
          layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
          VK_IMAGE_ASPECT_DEPTH_BIT, &texture->texture.image)) {
    log_fatal("Failed to create sampled depth attachment array image");
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VkFilter shadow_filter = VK_FILTER_NEAREST;
  VkSamplerMipmapMode shadow_mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  vulkan_select_shadow_sampler_filter_modes(state, depth_format, &shadow_filter,
                                            &shadow_mip);

  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = shadow_filter,
      .minFilter = shadow_filter,
      .mipmapMode = shadow_mip,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_FALSE,
      .maxAnisotropy = 1.0f,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create sampled depth attachment array sampler");
    vulkan_image_destroy(state, &texture->texture.image);
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  texture->description = (VkrTextureDescription){
      .width = width,
      .height = height,
      .channels = 1,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = vkr_format,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER,
      .min_filter = (shadow_filter == VK_FILTER_LINEAR) ? VKR_FILTER_LINEAR
                                                        : VKR_FILTER_NEAREST,
      .mag_filter = (shadow_filter == VK_FILTER_LINEAR) ? VKR_FILTER_LINEAR
                                                        : VKR_FILTER_NEAREST,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = 1,
  };

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrBackendResourceHandle renderer_vulkan_create_render_target_texture_msaa(
    void *backend_state, uint32_t width, uint32_t height, VkrTextureFormat format,
    VkrSampleCount samples) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  if (width == 0 || height == 0) {
    log_error("MSAA texture dimensions must be greater than zero");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (vulkan_texture_format_is_depth(format)) {
    log_error("MSAA color texture format must be a color format");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (samples < VKR_SAMPLE_COUNT_2) {
    log_warn("MSAA texture created with sample count < 2; use regular render "
             "target for 1x");
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VkFormat image_format = vulkan_image_format_from_texture_format(format);
  VkSampleCountFlagBits vk_samples = (VkSampleCountFlagBits)samples;

  // MSAA textures are only used as color attachments and transfer source
  // (for resolve). They cannot be directly sampled.
  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate MSAA texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, width, height, image_format,
                           VK_IMAGE_TILING_OPTIMAL, usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1, vk_samples,
                           VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                           &texture->texture.image)) {
    log_fatal("Failed to create MSAA image");
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // No sampler for MSAA textures (cannot be directly sampled)
  texture->texture.sampler = VK_NULL_HANDLE;

  texture->description = (VkrTextureDescription){
      .width = width,
      .height = height,
      .channels = vulkan_texture_format_channel_count(format),
      .type = VKR_TEXTURE_TYPE_2D,
      .format = format,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_NEAREST,
      .mag_filter = VKR_FILTER_NEAREST,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .sample_count = samples,
      .generation = 1,
  };

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};
}

VkrRendererError renderer_vulkan_transition_texture_layout(
    void *backend_state, VkrBackendResourceHandle handle,
    VkrTextureLayout old_layout, VkrTextureLayout new_layout) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");

  if (old_layout == new_layout) {
    return VKR_RENDERER_ERROR_NONE;
  }
  if (new_layout == VKR_TEXTURE_LAYOUT_UNDEFINED) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  VkImageLayout vk_old = vulkan_texture_layout_to_vk(old_layout);
  VkImageLayout vk_new = vulkan_texture_layout_to_vk(new_layout);
  if (vk_old == VK_IMAGE_LAYOUT_UNDEFINED &&
      vk_new == VK_IMAGE_LAYOUT_UNDEFINED) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  if (vulkan_texture_format_is_depth(texture->description.format)) {
    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (texture->description.format == VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT) {
      aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
  }

  VkImageSubresourceRange range = {
      .aspectMask = aspect,
      .baseMipLevel = 0,
      .levelCount = texture->texture.image.mip_levels,
      .baseArrayLayer = 0,
      .layerCount = texture->texture.image.array_layers,
  };

  VkFormat image_format =
      vulkan_image_format_from_texture_format(texture->description.format);
  if (state->frame_active) {
    if (state->render_pass_active) {
      log_error("Cannot transition texture layout during active render pass");
      return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    }
    if (state->image_index >= state->graphics_command_buffers.length) {
      return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
    }
    VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
        &state->graphics_command_buffers, state->image_index);
    if (!vulkan_image_transition_layout_range(state, &texture->texture.image,
                                              command_buffer, image_format,
                                              vk_old, vk_new, &range)) {
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_image_transition_layout_range(state, &texture->texture.image,
                                            &temp_command_buffer, image_format,
                                            vk_old, vk_new, &range)) {
    vulkan_command_buffer_free(state, &temp_command_buffer);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkrBackendResourceHandle renderer_vulkan_create_cube_texture(
    VulkanBackendState *state, const VkrTextureDescription *desc,
    const void *initial_data);

VkrBackendResourceHandle renderer_vulkan_create_texture_with_payload(
    void *backend_state, const VkrTextureDescription *desc,
    const VkrTextureUploadPayload *payload) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  assert_log(payload != NULL, "Payload is NULL");

  if (!payload->data || payload->data_size == 0 || payload->region_count == 0 ||
      !payload->regions || payload->mip_levels == 0 ||
      payload->array_layers == 0) {
    log_error("Invalid texture upload payload");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }
  if (desc->width == 0 || desc->height == 0) {
    log_error("Payload texture dimensions must be greater than zero");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (desc->type != VKR_TEXTURE_TYPE_2D) {
    log_error("Payload texture creation currently supports only 2D textures");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  const bool8_t format_is_compressed =
      vulkan_texture_format_is_compressed(desc->format);
  if (payload->is_compressed != format_is_compressed) {
    log_error("Payload compression flag must match texture format");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  const uint64_t expected_region_count =
      (uint64_t)payload->mip_levels * (uint64_t)payload->array_layers;
  if (payload->region_count != expected_region_count) {
    log_error("Payload must provide exactly one full region per mip/layer "
              "subresource (expected=%llu, provided=%u)",
              (unsigned long long)expected_region_count, payload->region_count);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  const uint32_t max_mip_levels =
      vulkan_calculate_mip_levels(desc->width, desc->height);
  if (payload->mip_levels > max_mip_levels) {
    log_error("Payload mip level count exceeds valid chain length "
              "(requested=%u, max=%u)",
              payload->mip_levels, max_mip_levels);
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  if (!format_is_compressed && (desc->channels == 0 || desc->channels > 4)) {
    log_error("Uncompressed payload upload requires channel count in [1,4]");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }
  MemZero(texture, sizeof(struct s_TextureHandle));
  texture->description = *desc;

  struct s_BufferHandle *staging_buffer = NULL;
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    goto cleanup_texture;
  }

  const uint64_t subresource_count = expected_region_count;
  uint8_t *subresource_seen = vkr_allocator_alloc(
      &state->temp_scope, subresource_count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkBufferImageCopy *copy_regions = vkr_allocator_alloc(
      &state->temp_scope, sizeof(VkBufferImageCopy) * payload->region_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!subresource_seen || !copy_regions) {
    log_error("Failed to allocate payload upload metadata");
    goto cleanup_texture;
  }
  MemZero(subresource_seen, subresource_count);

  for (uint32_t region_index = 0; region_index < payload->region_count;
       ++region_index) {
    const VkrTextureUploadRegion *region = &payload->regions[region_index];

    if (region->mip_level >= payload->mip_levels ||
        region->array_layer >= payload->array_layers) {
      log_error("Payload region index is out of bounds (mip=%u layer=%u)",
                region->mip_level, region->array_layer);
      goto cleanup_texture;
    }

    if (region->depth != 1) {
      log_error("Payload regions for 2D textures must use depth=1");
      goto cleanup_texture;
    }

    const uint32_t mip_width =
        vulkan_texture_mip_extent(desc->width, region->mip_level);
    const uint32_t mip_height =
        vulkan_texture_mip_extent(desc->height, region->mip_level);
    if (region->width != mip_width || region->height != mip_height) {
      log_error("Payload region extent must match full mip dimensions "
                "(mip=%u expected=%ux%u got=%ux%u)",
                region->mip_level, mip_width, mip_height, region->width,
                region->height);
      goto cleanup_texture;
    }

    if (region->byte_offset >= payload->data_size ||
        region->byte_offset + region->byte_size > payload->data_size ||
        region->byte_offset + region->byte_size < region->byte_offset) {
      log_error("Payload byte range is out of bounds");
      goto cleanup_texture;
    }

    const uint64_t expected_size = vulkan_texture_expected_region_size_bytes(
        desc->format, desc->channels, mip_width, mip_height);
    if (expected_size == 0 || region->byte_size != expected_size) {
      log_error("Payload region byte size mismatch for mip=%u layer=%u "
                "(expected=%llu got=%llu)",
                region->mip_level, region->array_layer,
                (unsigned long long)expected_size,
                (unsigned long long)region->byte_size);
      goto cleanup_texture;
    }

    const uint64_t subresource_index =
        (uint64_t)region->array_layer * (uint64_t)payload->mip_levels +
        (uint64_t)region->mip_level;
    if (subresource_seen[subresource_index]) {
      log_error("Payload contains duplicate region for mip=%u layer=%u",
                region->mip_level, region->array_layer);
      goto cleanup_texture;
    }
    subresource_seen[subresource_index] = 1;

    copy_regions[region_index] = (VkBufferImageCopy){
        .bufferOffset = region->byte_offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = region->mip_level,
                .baseArrayLayer = region->array_layer,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {region->width, region->height, region->depth},
    };
  }

  for (uint64_t i = 0; i < subresource_count; ++i) {
    if (!subresource_seen[i]) {
      const uint32_t missing_layer = (uint32_t)(i / payload->mip_levels);
      const uint32_t missing_mip = (uint32_t)(i % payload->mip_levels);
      log_error("Payload missing upload region for mip=%u layer=%u",
                missing_mip, missing_layer);
      goto cleanup_texture;
    }
  }

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription staging_buffer_desc = {
      .size = payload->data_size,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };

  staging_buffer = vkr_allocator_alloc(&state->temp_scope,
                                       sizeof(struct s_BufferHandle),
                                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!staging_buffer) {
    log_fatal("Failed to allocate staging buffer");
    goto cleanup_texture;
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    log_fatal("Failed to create staging buffer");
    goto cleanup_texture;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0,
                               payload->data_size, 0, payload->data)) {
    log_fatal("Failed to upload payload bytes to staging buffer");
    goto cleanup_texture;
  }

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!format_is_compressed) {
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  const VkFormat image_format =
      vulkan_image_format_from_texture_format(desc->format);
  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, desc->width, desc->height,
                           image_format, VK_IMAGE_TILING_OPTIMAL, usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           payload->mip_levels, payload->array_layers,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
                           VK_IMAGE_ASPECT_COLOR_BIT, &texture->texture.image)) {
    log_fatal("Failed to create Vulkan image for payload upload");
    goto cleanup_texture;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    log_fatal("Failed to allocate command buffer for payload upload");
    goto cleanup_texture;
  }

  const VkImageSubresourceRange full_range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = payload->mip_levels,
      .baseArrayLayer = 0,
      .layerCount = payload->array_layers,
  };

  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          &full_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    log_fatal("Failed to transition payload image to TRANSFER_DST");
    goto cleanup_texture;
  }

  vkCmdCopyBufferToImage(
      temp_command_buffer.handle, staging_buffer->buffer.handle,
      texture->texture.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      payload->region_count, copy_regions);

  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &full_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    log_fatal("Failed to transition payload image to SHADER_READ_ONLY");
    goto cleanup_texture;
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    log_fatal("Failed to submit payload upload commands");
    goto cleanup_texture;
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod =
      (texture->texture.image.mip_levels > 0)
          ? (float32_t)(texture->texture.image.mip_levels - 1)
          : 0.0f;
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create texture sampler");
    goto cleanup_texture;
  }

  if (staging_buffer && staging_buffer->buffer.handle != VK_NULL_HANDLE) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};

cleanup_texture:
  if (staging_buffer && staging_buffer->buffer.handle != VK_NULL_HANDLE) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
  }
  if (texture) {
    if (texture->texture.sampler != VK_NULL_HANDLE) {
      vkDestroySampler(state->device.logical_device, texture->texture.sampler,
                       state->allocator);
      texture->texture.sampler = VK_NULL_HANDLE;
    }
    if (texture->texture.image.handle != VK_NULL_HANDLE) {
      vulkan_image_destroy(state, &texture->texture.image);
    }
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  }
  if (vkr_allocator_scope_is_valid(&scope)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  return (VkrBackendResourceHandle){.ptr = NULL};
}

VkrBackendResourceHandle
renderer_vulkan_create_texture(void *backend_state,
                               const VkrTextureDescription *desc,
                               const void *initial_data) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  bool32_t writable =
      bitset8_is_set(&desc->properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
  assert_log(initial_data != NULL || writable,
             "Initial data is NULL and texture is not writable");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Branch to cube map creation if type is cube map
  if (desc->type == VKR_TEXTURE_TYPE_CUBE_MAP) {
    return renderer_vulkan_create_cube_texture(state, desc, initial_data);
  }

  if (vulkan_texture_format_is_compressed(desc->format)) {
    log_error("Compressed textures require payload upload path");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  // log_debug("Creating Vulkan texture");

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));

  texture->description = *desc;

  VkDeviceSize image_size = (VkDeviceSize)desc->width *
                            (VkDeviceSize)desc->height *
                            (VkDeviceSize)desc->channels;

  VkFormat image_format = vulkan_image_format_from_texture_format(desc->format);
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                      image_format, &format_props);
  bool32_t linear_blit_supported =
      (format_props.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  uint32_t mip_levels =
      linear_blit_supported
          ? vulkan_calculate_mip_levels(desc->width, desc->height)
          : 1;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  struct s_BufferHandle *staging_buffer = NULL;

  VkrAllocatorScope scope = {0};
  if (initial_data) {
    const VkrBufferDescription staging_buffer_desc = {
        .size = image_size,
        .usage =
            vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_HOST_VISIBLE |
            VKR_MEMORY_PROPERTY_HOST_COHERENT),
        .buffer_type = buffer_type,
        .bind_on_create = true_v,
    };

    scope = vkr_allocator_begin_scope(&state->temp_scope);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      goto cleanup_texture;
    }
    staging_buffer =
        vkr_allocator_alloc(&state->temp_scope, sizeof(struct s_BufferHandle),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!staging_buffer) {
      log_fatal("Failed to allocate staging buffer");
      goto cleanup_texture;
    }

    if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
      log_fatal("Failed to create staging buffer");
      goto cleanup_texture;
    }

    if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, image_size,
                                 0, initial_data)) {
      log_fatal("Failed to load data into staging buffer");
      goto cleanup_texture;
    }
  }

  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, desc->width, desc->height, image_format,
          VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels, 1,
          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
          VK_IMAGE_ASPECT_COLOR_BIT, &texture->texture.image)) {
    log_fatal("Failed to create Vulkan image");
    goto cleanup_texture;
  }

  if (initial_data) {
    // Use two-phase upload: transfer queue for base level, graphics for mipmaps
    bool8_t generate_mipmaps =
        (texture->texture.image.mip_levels > 1) && linear_blit_supported;

    if (!vulkan_image_upload_with_mipmaps(state, &texture->texture.image,
                                          staging_buffer->buffer.handle,
                                          image_format, generate_mipmaps)) {
      log_fatal("Failed to upload texture via transfer queue");
      goto cleanup_texture;
    }
  } else {
    // Writable texture - just transition layout on graphics queue
    VulkanCommandBuffer temp_command_buffer = {0};
    if (!vulkan_command_buffer_allocate_and_begin_single_use(
            state, &temp_command_buffer)) {
      log_fatal("Failed to allocate command buffer for writable texture");
      goto cleanup_texture;
    }

    if (!vulkan_image_transition_layout(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      log_fatal("Failed to transition writable image layout");
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      goto cleanup_texture;
    }

    if (!vulkan_command_buffer_end_single_use(
            state, &temp_command_buffer, state->device.graphics_queue,
            array_get_VulkanFence(&state->in_flight_fences,
                                  state->current_frame)
                ->handle)) {
      log_fatal("Failed to end single use command buffer");
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      goto cleanup_texture;
    }

    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
  }

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(texture->texture.image.mip_levels - 1);
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  // Create sampler
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create texture sampler");
    goto cleanup_texture;
  }

  if (staging_buffer)
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
  if (vkr_allocator_scope_is_valid(&scope))
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};

cleanup_texture:
  // Clean up resources on error path
  if (texture) {
    if (texture->texture.image.handle != VK_NULL_HANDLE) {
      vulkan_image_destroy(state, &texture->texture.image);
    }
    if (staging_buffer && staging_buffer->buffer.handle != VK_NULL_HANDLE) {
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
    }
    if (vkr_allocator_scope_is_valid(&scope))
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  }
  return (VkrBackendResourceHandle){.ptr = NULL};
}

vkr_internal VkrBackendResourceHandle renderer_vulkan_create_cube_texture(
    VulkanBackendState *state, const VkrTextureDescription *desc,
    const void *initial_data) {
  assert_log(state != NULL, "State is NULL");
  assert_log(desc != NULL, "Texture description is NULL");
  assert_log(initial_data != NULL,
             "Cube map requires initial data for all 6 faces");

  // log_debug("Creating Vulkan cube map texture");

  struct s_TextureHandle *texture = vkr_allocator_alloc(
      &state->texture_pool_alloc, sizeof(struct s_TextureHandle),
      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!texture) {
    log_fatal("Failed to allocate cube texture (pool exhausted)");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(texture, sizeof(struct s_TextureHandle));
  texture->description = *desc;

  // Each face has the same dimensions
  VkDeviceSize face_size = (VkDeviceSize)desc->width *
                           (VkDeviceSize)desc->height *
                           (VkDeviceSize)desc->channels;
  VkDeviceSize total_size = face_size * 6;

  VkFormat image_format = vulkan_image_format_from_texture_format(desc->format);

  // Cube maps typically don't use mipmaps initially for simplicity
  uint32_t mip_levels = 1;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  const VkrBufferDescription staging_buffer_desc = {
      .size = total_size,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    goto cleanup_texture;
  }
  struct s_BufferHandle *staging_buffer =
      vkr_allocator_alloc(&state->temp_scope, sizeof(struct s_BufferHandle),
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!staging_buffer) {
    log_fatal("Failed to allocate staging buffer");
    goto cleanup_texture;
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    log_fatal("Failed to create staging buffer for cube map");
    goto cleanup_texture;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, total_size, 0,
                               initial_data)) {
    log_fatal("Failed to load cube map data into staging buffer");
    goto cleanup_texture;
  }

  // Create cube map image with 6 array layers
  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, desc->width, desc->height,
                           image_format, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels, 6,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_CUBE,
                           VK_IMAGE_ASPECT_COLOR_BIT, &texture->texture.image)) {
    log_fatal("Failed to create Vulkan cube map image");
    goto cleanup_texture;
  }

  // Upload cube map faces via transfer queue
  if (!vulkan_image_upload_cube_via_transfer(state, &texture->texture.image,
                                             staging_buffer->buffer.handle,
                                             image_format, face_size)) {
    log_fatal("Failed to upload cube map via transfer queue");
    goto cleanup_texture;
  }

  // Create sampler for cube map (clamp to edge is typical for skyboxes)
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .mipLodBias = 0.0f,
      .anisotropyEnable = VK_FALSE,
      .maxAnisotropy = 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator,
                      &texture->texture.sampler) != VK_SUCCESS) {
    log_fatal("Failed to create cube map sampler");
    goto cleanup_texture;
  }

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  // log_debug("Created Vulkan cube map texture: %p",
  //           texture->texture.image.handle);

  ASSIGN_TEXTURE_GENERATION(state, texture);
  return (VkrBackendResourceHandle){.ptr = texture};

cleanup_texture:
  // Clean up resources on error path
  if (texture) {
    if (texture->texture.image.handle != VK_NULL_HANDLE) {
      vulkan_image_destroy(state, &texture->texture.image);
    }
    if (staging_buffer && staging_buffer->buffer.handle != VK_NULL_HANDLE) {
      vulkan_buffer_destroy(state, &staging_buffer->buffer);
    }
    if (vkr_allocator_scope_is_valid(&scope))
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(&state->texture_pool_alloc, texture,
                       sizeof(struct s_TextureHandle),
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  }
  return (VkrBackendResourceHandle){.ptr = NULL};
}

VkrRendererError
renderer_vulkan_update_texture(void *backend_state,
                               VkrBackendResourceHandle handle,
                               const VkrTextureDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");
  assert_log(desc != NULL, "Texture description is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  if (desc->width != texture->description.width ||
      desc->height != texture->description.height ||
      desc->channels != texture->description.channels ||
      desc->format != texture->description.format) {
    log_error("Texture update rejected: description dimensions or format "
              "differ from existing texture");
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(texture->texture.image.mip_levels - 1);
  vulkan_select_filter_modes(desc, state->device.features.samplerAnisotropy,
                             texture->texture.image.mip_levels, &min_filter,
                             &mag_filter, &mipmap_mode, &anisotropy_enable,
                             &max_lod);

  // Create new sampler for texture update
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU =
          vulkan_sampler_address_mode_from_repeat(desc->u_repeat_mode),
      .addressModeV =
          vulkan_sampler_address_mode_from_repeat(desc->v_repeat_mode),
      .addressModeW =
          vulkan_sampler_address_mode_from_repeat(desc->w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  VkSampler new_sampler;
  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator, &new_sampler) != VK_SUCCESS) {
    log_error("Failed to create sampler for texture update");
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  // Ensure no in-flight use of the old sampler before switching
  vkQueueWaitIdle(state->device.graphics_queue);

  // Destroy old sampler and use new one
  vkDestroySampler(state->device.logical_device, texture->texture.sampler,
                   state->allocator);
  texture->texture.sampler = new_sampler;

  texture->description.u_repeat_mode = desc->u_repeat_mode;
  texture->description.v_repeat_mode = desc->v_repeat_mode;
  texture->description.w_repeat_mode = desc->w_repeat_mode;
  texture->description.min_filter = desc->min_filter;
  texture->description.mag_filter = desc->mag_filter;
  texture->description.mip_filter = desc->mip_filter;
  texture->description.anisotropy_enable = desc->anisotropy_enable;
  texture->description.generation++;

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_write_texture(
    void *backend_state, VkrBackendResourceHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");
  assert_log(data != NULL, "Texture data is NULL");
  assert_log(size > 0, "Texture data size must be greater than zero");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  VkrRendererError compressed_error = vulkan_texture_reject_compressed_mutation(
      texture->description.format, "texture_write");
  if (compressed_error != VKR_RENDERER_ERROR_NONE) {
    return compressed_error;
  }

  uint32_t mip_level = region ? region->mip_level : 0;
  uint32_t array_layer = region ? region->array_layer : 0;
  uint32_t x = region ? region->x : 0;
  uint32_t y = region ? region->y : 0;
  uint32_t width =
      region ? region->width : (uint32_t)texture->texture.image.width;
  uint32_t height =
      region ? region->height : (uint32_t)texture->texture.image.height;

  if (width == 0 || height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (mip_level >= texture->texture.image.mip_levels ||
      array_layer >= texture->texture.image.array_layers) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t mip_width = Max(1u, texture->texture.image.width >> mip_level);
  uint32_t mip_height = Max(1u, texture->texture.image.height >> mip_level);

  if (x + width > mip_width || y + height > mip_height) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)width * (uint64_t)height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);
  const VkrBufferDescription staging_buffer_desc = {
      .size = size,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v,
  };

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  struct s_BufferHandle *staging_buffer =
      vkr_allocator_alloc(&state->temp_scope, sizeof(struct s_BufferHandle),
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!staging_buffer) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  if (!vulkan_buffer_create(state, &staging_buffer_desc, staging_buffer)) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  if (!vulkan_buffer_load_data(state, &staging_buffer->buffer, 0, size, 0,
                               data)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkImageSubresourceRange subresource_range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = mip_level,
      .levelCount = 1,
      .baseArrayLayer = array_layer,
      .layerCount = 1,
  };

  VkFormat image_format =
      vulkan_image_format_from_texture_format(texture->description.format);
  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &subresource_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkBufferImageCopy copy_region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip_level,
                           .baseArrayLayer = array_layer,
                           .layerCount = 1},
      .imageOffset = {(int32_t)x, (int32_t)y, 0},
      .imageExtent = {width, height, 1}};

  vkCmdCopyBufferToImage(temp_command_buffer.handle,
                         staging_buffer->buffer.handle,
                         texture->texture.image.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

  if (!vulkan_image_transition_layout_range(
          state, &texture->texture.image, &temp_command_buffer, image_format,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &subresource_range)) {
    vkEndCommandBuffer(temp_command_buffer.handle);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1,
                         &temp_command_buffer.handle);
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    vulkan_buffer_destroy(state, &staging_buffer->buffer);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  vulkan_buffer_destroy(state, &staging_buffer->buffer);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  texture->description.generation++;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_resize_texture(void *backend_state,
                                                VkrBackendResourceHandle handle,
                                                uint32_t new_width,
                                                uint32_t new_height,
                                                bool8_t preserve_contents) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Texture handle is NULL");

  if (new_width == 0 || new_height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  VkrRendererError compressed_error = vulkan_texture_reject_compressed_mutation(
      texture->description.format, "texture_resize");
  if (compressed_error != VKR_RENDERER_ERROR_NONE) {
    return compressed_error;
  }

  VkFormat image_format =
      vulkan_image_format_from_texture_format(texture->description.format);
  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                      image_format, &format_props);
  bool32_t linear_blit_supported =
      (format_props.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  uint32_t max_mip_levels =
      linear_blit_supported ? vulkan_calculate_mip_levels(new_width, new_height)
                            : 1;
  uint32_t mip_levels =
      (texture->description.mip_filter == VKR_MIP_FILTER_NONE)
          ? 1
          : Min(texture->texture.image.mip_levels, max_mip_levels);

  VulkanImage new_image = {0};
  if (!vulkan_image_create(
          state, VK_IMAGE_TYPE_2D, new_width, new_height, image_format,
          VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mip_levels,
          texture->texture.image.array_layers, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &new_image)) {
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  VulkanCommandBuffer temp_command_buffer = {0};
  if (!vulkan_command_buffer_allocate_and_begin_single_use(
          state, &temp_command_buffer)) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  VkImageSubresourceRange new_range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = new_image.mip_levels,
                                       .baseArrayLayer = 0,
                                       .layerCount = new_image.array_layers};

  if (preserve_contents) {
    VkImageSubresourceRange old_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = texture->texture.image.mip_levels,
        .baseArrayLayer = 0,
        .layerCount = texture->texture.image.array_layers,
    };

    if (!vulkan_image_transition_layout_range(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &old_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    uint32_t copy_width = Min(texture->texture.image.width, new_width);
    uint32_t copy_height = Min(texture->texture.image.height, new_height);

    if (linear_blit_supported) {
      VkImageBlit blit = {
          .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = texture->texture.image.array_layers},
          .srcOffsets = {{0, 0, 0},
                         {(int32_t)texture->texture.image.width,
                          (int32_t)texture->texture.image.height, 1}},
          .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = new_image.array_layers},
          .dstOffsets = {{0, 0, 0},
                         {(int32_t)new_width, (int32_t)new_height, 1}}};

      vkCmdBlitImage(temp_command_buffer.handle, texture->texture.image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
    } else {
      VkImageCopy copy_region = {
          .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = texture->texture.image.array_layers},
          .srcOffset = {0, 0, 0},
          .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = new_image.array_layers},
          .dstOffset = {0, 0, 0},
          .extent = {copy_width, copy_height, 1},
      };

      vkCmdCopyImage(temp_command_buffer.handle, texture->texture.image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_image.handle,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
    }

    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }

    if (!vulkan_image_transition_layout_range(
            state, &texture->texture.image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &old_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
  } else {
    if (!vulkan_image_transition_layout_range(
            state, &new_image, &temp_command_buffer, image_format,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            &new_range)) {
      vkEndCommandBuffer(temp_command_buffer.handle);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &temp_command_buffer.handle);
      vulkan_image_destroy(state, &new_image);
      return VKR_RENDERER_ERROR_DEVICE_ERROR;
    }
  }

  if (!vulkan_command_buffer_end_single_use(
          state, &temp_command_buffer, state->device.graphics_queue,
          array_get_VulkanFence(&state->in_flight_fences, state->current_frame)
              ->handle)) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }

  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1,
                       &temp_command_buffer.handle);

  VkFilter min_filter = VK_FILTER_LINEAR;
  VkFilter mag_filter = VK_FILTER_LINEAR;
  VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  VkBool32 anisotropy_enable = VK_FALSE;
  float32_t max_lod = (float32_t)(new_image.mip_levels - 1);
  vulkan_select_filter_modes(&texture->description,
                             state->device.features.samplerAnisotropy,
                             new_image.mip_levels, &min_filter, &mag_filter,
                             &mipmap_mode, &anisotropy_enable, &max_lod);

  // Create new sampler for resized texture
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = mag_filter,
      .minFilter = min_filter,
      .mipmapMode = mipmap_mode,
      .addressModeU = vulkan_sampler_address_mode_from_repeat(
          texture->description.u_repeat_mode),
      .addressModeV = vulkan_sampler_address_mode_from_repeat(
          texture->description.v_repeat_mode),
      .addressModeW = vulkan_sampler_address_mode_from_repeat(
          texture->description.w_repeat_mode),
      .mipLodBias = 0.0f,
      .anisotropyEnable = anisotropy_enable,
      .maxAnisotropy =
          anisotropy_enable
              ? state->device.properties.limits.maxSamplerAnisotropy
              : 1.0f,
      .compareEnable = VK_FALSE,
      .compareOp = VK_COMPARE_OP_ALWAYS,
      .minLod = 0.0f,
      .maxLod = max_lod,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
  };

  VkSampler new_sampler;
  if (vkCreateSampler(state->device.logical_device, &sampler_info,
                      state->allocator, &new_sampler) != VK_SUCCESS) {
    vulkan_image_destroy(state, &new_image);
    return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }

  // Ensure previous operations complete before swapping resources
  vkQueueWaitIdle(state->device.graphics_queue);

  VulkanImage old_image = texture->texture.image;
  VkSampler old_sampler = texture->texture.sampler;

  texture->texture.image = new_image;
  texture->texture.sampler = new_sampler;

  // Destroy old sampler
  vkDestroySampler(state->device.logical_device, old_sampler, state->allocator);

  vulkan_image_destroy(state, &old_image);

  texture->description.width = new_width;
  texture->description.height = new_height;
  texture->description.generation++;

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_texture(void *backend_state,
                                     VkrBackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  // log_debug("Destroying Vulkan texture");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_TextureHandle *texture = (struct s_TextureHandle *)handle.ptr;

  // Ensure the texture is not in use before destroying
  if (renderer_vulkan_wait_idle(backend_state) != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to wait for idle before destroying texture");
  }

  vulkan_image_destroy(state, &texture->texture.image);

  // Destroy the sampler
  vkDestroySampler(state->device.logical_device, texture->texture.sampler,
                   state->allocator);
  texture->texture.sampler = VK_NULL_HANDLE;

  // Return handle struct to pool
  vkr_allocator_free(&state->texture_pool_alloc, texture,
                     sizeof(struct s_TextureHandle),
                     VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  return;
}

VkrBackendResourceHandle renderer_vulkan_create_graphics_pipeline(
    void *backend_state, const VkrGraphicsPipelineDescription *desc) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Pipeline description is NULL");

  // log_debug("Creating Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline =
      vkr_allocator_alloc(&state->alloc, sizeof(struct s_GraphicsPipeline),
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pipeline) {
    log_fatal("Failed to allocate pipeline");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  MemZero(pipeline, sizeof(struct s_GraphicsPipeline));

  if (!vulkan_graphics_graphics_pipeline_create(state, desc, pipeline)) {
    log_fatal("Failed to create Vulkan pipeline layout");
    return (VkrBackendResourceHandle){.ptr = NULL};
  }

  return (VkrBackendResourceHandle){.ptr = pipeline};
}

bool8_t renderer_vulkan_pipeline_get_shader_runtime_layout(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrShaderRuntimeLayout *out_layout) {
  if (!backend_state || !pipeline_handle.ptr || !out_layout) {
    return false_v;
  }

  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;
  *out_layout = (VkrShaderRuntimeLayout){
      .global_ubo_size = pipeline->shader_object.global_ubo_size,
      .global_ubo_stride = pipeline->shader_object.global_ubo_stride,
      .instance_ubo_size = pipeline->shader_object.instance_ubo_size,
      .instance_ubo_stride = pipeline->shader_object.instance_ubo_stride,
      .push_constant_size = pipeline->shader_object.push_constant_size,
      .global_texture_count = pipeline->shader_object.global_texture_count,
      .instance_texture_count = pipeline->shader_object.instance_texture_count,
  };
  return true_v;
}

VkrRendererError renderer_vulkan_update_pipeline_state(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  // log_debug("Updating Vulkan pipeline state");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  return vulkan_graphics_pipeline_update_state(state, pipeline, uniform, data,
                                               material);
}

VkrRendererError renderer_vulkan_instance_state_acquire(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrRendererInstanceStateHandle *out_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  uint32_t object_id = 0;
  if (!vulkan_shader_acquire_instance(state, &pipeline->shader_object,
                                      &object_id)) {
    return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
  }

  out_handle->id = object_id;
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
renderer_vulkan_instance_state_release(void *backend_state,
                                       VkrBackendResourceHandle pipeline_handle,
                                       VkrRendererInstanceStateHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  if (handle.id == VKR_INVALID_ID) {
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  if (!vulkan_shader_release_instance(state, &pipeline->shader_object,
                                      handle.id)) {
    return VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      VkrBackendResourceHandle handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(handle.ptr != NULL, "Handle is NULL");

  // log_debug("Destroying Vulkan pipeline");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline = (struct s_GraphicsPipeline *)handle.ptr;

  vulkan_graphics_pipeline_destroy(state, pipeline);

  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   VkrBackendResourceHandle pipeline_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(pipeline_handle.ptr != NULL, "Pipeline handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  struct s_GraphicsPipeline *pipeline =
      (struct s_GraphicsPipeline *)pipeline_handle.ptr;

  // todo: add support for multiple command buffers
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdBindPipeline(command_buffer->handle, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->pipeline);

  return;
}

void renderer_vulkan_bind_buffer(void *backend_state,
                                 VkrBackendResourceHandle buffer_handle,
                                 uint64_t offset) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(buffer_handle.ptr != NULL, "Buffer handle is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_BufferHandle *buffer = (struct s_BufferHandle *)buffer_handle.ptr;

  // log_debug("Binding Vulkan buffer with usage flags");

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  // log_debug("Current command buffer handle: %p", command_buffer->handle);

  if (bitset8_is_set(&buffer->description.usage,
                     VKR_BUFFER_USAGE_VERTEX_BUFFER)) {
    vulkan_buffer_bind_vertex_buffer(state, command_buffer, 0,
                                     buffer->buffer.handle, offset);
  } else if (bitset8_is_set(&buffer->description.usage,
                            VKR_BUFFER_USAGE_INDEX_BUFFER)) {
    // Default to uint32 index type - could be improved by storing in buffer
    // description
    vulkan_buffer_bind_index_buffer(
        state, command_buffer, buffer->buffer.handle, offset,
        VK_INDEX_TYPE_UINT32); // todo: append index type to buffer
                               // description
  } else {
    log_warn("Buffer has unknown usage flags for pipeline binding");
  }

  return;
}

void renderer_vulkan_set_viewport(void *backend_state,
                                  const VkrViewport *viewport) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(viewport != NULL, "Viewport is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state->frame_active) {
    log_warn("set_viewport called outside active frame");
    return;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  VkViewport vk_viewport = {
      .x = viewport->x,
      .y = viewport->y,
      .width = viewport->width,
      .height = viewport->height,
      .minDepth = viewport->min_depth,
      .maxDepth = viewport->max_depth,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &vk_viewport);
}

void renderer_vulkan_set_scissor(void *backend_state,
                                 const VkrScissor *scissor) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(scissor != NULL, "Scissor is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  VkRect2D vk_scissor = {
      .offset = {scissor->x, scissor->y},
      .extent = {scissor->width, scissor->height},
  };

  vkCmdSetScissor(command_buffer->handle, 0, 1, &vk_scissor);
}

void renderer_vulkan_set_depth_bias(void *backend_state,
                                    float32_t constant_factor, float32_t clamp,
                                    float32_t slope_factor) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state->frame_active) {
    log_warn("set_depth_bias called outside active frame");
    return;
  }

  if (!state->device.features.depthBiasClamp) {
    clamp = 0.0f;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdSetDepthBias(command_buffer->handle, constant_factor, clamp,
                    slope_factor);
}

VkrRenderPassHandle
renderer_vulkan_renderpass_create_desc(void *backend_state,
                                       const VkrRenderPassDesc *desc,
                                       VkrRendererError *out_error) {
  if (!backend_state || !desc) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Check if render pass with this name already exists
  if (desc->name.length > 0) {
    struct s_RenderPass *existing =
        vulkan_backend_renderpass_lookup(state, desc->name);
    if (existing) {
      log_warn("Render pass '%.*s' already exists, returning existing",
               (int)desc->name.length, desc->name.str);
      return (VkrRenderPassHandle)existing;
    }
  }

  struct s_RenderPass *created =
      vulkan_backend_renderpass_create_from_desc_internal(state, desc);
  if (!created) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return NULL;
  }

  // Auto-assign to domain if domain is valid
  VkrPipelineDomain domain = desc->domain;
  if (domain < VKR_PIPELINE_DOMAIN_COUNT &&
      !state->domain_render_passes[domain]) {
    state->domain_render_passes[domain] = created->vk;
    state->domain_initialized[domain] = true;
    log_debug("Auto-assigned render pass to domain %d", domain);
    if (domain == VKR_PIPELINE_DOMAIN_WORLD) {
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] =
          created->vk;
      state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] = true;
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] =
          created->vk;
      state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] = true;
    } else if (domain == VKR_PIPELINE_DOMAIN_PICKING) {
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT] =
          created->vk;
      state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT] =
          true;
      state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING_OVERLAY] =
          created->vk;
      state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING_OVERLAY] = true;
    }
  }

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }
  return (VkrRenderPassHandle)created;
}

void renderer_vulkan_renderpass_destroy(void *backend_state,
                                        VkrRenderPassHandle pass_handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !pass_handle) {
    return;
  }

  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  VulkanRenderPass *pass_vk = pass->vk;
  VkRenderPass handle = pass_vk ? pass_vk->handle : VK_NULL_HANDLE;
  if (pass_vk) {
    pass_vk->handle = VK_NULL_HANDLE;
  }
  if (handle != VK_NULL_HANDLE) {
    if (!vulkan_deferred_destroy_enqueue(
            state, VKR_DEFERRED_DESTROY_RENDERPASS, handle, VK_NULL_HANDLE,
            NULL, 0)) {
      vkDestroyRenderPass(state->device.logical_device, handle,
                          state->allocator);
    }
  }
  if (state->active_named_render_pass == pass) {
    state->active_named_render_pass = NULL;
  }

  for (uint32_t i = 0; i < state->render_pass_count; ++i) {
    VkrRenderPassEntry *entry =
        array_get_VkrRenderPassEntry(&state->render_pass_registry, i);
    if (entry->pass == pass) {
      entry->pass = NULL;
      entry->name = (String8){0};
      break;
    }
  }

  for (uint32_t i = 0; i < VKR_PIPELINE_DOMAIN_COUNT; ++i) {
    if (state->domain_render_passes[i] == pass_vk) {
      state->domain_render_passes[i] = NULL;
      state->domain_initialized[i] = false;
    }
  }

  pass->vk = NULL;
  pass->name = (String8){0};
  pass->attachment_count = 0;
  pass->resolve_attachment_count = 0;
  pass->ends_in_present = false_v;
}

VkrRenderPassHandle renderer_vulkan_renderpass_get(void *backend_state,
                                                   const char *name) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !name) {
    return NULL;
  }

  uint64_t len = strlen(name);
  if (len == 0) {
    return NULL;
  }
  String8 lookup = string8_create_from_cstr((const uint8_t *)name, len);
  struct s_RenderPass *found = vulkan_backend_renderpass_lookup(state, lookup);
  return (VkrRenderPassHandle)found;
}

bool8_t renderer_vulkan_renderpass_get_signature(void *backend_state,
                                                  VkrRenderPassHandle pass_handle,
                                                  VkrRenderPassSignature *out_signature) {
  (void)backend_state; // Unused, but kept for interface consistency
  if (!pass_handle || !out_signature) {
    return false_v;
  }

  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  if (!pass->vk || pass->vk->handle == VK_NULL_HANDLE) {
    return false_v;
  }

  *out_signature = pass->vk->signature;
  return true_v;
}

bool8_t renderer_vulkan_domain_renderpass_set(void *backend_state,
                                               VkrPipelineDomain domain,
                                               VkrRenderPassHandle pass_handle,
                                               VkrDomainOverridePolicy policy,
                                               VkrRendererError *out_error) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || domain >= VKR_PIPELINE_DOMAIN_COUNT) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    }
    return false_v;
  }

  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  if (!pass || !pass->vk) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    }
    return false_v;
  }

  VulkanRenderPass *current = state->domain_render_passes[domain];

  // Check signature compatibility if policy requires it
  if (policy == VKR_DOMAIN_OVERRIDE_POLICY_REQUIRE_COMPATIBLE && current) {
    if (!vkr_renderpass_signature_compatible(&current->signature,
                                             &pass->vk->signature)) {
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE;
      }
      return false_v;
    }
  }

  // Invalidate framebuffer cache since we're changing the render pass
  // (framebuffers are tied to specific VkRenderPass handles)
  framebuffer_cache_invalidate(state);

  // Update the domain render pass
  state->domain_render_passes[domain] = pass->vk;
  state->domain_initialized[domain] = true;

  // Handle aliased domains - if setting WORLD, also update aliases
  if (domain == VKR_PIPELINE_DOMAIN_WORLD) {
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] =
        pass->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT] = true;
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] = pass->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_WORLD_OVERLAY] = true;
  }

  // Handle picking aliases
  if (domain == VKR_PIPELINE_DOMAIN_PICKING) {
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT] =
        pass->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT] = true;
    state->domain_render_passes[VKR_PIPELINE_DOMAIN_PICKING_OVERLAY] = pass->vk;
    state->domain_initialized[VKR_PIPELINE_DOMAIN_PICKING_OVERLAY] = true;
  }

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }
  return true_v;
}

/**
 * @brief Create a subresource image view for specific mip level and array layer range.
 */
vkr_internal VkImageView
vulkan_create_subresource_view(VulkanBackendState *state,
                               struct s_TextureHandle *tex, uint32_t mip_level,
                               uint32_t base_layer, uint32_t layer_count) {
  VulkanImage *image = &tex->texture.image;
  VkImageAspectFlags aspect =
      vulkan_aspect_flags_from_texture_format(tex->description.format);

  VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
  if (layer_count > 1) {
    if (tex->description.type == VKR_TEXTURE_TYPE_CUBE_MAP &&
        layer_count == 6 && (base_layer % 6) == 0) {
      view_type = VK_IMAGE_VIEW_TYPE_CUBE;
    } else {
      view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
  }

  VkImageViewCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image->handle,
      .viewType = view_type,
      .format = vulkan_image_format_from_texture_format(tex->description.format),
      .components =
          {
              .r = VK_COMPONENT_SWIZZLE_IDENTITY,
              .g = VK_COMPONENT_SWIZZLE_IDENTITY,
              .b = VK_COMPONENT_SWIZZLE_IDENTITY,
              .a = VK_COMPONENT_SWIZZLE_IDENTITY,
          },
      .subresourceRange =
          {
              .aspectMask = aspect,
              .baseMipLevel = mip_level,
              .levelCount = 1, // Single mip level for render target
              .baseArrayLayer = base_layer,
              .layerCount = layer_count,
          },
  };

  VkImageView view;
  if (vkCreateImageView(state->device.logical_device, &create_info,
                        state->allocator, &view) != VK_SUCCESS) {
    log_error("Failed to create subresource image view");
    return VK_NULL_HANDLE;
  }
  return view;
}

VkrRenderTargetHandle
renderer_vulkan_render_target_create(void *backend_state,
                                      const VkrRenderTargetDesc *desc,
                                      VkrRenderPassHandle pass_handle,
                                      VkrRendererError *out_error) {
  if (!backend_state || !desc || !pass_handle) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;

  if (!pass->vk || pass->vk->handle == VK_NULL_HANDLE ||
      desc->attachment_count == 0 || !desc->attachments) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  uint8_t color_count = pass->vk->signature.color_attachment_count;
  uint8_t depth_count = pass->vk->signature.has_depth_stencil ? 1u : 0u;
  uint8_t resolve_count = pass->resolve_attachment_count;
  uint8_t expected_count = color_count + depth_count + resolve_count;

  if (desc->attachment_count != expected_count) {
    log_error("Render target attachment count %u does not match render pass "
              "signature (%u)",
              desc->attachment_count, expected_count);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  if (desc->attachment_count > VKR_RENDER_TARGET_MAX_ATTACHMENTS) {
    log_error("Render target attachment count %u exceeds max %u",
              desc->attachment_count, VKR_RENDER_TARGET_MAX_ATTACHMENTS);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    return NULL;
  }

  struct s_RenderTarget *target = vkr_allocator_alloc(
      &state->render_target_alloc, sizeof(struct s_RenderTarget),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!target) {
    log_fatal("Failed to allocate render target from pool");
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }
  MemZero(target, sizeof(struct s_RenderTarget));

  target->attachment_count = desc->attachment_count;
  target->sync_to_window_size = desc->sync_to_window_size;
  target->width =
      desc->sync_to_window_size ? state->swapchain.extent.width : desc->width;
  target->height =
      desc->sync_to_window_size ? state->swapchain.extent.height : desc->height;
  if (target->width == 0 || target->height == 0) {
    vkr_allocator_free(&state->render_target_alloc, target,
                       sizeof(struct s_RenderTarget),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    }
    log_error("Render target dimensions must be greater than zero");
    return NULL;
  }

  uint32_t expected_layer_count = 0;

  // Temporary allocator scope for views array
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    vkr_allocator_free(&state->render_target_alloc, target,
                       sizeof(struct s_RenderTarget),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  VkImageView *views = (VkImageView *)vkr_allocator_alloc(
      &state->temp_scope,
      sizeof(VkImageView) * (uint64_t)target->attachment_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!views) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(&state->render_target_alloc, target,
                       sizeof(struct s_RenderTarget),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  // Track which views we created (need to destroy on error/cleanup)
  VkImageView created_views[VKR_RENDER_TARGET_MAX_ATTACHMENTS] = {
      VK_NULL_HANDLE};
  uint32_t created_view_count = 0;

  for (uint32_t i = 0; i < target->attachment_count; ++i) {
    const VkrRenderTargetAttachmentRef *ref = &desc->attachments[i];
    struct s_TextureHandle *tex = (struct s_TextureHandle *)ref->texture;

    if (!tex) {
      log_error("Render target attachment %u is NULL", i);
      goto cleanup_error;
    }
    if (ref->layer_count == 0) {
      log_error("Render target attachment %u has invalid layer count", i);
      goto cleanup_error;
    }
    if (ref->mip_level >= tex->texture.image.mip_levels) {
      log_error("Render target attachment %u mip level %u exceeds max %u", i,
                ref->mip_level, tex->texture.image.mip_levels);
      goto cleanup_error;
    }
    if (ref->base_layer + ref->layer_count > tex->texture.image.array_layers) {
      log_error("Render target attachment %u layer range out of bounds", i);
      goto cleanup_error;
    }

    target->attachments[i] = tex;
#ifndef NDEBUG
    target->attachment_generations[i] = tex->generation;
#endif

    if (expected_layer_count == 0) {
      expected_layer_count = ref->layer_count;
    } else if (ref->layer_count != expected_layer_count) {
      log_error("Render target attachment %u layer count %u does not match "
                "expected %u",
                i, ref->layer_count, expected_layer_count);
      goto cleanup_error;
    }

    uint32_t mip_width =
        Max(1u, tex->texture.image.width >> ref->mip_level);
    uint32_t mip_height =
        Max(1u, tex->texture.image.height >> ref->mip_level);
    if (target->width > mip_width || target->height > mip_height) {
      log_error("Render target attachment %u size %ux%u exceeds mip %ux%u", i,
                target->width, target->height, mip_width, mip_height);
      goto cleanup_error;
    }

    VkrTextureFormat expected_format = tex->description.format;
    VkrSampleCount expected_samples = VKR_SAMPLE_COUNT_1;
    if (i < color_count) {
      expected_format = pass->vk->signature.color_formats[i];
      expected_samples = pass->vk->signature.color_samples[i];
    } else if (pass->vk->signature.has_depth_stencil &&
               i == color_count) {
      expected_format = pass->vk->signature.depth_stencil_format;
      expected_samples = pass->vk->signature.depth_stencil_samples;
    } else {
      uint8_t resolve_index = (uint8_t)(i - color_count - depth_count);
      VkrResolveAttachmentRef *resolve_ref = NULL;
      for (uint8_t r = 0; r < pass->resolve_attachment_count; ++r) {
        if (pass->resolve_attachments[r].dst_attachment_index == resolve_index) {
          resolve_ref = &pass->resolve_attachments[r];
          break;
        }
      }
      if (!resolve_ref ||
          resolve_ref->src_attachment_index >= color_count) {
        log_error("Render target resolve attachment %u has invalid source", i);
        goto cleanup_error;
      }
      expected_format =
          pass->vk->signature.color_formats[resolve_ref->src_attachment_index];
      expected_samples = VKR_SAMPLE_COUNT_1;
    }

    if (tex->description.format != expected_format) {
      log_error("Render target attachment %u format mismatch", i);
      goto cleanup_error;
    }
    VkrSampleCount texture_samples =
        vulkan_vk_samples_to_vkr(tex->texture.image.samples);
    if (texture_samples != expected_samples) {
      log_error("Render target attachment %u sample count mismatch", i);
      goto cleanup_error;
    }

    // Determine if we need a subresource view
    bool8_t needs_subresource =
        ref->mip_level != 0 || ref->base_layer != 0 ||
        (ref->layer_count != 1 &&
         ref->layer_count != tex->texture.image.array_layers);

    if (needs_subresource) {
      VkImageView subview = vulkan_create_subresource_view(
          state, tex, ref->mip_level, ref->base_layer, ref->layer_count);
      if (subview == VK_NULL_HANDLE) {
        goto cleanup_error;
      }
      views[i] = subview;
      created_views[created_view_count++] = subview;
      target->attachment_view_owned[i] = true_v;
    } else {
      // Use texture's default view
      if (tex->texture.image.view == VK_NULL_HANDLE) {
        log_error("Render target attachment %u has no image view", i);
        goto cleanup_error;
      }
      views[i] = tex->texture.image.view;
      target->attachment_view_owned[i] = false_v;
    }
    target->attachment_views[i] = views[i];
  }

  target->layer_count = expected_layer_count > 0 ? expected_layer_count : 1u;

  // Create framebuffer (render target owns the framebuffer)
  VkFramebufferCreateInfo fb_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = pass->vk->handle,
      .attachmentCount = target->attachment_count,
      .pAttachments = views,
      .width = target->width,
      .height = target->height,
      .layers = target->layer_count,
  };

  if (vkCreateFramebuffer(state->device.logical_device, &fb_info,
                          state->allocator, &target->handle) != VK_SUCCESS) {
    log_fatal("Failed to create framebuffer for render target");
    goto cleanup_error;
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }
  return (VkrRenderTargetHandle)target;

cleanup_error:
  for (uint32_t i = 0; i < created_view_count; ++i) {
    vkDestroyImageView(state->device.logical_device, created_views[i],
                       state->allocator);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(&state->render_target_alloc, target,
                     sizeof(struct s_RenderTarget),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
  }
  return NULL;
}

void renderer_vulkan_render_target_destroy(
    void *backend_state, VkrRenderTargetHandle target_handle) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !target_handle) {
    return;
  }

  struct s_RenderTarget *target = (struct s_RenderTarget *)target_handle;

  VkFramebuffer framebuffer = target->handle;
  target->handle = VK_NULL_HANDLE;

  for (uint32_t i = 0; i < target->attachment_count; ++i) {
    if (target->attachment_view_owned[i] &&
        target->attachment_views[i] != VK_NULL_HANDLE) {
      if (!vulkan_deferred_destroy_enqueue(
              state, VKR_DEFERRED_DESTROY_IMAGE_VIEW,
              target->attachment_views[i], VK_NULL_HANDLE, NULL, 0)) {
        vkDestroyImageView(state->device.logical_device,
                           target->attachment_views[i], state->allocator);
      }
    }
    target->attachment_views[i] = VK_NULL_HANDLE;
    target->attachment_view_owned[i] = false_v;
  }

  if (framebuffer != VK_NULL_HANDLE) {
    if (!vulkan_deferred_destroy_enqueue(
            state, VKR_DEFERRED_DESTROY_FRAMEBUFFER, framebuffer,
            VK_NULL_HANDLE, NULL, 0)) {
      vkDestroyFramebuffer(state->device.logical_device, framebuffer,
                           state->allocator);
    }
  }

  if (!vulkan_deferred_destroy_enqueue(
          state, VKR_DEFERRED_DESTROY_RENDER_TARGET_WRAPPER, target,
          VK_NULL_HANDLE, &state->render_target_alloc,
          sizeof(struct s_RenderTarget))) {
    vkr_allocator_free(&state->render_target_alloc, target,
                       sizeof(struct s_RenderTarget),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
}

VkrRendererError
renderer_vulkan_begin_render_pass(void *backend_state,
                                  VkrRenderPassHandle pass_handle,
                                  VkrRenderTargetHandle target_handle) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  struct s_RenderPass *pass = (struct s_RenderPass *)pass_handle;
  struct s_RenderTarget *target = (struct s_RenderTarget *)target_handle;

  if (!pass || !target || !pass->vk || target->handle == VK_NULL_HANDLE) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

#ifndef NDEBUG
  // Debug: validate attachment liveness - detect use-after-free
  for (uint32_t i = 0; i < target->attachment_count; ++i) {
    struct s_TextureHandle *attachment = target->attachments[i];
    if (attachment && attachment->generation != target->attachment_generations[i]) {
      log_error("Render target attachment %u has stale texture reference "
                "(captured gen %u, current gen %u). "
                "Texture was likely destroyed and recreated.",
                i, target->attachment_generations[i], attachment->generation);
      assert_log(false_v, "Stale texture attachment detected");
      return VKR_RENDERER_ERROR_INVALID_HANDLE;
    }
  }
#endif

  if (pass->attachment_count != target->attachment_count) {
    log_error("Render pass attachment count %u does not match target (%u)",
              pass->attachment_count, target->attachment_count);
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  VkRect2D render_area = {
      .offset = {0, 0},
      .extent = {target->width, target->height},
  };

  VkRenderPassBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass->vk->handle,
      .framebuffer = target->handle,
      .renderArea = render_area,
      .clearValueCount = pass->attachment_count,
      .pClearValues = pass->clear_values,
  };

  vkCmdBeginRenderPass(command_buffer->handle, &begin_info,
                       VK_SUBPASS_CONTENTS_INLINE);

  state->render_pass_active = true;
  state->current_render_pass_domain = pass->vk->domain;
  state->active_named_render_pass = pass;

  VkViewport viewport = {
      .x = (float32_t)render_area.offset.x,
      .y = (float32_t)render_area.offset.y,
      .width = (float32_t)render_area.extent.width,
      .height = (float32_t)render_area.extent.height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };

  vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
  vkCmdSetScissor(command_buffer->handle, 0, 1, &render_area);

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError renderer_vulkan_end_render_pass(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state->render_pass_active) {
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vkCmdEndRenderPass(command_buffer->handle);

  if (state->active_named_render_pass &&
      state->active_named_render_pass->ends_in_present) {
    state->swapchain_image_is_present_ready = true;
  }

  state->active_named_render_pass = NULL;
  state->render_pass_active = false;
  state->current_render_pass_domain = VKR_PIPELINE_DOMAIN_COUNT;
  return VKR_RENDERER_ERROR_NONE;
}

VkrTextureOpaqueHandle
renderer_vulkan_window_attachment_get(void *backend_state,
                                      uint32_t image_index) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state || !state->swapchain_image_textures ||
      image_index >= state->swapchain.image_count) {
    return NULL;
  }

  return (VkrTextureOpaqueHandle)state->swapchain_image_textures[image_index];
}

VkrTextureOpaqueHandle
renderer_vulkan_depth_attachment_get(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return NULL;
  }
  return (VkrTextureOpaqueHandle)state->depth_texture;
}

uint32_t renderer_vulkan_window_attachment_count(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return 0;
  }
  return state->swapchain.image_count;
}

VkrTextureFormat renderer_vulkan_swapchain_format_get(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB;
  }
  return vulkan_vk_format_to_vkr(state->swapchain.format);
}

VkrTextureFormat
renderer_vulkan_shadow_depth_format_get(void *backend_state) {
  return vulkan_shadow_depth_vkr_format_get(
      (const VulkanBackendState *)backend_state);
}

uint32_t renderer_vulkan_window_attachment_index(void *backend_state) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  if (!state) {
    return 0;
  }
  return state->image_index;
}

vkr_internal bool8_t vulkan_create_readback_buffer(VulkanBackendState *state,
                                                   uint64_t size,
                                                   VulkanBuffer *out_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_buffer != NULL, "Out buffer is NULL");

  MemZero(out_buffer, sizeof(VulkanBuffer));
  out_buffer->total_size = size;
  out_buffer->usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  if (vkCreateBuffer(state->device.logical_device, &buffer_info,
                     state->allocator, &out_buffer->handle) != VK_SUCCESS) {
    log_error("Failed to create readback buffer");
    return false_v;
  }

  VkMemoryRequirements memory_requirements;
  vkGetBufferMemoryRequirements(state->device.logical_device,
                                out_buffer->handle, &memory_requirements);
  out_buffer->allocation_size = memory_requirements.size;

  // Try HOST_VISIBLE + HOST_CACHED first, fall back to HOST_VISIBLE + COHERENT
  VkMemoryPropertyFlags desired_flags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  out_buffer->memory_index =
      find_memory_index(state->device.physical_device,
                        memory_requirements.memoryTypeBits, desired_flags);

  if (out_buffer->memory_index == -1) {
    // Fall back to HOST_VISIBLE + COHERENT
    desired_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    out_buffer->memory_index =
        find_memory_index(state->device.physical_device,
                          memory_requirements.memoryTypeBits, desired_flags);
  }

  if (out_buffer->memory_index == -1) {
    log_error("Failed to find suitable memory type for readback buffer");
    vkDestroyBuffer(state->device.logical_device, out_buffer->handle,
                    state->allocator);
    out_buffer->handle = VK_NULL_HANDLE;
    return false_v;
  }

  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(state->device.physical_device,
                                      &mem_props);
  out_buffer->memory_property_flags =
      mem_props.memoryTypes[out_buffer->memory_index].propertyFlags;

  VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = (uint32_t)out_buffer->memory_index,
  };

  if (vkAllocateMemory(state->device.logical_device, &alloc_info,
                       state->allocator, &out_buffer->memory) != VK_SUCCESS) {
    log_error("Failed to allocate memory for readback buffer");
    vkDestroyBuffer(state->device.logical_device, out_buffer->handle,
                    state->allocator);
    out_buffer->handle = VK_NULL_HANDLE;
    return false_v;
  }

  vkr_allocator_report(&state->alloc, out_buffer->allocation_size,
                       VKR_ALLOCATOR_MEMORY_TAG_VULKAN, true_v);

  if (vkBindBufferMemory(state->device.logical_device, out_buffer->handle,
                         out_buffer->memory, 0) != VK_SUCCESS) {
    log_error("Failed to bind readback buffer memory");
    vkFreeMemory(state->device.logical_device, out_buffer->memory,
                 state->allocator);
    vkDestroyBuffer(state->device.logical_device, out_buffer->handle,
                    state->allocator);
    out_buffer->handle = VK_NULL_HANDLE;
    out_buffer->memory = VK_NULL_HANDLE;
    return false_v;
  }

  return true_v;
}

vkr_internal void vulkan_destroy_readback_buffer(VulkanBackendState *state,
                                                 VulkanBuffer *buffer) {
  if (buffer->handle == VK_NULL_HANDLE) {
    return;
  }

  vkDestroyBuffer(state->device.logical_device, buffer->handle,
                  state->allocator);
  if (buffer->memory != VK_NULL_HANDLE) {
    if (buffer->allocation_size > 0) {
      vkr_allocator_report(&state->alloc, buffer->allocation_size,
                           VKR_ALLOCATOR_MEMORY_TAG_VULKAN, false_v);
    }
    vkFreeMemory(state->device.logical_device, buffer->memory,
                 state->allocator);
  }

  buffer->handle = VK_NULL_HANDLE;
  buffer->memory = VK_NULL_HANDLE;
}

VkrRendererError renderer_vulkan_readback_ring_init(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanReadbackRing *ring = &state->readback_ring;
  if (ring->initialized) {
    return VKR_RENDERER_ERROR_NONE;
  }

  // Initialize each readback slot with a small buffer for single pixel readback
  // Size: 8 bytes (supports up to R32G32_UINT which is 8 bytes per pixel)
  const uint64_t slot_buffer_size = 8;

  for (uint32_t i = 0; i < VKR_READBACK_RING_SIZE; i++) {
    VulkanReadbackSlot *slot = &ring->slots[i];

    if (!vulkan_create_readback_buffer(state, slot_buffer_size,
                                       &slot->buffer)) {
      log_error("Failed to create readback buffer for slot %u", i);
      for (uint32_t j = 0; j < i; j++) {
        vulkan_destroy_readback_buffer(state, &ring->slots[j].buffer);
        vulkan_fence_destroy(state, &ring->slots[j].fence);
      }
      return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }

    vulkan_fence_create(state, true_v, &slot->fence); // Start signaled
    if (slot->fence.handle == VK_NULL_HANDLE) {
      log_error("Failed to create fence for readback slot %u", i);
      vulkan_destroy_readback_buffer(state, &slot->buffer);
      for (uint32_t j = 0; j < i; j++) {
        vulkan_destroy_readback_buffer(state, &ring->slots[j].buffer);
        vulkan_fence_destroy(state, &ring->slots[j].fence);
      }
      return VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }

    slot->is_coherent = (slot->buffer.memory_property_flags &
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    slot->state = VULKAN_READBACK_SLOT_IDLE;
    slot->pixel_size = 4; // Default R32_UINT
  }

  ring->write_index = 0;
  ring->read_index = 0;
  ring->pending_count = 0;
  ring->initialized = true_v;

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_readback_ring_shutdown(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanReadbackRing *ring = &state->readback_ring;
  if (!ring->initialized) {
    return;
  }

  for (uint32_t i = 0; i < VKR_READBACK_RING_SIZE; i++) {
    VulkanReadbackSlot *slot = &ring->slots[i];
    if (slot->state == VULKAN_READBACK_SLOT_PENDING) {
      vulkan_fence_wait(state, UINT64_MAX, &slot->fence);
    }
    vulkan_destroy_readback_buffer(state, &slot->buffer);
    vulkan_fence_destroy(state, &slot->fence);
  }

  MemZero(ring, sizeof(VulkanReadbackRing));
}

VkrRendererError
renderer_vulkan_request_pixel_readback(void *backend_state,
                                       VkrBackendResourceHandle texture,
                                       uint32_t x, uint32_t y) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(texture.ptr != NULL, "Texture is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VulkanReadbackRing *ring = &state->readback_ring;

  if (!ring->initialized) {
    VkrRendererError err = renderer_vulkan_readback_ring_init(backend_state);
    if (err != VKR_RENDERER_ERROR_NONE) {
      return err;
    }
  }

  VulkanReadbackSlot *slot = &ring->slots[ring->write_index];

  // If slot is still pending from a previous request, wait for the frame's
  // in_flight fence to ensure the GPU has finished with the buffer
  if (slot->state == VULKAN_READBACK_SLOT_PENDING) {
    uint32_t fence_idx =
        slot->request_frame % state->swapchain.max_in_flight_frames;
    VulkanFence *fence =
        array_get_VulkanFence(&state->in_flight_fences, fence_idx);
    vulkan_fence_wait(state, UINT64_MAX, fence);
    slot->state = VULKAN_READBACK_SLOT_IDLE;
    ring->pending_count--;
  }

  struct s_TextureHandle *tex = (struct s_TextureHandle *)texture.ptr;

  if (x >= tex->texture.image.width || y >= tex->texture.image.height) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VulkanCommandBuffer *cmd = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  vulkan_image_copy_to_buffer(state, &tex->texture.image, slot->buffer.handle,
                              0, x, y, 1, 1, cmd);

  VkBufferMemoryBarrier buffer_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->buffer.handle,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
  };

  vkCmdPipelineBarrier(cmd->handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                       &buffer_barrier, 0, NULL);

  slot->requested_x = x;
  slot->requested_y = y;
  slot->width = 1;
  slot->height = 1;
  slot->pixel_size = 4; // R32_UINT
  slot->request_frame = state->current_frame;
  slot->request_submit_serial = state->submit_serial;
  slot->state = VULKAN_READBACK_SLOT_PENDING;

  ring->write_index = (ring->write_index + 1) % VKR_READBACK_RING_SIZE;
  ring->pending_count++;

  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
renderer_vulkan_get_pixel_readback_result(void *backend_state,
                                          VkrPixelReadbackResult *result) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  assert_log(result != NULL, "Result is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VulkanReadbackRing *ring = &state->readback_ring;

  result->status = VKR_READBACK_STATUS_IDLE;
  result->valid = false_v;
  result->data = 0;

  if (!ring->initialized || ring->pending_count == 0) {
    return VKR_RENDERER_ERROR_NONE;
  }

  VulkanReadbackSlot *slot = &ring->slots[ring->read_index];
  if (slot->state == VULKAN_READBACK_SLOT_IDLE) {
    for (uint32_t i = 0; i < VKR_READBACK_RING_SIZE; i++) {
      uint32_t idx = (ring->read_index + i) % VKR_READBACK_RING_SIZE;
      if (ring->slots[idx].state != VULKAN_READBACK_SLOT_IDLE) {
        slot = &ring->slots[idx];
        ring->read_index = idx;
        break;
      }
    }
  }

  if (slot->state == VULKAN_READBACK_SLOT_PENDING) {
    // Check if the frame that recorded the readback has been submitted.
    // IMPORTANT: current_frame wraps (0..max_in_flight_frames-1), so it can't
    // be used to determine submission ordering. Use a monotonic submit serial
    // instead.
    if (state->submit_serial > slot->request_submit_serial) {
      uint32_t fence_idx =
          slot->request_frame % state->swapchain.max_in_flight_frames;
      VulkanFence *fence =
          array_get_VulkanFence(&state->in_flight_fences, fence_idx);
      VkResult fence_result =
          vkGetFenceStatus(state->device.logical_device, fence->handle);
      if (fence_result == VK_SUCCESS) {
        slot->state = VULKAN_READBACK_SLOT_READY;
      } else if (fence_result == VK_NOT_READY) {
        result->status = VKR_READBACK_STATUS_PENDING;
        result->x = slot->requested_x;
        result->y = slot->requested_y;
        return VKR_RENDERER_ERROR_NONE;
      } else {
        result->status = VKR_READBACK_STATUS_ERROR;
        return VKR_RENDERER_ERROR_DEVICE_ERROR;
      }
    } else {
      // Frame not yet submitted
      result->status = VKR_READBACK_STATUS_PENDING;
      result->x = slot->requested_x;
      result->y = slot->requested_y;
      return VKR_RENDERER_ERROR_NONE;
    }
  }

  if (slot->state == VULKAN_READBACK_SLOT_READY) {
    void *mapped_data =
        vulkan_buffer_lock_memory(state, &slot->buffer, 0, slot->pixel_size, 0);
    if (!mapped_data) {
      result->status = VKR_READBACK_STATUS_ERROR;
      return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }

    if (!slot->is_coherent) {
      VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
          .memory = slot->buffer.memory,
          .offset = 0,
          .size = VK_WHOLE_SIZE,
      };
      vkInvalidateMappedMemoryRanges(state->device.logical_device, 1, &range);
    }

    MemCopy(&result->data, mapped_data, sizeof(uint32_t));

    vulkan_buffer_unlock_memory(state, &slot->buffer);

    result->status = VKR_READBACK_STATUS_READY;
    result->x = slot->requested_x;
    result->y = slot->requested_y;
    result->valid = true_v;

    slot->state = VULKAN_READBACK_SLOT_IDLE;
    ring->read_index = (ring->read_index + 1) % VKR_READBACK_RING_SIZE;
    ring->pending_count--;
  }

  return VKR_RENDERER_ERROR_NONE;
}

void renderer_vulkan_update_readback_ring(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  VulkanReadbackRing *ring = &state->readback_ring;
  if (!ring->initialized || ring->pending_count == 0) {
    return;
  }

  for (uint32_t i = 0; i < VKR_READBACK_RING_SIZE; i++) {
    VulkanReadbackSlot *slot = &ring->slots[i];
    if (slot->state == VULKAN_READBACK_SLOT_PENDING) {
      // The readback was recorded into the command buffer for request_frame.
      // That frame's fence is at index (request_frame % max_in_flight_frames).
      // We can check if the frame has been submitted and completed.
      if (state->submit_serial > slot->request_submit_serial) {
        // Frame has been submitted, check the in_flight fence
        uint32_t fence_idx =
            slot->request_frame % state->swapchain.max_in_flight_frames;
        VulkanFence *fence =
            array_get_VulkanFence(&state->in_flight_fences, fence_idx);
        VkResult fence_result =
            vkGetFenceStatus(state->device.logical_device, fence->handle);
        if (fence_result == VK_SUCCESS) {
          slot->state = VULKAN_READBACK_SLOT_READY;
          // pending_count is decremented when result is consumed (IDLE)
        }
      }
    }
  }
}

VkrAllocator *renderer_vulkan_get_allocator(void *backend_state) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  return &state->alloc;
}

void renderer_vulkan_set_default_2d_texture(void *backend_state,
                                            VkrTextureOpaqueHandle texture) {
  assert_log(backend_state != NULL, "Backend state is NULL");
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  state->default_2d_texture = (struct s_TextureHandle *)texture;
}

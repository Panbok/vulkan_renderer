#include "renderer/resources/loaders/texture_loader.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

vkr_internal const char *vkr_texture_loader_supported_extensions[] = {
    "png", "jpg", "jpeg", "bmp", "tga", "hdr", "vkt",
};

#define VKR_TEXTURE_LOADER_SUPPORTED_EXTENSION_COUNT                           \
  (sizeof(vkr_texture_loader_supported_extensions) /                           \
   sizeof(vkr_texture_loader_supported_extensions[0]))

/**
 * @brief Strip query parameters from a texture name for extension checks.
 */
vkr_internal String8 vkr_texture_loader_strip_query(String8 name) {
  for (uint64_t i = 0; i < name.length; ++i) {
    if (name.str[i] == '?') {
      return string8_substring(&name, 0, i);
    }
  }
  return name;
}

/**
 * @brief Returns the file extension (without dot) from a query-stripped path.
 */
vkr_internal String8 vkr_texture_loader_extract_extension(String8 base_name) {
  for (uint64_t ext_length = base_name.length; ext_length > 0; ext_length--) {
    if (base_name.str[ext_length - 1] == '.') {
      return string8_substring(&base_name, ext_length, base_name.length);
    }
  }
  return (String8){0};
}

/**
 * @brief Checks whether the extension is accepted by the texture loader.
 */
vkr_internal bool8_t
vkr_texture_loader_extension_is_supported(String8 extension) {
  if (!extension.str || extension.length == 0) {
    return false_v;
  }

  for (uint32_t i = 0; i < VKR_TEXTURE_LOADER_SUPPORTED_EXTENSION_COUNT; ++i) {
    const char *candidate_cstr = vkr_texture_loader_supported_extensions[i];
    String8 candidate = string8_create_from_cstr(
        (const uint8_t *)candidate_cstr, string_length(candidate_cstr));
    if (string8_equalsi(&extension, &candidate)) {
      return true_v;
    }
  }

  return false_v;
}

typedef struct VkrTextureLoaderAsyncPayload {
  VkrTexturePreparedLoad prepared;
} VkrTextureLoaderAsyncPayload;

vkr_internal bool8_t
vkr_texture_loader_async_allocator_is_ready(const VkrTextureSystem *system) {
  return system && system->async_allocator.ctx &&
         system->async_allocator.alloc && system->async_allocator.free;
}

vkr_internal bool8_t vkr_texture_loader_can_load(VkrResourceLoader *self,
                                                 String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  String8 base_name = vkr_texture_loader_strip_query(name);
  String8 extension = vkr_texture_loader_extract_extension(base_name);
  return vkr_texture_loader_extension_is_supported(extension);
}

vkr_internal bool8_t vkr_texture_loader_load(VkrResourceLoader *self,
                                             String8 name,
                                             VkrAllocator *temp_alloc,
                                             VkrResourceHandleInfo *out_handle,
                                             VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load(system, name, &handle, &renderer_error) ||
      renderer_error != VKR_RENDERER_ERROR_NONE) {
    *out_error = renderer_error;
    String8 error_string = vkr_renderer_get_error_string(renderer_error);
    log_error("Failed to load texture '%s': %s", string8_cstr(&name),
              string8_cstr(&error_string));
    return false_v;
  }

  /* One reference belongs to the loader result, including its PENDING_GPU
   * interval. Request deduplication shares this owner until the final unload. */
  vkr_texture_system_add_ref_by_handle(system, handle);
  out_handle->type = VKR_RESOURCE_TYPE_TEXTURE;
  out_handle->loader_id = self->id;
  out_handle->as.texture = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

vkr_internal bool8_t vkr_texture_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_payload != NULL, "Out payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_payload = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;
  VkrTextureLoaderAsyncPayload *payload =
      (VkrTextureLoaderAsyncPayload *)vkr_allocator_alloc_ts(
          &system->async_allocator, sizeof(*payload),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, system->async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));

  if (!vkr_texture_system_prepare_load_from_file(
          system, name, VKR_TEXTURE_RGBA_CHANNELS, temp_alloc,
          &payload->prepared, out_error)) {
    vkr_allocator_free_ts(&system->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, system->async_mutex);
    return false_v;
  }

  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_texture_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTextureLoaderAsyncPayload *async_payload =
      (VkrTextureLoaderAsyncPayload *)payload;
  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_texture_system_finalize_prepared_load(
          system, name, &async_payload->prepared, &handle, out_error)) {
    return false_v;
  }

  /* One reference belongs to the loader result, including its PENDING_GPU
   * interval. Request deduplication shares this owner until the final unload. */
  vkr_texture_system_add_ref_by_handle(system, handle);
  out_handle->type = VKR_RESOURCE_TYPE_TEXTURE;
  out_handle->loader_id = self->id;
  out_handle->as.texture = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_texture_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_cost != NULL, "Out cost is NULL");

  VkrTextureLoaderAsyncPayload *async_payload =
      (VkrTextureLoaderAsyncPayload *)payload;
  MemZero(out_cost, sizeof(*out_cost));
  if (async_payload->prepared.upload_data_size == 0) {
    return true_v;
  }

  out_cost->gpu_upload_bytes = async_payload->prepared.upload_data_size;
  out_cost->gpu_upload_ops = async_payload->prepared.upload_region_count > 0
                                 ? async_payload->prepared.upload_region_count
                                 : 1u;
  return true_v;
}

vkr_internal void
vkr_texture_loader_release_async_payload(VkrResourceLoader *self,
                                         void *payload) {
  assert_log(self != NULL, "Self is NULL");
  if (!payload) {
    return;
  }

  VkrTextureLoaderAsyncPayload *async_payload =
      (VkrTextureLoaderAsyncPayload *)payload;
  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;
  vkr_texture_system_release_prepared_load(&async_payload->prepared);
  vkr_allocator_free_ts(&system->async_allocator, async_payload,
                        sizeof(*async_payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        system->async_mutex);
}

vkr_internal void vkr_texture_loader_unload(VkrResourceLoader *self,
                                            const VkrResourceHandleInfo *handle,
                                            String8 name) {
  (void)name;
  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;
  (void)vkr_texture_system_release_by_handle(system, handle->as.texture);
}

VkrResourceLoader vkr_texture_loader_create(void) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_TEXTURE;
  loader.can_load = vkr_texture_loader_can_load;
  loader.load = vkr_texture_loader_load;
  loader.prepare_async = vkr_texture_loader_prepare_async;
  loader.finalize_async = vkr_texture_loader_finalize_async;
  loader.estimate_async_finalize_cost =
      vkr_texture_loader_estimate_async_finalize_cost;
  loader.release_async_payload = vkr_texture_loader_release_async_payload;
  loader.unload = vkr_texture_loader_unload;
  return loader;
}

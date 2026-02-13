#include "renderer/resources/loaders/texture_loader.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

#include <stdlib.h>

vkr_internal const char *vkr_texture_loader_supported_extensions[] = {
    "png", "jpg", "jpeg", "bmp", "tga", "vkt",
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
 * @brief Remove accidental `<type>|` request-key prefixes from texture paths.
 */
vkr_internal String8
vkr_texture_loader_strip_resource_key_prefix(String8 name) {
  if (!name.str || name.length < 3) {
    return name;
  }

  String8 stripped = name;
  for (uint32_t pass = 0; pass < 4; ++pass) {
    uint64_t pipe_index = UINT64_MAX;

    for (uint64_t segment_start = 0; segment_start < stripped.length;) {
      if (segment_start > 0) {
        uint8_t prev = stripped.str[segment_start - 1];
        if (prev != '/' && prev != '\\') {
          segment_start++;
          continue;
        }
      }

      uint64_t index = segment_start;
      while (index < stripped.length && stripped.str[index] >= '0' &&
             stripped.str[index] <= '9') {
        index++;
      }

      if (index > segment_start && index < stripped.length &&
          stripped.str[index] == '|') {
        pipe_index = index;
        break;
      }

      while (segment_start < stripped.length && stripped.str[segment_start] != '/' &&
             stripped.str[segment_start] != '\\') {
        segment_start++;
      }
      if (segment_start < stripped.length) {
        segment_start++;
      }
    }

    if (pipe_index == UINT64_MAX || pipe_index + 1 >= stripped.length) {
      break;
    }

    stripped = string8_substring(&stripped, pipe_index + 1, stripped.length);
  }

  return stripped;
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

  VkrTextureLoaderAsyncPayload *payload =
      (VkrTextureLoaderAsyncPayload *)malloc(sizeof(*payload));
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;
  if (!vkr_texture_system_prepare_load_from_file(
          system, name, VKR_TEXTURE_RGBA_CHANNELS, temp_alloc,
          &payload->prepared, out_error)) {
    free(payload);
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
  vkr_texture_system_release_prepared_load(&async_payload->prepared);
  free(async_payload);
}

vkr_internal void vkr_texture_loader_unload(VkrResourceLoader *self,
                                            const VkrResourceHandleInfo *handle,
                                            String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  name = vkr_texture_loader_strip_resource_key_prefix(name);
  String8 queryless_name = vkr_texture_loader_strip_query(name);

  char *primary_key = (char *)malloc((size_t)name.length + 1);
  if (!primary_key) {
    log_error("Failed to allocate texture unload key buffer");
    return;
  }
  MemCopy(primary_key, name.str, (size_t)name.length);
  primary_key[(size_t)name.length] = '\0';

  char *queryless_key = NULL;
  const char *remove_key = primary_key;
  VkrTextureEntry *entry = vkr_hash_table_get_VkrTextureEntry(
      &system->texture_map, primary_key);

  if (!entry && queryless_name.str && queryless_name.length > 0 &&
      queryless_name.length < name.length) {
    queryless_key = (char *)malloc((size_t)queryless_name.length + 1);
    if (queryless_key) {
      MemCopy(queryless_key, queryless_name.str, (size_t)queryless_name.length);
      queryless_key[(size_t)queryless_name.length] = '\0';
      entry = vkr_hash_table_get_VkrTextureEntry(&system->texture_map,
                                                 queryless_key);
      if (entry) {
        remove_key = queryless_key;
      }
    }
  }

  /*
   * Async request path canonicalization can diverge from texture-map keys when
   * legacy callers pass mixed aliases. Use the texture-system reverse lookup
   * (slot index -> stable key) to resolve by handle in O(1).
   */
  if (!entry && handle->type == VKR_RESOURCE_TYPE_TEXTURE &&
      handle->as.texture.id != 0) {
    uint32_t texture_index = handle->as.texture.id - 1;
    if (texture_index < system->textures.length &&
        system->texture_keys_by_index) {
      VkrTexture *mapped_texture = &system->textures.data[texture_index];
      const char *reverse_key = system->texture_keys_by_index[texture_index];
      if (reverse_key &&
          mapped_texture->description.id == handle->as.texture.id &&
          mapped_texture->description.generation ==
              handle->as.texture.generation) {
        entry = vkr_hash_table_get_VkrTextureEntry(&system->texture_map,
                                                   reverse_key);
        if (entry) {
          remove_key = reverse_key;
        }
      }
    }
  }

  if (!entry) {
    /*
     * Async dedup/cancel paths can legitimately race request teardown against
     * owner-driven release, so missing map entries are not always an error.
     */
    log_debug("Texture '%s' already released before loader unload",
              primary_key);
    free(primary_key);
    if (queryless_key) {
      free(queryless_key);
    }
    return;
  }

  /*
   * Async resource requests do not hold a texture-system refcount. If the
   * texture has been acquired by a material/mesh, keep it alive and let the
   * normal ref-counted release path destroy it when the last user releases.
   */
  if (entry->ref_count > 0) {
    free(primary_key);
    if (queryless_key) {
      free(queryless_key);
    }
    return;
  }

  uint32_t texture_index = entry->index;
  const char *stable_name = entry->name;

  // Don't remove default texture
  if (texture_index == system->default_texture.id - 1) {
    log_warn("Cannot remove default texture");
    free(primary_key);
    if (queryless_key) {
      free(queryless_key);
    }
    return;
  }

  // Destroy GPU resources
  VkrTexture *texture = &system->textures.data[texture_index];
  vkr_texture_destroy(self->renderer, texture);

  // Mark slot as free
  texture->description.id = VKR_INVALID_ID;
  texture->description.generation = VKR_INVALID_ID;

  // Remove from hash table
  bool8_t removed =
      vkr_hash_table_remove_VkrTextureEntry(&system->texture_map, remove_key);
  if (removed && system->texture_keys_by_index &&
      texture_index < system->textures.length) {
    system->texture_keys_by_index[texture_index] = NULL;
  } else if (!removed) {
    log_warn("Texture map remove failed for key '%s' during unload", remove_key);
  }

  if (stable_name &&
      vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_name)) {
    uint64_t len = string_length(stable_name) + 1;
    vkr_allocator_free(&system->string_allocator, (void *)stable_name, len,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  // Update free index for slot reuse
  if (texture_index < system->next_free_index) {
    system->next_free_index = texture_index;
  }

  free(primary_key);
  if (queryless_key) {
    free(queryless_key);
  }
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

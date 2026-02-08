#include "renderer/resources/loaders/texture_loader.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

#include "stb_image.h"

vkr_internal const char *vkr_texture_loader_supported_extensions[] = {
    "png",
    "jpg",
    "jpeg",
    "bmp",
    "tga",
    "vkt",
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

// Forward declarations
vkr_internal VkrRendererError vkr_texture_loader_load_from_file(
    VkrResourceLoader *self, String8 file_path, uint32_t desired_channels,
    VkrTexture *out_texture);

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

vkr_internal void vkr_texture_loader_unload(VkrResourceLoader *self,
                                            const VkrResourceHandleInfo *handle,
                                            String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  char texture_key_buffer[256];
  uint64_t copy_length = (name.length < sizeof(texture_key_buffer) - 1)
                             ? name.length
                             : sizeof(texture_key_buffer) - 1;
  MemCopy(texture_key_buffer, name.str, copy_length);
  texture_key_buffer[copy_length] = '\0';

  VkrTextureEntry *entry = vkr_hash_table_get_VkrTextureEntry(
      &system->texture_map, texture_key_buffer);
  if (!entry) {
    log_warn("Attempted to remove unknown texture '%s'", texture_key_buffer);
    return;
  }

  uint32_t texture_index = entry->index;
  const char *stable_name = entry->name;

  // Don't remove default texture
  if (texture_index == system->default_texture.id - 1) {
    log_warn("Cannot remove default texture");
    return;
  }

  // Destroy GPU resources
  VkrTexture *texture = &system->textures.data[texture_index];
  vkr_texture_destroy(self->renderer, texture);

  // Mark slot as free
  texture->description.id = VKR_INVALID_ID;
  texture->description.generation = VKR_INVALID_ID;

  // Remove from hash table
  vkr_hash_table_remove_VkrTextureEntry(&system->texture_map,
                                        texture_key_buffer);

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
}

VkrResourceLoader vkr_texture_loader_create(void) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_TEXTURE;
  loader.can_load = vkr_texture_loader_can_load;
  loader.load = vkr_texture_loader_load;
  loader.unload = vkr_texture_loader_unload;
  return loader;
}

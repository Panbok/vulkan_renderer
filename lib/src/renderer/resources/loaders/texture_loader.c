#include "renderer/resources/loaders/texture_loader.h"

vkr_global const char *png_ext = "png";
vkr_global const char *jpg_ext = "jpg";
vkr_global const char *jpeg_ext = "jpeg";
vkr_global const char *bmp_ext = "bmp";
vkr_global const char *tga_ext = "tga";

// Forward declarations
vkr_internal RendererError vkr_texture_loader_load_from_file(
    VkrResourceLoader *self, String8 file_path, uint32_t desired_channels,
    VkrTexture *out_texture);

vkr_internal bool8_t vkr_texture_loader_can_load(VkrResourceLoader *self,
                                                 String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  const uint8_t *extension_chars = name.str;
  for (uint64_t ext_length = name.length; ext_length > 0; ext_length--) {
    if (extension_chars[ext_length - 1] == '.') {
      String8 ext = string8_substring(&name, ext_length, name.length);
      return string_equalsi(string8_cstr(&ext), png_ext) ||
             string_equalsi(string8_cstr(&ext), jpg_ext) ||
             string_equalsi(string8_cstr(&ext), jpeg_ext) ||
             string_equalsi(string8_cstr(&ext), bmp_ext) ||
             string_equalsi(string8_cstr(&ext), tga_ext);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_texture_loader_load(VkrResourceLoader *self,
                                             String8 name, Arena *temp_arena,
                                             VkrResourceHandleInfo *out_handle,
                                             RendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
  RendererError renderer_error = RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load(system, name, temp_arena, &handle,
                               &renderer_error) ||
      renderer_error != RENDERER_ERROR_NONE) {
    log_error("Failed to load texture '%s': %s", string8_cstr(&name),
              renderer_get_error_string(renderer_error).str);
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_TEXTURE;
  out_handle->loader_id = self->id;
  out_handle->as.texture = handle;
  *out_error = RENDERER_ERROR_NONE;

  return true_v;
}

vkr_internal void vkr_texture_loader_unload(VkrResourceLoader *self,
                                            const VkrResourceHandleInfo *handle,
                                            String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrTextureSystem *system = (VkrTextureSystem *)self->resource_system;

  const char *texture_key = (const char *)name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (!entry) {
    log_warn("Attempted to remove unknown texture '%s'", texture_key);
    return;
  }

  uint32_t texture_index = entry->index;

  // Don't remove default texture
  if (texture_index == system->default_texture.id - 1) {
    log_warn("Cannot remove default texture");
    return;
  }

  // Destroy GPU resources
  VkrTexture *texture = &system->textures.data[texture_index];
  vkr_texture_destroy_internal(self->renderer, texture);

  // Mark slot as free
  texture->description.id = VKR_INVALID_ID;
  texture->description.generation = VKR_INVALID_ID;

  // Remove from hash table
  vkr_hash_table_remove_VkrTextureEntry(&system->texture_map, texture_key);

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
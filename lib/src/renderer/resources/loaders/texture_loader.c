#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/systems/vkr_texture_system.h"

#include "stb_image.h"

vkr_global const char *png_ext = "png";
vkr_global const char *jpg_ext = "jpg";
vkr_global const char *jpeg_ext = "jpeg";
vkr_global const char *bmp_ext = "bmp";
vkr_global const char *tga_ext = "tga";

// Forward declarations
vkr_internal VkrRendererError vkr_texture_loader_load_from_file(
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
      String8 png = string8_create_from_cstr((const uint8_t *)png_ext,
                                             string_length(png_ext));
      String8 jpg = string8_create_from_cstr((const uint8_t *)jpg_ext,
                                             string_length(jpg_ext));
      String8 jpeg = string8_create_from_cstr((const uint8_t *)jpeg_ext,
                                              string_length(jpeg_ext));
      String8 bmp = string8_create_from_cstr((const uint8_t *)bmp_ext,
                                             string_length(bmp_ext));
      String8 tga = string8_create_from_cstr((const uint8_t *)tga_ext,
                                             string_length(tga_ext));
      return string8_equalsi(&ext, &png) || string8_equalsi(&ext, &jpg) ||
             string8_equalsi(&ext, &jpeg) || string8_equalsi(&ext, &bmp) ||
             string8_equalsi(&ext, &tga);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_texture_loader_load(VkrResourceLoader *self,
                                             String8 name, Arena *temp_arena,
                                             VkrResourceHandleInfo *out_handle,
                                             VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
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
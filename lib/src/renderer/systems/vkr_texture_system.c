#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_resource_system.h"

uint32_t vkr_texture_system_find_free_slot(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");

  for (uint32_t texture_id = system->next_free_index;
       texture_id < system->config.max_texture_count; texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation == VKR_INVALID_ID) {
      system->next_free_index = texture_id + 1;
      return texture_id;
    }
  }

  for (uint32_t texture_id = 0; texture_id < system->next_free_index;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation == VKR_INVALID_ID) {
      system->next_free_index = texture_id + 1;
      return texture_id;
    }
  }

  return VKR_INVALID_ID;
}

bool8_t vkr_texture_system_init(VkrRendererFrontendHandle renderer,
                                const VkrTextureSystemConfig *config,
                                VkrTextureSystem *out_system) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_system != NULL, "Out system is NULL");
  assert_log(config->max_texture_count > 0,
             "Max texture count must be greater than 0");
  assert_log(config->max_texture_count >= 3,
             "Texture system requires at least 3 textures for defaults");

  MemZero(out_system, sizeof(*out_system));

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  out_system->arena =
      arena_create(VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_RSV,
                   VKR_TEXTURE_SYSTEM_DEFAULT_ARENA_CMT, app_arena_flags);
  if (!out_system->arena) {
    log_fatal("Failed to create texture system arena");
    return false_v;
  }

  out_system->renderer = renderer;
  out_system->config = *config;
  out_system->textures =
      array_create_VkrTexture(out_system->arena, config->max_texture_count);
  out_system->texture_map = vkr_hash_table_create_VkrTextureEntry(
      out_system->arena, ((uint64_t)config->max_texture_count) * 2ULL);
  out_system->next_free_index = 0;
  out_system->generation_counter = 1;

  // Initialize slots as invalid
  for (uint32_t texture_index = 0; texture_index < config->max_texture_count;
       texture_index++) {
    out_system->textures.data[texture_index].description.id = VKR_INVALID_ID;
    out_system->textures.data[texture_index].description.generation =
        VKR_INVALID_ID;
  }

  // Create default checkerboard texture at index 0
  VkrTexture *default_texture = &out_system->textures.data[0];
  default_texture->description = (VkrTextureDescription){
      .width = 256,
      .height = 256,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  uint64_t image_size = (uint64_t)default_texture->description.width *
                        (uint64_t)default_texture->description.height *
                        (uint64_t)default_texture->description.channels;
  Scratch scratch = scratch_create(out_system->arena);
  default_texture->image =
      arena_alloc(scratch.arena, image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (!default_texture->image) {
    log_error("Failed to allocate memory for default texture");
    return false_v;
  }
  MemSet(default_texture->image, 255, image_size);

  const uint32_t tile_size = 8;
  for (uint32_t row = 0; row < default_texture->description.height; row++) {
    for (uint32_t col = 0; col < default_texture->description.width; col++) {
      uint32_t pixel_index = (row * default_texture->description.width + col) *
                             default_texture->description.channels;
      uint32_t tile_row = row / tile_size;
      uint32_t tile_col = col / tile_size;
      bool32_t is_white = ((tile_row + tile_col) % 2) == 0;
      uint8_t channel_value = is_white ? 255 : 0;
      default_texture->image[pixel_index + 0] = channel_value;
      default_texture->image[pixel_index + 1] = channel_value;
      default_texture->image[pixel_index + 2] = channel_value;
      default_texture->image[pixel_index + 3] = 255;
    }
  }

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  default_texture->handle =
      vkr_renderer_create_texture(renderer, &default_texture->description,
                                  default_texture->image, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(renderer_error);
    log_error("Failed to create default checkerboard texture: %s",
              string8_cstr(&error_string));
    scratch_destroy(scratch, ARENA_MEMORY_TAG_TEXTURE);
    return false_v;
  }

  // Assign a stable id for default texture and lock index 0 as occupied
  default_texture->description.id = 1; // slot 0 -> id 1
  default_texture->description.generation = out_system->generation_counter++;

  out_system->default_texture =
      (VkrTextureHandle){.id = default_texture->description.id,
                         .generation = default_texture->description.generation};

  // Free CPU-side pixels after upload
  scratch_destroy(scratch, ARENA_MEMORY_TAG_TEXTURE);
  default_texture->image = NULL;

  // Create a 1x1 flat normal texture for cases where no normal map is provided
  VkrTexture *default_normal = &out_system->textures.data[1];
  default_normal->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  const uint8_t flat_normal_pixel[4] = {128, 128, 255, 255};
  VkrRendererError normal_err = VKR_RENDERER_ERROR_NONE;
  default_normal->handle = vkr_renderer_create_texture(
      renderer, &default_normal->description, flat_normal_pixel, &normal_err);
  if (normal_err != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(normal_err);
    log_error("Failed to create default normal texture: %s",
              string8_cstr(&error_string));
    return false_v;
  }

  default_normal->description.id = 2; // slot 1 -> id 2
  default_normal->description.generation = out_system->generation_counter++;
  default_normal->image = NULL;
  out_system->default_normal_texture =
      (VkrTextureHandle){.id = default_normal->description.id,
                         .generation = default_normal->description.generation};

  // Create a 1x1 flat specular texture for cases where no specular map is
  // provided
  VkrTexture *default_specular = &out_system->textures.data[2];
  default_specular->description = (VkrTextureDescription){
      .width = 1,
      .height = 1,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_from_bits(
          VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  const uint8_t flat_specular_pixel[4] = {255, 255, 255, 255};
  VkrRendererError specular_err = VKR_RENDERER_ERROR_NONE;
  default_specular->handle =
      vkr_renderer_create_texture(renderer, &default_specular->description,
                                  flat_specular_pixel, &specular_err);
  if (specular_err != VKR_RENDERER_ERROR_NONE) {
    String8 error_string = vkr_renderer_get_error_string(specular_err);
    log_error("Failed to create default specular texture: %s",
              string8_cstr(&error_string));
    // Clean up the already-created default normal texture
    vkr_renderer_destroy_texture(renderer, default_normal->handle);
    default_normal->handle = NULL;
    default_normal->description.generation = VKR_INVALID_ID;
    out_system->default_normal_texture.id = VKR_INVALID_ID;
    out_system->default_normal_texture.generation = VKR_INVALID_ID;
    return false_v;
  }

  default_specular->description.id = 3; // slot 2 -> id 3
  default_specular->description.generation = out_system->generation_counter++;
  default_specular->image = NULL;
  out_system->default_specular_texture = (VkrTextureHandle){
      .id = default_specular->description.id,
      .generation = default_specular->description.generation};

  // Ensure first free search starts after reserved defaults
  out_system->next_free_index = 3;

  return true_v;
}

void vkr_texture_system_shutdown(VkrRendererFrontendHandle renderer,
                                 VkrTextureSystem *system) {
  if (!system)
    return;

  for (uint32_t texture_id = 0; texture_id < system->textures.length;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation != VKR_INVALID_ID && texture->handle) {
      vkr_texture_destroy(renderer, texture);
    }
  }

  array_destroy_VkrTexture(&system->textures);
  arena_destroy(system->arena);
  MemZero(system, sizeof(*system));
}

VkrTextureHandle vkr_texture_system_acquire(VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (entry) {
    if (entry->ref_count == 0) {
      entry->auto_release = auto_release;
    }
    entry->ref_count++;
    *out_error = VKR_RENDERER_ERROR_NONE;
    VkrTexture *texture = &system->textures.data[entry->index];
    return (VkrTextureHandle){.id = texture->description.id,
                              .generation = texture->description.generation};
  }

  // Texture not loaded - return error
  log_warn("Texture '%s' not yet loaded, use resource system to load first",
           string8_cstr(&texture_name));
  *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
  return VKR_TEXTURE_HANDLE_INVALID;
}

bool8_t vkr_texture_system_create_writable(VkrTextureSystem *system,
                                           String8 name,
                                           const VkrTextureDescription *desc,
                                           VkrTextureHandle *out_handle,
                                           VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!name.str) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Check for duplicate name before allocating resources
  const char *texture_key = (const char *)name.str;
  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (existing_entry) {
    log_error("Texture with name '%s' already exists", texture_key);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrTextureDescription desc_copy = *desc;
  bitset8_set(&desc_copy.properties, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
  desc_copy.id = free_slot_index + 1;
  desc_copy.generation = system->generation_counter++;

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  VkrTextureOpaqueHandle handle = vkr_renderer_create_writable_texture(
      system->renderer, &desc_copy, &renderer_error);
  if (renderer_error != VKR_RENDERER_ERROR_NONE || handle == NULL) {
    *out_error = renderer_error;
    return false_v;
  }

  char *stable_key = (char *)arena_alloc(system->arena, name.length + 1,
                                         ARENA_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for texture map");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_renderer_destroy_texture(system->renderer, handle);
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = desc_copy;
  texture->handle = handle;

  VkrTextureEntry entry = {
      .index = free_slot_index, .ref_count = 1, .auto_release = false_v};
  bool8_t insert_success = vkr_hash_table_insert_VkrTextureEntry(
      &system->texture_map, stable_key, entry);
  if (!insert_success) {
    log_error("Failed to insert texture '%s' into hash table", stable_key);
    vkr_renderer_destroy_texture(system->renderer, handle);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_texture_system_release(VkrTextureSystem *system,
                                String8 texture_name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(texture_name.str != NULL, "Name is NULL");

  const char *texture_key = (const char *)texture_name.str;
  VkrTextureEntry *entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);

  if (!entry) {
    log_warn("Attempted to release unknown texture '%s'", texture_key);
    return;
  }

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for texture '%s'", texture_key);
    return;
  }

  entry->ref_count--;

  if (entry->ref_count == 0 && entry->auto_release) {
    uint32_t texture_index = entry->index;
    if (texture_index != system->default_texture.id - 1) {
      VkrResourceHandleInfo handle_info = {
          .type = VKR_RESOURCE_TYPE_TEXTURE,
          .loader_id = vkr_resource_system_get_loader_id(
              VKR_RESOURCE_TYPE_TEXTURE, texture_name),
          .as.texture = (VkrTextureHandle){
              .id = system->textures.data[texture_index].description.id,
              .generation =
                  system->textures.data[texture_index].description.generation}};
      vkr_resource_system_unload(&handle_info, texture_name);
    }
  }
}

void vkr_texture_system_release_by_handle(VkrTextureSystem *system,
                                          VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0) {
    log_warn("Attempted to release invalid texture handle");
    return;
  }

  for (uint64_t i = 0; i < system->texture_map.capacity; i++) {

    VkrHashEntry_VkrTextureEntry *entry = &system->texture_map.entries[i];
    if (entry->occupied == VKR_OCCUPIED) {

      uint32_t texture_index = entry->value.index;
      if (texture_index < system->textures.length) {

        VkrTexture *texture = &system->textures.data[texture_index];
        if (texture->description.id == handle.id &&
            texture->description.generation == handle.generation) {

          String8 texture_name = string8_create_from_cstr(
              (const uint8_t *)entry->key, strlen(entry->key));
          vkr_texture_system_release(system, texture_name);
          return;
        }
      }
    }
  }

  log_warn("Attempted to release unknown texture handle (id=%u, generation=%u)",
           handle.id, handle.generation);
}

VkrRendererError vkr_texture_system_update_sampler(
    VkrTextureSystem *system, VkrTextureHandle handle, VkrFilter min_filter,
    VkrFilter mag_filter, VkrMipFilter mip_filter, bool8_t anisotropy_enable,
    VkrTextureRepeatMode u_repeat_mode, VkrTextureRepeatMode v_repeat_mode,
    VkrTextureRepeatMode w_repeat_mode) {
  assert_log(system != NULL, "System is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  VkrTextureDescription updated_desc = texture->description;
  updated_desc.min_filter = min_filter;
  updated_desc.mag_filter = mag_filter;
  updated_desc.mip_filter = mip_filter;
  updated_desc.anisotropy_enable = anisotropy_enable;
  updated_desc.u_repeat_mode = u_repeat_mode;
  updated_desc.v_repeat_mode = v_repeat_mode;
  updated_desc.w_repeat_mode = w_repeat_mode;

  VkrRendererError err = vkr_renderer_update_texture(
      system->renderer, texture->handle, &updated_desc);
  if (err == VKR_RENDERER_ERROR_NONE) {
    texture->description = updated_desc;
  }
  return err;
}

VkrRendererError vkr_texture_system_write(VkrTextureSystem *system,
                                          VkrTextureHandle handle,
                                          const void *data, uint64_t size) {
  assert_log(system != NULL, "System is NULL");
  assert_log(data != NULL, "Data is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)texture->description.width *
                           (uint64_t)texture->description.height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  return vkr_renderer_write_texture(system->renderer, texture->handle, data,
                                    size);
}

VkrRendererError vkr_texture_system_write_region(
    VkrTextureSystem *system, VkrTextureHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size) {
  assert_log(system != NULL, "System is NULL");
  assert_log(region != NULL, "Region is NULL");
  assert_log(data != NULL, "Data is NULL");

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (region->mip_level >= 32) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  if (region->width == 0 || region->height == 0) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t mip_width = Max(1u, texture->description.width >> region->mip_level);
  uint32_t mip_height =
      Max(1u, texture->description.height >> region->mip_level);
  if (region->x + region->width > mip_width ||
      region->y + region->height > mip_height) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t expected_size = (uint64_t)region->width * (uint64_t)region->height *
                           (uint64_t)texture->description.channels;
  if (size < expected_size) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  return vkr_renderer_write_texture_region(system->renderer, texture->handle,
                                           region, data, size);
}

bool8_t vkr_texture_system_resize(VkrTextureSystem *system,
                                  VkrTextureHandle handle, uint32_t new_width,
                                  uint32_t new_height,
                                  bool8_t preserve_contents,
                                  VkrTextureHandle *out_handle,
                                  VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (new_width == 0 || new_height == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrTexture *texture = vkr_texture_system_get_by_handle(system, handle);
  if (!texture || !texture->handle) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return false_v;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_WRITABLE_BIT)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrRendererError err =
      vkr_renderer_resize_texture(system->renderer, texture->handle, new_width,
                                  new_height, preserve_contents);
  if (err != VKR_RENDERER_ERROR_NONE) {
    *out_error = err;
    return false_v;
  }

  texture->description.width = new_width;
  texture->description.height = new_height;
  texture->description.generation = system->generation_counter++;

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t
vkr_texture_system_register_external(VkrTextureSystem *system, String8 name,
                                     VkrTextureOpaqueHandle backend_handle,
                                     const VkrTextureDescription *desc,
                                     VkrTextureHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(desc != NULL, "Description is NULL");
  assert_log(backend_handle != NULL, "Backend handle is NULL");

  const char *texture_key = (const char *)name.str;
  VkrTextureEntry *existing_entry =
      vkr_hash_table_get_VkrTextureEntry(&system->texture_map, texture_key);
  if (existing_entry) {
    log_error("Texture with name '%s' is already registered",
              string8_cstr(&name));
    return false_v;
  }

  for (uint32_t i = 0; i < system->textures.length; ++i) {
    VkrTexture *texture = &system->textures.data[i];
    if (texture->handle == backend_handle) {
      log_error("Backend handle is already registered for texture '%s'",
                string8_cstr(&name));
      return false_v;
    }
  }

  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    return false_v;
  }

  char *stable_key = (char *)arena_alloc(system->arena, name.length + 1,
                                         ARENA_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for external texture map");
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrTexture *texture = &system->textures.data[free_slot_index];
  MemZero(texture, sizeof(*texture));
  texture->description = *desc;
  texture->description.id = free_slot_index + 1;
  texture->description.generation = system->generation_counter++;
  texture->handle = backend_handle;

  VkrTextureEntry entry = {
      .index = free_slot_index, .ref_count = 1, .auto_release = false_v};
  vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                        entry);

  if (out_handle) {
    *out_handle =
        (VkrTextureHandle){.id = texture->description.id,
                           .generation = texture->description.generation};
  }

  return true_v;
}

void vkr_texture_destroy(VkrRendererFrontendHandle renderer,
                         VkrTexture *texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  if (texture->handle) {
    vkr_renderer_destroy_texture(renderer, texture->handle);
  }

  MemZero(texture, sizeof(VkrTexture));
}

VkrTexture *vkr_texture_system_get_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == VKR_INVALID_ID)
    return NULL;

  uint32_t idx = handle.id - 1;
  if (idx >= system->textures.length)
    return NULL;
  VkrTexture *texture = &system->textures.data[idx];
  if (texture->description.generation != handle.generation)
    return NULL;
  return texture;
}

VkrTexture *vkr_texture_system_get_by_index(VkrTextureSystem *system,
                                            uint32_t texture_index) {
  if (!system || texture_index >= system->textures.length)
    return NULL;

  return array_get_VkrTexture(&system->textures, texture_index);
}

VkrTexture *vkr_texture_system_get_default(VkrTextureSystem *system) {
  return vkr_texture_system_get_by_index(system,
                                         system->default_texture.id - 1);
}

VkrTextureHandle
vkr_texture_system_get_default_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");

  if (system->textures.length == 0)
    return VKR_TEXTURE_HANDLE_INVALID;

  VkrTexture *texture = &system->textures.data[0];
  if (texture->description.id == VKR_INVALID_ID ||
      texture->description.generation == VKR_INVALID_ID)
    return VKR_TEXTURE_HANDLE_INVALID;
  return (VkrTextureHandle){.id = texture->description.id,
                            .generation = texture->description.generation};
}

VkrTextureHandle
vkr_texture_system_get_default_normal_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_normal_texture;
}

VkrTextureHandle
vkr_texture_system_get_default_specular_handle(VkrTextureSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->default_specular_texture;
}

VkrRendererError vkr_texture_system_load_from_file(VkrTextureSystem *system,
                                                   String8 file_path,
                                                   uint32_t desired_channels,
                                                   VkrTexture *out_texture) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");
  assert_log(file_path.str != NULL, "Path is NULL");

  // Ensure a NUL-terminated copy of the path (String8 is not NUL-terminated)
  Scratch c_string_scratch = scratch_create(system->arena);
  char *c_string_path = (char *)arena_alloc(
      c_string_scratch.arena, file_path.length + 1, ARENA_MEMORY_TAG_STRING);
  assert_log(c_string_path != NULL,
             "Failed to allocate path buffer for texture load");
  MemCopy(c_string_path, file_path.str, (size_t)file_path.length);
  c_string_path[file_path.length] = '\0';
  out_texture->file_path =
      file_path_create(c_string_path, system->arena, FILE_PATH_TYPE_RELATIVE);
  scratch_destroy(c_string_scratch, ARENA_MEMORY_TAG_STRING);

  // todo: this should be toggle only once on application startup
  stbi_set_flip_vertically_on_load(true);

  int32_t width, height, original_channels;
  int32_t stbi_requested_components =
      (desired_channels <= VKR_TEXTURE_RGBA_CHANNELS) ? (int)desired_channels
                                                      : 0;
  uint8_t *loaded_image_data =
      stbi_load((char *)out_texture->file_path.path.str, &width, &height,
                &original_channels, stbi_requested_components);
  if (!loaded_image_data) {
    const char *failure_reason = stbi_failure_reason();
    log_error("Failed to load texture: %s",
              failure_reason ? failure_reason : "unknown");
    return VKR_RENDERER_ERROR_FILE_NOT_FOUND;
  }

  if (width <= 0 || height <= 0 || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION) {
    stbi_image_free(loaded_image_data);
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t actual_channels =
      desired_channels > 0 ? desired_channels : (uint32_t)original_channels;

  VkrTextureFormat format;
  switch (actual_channels) {
  case VKR_TEXTURE_R_CHANNELS:
    format = VKR_TEXTURE_FORMAT_R8_UNORM;
    break;
  case VKR_TEXTURE_RG_CHANNELS:
    format = VKR_TEXTURE_FORMAT_R8G8_UNORM;
    break;
  case VKR_TEXTURE_RGB_CHANNELS:
    format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    break;
  case VKR_TEXTURE_RGBA_CHANNELS:
    format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
    break;
  default:
    format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    break;
  }

  out_texture->description = (VkrTextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = actual_channels,
      .format = format,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_LINEAR,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID, // Will be set by system
  };

  uint32_t loaded_channels =
      desired_channels > 0 ? desired_channels : (uint32_t)original_channels;
  uint64_t loaded_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)loaded_channels;

  uint64_t final_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)actual_channels;

  Scratch image_scratch = scratch_create(system->arena);
  out_texture->image = arena_alloc(image_scratch.arena, final_image_size,
                                   ARENA_MEMORY_TAG_TEXTURE);
  if (!out_texture->image) {
    stbi_image_free(loaded_image_data);
    scratch_destroy(image_scratch, ARENA_MEMORY_TAG_TEXTURE);
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }

  if (loaded_channels == VKR_TEXTURE_RGB_CHANNELS &&
      actual_channels == VKR_TEXTURE_RGBA_CHANNELS) {
    for (uint32_t pixel_index = 0;
         pixel_index < (uint32_t)width * (uint32_t)height; pixel_index++) {
      uint32_t source_pixel_index = pixel_index * VKR_TEXTURE_RGB_CHANNELS;
      uint32_t destination_pixel_index =
          pixel_index * VKR_TEXTURE_RGBA_CHANNELS;
      out_texture->image[destination_pixel_index + 0] =
          loaded_image_data[source_pixel_index + 0];
      out_texture->image[destination_pixel_index + 1] =
          loaded_image_data[source_pixel_index + 1];
      out_texture->image[destination_pixel_index + 2] =
          loaded_image_data[source_pixel_index + 2];
      out_texture->image[destination_pixel_index + 3] = 255;
    }
  } else {
    MemCopy(out_texture->image, loaded_image_data, (size_t)loaded_image_size);
  }

  stbi_image_free(loaded_image_data);

  VkrRendererError renderer_error = VKR_RENDERER_ERROR_NONE;
  out_texture->handle =
      vkr_renderer_create_texture(system->renderer, &out_texture->description,
                                  out_texture->image, &renderer_error);

  // Free CPU-side pixels after upload
  scratch_destroy(image_scratch, ARENA_MEMORY_TAG_TEXTURE);
  out_texture->image = NULL;
  return renderer_error;
}

bool8_t vkr_texture_system_load(VkrTextureSystem *system, String8 name,
                                VkrTextureHandle *out_handle,
                                VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrTexture loaded_texture = {0};
  *out_error = vkr_texture_system_load_from_file(
      system, name, VKR_TEXTURE_RGBA_CHANNELS, &loaded_texture);
  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }

  // Find free slot in system
  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_error("Texture system is full (max=%u)",
              system->config.max_texture_count);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    if (loaded_texture.handle) {
      vkr_renderer_destroy_texture(system->renderer, loaded_texture.handle);
    }
    return false_v;
  }

  // Store stable copy of key in system arena
  char *stable_key = (char *)arena_alloc(system->arena, name.length + 1,
                                         ARENA_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key copy for texture map");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    if (loaded_texture.handle) {
      vkr_renderer_destroy_texture(system->renderer, loaded_texture.handle);
    }
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  // Copy texture data to system
  VkrTexture *texture = &system->textures.data[free_slot_index];
  *texture = loaded_texture;

  // Assign stable id and generation
  texture->description.id = free_slot_index + 1;
  texture->description.generation = system->generation_counter++;

  // Add to hash table with 0 ref count
  VkrTextureEntry new_entry = {
      .index = free_slot_index, .ref_count = 0, .auto_release = true_v};
  vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                        new_entry);

  VkrTextureHandle handle = {.id = texture->description.id,
                             .generation = texture->description.generation};

  *out_handle = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

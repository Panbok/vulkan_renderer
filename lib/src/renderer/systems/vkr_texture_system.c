#include "renderer/systems/vkr_texture_system.h"
#include "defines.h"

vkr_internal INLINE uint32_t
vkr_texture_system_find_free_slot(VkrTextureSystem *system) {
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

bool8_t vkr_texture_system_init(RendererFrontendHandle renderer,
                                const VkrTextureSystemConfig *config,
                                VkrTextureSystem *out_system) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_system != NULL, "Out system is NULL");
  assert_log(config->max_texture_count > 0,
             "Max texture count must be greater than 0");

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
  RendererError renderer_error = vkr_texture_system_create_default(
      renderer, out_system, &out_system->textures.data[0]);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create default checkerboard texture: %s",
              renderer_get_error_string(renderer_error));
  }
  out_system->default_texture = (VkrTextureHandle){
      .id = out_system->textures.data[0].description.id,
      .generation = out_system->textures.data[0].description.generation};
  // Assign a stable id for default texture and lock index 0 as occupied
  out_system->textures.data[0].description.id = 1; // slot 0 -> id 1
  out_system->textures.data[0].description.generation =
      out_system->generation_counter++;
  // Ensure first free search starts at 1 so index 0 remains the default
  out_system->next_free_index = 1;

  return true_v;
}

void vkr_texture_system_shutdown(RendererFrontendHandle renderer,
                                 VkrTextureSystem *system) {
  if (!system)
    return;

  for (uint32_t texture_id = 0; texture_id < system->textures.length;
       texture_id++) {
    VkrTexture *texture = &system->textures.data[texture_id];
    if (texture->description.generation != VKR_INVALID_ID && texture->handle) {
      vkr_texture_system_destroy(renderer, system, texture);
    }
  }

  array_destroy_VkrTexture(&system->textures);
  arena_destroy(system->arena);
  MemZero(system, sizeof(*system));
}

VkrTextureHandle vkr_texture_system_acquire(RendererFrontendHandle renderer,
                                            VkrTextureSystem *system,
                                            String8 texture_name,
                                            bool8_t auto_release,
                                            Arena *temp_arena,
                                            RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
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
    *out_error = RENDERER_ERROR_NONE;
    VkrTexture *texture = &system->textures.data[entry->index];
    return (VkrTextureHandle){.id = texture->description.id,
                              .generation = texture->description.generation};
  }

  // Need to load; find free slot
  uint32_t free_slot_index = vkr_texture_system_find_free_slot(system);
  if (free_slot_index == VKR_INVALID_ID) {
    log_fatal("Texture system is full (max=%u)",
              system->config.max_texture_count);
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
    return VKR_TEXTURE_HANDLE_INVALID;
  }

  VkrTexture *texture = &system->textures.data[free_slot_index];
  *out_error = vkr_texture_system_load(renderer, system, texture_name,
                                       VKR_TEXTURE_RGBA_CHANNELS, texture);
  if (*out_error != RENDERER_ERROR_NONE) {
    log_warn("Falling back to default texture for '%s'",
             string8_cstr(&texture_name));
    VkrTexture *default_texture =
        &system->textures.data[system->default_texture.id - 1];
    return (VkrTextureHandle){.id = default_texture->description.id,
                              .generation =
                                  default_texture->description.generation};
  }

  // Assign a stable id for the texture based on slot
  texture->description.id = free_slot_index + 1;

  // Store a stable copy of the key in the system arena to avoid lifetime bugs
  char *stable_key = (char *)arena_alloc(system->arena, texture_name.length + 1,
                                         ARENA_MEMORY_TAG_STRING);
  assert_log(stable_key != NULL, "Failed to allocate key copy for texture map");
  MemCopy(stable_key, texture_name.str, (size_t)texture_name.length);
  stable_key[texture_name.length] = '\0';

  VkrTextureEntry new_entry = {
      .index = free_slot_index, .ref_count = 1, .auto_release = auto_release};
  vkr_hash_table_insert_VkrTextureEntry(&system->texture_map, stable_key,
                                        new_entry);
  return (VkrTextureHandle){.id = texture->description.id,
                            .generation = texture->description.generation};
}

void vkr_texture_system_release(RendererFrontendHandle renderer,
                                VkrTextureSystem *system,
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
      VkrTexture *texture = &system->textures.data[texture_index];
      vkr_texture_system_destroy(renderer, system, texture);
      texture->description.id = VKR_INVALID_ID;
      texture->description.generation = VKR_INVALID_ID;
    }
    vkr_hash_table_remove_VkrTextureEntry(&system->texture_map, texture_key);
    if (texture_index < system->next_free_index) {
      system->next_free_index = texture_index; // reuse earlier slot
    }
  }
}

RendererError vkr_texture_system_create_default(RendererFrontendHandle renderer,
                                                VkrTextureSystem *system,
                                                VkrTexture *out_texture) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");

  out_texture->description = (TextureDescription){
      .width = 256,
      .height = 256,
      .channels = 4,
      .format = TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = TEXTURE_TYPE_2D,
      .properties = texture_property_flags_from_bits(
          TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT),
      .generation = VKR_INVALID_ID,
  };

  uint64_t image_size = (uint64_t)out_texture->description.width *
                        (uint64_t)out_texture->description.height *
                        (uint64_t)out_texture->description.channels;
  Scratch scratch = scratch_create(system->arena);
  out_texture->image =
      arena_alloc(scratch.arena, image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (!out_texture->image) {
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }
  MemSet(out_texture->image, 255, image_size);

  const uint32_t tile_size = 8;
  for (uint32_t row = 0; row < out_texture->description.height; row++) {
    for (uint32_t col = 0; col < out_texture->description.width; col++) {
      uint32_t pixel_index = (row * out_texture->description.width + col) *
                             out_texture->description.channels;
      uint32_t tile_row = row / tile_size;
      uint32_t tile_col = col / tile_size;
      bool32_t is_white = ((tile_row + tile_col) % 2) == 0;
      uint8_t channel_value = is_white ? 255 : 0;
      out_texture->image[pixel_index + 0] = channel_value;
      out_texture->image[pixel_index + 1] = channel_value;
      out_texture->image[pixel_index + 2] = channel_value;
      out_texture->image[pixel_index + 3] = 255;
    }
  }

  RendererError renderer_error = RENDERER_ERROR_NONE;
  out_texture->handle = renderer_create_texture(
      renderer, &out_texture->description, out_texture->image, &renderer_error);
  if (renderer_error == RENDERER_ERROR_NONE) {
    out_texture->description.generation = system->generation_counter++;
  }
  // Free CPU-side pixels after upload
  scratch_destroy(scratch, ARENA_MEMORY_TAG_TEXTURE);
  out_texture->image = NULL;
  return renderer_error;
}

RendererError vkr_texture_system_load(RendererFrontendHandle renderer,
                                      VkrTextureSystem *system,
                                      String8 file_path,
                                      uint32_t desired_channels,
                                      VkrTexture *out_texture) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_texture != NULL, "Out texture is NULL");
  assert_log(file_path.str != NULL, "Path is NULL");

  // Ensure a NUL-terminated copy of the path (String8 is not NUL-terminated)
  char *c_string_path = (char *)arena_alloc(system->arena, file_path.length + 1,
                                            ARENA_MEMORY_TAG_STRING);
  assert_log(c_string_path != NULL,
             "Failed to allocate path buffer for texture load");
  MemCopy(c_string_path, file_path.str, (size_t)file_path.length);
  c_string_path[file_path.length] = '\0';
  out_texture->file_path =
      file_path_create(c_string_path, system->arena, FILE_PATH_TYPE_RELATIVE);

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
    return RENDERER_ERROR_FILE_NOT_FOUND;
  }

  if (width <= 0 || height <= 0 || width > VKR_TEXTURE_MAX_DIMENSION ||
      height > VKR_TEXTURE_MAX_DIMENSION) {
    stbi_image_free(loaded_image_data);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t actual_channels =
      desired_channels > 0 ? desired_channels : (uint32_t)original_channels;

  TextureFormat format;
  switch (actual_channels) {
  case VKR_TEXTURE_R_CHANNELS:
    format = TEXTURE_FORMAT_R8_UNORM;
    break;
  case VKR_TEXTURE_RG_CHANNELS:
    format = TEXTURE_FORMAT_R8G8_UNORM;
    break;
  case VKR_TEXTURE_RGB_CHANNELS:
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    break;
  case VKR_TEXTURE_RGBA_CHANNELS:
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    break;
  default:
    format = TEXTURE_FORMAT_R8G8B8A8_UNORM;
    actual_channels = VKR_TEXTURE_RGBA_CHANNELS;
    break;
  }

  out_texture->description = (TextureDescription){
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .channels = actual_channels,
      .format = format,
      .type = TEXTURE_TYPE_2D,
      .properties = texture_property_flags_create(),
      .generation = VKR_INVALID_ID,
  };

  uint32_t loaded_channels =
      desired_channels > 0 ? desired_channels : (uint32_t)original_channels;
  uint64_t loaded_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)loaded_channels;

  uint64_t final_image_size =
      (uint64_t)width * (uint64_t)height * (uint64_t)actual_channels;

  Scratch scratch = scratch_create(system->arena);
  out_texture->image =
      arena_alloc(scratch.arena, final_image_size, ARENA_MEMORY_TAG_TEXTURE);
  if (!out_texture->image) {
    stbi_image_free(loaded_image_data);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_TEXTURE);
    return RENDERER_ERROR_OUT_OF_MEMORY;
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

  RendererError renderer_error = RENDERER_ERROR_NONE;
  out_texture->handle = renderer_create_texture(
      renderer, &out_texture->description, out_texture->image, &renderer_error);
  if (renderer_error == RENDERER_ERROR_NONE) {
    out_texture->description.generation = system->generation_counter++;
  }
  // Free CPU-side pixels after upload
  scratch_destroy(scratch, ARENA_MEMORY_TAG_TEXTURE);
  out_texture->image = NULL;
  return renderer_error;
}

void vkr_texture_system_destroy(RendererFrontendHandle renderer,
                                VkrTextureSystem *system, VkrTexture *texture) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(texture != NULL, "Texture is NULL");

  if (texture->handle) {
    renderer_destroy_texture(renderer, texture->handle);
  }

  MemZero(texture, sizeof(VkrTexture));
}

VkrTexture *vkr_texture_system_get_by_handle(VkrTextureSystem *system,
                                             VkrTextureHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (!system || handle.id == 0)
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

  if (system->default_texture.id - 1 >= system->textures.length)
    return VKR_TEXTURE_HANDLE_INVALID;
  VkrTexture *texture = &system->textures.data[system->default_texture.id - 1];
  if (texture->description.id == VKR_INVALID_ID ||
      texture->description.generation == VKR_INVALID_ID)
    return VKR_TEXTURE_HANDLE_INVALID;
  return (VkrTextureHandle){.id = texture->description.id,
                            .generation = texture->description.generation};
}
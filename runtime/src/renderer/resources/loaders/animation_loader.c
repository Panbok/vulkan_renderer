#include "renderer/resources/loaders/animation_loader.h"

#include "assets/vkr_animation_cooked.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"

vkr_internal bool8_t animation_loader_can_load(VkrResourceLoader *self,
                                               String8 name) {
  (void)self;
  return name.str && name.length >= 4u &&
         MemCompare(name.str + name.length - 4u, ".vka", 4u) == 0;
}

vkr_internal bool8_t animation_loader_load(VkrResourceLoader *self,
                                           String8 name, VkrAllocator *scratch,
                                           VkrResourceHandleInfo *out_handle,
                                           VkrRendererError *out_error) {
  if (!out_handle || !out_error) {
    return false_v;
  }
  *out_handle = (VkrResourceHandleInfo){0};
  *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
  if (!self || !scratch || scratch->type != VKR_ALLOCATOR_TYPE_ARENA ||
      !animation_loader_can_load(self, name) || name.length >= SIZE_MAX) {
    return false_v;
  }
  for (uint64_t i = 0; i < name.length; ++i) {
    if (!name.str[i]) {
      return false_v;
    }
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }
  FileHandle file = {0};
  Arena *arena = NULL;
  VkrAllocator allocator = {0};
  bool8_t success = false_v;
  char *path = vkr_allocator_alloc(scratch, name.length + 1u,
                                   VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!path) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  MemCopy(path, name.str, name.length);
  path[name.length] = 0;
  const FilePath file_path = file_path_view(path);
  if (file_open(&file_path,
                (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY},
                &file) != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    goto cleanup;
  }
  uint64_t size = 0u;
  if (file_remaining_size(&file, &size) != FILE_ERROR_NONE || size < 48u ||
      size > VKR_ANIMATION_COOKED_MAX_BYTES) {
    goto cleanup;
  }
  uint8_t *bytes =
      vkr_allocator_alloc(scratch, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!bytes) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  uint64_t bytes_read = 0u;
  if (file_read_into(&file, bytes, size, &bytes_read) != FILE_ERROR_NONE ||
      bytes_read != size) {
    goto cleanup;
  }
  arena = arena_create(GB(2), KB(64));
  allocator.ctx = arena;
  if (!arena || !vkr_allocator_arena(&allocator)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  VkrAnimationLoaderResult *result = vkr_allocator_alloc_aligned(
      &allocator, sizeof(*result), AlignOf(VkrAnimationLoaderResult),
      VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  *result = (VkrAnimationLoaderResult){.arena = arena};
  const char *decode_error = NULL;
  if (!vkr_animation_cooked_decode(&allocator, scratch, bytes, size,
                                   &result->asset, &decode_error)) {
    goto cleanup;
  }
  uint8_t *owned_path = vkr_allocator_alloc(&allocator, name.length,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!owned_path) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  MemCopy(owned_path, name.str, name.length);
  result->source_path = (String8){.str = owned_path, .length = name.length};
  result->allocator = allocator;
  *out_handle = (VkrResourceHandleInfo){
      .loader_id = self->id,
      .type = VKR_RESOURCE_TYPE_ANIMATION,
      .load_state = VKR_RESOURCE_LOAD_STATE_READY,
      .as.animation = result,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  success = true_v;
cleanup:
  file_close(&file);
  if (!success && arena) {
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return success;
}

vkr_internal void animation_loader_unload(VkrResourceLoader *self,
                                          const VkrResourceHandleInfo *handle,
                                          String8 name) {
  (void)self;
  (void)name;
  if (handle && handle->type == VKR_RESOURCE_TYPE_ANIMATION &&
      handle->as.animation) {
    VkrAnimationLoaderResult *result = handle->as.animation;
    Arena *arena = result->arena;
    vkr_allocator_release_global_accounting(&result->allocator);
    arena_destroy(arena);
  }
}

VkrResourceLoader vkr_animation_loader_create(void) {
  return (VkrResourceLoader){
      .type = VKR_RESOURCE_TYPE_ANIMATION,
      .can_load = animation_loader_can_load,
      .load = animation_loader_load,
      .unload = animation_loader_unload,
  };
}

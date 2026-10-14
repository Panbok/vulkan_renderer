#include "physics/vkr_collision_asset.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

struct s_VkrCollisionAsset {
  Arena *arena;
  VkrAllocator allocator;
  VkrCollisionGeometry geometry;
  uint32_t references;
};

VkrCollisionAsset *vkr_collision_asset_open(String8 path, const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!path.str || !path.length || path.length >= 32768 ||
      MemFindByte(path.str, 0, path.length)) {
    if (error) {
      *error = "Invalid collision asset path";
    }
    return NULL;
  }
  Arena *arena =
      arena_create(VKR_COLLISION_COOKED_MAX_BYTES * 2 + MB(1), KB(64));
  if (!arena) {
    if (error) {
      *error = "Collision asset arena allocation failed";
    }
    return NULL;
  }
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    if (error) {
      *error = "Collision asset allocator initialization failed";
    }
    return NULL;
  }
  VkrCollisionAsset *asset = vkr_allocator_alloc(
      &allocator, sizeof(*asset), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  FileHandle file = {0};
  const char *message = "Collision asset could not be opened";
  String8 filename = string8_duplicate(&allocator, &path);
  if (!asset || !filename.str) {
    goto cleanup;
  }
  const FilePath file_path = file_path_view(string8_cstr(&filename));
  uint64_t size = 0u;
  if (file_open(&file_path,
                (FileMode){.set = FILE_MODE_READ | FILE_MODE_BINARY},
                &file) != FILE_ERROR_NONE) {
    goto cleanup;
  }
  if (file_remaining_size(&file, &size) != FILE_ERROR_NONE || size < 48u ||
      size > VKR_COLLISION_COOKED_MAX_BYTES) {
    message = "Collision asset file size is invalid";
    goto cleanup;
  }
  uint8_t *bytes =
      vkr_allocator_alloc(&allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  uint64_t bytes_read = 0u;
  if (!bytes ||
      file_read_into(&file, bytes, size, &bytes_read) != FILE_ERROR_NONE ||
      bytes_read != size) {
    message = "Collision asset read failed";
    goto cleanup;
  }
  VkrCollisionGeometry geometry;
  if (!vkr_collision_cooked_decode(&allocator, bytes, size, &geometry,
                                   &message)) {
    goto cleanup;
  }
  file_close(&file);
  *asset = (VkrCollisionAsset){.arena = arena,
                               .allocator = allocator,
                               .geometry = geometry,
                               .references = 1};
  return asset;
cleanup:
  file_close(&file);
  if (error) {
    *error = message;
  }
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return NULL;
}

bool8_t vkr_collision_asset_retain(VkrCollisionAsset *asset) {
  if (!asset || asset->references == UINT32_MAX) {
    return false_v;
  }
  asset->references++;
  return true_v;
}

void vkr_collision_asset_close(VkrCollisionAsset *asset) {
  if (!asset || --asset->references) {
    return;
  }
  Arena *arena = asset->arena;
  vkr_allocator_release_global_accounting(&asset->allocator);
  arena_destroy(arena);
}

const VkrCollisionGeometry *
vkr_collision_asset_geometry(const VkrCollisionAsset *asset) {
  return asset ? &asset->geometry : NULL;
}

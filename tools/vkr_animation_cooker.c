#include "assets/vkr_animation_cooked.h"
#include "assets/vkr_animation_encode.h"
#include "assets/vkr_animation_import.h"
#include "assets/vkr_mesh_encode.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_entry.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void animation_usage(const char *program) {
  fprintf(stderr,
          "Cook: %s --input <source.gltf|source.glb> --output <bank.vka>\n"
          "Inspect: %s --inspect --input <bank.vka> "
          "[--clip <index> --time <seconds>]\n",
          program, program);
}

static bool8_t animation_read(VkrAllocator *allocator, const char *path,
                              uint8_t **bytes, uint64_t *size) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  bool8_t success = false_v;
  if (fseek(file, 0, SEEK_END) != 0) {
    goto cleanup;
  }
  const long length = ftell(file);
  if (length < 0 || (uint64_t)length > VKR_ANIMATION_COOKED_MAX_BYTES ||
      fseek(file, 0, SEEK_SET) != 0) {
    goto cleanup;
  }
  *size = (uint64_t)length;
  *bytes = vkr_allocator_alloc(allocator, *size ? *size : 1u,
                               VKR_ALLOCATOR_MEMORY_TAG_FILE);
  success = *bytes && fread(*bytes, 1u, *size, file) == *size;
cleanup:
  fclose(file);
  return success;
}

static bool8_t animation_print_pose(VkrAllocator *allocator,
                                    const VkrAnimationAsset *asset,
                                    uint32_t clip, float64_t seconds) {
  VkrAnimationTrs *pose =
      vkr_allocator_alloc(allocator, asset->node_count * sizeof(*pose),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  Mat4 *globals =
      vkr_allocator_alloc(allocator, asset->node_count * sizeof(*globals),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!pose || !globals || !vkr_animation_sample(asset, clip, seconds, pose) ||
      !vkr_animation_global_pose(asset, pose, globals)) {
    return false_v;
  }
  printf("pose clip=%u seconds=%.17g\n", clip, seconds);
  for (uint32_t i = 0; i < asset->node_count; ++i) {
    printf("node=%u global=", i);
    for (uint32_t c = 0; c < 16u; ++c) {
      printf("%s%.9g", c ? "," : "", (float64_t)globals[i].elements[c]);
    }
    printf("\n");
  }
  return true_v;
}

int main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  bool8_t inspect = false_v;
  bool8_t has_clip = false_v;
  bool8_t has_time = false_v;
  uint32_t clip = 0u;
  float64_t seconds = 0.0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--inspect") == 0) {
      inspect = true_v;
    } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
      input = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output = argv[++i];
    } else if (strcmp(argv[i], "--clip") == 0 && i + 1 < argc) {
      const char *value = argv[++i];
      char *end = NULL;
      errno = 0;
      const unsigned long parsed = strtoul(value, &end, 10);
      if (errno || value == end || *end || value[0] == '-' ||
          parsed > UINT32_MAX) {
        animation_usage(argv[0]);
        return 2;
      }
      clip = (uint32_t)parsed;
      has_clip = true_v;
    } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
      const char *value = argv[++i];
      char *end = NULL;
      errno = 0;
      seconds = strtod(value, &end);
      if (errno || value == end || *end || !isfinite(seconds)) {
        animation_usage(argv[0]);
        return 2;
      }
      has_time = true_v;
    } else {
      animation_usage(argv[0]);
      return 2;
    }
  }
  const size_t output_length = output ? strlen(output) : 0u;
  if (!input || (inspect && output) || (!inspect && !output) ||
      has_clip != has_time || (has_clip && !inspect) ||
      (output && (output_length < 4u ||
                  strcmp(output + output_length - 4u, ".vka") != 0))) {
    animation_usage(argv[0]);
    return 2;
  }
  Arena *result_arena = arena_create(GB(2), MB(8));
  Arena *scratch_arena = arena_create(GB(2), MB(8));
  VkrAllocator result = {.ctx = result_arena};
  VkrAllocator scratch = {.ctx = scratch_arena};
  bool8_t success = false_v;
  const char *error = "Unable to initialize animation cooker arenas";
  if (!result_arena || !scratch_arena || !vkr_allocator_arena(&result) ||
      !vkr_allocator_arena(&scratch)) {
    goto cleanup;
  }
  VkrAnimationAsset asset = {0};
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (inspect) {
    error = "Unable to read animation bank";
    if (!animation_read(&scratch, input, &bytes, &size) ||
        !vkr_animation_cooked_decode(&result, &scratch, bytes, size, &asset,
                                     &error)) {
      goto cleanup;
    }
  } else {
    if (!vkr_animation_import_gltf(
            &result, &scratch, string8_create((uint8_t *)input, strlen(input)),
            &asset, &error) ||
        !vkr_animation_cooked_encode(&scratch, &asset, &bytes, &size, &error)) {
      goto cleanup;
    }
    error = "Unable to publish animation bank";
    if (!vkr_mesh_cooked_write_atomic(
            &scratch, string8_create((uint8_t *)output, strlen(output)), bytes,
            size)) {
      goto cleanup;
    }
  }
  printf("animation_bank version=%u bytes=%llu nodes=%u skins=%u clips=%u "
         "source_fingerprint=%016llx\n",
         VKR_ANIMATION_COOKED_VERSION, (unsigned long long)size,
         asset.node_count, asset.skin_count, asset.clip_count,
         (unsigned long long)asset.source_fingerprint);
  for (uint32_t i = 0; i < asset.clip_count; ++i) {
    const VkrAnimationClip *entry = &asset.clips[i];
    printf("clip=%u duration=%.9g channels=%u name=%.*s\n", i,
           (float64_t)entry->duration, entry->channel_count,
           (int)entry->name.length,
           entry->name.str ? (char *)entry->name.str : "");
  }
  if (has_clip) {
    error = "Invalid pose query or non-finite evaluated pose";
    if (!animation_print_pose(&scratch, &asset, clip, seconds)) {
      goto cleanup;
    }
  }
  success = true_v;
cleanup:
  if (!success) {
    fprintf(stderr, "Animation cooker: %s\n",
            error ? error : "operation failed");
  }
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&result);
  arena_destroy(scratch_arena);
  arena_destroy(result_arena);
  return success ? 0 : 1;
}

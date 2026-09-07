#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "assets/vkr_mesh_cook_source.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKR_MESH_COOKER_MAX_LIGHT_RANGES 64u

static void vkr_mesh_cooker_print_usage(const char *program) {
  fprintf(stderr,
          "Usage: %s --input <mesh.obj|mesh.gltf|mesh.glb> "
          "--output <mesh.vkb> [--light-range <definition-name>=<meters>]...\n",
          program);
}

int main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  VkrSceneLightRangeOverride light_ranges[VKR_MESH_COOKER_MAX_LIGHT_RANGES] = {
      0};
  uint32_t light_range_count = 0u;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
      input = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output = argv[++i];
    } else if (strcmp(argv[i], "--light-range") == 0 && i + 1 < argc) {
      if (light_range_count == VKR_MESH_COOKER_MAX_LIGHT_RANGES) {
        vkr_mesh_cooker_print_usage(argv[0]);
        return 2;
      }
      const char *value = argv[++i];
      const char *separator = strchr(value, '=');
      char *range_end = NULL;
      const float32_t range =
          separator ? strtof(separator + 1, &range_end) : 0.0f;
      if (!separator || separator == value || separator[1] == '\0' ||
          range_end == separator + 1 || *range_end != '\0' ||
          !isfinite(range) || range <= 0.0f) {
        vkr_mesh_cooker_print_usage(argv[0]);
        return 2;
      }
      light_ranges[light_range_count++] = (VkrSceneLightRangeOverride){
          .light_name = string8_create(
              (uint8_t *)value, (uint64_t)(separator - value)),
          .range = range,
      };
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      vkr_mesh_cooker_print_usage(argv[0]);
      return 0;
    } else {
      vkr_mesh_cooker_print_usage(argv[0]);
      return 2;
    }
  }
  if (!input || !output) {
    vkr_mesh_cooker_print_usage(argv[0]);
    return 2;
  }

  Arena *source_arena = arena_create(GB(8), MB(64));
  Arena *scratch_arena = arena_create(GB(8), MB(64));
  if (!source_arena || !scratch_arena) {
    arena_destroy(scratch_arena);
    arena_destroy(source_arena);
    fprintf(stderr, "Unable to create cooker arenas\n");
    return 1;
  }
  if (!log_init(scratch_arena)) {
    arena_destroy(scratch_arena);
    arena_destroy(source_arena);
    fprintf(stderr, "Unable to initialize cooker logging\n");
    return 1;
  }
  VkrAllocator source_allocator = {.ctx = source_arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  if (!vkr_allocator_arena(&source_allocator) ||
      !vkr_allocator_arena(&scratch_allocator)) {
    log_shutdown();
    arena_destroy(scratch_arena);
    arena_destroy(source_arena);
    return 1;
  }

  String8 input_path =
      string8_create((uint8_t *)input, (uint64_t)strlen(input));
  String8 output_path =
      string8_create((uint8_t *)output, (uint64_t)strlen(output));
  VkrMeshCookStats stats = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  bool8_t success = vkr_mesh_cook_source_with_light_ranges(
      input_path, output_path, light_ranges, light_range_count,
      &source_allocator, &scratch_allocator, &stats, &error);
  if (success) {
    printf("cooked=%llu decoded=%llu vertices=%u indices=%u ranges=%u "
           "output=%s\n",
           (unsigned long long)stats.cooked_bytes,
           (unsigned long long)stats.decoded_bytes, stats.vertex_count,
           stats.index_count, stats.range_count, output);
  } else {
    fprintf(stderr, "Mesh cooking failed with renderer error %u\n",
            (uint32_t)error);
  }

  vkr_allocator_release_global_accounting(&scratch_allocator);
  vkr_allocator_release_global_accounting(&source_allocator);
  log_shutdown();
  arena_destroy(scratch_arena);
  arena_destroy(source_arena);
  return success ? 0 : 1;
}

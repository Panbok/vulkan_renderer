#include "assets/vkr_collision_cooked.h"
#include "assets/vkr_collision_import.h"
#include "assets/vkr_mesh_encode.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_entry.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int collision_usage(const char *program) {
  fprintf(stderr,
          "Cook: %s --input source.gltf|source.glb --output shape.vkc "
          "--kind hull|mesh [--node index]\n"
          "Inspect: %s --inspect --input shape.vkc\n",
          program, program);
  return 2;
}

static bool8_t collision_read(VkrAllocator *allocator, const char *path,
                              uint8_t **bytes, uint64_t *size) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  bool8_t success = false_v;
  if (fseek(file, 0, SEEK_END)) {
    goto cleanup;
  }
  const long length = ftell(file);
  if (length < 48 || (uint64_t)length > VKR_COLLISION_COOKED_MAX_BYTES ||
      fseek(file, 0, SEEK_SET)) {
    goto cleanup;
  }
  *size = (uint64_t)length;
  *bytes = vkr_allocator_alloc(allocator, *size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  success = *bytes && fread(*bytes, 1, *size, file) == *size;
cleanup:
  fclose(file);
  return success;
}

VKR_MAIN(argc, argv) {
  const char *input = NULL;
  const char *output = NULL;
  VkrCollisionKind kind = 0;
  uint32_t node = UINT32_MAX;
  bool8_t inspect = false_v;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--inspect")) {
      inspect = true_v;
    } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      input = argv[++i];
    } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
      output = argv[++i];
    } else if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
      const char *value = argv[++i];
      kind = !strcmp(value, "hull")   ? VKR_COLLISION_CONVEX_HULL
             : !strcmp(value, "mesh") ? VKR_COLLISION_TRIANGLE_MESH
                                      : 0;
      if (!kind) {
        return collision_usage(argv[0]);
      }
    } else if (!strcmp(argv[i], "--node") && i + 1 < argc) {
      const char *value = argv[++i];
      char *end = NULL;
      errno = 0;
      const unsigned long parsed = strtoul(value, &end, 10);
      if (errno || value == end || *end || value[0] == '-' ||
          parsed >= UINT32_MAX) {
        return collision_usage(argv[0]);
      }
      node = (uint32_t)parsed;
    } else {
      return collision_usage(argv[0]);
    }
  }
  if (!input || (inspect && (output || kind || node != UINT32_MAX)) ||
      (!inspect && (!output || !kind || strlen(output) < 4 ||
                    strcmp(output + strlen(output) - 4, ".vkc")))) {
    return collision_usage(argv[0]);
  }
  Arena *result_arena = arena_create(MB(128), MB(1));
  Arena *scratch_arena = arena_create(MB(256), MB(1));
  VkrAllocator result = {.ctx = result_arena};
  VkrAllocator scratch = {.ctx = scratch_arena};
  bool8_t success = false_v;
  const char *error = "Collision cooker allocation failed";
  if (!result_arena || !scratch_arena || !vkr_allocator_arena(&result) ||
      !vkr_allocator_arena(&scratch)) {
    goto cleanup;
  }
  VkrCollisionGeometry geometry = {0};
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  if (inspect) {
    error = "Collision asset read failed";
    if (!collision_read(&scratch, input, &bytes, &size) ||
        !vkr_collision_cooked_decode(&result, bytes, size, &geometry, &error)) {
      goto cleanup;
    }
  } else {
    if (!vkr_collision_import_gltf(
            &result, &scratch, string8_create((uint8_t *)input, strlen(input)),
            node, kind, &geometry, &error) ||
        !vkr_collision_cooked_encode(&scratch, &geometry, &bytes, &size,
                                     &error)) {
      goto cleanup;
    }
    error = "Collision asset atomic publication failed";
    if (!vkr_mesh_cooked_write_atomic(
            &scratch, string8_create((uint8_t *)output, strlen(output)), bytes,
            size)) {
      goto cleanup;
    }
  }
  printf(
      "collision_asset version=%u kind=%s vertices=%u triangles=%u bytes=%llu "
      "source_fingerprint=%016llx\n",
      VKR_COLLISION_COOKED_VERSION,
      geometry.kind == VKR_COLLISION_CONVEX_HULL ? "hull" : "mesh",
      geometry.vertex_count, geometry.index_count / 3, (unsigned long long)size,
      (unsigned long long)geometry.source_fingerprint);
  success = true_v;
cleanup:
  if (!success) {
    fprintf(stderr, "%s\n", error ? error : "Collision cook failed");
  }
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&result);
  if (scratch_arena) {
    arena_destroy(scratch_arena);
  }
  if (result_arena) {
    arena_destroy(result_arena);
  }
  return success ? 0 : 1;
}

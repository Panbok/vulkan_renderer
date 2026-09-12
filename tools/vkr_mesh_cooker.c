#include "assets/vkr_mesh_cook_source.h"
#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_json.h"
#include "defines.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKR_MESH_COOKER_MAX_LIGHT_RANGES 64u

static void vkr_mesh_cooker_print_usage(const char *program) {
  fprintf(stderr,
          "Inspect cooked dependencies: %s --inspect --input <mesh.vkb> "
          "--output <report.json>\n"
          "Source metadata variant: %s --input <mesh.vkb> --output <mesh.vkb> "
          "--source-patches <patches.json>\n"
          "Usage: %s --input <mesh.obj|mesh.gltf|mesh.glb> "
          "--output <mesh.vkb> [--light-range <definition-name>=<meters>]... "
          "[--bundle-root <absolute-directory> --import-id <id>]\n",
          program, program, program);
}

static bool8_t vkr_mesh_cooker_float_array(VkrJsonReader object,
                                           const char *name, float32_t *values,
                                           uint32_t count) {
  VkrJsonReader array = object;
  if (!vkr_json_find_array(&array, name)) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_json_next_array_element(&array) ||
        !vkr_json_parse_float(&array, &values[i]) || !isfinite(values[i])) {
      return false_v;
    }
  }
  return !vkr_json_next_array_element(&array);
}

static bool8_t vkr_mesh_cooker_apply_patches(const char *path,
                                             VkrAllocator *allocator,
                                             VkrMeshCookedDecoded *decoded) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  bool8_t success = false_v;
  if (fseek(file, 0, SEEK_END) != 0) {
    goto cleanup;
  }
  long size = ftell(file);
  if (size <= 0 || size > MB(16) || fseek(file, 0, SEEK_SET) != 0) {
    goto cleanup;
  }
  uint8_t *bytes = vkr_allocator_alloc(allocator, (uint64_t)size,
                                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!bytes || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    goto cleanup;
  }
  VkrJsonReader root = vkr_json_reader_create(bytes, size);
  int32_t version = 0;
  String8 fingerprint = {0};
  char expected[17];
  snprintf(expected, sizeof(expected), "%016llx",
           (unsigned long long)decoded->source.fingerprint);
  if (!vkr_json_get_int(&root, "version", &version) || version != 1 ||
      !vkr_json_get_string(&root, "source_fingerprint", &fingerprint) ||
      fingerprint.length != 16 ||
      MemCompare(fingerprint.str, expected, 16) != 0) {
    goto cleanup;
  }
  VkrJsonReader nodes = root;
  if (!vkr_json_find_array(&nodes, "nodes")) {
    goto cleanup;
  }
  bool8_t *seen = vkr_allocator_alloc(allocator, decoded->source.nodes.length,
                                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (decoded->source.nodes.length && !seen) {
    goto cleanup;
  }
  if (decoded->source.nodes.length) {
    MemZero(seen, decoded->source.nodes.length);
  }
  while (vkr_json_next_array_element(&nodes)) {
    VkrJsonReader patch = {0};
    int32_t index = -1;
    if (!vkr_json_enter_object(&nodes, &patch) ||
        !vkr_json_get_int(&patch, "index", &index) || index < 0 ||
        (uint64_t)index >= decoded->source.nodes.length || seen[index]) {
      goto cleanup;
    }
    seen[index] = true_v;
    VkrMeshSourceNode *node = &decoded->source.nodes.data[index];
    VkrJsonReader field = patch;
    if (vkr_json_find_field(&field, "position")) {
      Vec3 position = {0};
      Vec3 scale = {0};
      VkrQuat rotation = {0};
      if (!vkr_mesh_cooker_float_array(patch, "position", &position.x, 3) ||
          !vkr_mesh_cooker_float_array(patch, "rotation", &rotation.x, 4) ||
          !vkr_mesh_cooker_float_array(patch, "scale", &scale.x, 3) ||
          !isfinite(vec4_dot(rotation, rotation)) ||
          vec4_dot(rotation, rotation) < 0.000001f ||
          fabsf(scale.x) < 0.000001f || fabsf(scale.y) < 0.000001f ||
          fabsf(scale.z) < 0.000001f) {
        goto cleanup;
      }
      node->local = mat4_mul(
          mat4_from_vkr_quat_pos(vkr_quat_normalize(rotation), position),
          mat4_scale(scale));
    }
    field = patch;
    if (vkr_json_find_field(&field, "punctual")) {
      VkrJsonReader light = {0};
      int32_t kind = -1;
      if (!vkr_json_enter_object(&field, &light) ||
          !vkr_json_get_int(&light, "kind", &kind) || kind < 0 || kind > 3) {
        goto cleanup;
      }
      node->punctual.kind = (uint32_t)kind;
      if (kind) {
        if (!vkr_mesh_cooker_float_array(light, "color",
                                         &node->punctual.color.x, 3) ||
            !vkr_json_get_float(&light, "intensity",
                                &node->punctual.intensity) ||
            !vkr_json_get_float(&light, "range", &node->punctual.range) ||
            !vkr_json_get_float(&light, "inner_cone",
                                &node->punctual.inner_cone) ||
            !vkr_json_get_float(&light, "outer_cone",
                                &node->punctual.outer_cone)) {
          goto cleanup;
        }
      }
    }
    field = patch;
    if (vkr_json_find_field(&field, "visible")) {
      bool8_t visible = false_v;
      if (!vkr_json_parse_bool(&field, &visible)) {
        goto cleanup;
      }
      if (!visible) {
        node->mesh = UINT32_MAX;
        node->mesh_variant = UINT32_MAX;
        node->punctual.kind = 0;
      }
    }
  }
  success = true_v;
cleanup:
  fclose(file);
  return success;
}

/* Inspect through the runtime decoder so malformed streams and unsupported
 * artifact versions cannot enter a managed import manifest. */
static bool8_t vkr_mesh_cooker_inspect(const char *input, const char *output,
                                       const char *patches,
                                       VkrAllocator *allocator,
                                       VkrAllocator *scratch) {
  FILE *source = fopen(input, "rb");
  if (!source) {
    return false_v;
  }
  bool8_t success = false_v;
  FILE *destination = NULL;
  if (fseek(source, 0, SEEK_END) != 0) {
    goto cleanup;
  }
  long size = ftell(source);
  if (size <= 0 || (uint64_t)size > GB(4) || fseek(source, 0, SEEK_SET) != 0) {
    goto cleanup;
  }
  uint8_t *bytes = vkr_allocator_alloc(allocator, (uint64_t)size,
                                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!bytes || fread(bytes, 1, (size_t)size, source) != (size_t)size) {
    goto cleanup;
  }
  VkrMeshCookedDecoded decoded = {0};
  if (!vkr_mesh_cooked_decode(allocator, scratch, bytes, (uint64_t)size,
                              &decoded)) {
    goto cleanup;
  }
  if (patches) {
    uint8_t *variant = NULL;
    if (vkr_mesh_cooker_apply_patches(patches, scratch, &decoded) &&
        vkr_mesh_cooked_source_variant(scratch, bytes, (uint64_t)size,
                                       &decoded.source, &variant)) {
      String8 output_path = {.str = (uint8_t *)output,
                             .length = strlen(output)};
      success = vkr_mesh_cooked_write_atomic(scratch, output_path, variant,
                                             (uint64_t)size);
    }
    goto cleanup;
  }
  destination = fopen(output, "wb");
  if (!destination) {
    goto cleanup;
  }
  if (fprintf(destination,
              "{\"version\":1,\"fingerprint\":\"%016llx\",\"materials\":[",
              (unsigned long long)decoded.source.fingerprint) < 0) {
    goto cleanup;
  }
  bool8_t comma = false_v;
  for (uint64_t i = 0; i < decoded.ranges.length; ++i) {
    String8 path = decoded.ranges.data[i].material_name;
    if (!path.length) {
      continue;
    }
    if (fprintf(destination, "%s\"", comma ? "," : "") < 0) {
      goto cleanup;
    }
    comma = true_v;
    for (uint64_t c = 0; c < path.length; ++c) {
      uint8_t value = path.str[c];
      if (value < 32 || value == '\\' || value == '"') {
        if (fprintf(destination, "\\u%04x", value) < 0) {
          goto cleanup;
        }
      } else if (fputc(value, destination) == EOF) {
        goto cleanup;
      }
    }
    if (fputc('"', destination) == EOF) {
      goto cleanup;
    }
  }
  if (fputs("],\"nodes\":[", destination) == EOF) {
    goto cleanup;
  }
  for (uint64_t i = 0; i < decoded.source.nodes.length; ++i) {
    const VkrMeshSourceNode *node = &decoded.source.nodes.data[i];
    if (fprintf(
            destination,
            "%s{\"index\":%llu,\"parent\":%lld,\"in_scene\":%s,\"matrix\":[",
            i ? "," : "", (unsigned long long)i,
            node->parent == UINT32_MAX ? -1LL : (long long)node->parent,
            node->in_scene ? "true" : "false") < 0) {
      goto cleanup;
    }
    for (uint32_t component = 0; component < 16u; ++component) {
      if (fprintf(destination, "%s%.9g", component ? "," : "",
                  node->local.elements[component]) < 0) {
        goto cleanup;
      }
    }
    if (fputs("]}", destination) == EOF) {
      goto cleanup;
    }
  }
  success = fputs("]}\n", destination) != EOF;
cleanup:
  fclose(source);
  if (destination && fclose(destination) != 0) {
    success = false_v;
  }
  if (!success && destination) {
    (void)remove(output);
  }
  return success;
}

int main(int argc, char **argv) {
  const char *input = NULL;
  const char *output = NULL;
  const char *bundle_root = NULL;
  const char *import_id = NULL;
  bool8_t inspect = false_v;
  const char *source_patches = NULL;
  VkrSceneLightRangeOverride light_ranges[VKR_MESH_COOKER_MAX_LIGHT_RANGES] = {
      0};
  uint32_t light_range_count = 0u;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--inspect") == 0) {
      inspect = true_v;
    } else if (strcmp(argv[i], "--source-patches") == 0 && i + 1 < argc) {
      source_patches = argv[++i];
    } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
      input = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output = argv[++i];
    } else if (strcmp(argv[i], "--bundle-root") == 0 && i + 1 < argc) {
      bundle_root = argv[++i];
    } else if (strcmp(argv[i], "--import-id") == 0 && i + 1 < argc) {
      import_id = argv[++i];
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
          .light_name =
              string8_create((uint8_t *)value, (uint64_t)(separator - value)),
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
  if (!input || !output || ((bundle_root != NULL) != (import_id != NULL))) {
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
  bool8_t success =
      (inspect || source_patches)
          ? vkr_mesh_cooker_inspect(input, output, source_patches,
                                    &source_allocator, &scratch_allocator)
      : bundle_root
          ? vkr_mesh_cook_source_managed(
                input_path, output_path,
                string8_create((uint8_t *)bundle_root, strlen(bundle_root)),
                string8_create((uint8_t *)import_id, strlen(import_id)),
                light_ranges, light_range_count, &source_allocator,
                &scratch_allocator, &stats, &error)
          : vkr_mesh_cook_source_with_light_ranges(
                input_path, output_path, light_ranges, light_range_count,
                &source_allocator, &scratch_allocator, &stats, &error);
  if (success && !inspect && !source_patches) {
    printf("cooked=%llu decoded=%llu vertices=%u indices=%u ranges=%u "
           "output=%s\n",
           (unsigned long long)stats.cooked_bytes,
           (unsigned long long)stats.decoded_bytes, stats.vertex_count,
           stats.index_count, stats.range_count, output);
  } else if (!success) {
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

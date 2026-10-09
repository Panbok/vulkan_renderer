/*
 * `vkr_bakery materials`: the project's Custom material graphs (ADR-096).
 * Every `.mtg` under the content root's assets that lowers to the Custom tier
 * contributes its generated surface function and fragment entry points; the
 * engine's tiled shader source and these compile into one project library,
 * `metal/project_materials.metallib` of the shader catalog, with a manifest
 * of its functions. A graph change recompiles; an unchanged source does not.
 */

#include "vkr_bakery_commands.h"
#include "vkr_bakery_events.h"
#include "vkr_bakery_json.h"
#include "vkr_bakery_os.h"

#include "assets/vkr_material_codegen.h"
#include "assets/vkr_material_graph.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATERIALS_DEPTH 12u
#define MATERIALS_GRAPH_MAX 1024u
#define MATERIALS_FUNCTION_MAX 256u

typedef struct MaterialsFunction {
  char name[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  /* The first graph that generated it, for the manifest. */
  char graph[VKR_BAKERY_PATH_CAPACITY];
} MaterialsFunction;

typedef struct MaterialsScan {
  const char *root;
  char relative[VKR_BAKERY_PATH_CAPACITY];
  uint32_t depth;
  char (*graphs)[VKR_BAKERY_PATH_CAPACITY];
  uint32_t graph_count;
} MaterialsScan;

static bool8_t materials_visit(void *context, const char *name,
                               bool8_t is_directory) {
  MaterialsScan *scan = context;
  const size_t length = strlen(scan->relative);
  if (name[0] == '.' ||
      snprintf(scan->relative + length, sizeof(scan->relative) - length, "/%s",
               name) >= (int)(sizeof(scan->relative) - length)) {
    scan->relative[length] = '\0';
    return true_v;
  }
  if (is_directory && scan->depth < MATERIALS_DEPTH) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    if (vkr_bakery_path_join(absolute, sizeof(absolute), scan->root,
                             scan->relative)) {
      scan->depth++;
      (void)vkr_bakery_list_directory(absolute, materials_visit, scan);
      scan->depth--;
    }
  } else if (!is_directory) {
    const size_t total = strlen(scan->relative);
    if (total > 4u && strcmp(scan->relative + total - 4u, ".mtg") == 0 &&
        scan->graph_count < MATERIALS_GRAPH_MAX) {
      snprintf(scan->graphs[scan->graph_count++], VKR_BAKERY_PATH_CAPACITY,
               "%s", scan->relative + 1u);
    }
  }
  scan->relative[length] = '\0';
  return true_v;
}

static int materials_compare(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/* Runs `xcrun` with `arguments`, printing its output on failure. */
static bool8_t materials_xcrun(const char *const *arguments, uint32_t count) {
  char output[16384];
  int32_t exit_code = -1;
  if (!vkr_platform_process_capture("xcrun", arguments, count, NULL, output,
                                    sizeof(output), &exit_code) ||
      exit_code != 0) {
    char message[256];
    snprintf(message, sizeof(message), "xcrun %s failed (%d)", arguments[2],
             (int)exit_code);
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_MAT_FAILED, NULL, 0u, 0u, message,
                          output);
    return false_v;
  }
  return true_v;
}

int vkr_bakery_cmd_materials(VkrBakeryCli *cli) {
  if (!cli->shaders) {
    return vkr_bakery_usage("materials needs --shaders <catalog directory>");
  }
  char catalog[VKR_BAKERY_PATH_CAPACITY];
  char metal[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(cli->shaders, catalog, sizeof(catalog)) ||
      !vkr_bakery_path_join(metal, sizeof(metal), catalog, "metal")) {
    return vkr_bakery_usage("invalid --shaders path");
  }
  const char *root = cli->config.root;
  char paths[5][VKR_BAKERY_PATH_CAPACITY];
  static const char *const names[] = {
      "library.metal", "project_materials.metal", "project_materials.air",
      "project_materials.metallib", "project_materials.json"};
  for (uint32_t i = 0; i < ArrayCount(names); ++i) {
    (void)vkr_bakery_path_join(paths[i], sizeof(paths[i]), metal, names[i]);
  }

  Arena *arena = arena_create(MB(64), MB(1));
  VkrAllocator allocator = {.ctx = arena};
  MaterialsScan scan = {.root = root};
  scan.graphs = malloc(sizeof(*scan.graphs) * MATERIALS_GRAPH_MAX);
  MaterialsFunction *functions =
      malloc(sizeof(*functions) * MATERIALS_FUNCTION_MAX);
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  char *generated = NULL;
  uint64_t generated_length = 0u;
  int code = VKR_BAKERY_EXIT_OK;
  if (!arena || !vkr_allocator_arena(&allocator) || !scan.graphs ||
      !functions || !graph) {
    code = VKR_BAKERY_EXIT_ENVIRONMENT;
    goto cleanup;
  }
  snprintf(scan.relative, sizeof(scan.relative), "/assets");
  char assets[VKR_BAKERY_PATH_CAPACITY];
  if (vkr_bakery_path_join(assets, sizeof(assets), root, "assets")) {
    (void)vkr_bakery_list_directory(assets, materials_visit, &scan);
  }
  qsort(scan.graphs, scan.graph_count, sizeof(*scan.graphs), materials_compare);

  /* Each Custom graph's function, once. */
  uint32_t function_count = 0u;
  uint32_t failed = 0u;
  VkrAllocatorScope sources_scope = vkr_allocator_begin_scope(&allocator);
  for (uint32_t i = 0; i < scan.graph_count; ++i) {
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    uint8_t *json = NULL;
    uint64_t length = 0u;
    char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
    if (!vkr_bakery_path_join(absolute, sizeof(absolute), root,
                              scan.graphs[i]) ||
        !vkr_bakery_read_file(absolute, 0u, &json, &length)) {
      continue;
    }
    const bool8_t read = vkr_material_graph_read(
        (String8){.str = json, .length = length}, graph, error, sizeof(error));
    free(json);
    /* A graph the Standard tier lowers needs no function. */
    const String8 graph_path = string8_create_from_cstr(
        (const uint8_t *)scan.graphs[i], strlen(scan.graphs[i]));
    String8 definition = {0};
    VkrMaterialLowering tier = {0};
    VkrMaterialCustomBinding binding;
    VkrMaterialLowering lowering = {0};
    String8 source = {0};
    if (!read ||
        !vkr_material_graph_lower(graph, graph_path, NULL, 0u, &allocator,
                                  &definition, &tier) ||
        tier.tier != VKR_MATERIAL_TIER_CUSTOM ||
        !vkr_material_codegen_msl(graph, &allocator, &source, &binding,
                                  &lowering)) {
      continue;
    }
    uint32_t at = 0u;
    while (at < function_count &&
           strcmp(functions[at].name, lowering.function) != 0) {
      at++;
    }
    if (at < function_count) {
      continue;
    }
    if (function_count == MATERIALS_FUNCTION_MAX) {
      failed++;
      continue;
    }
    snprintf(functions[at].name, sizeof(functions[at].name), "%s",
             lowering.function);
    snprintf(functions[at].graph, sizeof(functions[at].graph), "%s",
             scan.graphs[i]);
    function_count++;
    String8 entries = {0};
    if (!vkr_material_codegen_msl_entries(lowering.function, &allocator,
                                          &entries)) {
      code = VKR_BAKERY_EXIT_FAILED;
      goto cleanup;
    }
    const uint64_t added =
        source.length + entries.length + strlen(scan.graphs[i]) + 64u;
    char *grown = realloc(generated, generated_length + added + 1u);
    if (!grown) {
      code = VKR_BAKERY_EXIT_ENVIRONMENT;
      goto cleanup;
    }
    generated = grown;
    generated_length += (uint64_t)snprintf(
        generated + generated_length, added + 1u, "\n// %s from %s\n%.*s\n%.*s",
        lowering.function, scan.graphs[i], (int)source.length,
        (const char *)source.str, (int)entries.length,
        (const char *)entries.str);
  }
  vkr_allocator_end_scope(&sources_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  if (function_count == 0u) {
    /* No Custom graph: no project library. Removing one changes what the
       renderer loads, so the summary counts it as work. */
    const bool8_t had_library = vkr_bakery_is_file(paths[3]);
    for (uint32_t i = 1; i < ArrayCount(names); ++i) {
      (void)vkr_bakery_remove_file(paths[i]);
    }
    vkr_bakery_print("vkr_bakery materials: no Custom graphs among %u\n",
                     scan.graph_count);
    vkr_bakery_event_summary(1u, 0u, 0u, had_library ? 0u : 1u,
                             vkr_bakery_events_elapsed_ms());
    goto cleanup;
  }

  uint8_t *library = NULL;
  uint64_t library_length = 0u;
  if (!vkr_bakery_read_file(paths[0], 0u, &library, &library_length)) {
    fprintf(stderr,
            "vkr_bakery materials: %s does not read; build the shaders "
            "first\n",
            paths[0]);
    code = VKR_BAKERY_EXIT_ENVIRONMENT;
    goto cleanup;
  }
  const uint64_t total = library_length + generated_length + 128u;
  char *text = malloc(total);
  if (!text) {
    free(library);
    code = VKR_BAKERY_EXIT_ENVIRONMENT;
    goto cleanup;
  }
  const int header =
      snprintf(text, total,
               "// Project materials (vkr_bakery materials): %u Custom "
               "graphs over the engine's tiled shaders.\n",
               function_count);
  MemCopy(text + header, library, library_length);
  MemCopy(text + header + library_length, generated, generated_length);
  const uint64_t text_length =
      (uint64_t)header + library_length + generated_length;
  free(library);

  /* An unchanged source with its library in place needs no compile. */
  uint8_t *previous = NULL;
  uint64_t previous_length = 0u;
  const bool8_t unchanged =
      vkr_bakery_read_file(paths[1], 0u, &previous, &previous_length) &&
      previous_length == text_length &&
      MemCompare(previous, text, text_length) == 0 &&
      vkr_bakery_is_file(paths[3]);
  free(previous);
  if (!unchanged) {
    /* The library repeats the engine's source, whose helpers serve entries
       it leaves out. The new library replaces the old one only once it
       linked, so a renderer never loads a partial file and a failed compile
       keeps the last good library; dropping the source makes the next run
       compile again. */
    char staged[VKR_BAKERY_PATH_CAPACITY];
    snprintf(staged, sizeof(staged), "%s.tmp", paths[3]);
    const char *compile[] = {"-sdk",     "macosx",       "metal",
                             "-c",       "-x",           "metal",
                             "-include", "metal_stdlib", "-Wno-unused-function",
                             paths[1],   "-o",           paths[2]};
    const char *link[] = {"-sdk", "macosx", "metallib", paths[2], "-o", staged};
    const bool8_t compiled =
        vkr_bakery_write_file_atomic(paths[1], text, text_length) &&
        materials_xcrun(compile, ArrayCount(compile)) &&
        materials_xcrun(link, ArrayCount(link)) &&
        vkr_bakery_rename(staged, paths[3], true_v);
    (void)vkr_bakery_remove_file(paths[2]);
    if (!compiled) {
      (void)vkr_bakery_remove_file(staged);
      (void)vkr_bakery_remove_file(paths[1]);
      free(text);
      code = VKR_BAKERY_EXIT_FAILED;
      goto cleanup;
    }
  }
  free(text);

  /* The manifest: each function and the graph that made it. */
  VkrBakeryJson *manifest = vkr_bakery_json_object(arena);
  VkrBakeryJson *listed = vkr_bakery_json_array(arena);
  vkr_bakery_json_set(arena, manifest, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, manifest, "budget",
                      vkr_bakery_json_int(arena, VKR_MATERIAL_CUSTOM_BUDGET));
  for (uint32_t i = 0; i < function_count; ++i) {
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, functions[i].name));
    vkr_bakery_json_set(arena, entry, "graph",
                        vkr_bakery_json_cstr(arena, functions[i].graph));
    vkr_bakery_json_append(listed, entry);
  }
  vkr_bakery_json_set(arena, manifest, "functions", listed);
  String8 manifest_text = {0};
  if (!vkr_bakery_json_write(arena, manifest, VKR_BAKERY_JSON_PRETTY,
                             &manifest_text) ||
      !vkr_bakery_write_file_atomic(paths[4], manifest_text.str,
                                    manifest_text.length)) {
    code = VKR_BAKERY_EXIT_FAILED;
    goto cleanup;
  }
  /* One action, cached when the library was up to date: the editor
     reloads the renderer's library only after a compile. */
  vkr_bakery_print(
      "vkr_bakery materials: %u Custom graphs%s, %s%s\n", function_count,
      function_count > VKR_MATERIAL_CUSTOM_BUDGET ? " (over the budget)" : "",
      unchanged ? "up to date: " : "compiled ", paths[3]);
  vkr_bakery_event_summary(1u, 0u, 0u, unchanged ? 1u : 0u,
                           vkr_bakery_events_elapsed_ms());
  if (function_count > VKR_MATERIAL_CUSTOM_BUDGET || failed) {
    char message[160];
    snprintf(message, sizeof(message), "%u Custom graphs over a budget of %u%s",
             function_count + failed, VKR_MATERIAL_CUSTOM_BUDGET,
             failed ? "; the library left the rest out" : "");
    vkr_bakery_event_diag(0u, VKR_BAKERY_DIAG_MAT_OVER_BUDGET, NULL, 0u, 0u,
                          message,
                          "Merge graphs that differ only in constants by "
                          "exposing those constants as parameters.");
  }

cleanup:
  free(generated);
  free(graph);
  free(functions);
  free(scan.graphs);
  if (arena) {
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
  }
  return code;
}

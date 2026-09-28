#include "filesystem/filesystem.h"
#include "vkr_bakery_internal.h"

#include "platform/vkr_platform.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Legacy cooker command lines, linked from their own libraries. Each keeps its
 * original flags so existing recipes and scripts can call it verbatim. */
#if VKR_BAKERY_WITH_COOKERS
int vkr_mesh_cooker_tool_main(int argc, char **argv);
int vkr_animation_cooker_tool_main(int argc, char **argv);
int vkr_collision_cooker_tool_main(int argc, char **argv);
int vkr_font_cooker_tool_main(int argc, char **argv);
int vkr_vkt_packer_tool_main(int argc, char **argv);
int vkr_hdr_cube_packer_tool_main(int argc, char **argv);
int vkr_dfg_cooker_tool_main(int argc, char **argv);
int vkr_sheen_cooker_tool_main(int argc, char **argv);
int vkr_anisotropy_cooker_tool_main(int argc, char **argv);
int vkr_diffuse_baker_tool_main(int argc, char **argv);
#endif

vkr_internal int vkr_bakery_tool_exec(int argc, char **argv);

vkr_internal const VkrBakeryTool vkr_bakery_tools[] = {
#if VKR_BAKERY_WITH_COOKERS
    {"mesh", vkr_mesh_cooker_tool_main,
     "Mesh cooker: --input <gltf|glb|obj> --output <vkb> [--light-range "
     "name=r] [--inspect]"},
    {"animation", vkr_animation_cooker_tool_main,
     "Animation cooker: --input <gltf> --output <vka> [--inspect]"},
    {"collision", vkr_collision_cooker_tool_main,
     "Collision cooker: --input <gltf> --output <vkc> --kind hull|mesh"},
    {"font", vkr_font_cooker_tool_main,
     "Font cooker: --config <fontcfg> [--output <vkfa>] [--force]"},
    {"texture", vkr_vkt_packer_tool_main,
     "Texture packer: --input-dir <dir> | --output <vkt> --type <shape> "
     "--layer <image>..."},
    {"hdr-cube", vkr_hdr_cube_packer_tool_main,
     "HDR cube packer for reflection probes."},
    {"dfg", vkr_dfg_cooker_tool_main, "GGX DFG table generator: <output.inc>"},
    {"sheen", vkr_sheen_cooker_tool_main,
     "Charlie sheen table generator: <output.inc>"},
    {"anisotropy", vkr_anisotropy_cooker_tool_main,
     "Anisotropic LTC table generator: <output.inc>"},
    {"diffuse-baker", vkr_diffuse_baker_tool_main,
     "CPU diffuse-volume baker (used by the diffuse_volume producer)."},
#endif
    {"exec", vkr_bakery_tool_exec,
     "Runs an external command and records its CPU time and peak memory."},
};

const VkrBakeryTool *vkr_bakery_tool_find(const char *name) {
  for (uint32_t i = 0u; i < ArrayCount(vkr_bakery_tools); ++i) {
    if (strcmp(vkr_bakery_tools[i].name, name) == 0) {
      return &vkr_bakery_tools[i];
    }
  }
  return NULL;
}

uint32_t vkr_bakery_tool_count(void) { return ArrayCount(vkr_bakery_tools); }

const VkrBakeryTool *vkr_bakery_tool_at(uint32_t index) {
  return index < ArrayCount(vkr_bakery_tools) ? &vkr_bakery_tools[index] : NULL;
}

// =============================================================================
// Usage report
// =============================================================================

/* The scheduler reads CPU time and peak memory of each tool process from this
 * file. atexit covers tools that call exit() directly. */
vkr_internal void vkr_bakery_tool_write_usage(void) {
  const char *path = getenv("VKR_BAKERY_USAGE_PATH");
  if (!path || !path[0]) {
    return;
  }
  uint64_t cpu_ms = 0u;
  uint64_t peak = 0u;
  uint64_t child_cpu_ms = 0u;
  uint64_t child_peak = 0u;
  (void)vkr_bakery_self_usage(&cpu_ms, &peak);
  (void)vkr_bakery_children_usage(&child_cpu_ms, &child_peak);
  char text[256];
  const int length = snprintf(
      text, sizeof(text),
      "{\"cpu_ms\":%llu,\"peak_rss_bytes\":%llu,\"children_cpu_ms\":%llu,"
      "\"children_peak_rss_bytes\":%llu}\n",
      (unsigned long long)cpu_ms, (unsigned long long)peak,
      (unsigned long long)child_cpu_ms, (unsigned long long)child_peak);
  FILE *file = file_fopen(path, "wb");
  if (file) {
    fwrite(text, 1u, (size_t)length, file);
    fclose(file);
  }
}

// =============================================================================
// exec
// =============================================================================

vkr_internal VkrAtomicBool vkr_bakery_exec_cancel = false;

vkr_internal void vkr_bakery_exec_signal(int signal_number) {
  (void)signal_number;
  vkr_atomic_bool_store(&vkr_bakery_exec_cancel, true_v,
                        VKR_MEMORY_ORDER_RELAXED);
}

vkr_internal bool8_t vkr_bakery_exec_cancelled(void *context) {
  (void)context;
  return vkr_atomic_bool_load(&vkr_bakery_exec_cancel, VKR_MEMORY_ORDER_RELAXED)
             ? true_v
             : false_v;
}

/* argv: [label, executable, arguments...]. Output is inherited, so the
 * caller's capture files receive it directly. */
vkr_internal int vkr_bakery_tool_exec(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: vkr_bakery tool exec <executable> [args...]\n");
    return 2;
  }
  signal(SIGINT, vkr_bakery_exec_signal);
  signal(SIGTERM, vkr_bakery_exec_signal);
  const VkrPlatformProcessConfig config = {
      .executable = argv[1],
      .arguments = (const char *const *)(argv + 2),
      .argument_count = (uint32_t)(argc - 2),
      .termination_grace_ms = 250u,
      .is_cancelled = vkr_bakery_exec_cancelled,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  if (!vkr_platform_process_run(&config, &exit_code, &timed_out)) {
    fprintf(stderr, "vkr_bakery: unable to run %s\n", argv[1]);
    return 127;
  }
  return exit_code;
}

// =============================================================================
// Dispatch
// =============================================================================

int vkr_bakery_tool_main(int argc, char **argv) {
  /* argv[0] is "tool", argv[1] the tool name. */
  if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
      strcmp(argv[1], "-h") == 0) {
    printf("usage: vkr_bakery tool <name> [arguments]\n\n");
    for (uint32_t i = 0u; i < ArrayCount(vkr_bakery_tools); ++i) {
      printf("  %-14s %s\n", vkr_bakery_tools[i].name,
             vkr_bakery_tools[i].summary);
    }
    return argc < 2 ? 2 : 0;
  }
  const VkrBakeryTool *tool = vkr_bakery_tool_find(argv[1]);
  if (!tool) {
    fprintf(stderr,
            "error VKR-CLI-0001: unknown tool '%s'; run `vkr_bakery "
            "tool --help`\n",
            argv[1]);
    return 2;
  }
  atexit(vkr_bakery_tool_write_usage);
  char program[64];
  (void)snprintf(program, sizeof(program), "vkr_bakery tool %s", tool->name);
  char **tool_argv = (char **)calloc((size_t)argc, sizeof(char *));
  if (!tool_argv) {
    return 1;
  }
  tool_argv[0] = program;
  for (int i = 2; i < argc; ++i) {
    tool_argv[i - 1] = argv[i];
  }
  fflush(stdout);
  const int code = tool->entry(argc - 1, tool_argv);
  fflush(stdout);
  fflush(stderr);
  free(tool_argv);
  return code;
}

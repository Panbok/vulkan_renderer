/**
 * @file vkr_harness_main.c
 * @brief Command-line entry point.
 *
 * `vkr_harness profile ...` is the user-facing command; `--child-profile` is
 * the internal re-entry the parent uses to launch one isolated repetition and
 * is not part of the documented interface.
 */
#include "vkr_harness_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <limits.h>
#endif

static const char *vkr_harness_option(int argc, char **argv, const char *name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) {
      return argv[i + 1];
    }
  }
  return NULL;
}

static bool8_t vkr_harness_flag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/**
 * A child writes into a directory chosen by its parent, so that directory is
 * re-validated here rather than trusted from the command line.
 */
static bool8_t vkr_harness_child_run_dir_valid(const char *repo_root,
                                               const char *run_dir) {
  char artifact_root[VKR_HARNESS_PATH_MAX];
  snprintf(artifact_root, sizeof(artifact_root), "%s/%s", repo_root,
           VKR_HARNESS_ARTIFACT_ROOT);
  return vkr_harness_existing_path_is_below(artifact_root, run_dir);
}

int main(int argc, char **argv) {
  const bool8_t child = argc > 1 && strcmp(argv[1], "--child-profile") == 0;
  const bool8_t profile_command = argc > 1 && strcmp(argv[1], "profile") == 0;
  if (!child && !profile_command) {
    fprintf(stderr, "usage: vkr_harness profile --case <relative.case.json> "
                    "--profile <relative.profile.json>\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  const char *case_path = vkr_harness_option(argc, argv, "--case");
  const char *profile_path = vkr_harness_option(argc, argv, "--profile");
  const char *repo_root = vkr_harness_option(argc, argv, "--repo-root");
  const char *run_dir = vkr_harness_option(argc, argv, "--run-dir");
  if (!repo_root) {
    repo_root = PROJECT_SOURCE_DIR;
  }
  if (!case_path || !profile_path || (child && !run_dir)) {
    fprintf(stderr, "Required harness arguments are missing\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  /* The parent re-executes this binary by path, so argv[0] must be resolved. */
  char executable[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(argv[0], executable)) {
    fprintf(stderr, "Unable to resolve the harness executable path\n");
    return VKR_HARNESS_EXIT_ERROR;
  }
  if (child) {
    if (!vkr_harness_child_run_dir_valid(repo_root, run_dir)) {
      fprintf(stderr, "Child run directory escapes the artifact root\n");
      return VKR_HARNESS_EXIT_INVALID;
    }
    return vkr_harness_child_run(executable, repo_root, case_path, profile_path,
                                 run_dir,
                                 vkr_harness_flag(argc, argv, "--prewarm"));
  }
  return vkr_harness_profile_run(executable, repo_root, case_path,
                                 profile_path);
}

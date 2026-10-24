#pragma once

#include "vkr_depot_sync.h"

/* Merges two histories of a branch (docs/proposals/depot-git-model.md,
 * "Merge"). Each path is decided against the merge base: a path one side
 * changed takes that side, and a path both sides changed to different
 * content is a conflict.
 *
 * `.vkrdepotmerge` at the working copy's root names units, groups of paths
 * that merge together through a driver, one per line:
 *
 *   unit <driver> <anchor pattern> [member patterns relative to the anchor]
 *
 * with the patterns of `.vkrdepotignore` (the proposal lists the `scene` and
 * `project` rules a project writes). A unit is the anchor's folder: the anchor
 * file and the files below it a member pattern matches. When both sides changed
 * paths of one unit, the unit goes to its driver whole, with each side's member
 * files; when the driver is missing or fails, the whole unit is a conflict. A
 * driver name is a key into drivers the caller supplies, never a command, so a
 * pulled file cannot make the depot run anything. */

#define VKR_DEPOT_MERGE_FILE ".vkrdepotmerge"
#define VKR_DEPOT_MERGE_RULES_MAX 16u
#define VKR_DEPOT_MERGE_MEMBERS_MAX 8u
#define VKR_DEPOT_MERGE_DRIVER_NAME 32u
#define VKR_DEPOT_MERGE_PATTERN 256u

typedef struct VkrDepotMergeRule {
  char driver[VKR_DEPOT_MERGE_DRIVER_NAME];
  char anchor[VKR_DEPOT_MERGE_PATTERN];
  char members[VKR_DEPOT_MERGE_MEMBERS_MAX][VKR_DEPOT_MERGE_PATTERN];
  uint32_t member_count;
} VkrDepotMergeRule;

typedef struct VkrDepotMergeRules {
  VkrDepotMergeRule rules[VKR_DEPOT_MERGE_RULES_MAX];
  uint32_t count;
} VkrDepotMergeRules;

/* Parses `.vkrdepotmerge`; lines it cannot read are skipped. */
void vkr_depot_merge_rules_parse(const char *text, uint64_t size,
                                 VkrDepotMergeRules *out_rules);

/* The rule whose unit holds `path`, and the unit's folder; false when no
   unit holds it. */
bool8_t vkr_depot_merge_unit_of(const VkrDepotMergeRules *rules,
                                const char *path,
                                const VkrDepotMergeRule **out_rule,
                                char *out_folder, uint32_t capacity);

typedef struct VkrDepotMergeUnitRequest {
  const char *driver;
  /* The working copy's root. */
  const char *workspace_root;
  /* The anchor's repository path and its folder. */
  const char *anchor;
  const char *folder;
  /* Each side's member files, at their paths relative to `folder`. */
  const char *base_root;
  const char *mine_root;
  const char *theirs_root;
  /* Where the driver writes the merged files, relative to `folder`. */
  const char *output_root;
} VkrDepotMergeUnitRequest;

typedef enum VkrDepotMergeUnitStatus {
  VKR_DEPOT_MERGE_UNIT_MERGED = 0,
  VKR_DEPOT_MERGE_UNIT_CONFLICT,
  VKR_DEPOT_MERGE_UNIT_FAILED,
} VkrDepotMergeUnitStatus;

/* A driver merges one unit. On MERGED it lists in `out_changes` the paths,
   relative to the folder, it wrote under `output_root` (ADDED or MODIFIED)
   and those to remove (DELETED); on CONFLICT it lists the conflicting paths
   in `out_conflicts`. `detail` says why on CONFLICT or FAILED. */
typedef VkrDepotMergeUnitStatus (*VkrDepotMergeDriver)(
    void *context, const VkrDepotMergeUnitRequest *request,
    VkrDepotChangeList *out_changes, VkrDepotChangeList *out_conflicts,
    char *detail, uint32_t capacity);

/* `vkr_bakery` for the merge driver: `override` when given, else
   `$VKR_BAKERY`, else the one beside this executable. */
bool8_t vkr_depot_bakery_path(const char *override, char *out,
                              uint32_t capacity);

/* The driver that runs `vkr_bakery project --request --result` with the
   operation `merge_scene_unit` for the `scene` driver and
   `merge_project_document` for `project`. `context` is the path of
   vkr_bakery. */
VkrDepotMergeUnitStatus vkr_depot_merge_bakery_driver(
    void *context, const VkrDepotMergeUnitRequest *request,
    VkrDepotChangeList *out_changes, VkrDepotChangeList *out_conflicts,
    char *detail, uint32_t capacity);

typedef struct VkrDepotMergeConfig {
  /* NULL: every unit both sides changed is a conflict. */
  VkrDepotMergeDriver driver;
  void *driver_context;
  /* What a conflict left after the drivers becomes: FAIL changes nothing
     and lists them. */
  VkrDepotConflictPolicy policy;
  /* Fetches merge-base files the store lacks (history arrives without its
     files); NULL when the store holds them. */
  VkrDepotClient *client;
  /* The merge commit's message. */
  const char *message;
} VkrDepotMergeConfig;

typedef enum VkrDepotMergeStatus {
  VKR_DEPOT_MERGE_UP_TO_DATE = 0,
  VKR_DEPOT_MERGE_FAST_FORWARD,
  VKR_DEPOT_MERGE_MERGED,
  /* Conflicts remain under FAIL; nothing changed. */
  VKR_DEPOT_MERGE_CONFLICTS,
  VKR_DEPOT_MERGE_FAILED,
} VkrDepotMergeStatus;

typedef struct VkrDepotMergeResult {
  VkrDepotMergeStatus status;
  /* The merge commit, or the commit fast-forwarded to. */
  VkrDepotId commit;
  /* Paths that took the other side's content, and units a driver merged. */
  uint32_t taken;
  uint32_t units_merged;
  /* Conflicting paths; a unit's conflict lists its anchor. The caller
     frees it. */
  VkrDepotChangeList conflicts;
} VkrDepotMergeResult;

/* Merges `theirs` into the working copy's branch and commits the result
   with the branch's commit and `theirs` as parents. The working copy must
   hold every file (not sparse) and have no local changes. True for every
   status but FAILED. */
bool8_t vkr_depot_merge(VkrDepotWorkspace *workspace, const VkrDepotId *theirs,
                        const VkrDepotMergeConfig *config,
                        VkrDepotMergeResult *out_result, char *error,
                        uint32_t capacity);

/* vkr_depot_pull, then, when both sides moved the branch, a merge of the
   fetched commit through a connection of its own, which fetches merge-base
   files the store lacks. `config->client` is ignored; a NULL message names
   the remote branch. `out_merge` is zero unless a merge ran. */
bool8_t vkr_depot_pull_merge(VkrAllocator *allocator,
                             VkrDepotWorkspace *workspace, const char *remote,
                             const VkrDepotMergeConfig *config,
                             VkrDepotPullResult *out_pull,
                             VkrDepotMergeResult *out_merge, char *error,
                             uint32_t capacity);

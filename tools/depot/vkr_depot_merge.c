#include "vkr_depot_merge.h"

#include "core/vkr_json.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Units one merge sends to drivers at most. */
#define MERGE_UNITS_MAX 64u
/* Bytes of a driver's result document. */
#define MERGE_RESULT_MAX (4u * 1024u * 1024u)

// =============================================================================
// Rules and units
// =============================================================================

/* Copies the next space-separated word of `*cursor` into `out`. */
static bool8_t merge_word(const char **cursor, const char *end, char *out,
                          uint32_t capacity) {
  const char *at = *cursor;
  while (at < end && (*at == ' ' || *at == '\t')) {
    ++at;
  }
  const char *start = at;
  while (at < end && *at != ' ' && *at != '\t') {
    ++at;
  }
  const size_t length = (size_t)(at - start);
  *cursor = at;
  if (length == 0u || length >= capacity) {
    return false_v;
  }
  MemCopy(out, start, length);
  out[length] = '\0';
  return true_v;
}

void vkr_depot_merge_rules_parse(const char *text, uint64_t size,
                                 VkrDepotMergeRules *out_rules) {
  MemZero(out_rules, sizeof(*out_rules));
  const char *line = text;
  const char *end = text + size;
  while (line < end && out_rules->count < VKR_DEPOT_MERGE_RULES_MAX) {
    const char *next = memchr(line, '\n', (size_t)(end - line));
    const char *line_end = next ? next : end;
    if (line_end > line && line_end[-1] == '\r') {
      --line_end;
    }
    const char *cursor = line;
    char keyword[8];
    VkrDepotMergeRule rule = {0};
    if (line[0] != '#' &&
        merge_word(&cursor, line_end, keyword, sizeof(keyword)) &&
        strcmp(keyword, "unit") == 0 &&
        merge_word(&cursor, line_end, rule.driver, sizeof(rule.driver)) &&
        merge_word(&cursor, line_end, rule.anchor, sizeof(rule.anchor)) &&
        !strchr(rule.anchor, '\\') && rule.anchor[0] != '/') {
      while (rule.member_count < VKR_DEPOT_MERGE_MEMBERS_MAX &&
             merge_word(&cursor, line_end, rule.members[rule.member_count],
                        VKR_DEPOT_MERGE_PATTERN)) {
        rule.member_count++;
      }
      out_rules->rules[out_rules->count++] = rule;
    }
    line = next ? next + 1 : end;
  }
}

bool8_t vkr_depot_merge_unit_of(const VkrDepotMergeRules *rules,
                                const char *path,
                                const VkrDepotMergeRule **out_rule,
                                char *out_folder, uint32_t capacity) {
  for (uint32_t r = 0u; r < rules->count; ++r) {
    const VkrDepotMergeRule *rule = &rules->rules[r];
    const char *slash = strrchr(rule->anchor, '/');
    const char *anchor_name = slash ? slash + 1 : rule->anchor;
    /* Each folder above the path could be the unit's: the anchor beside it
       must match the anchor pattern, and the rest of the path must be the
       anchor or match a member pattern. */
    for (const char *at = path + strlen(path); at >= path; --at) {
      if (at != path && *at != '/') {
        continue;
      }
      const size_t folder_length = (size_t)(at - path);
      const char *rest = at == path ? path : at + 1;
      char candidate[VKR_DEPOT_PATH_CAPACITY];
      const int written =
          snprintf(candidate, sizeof(candidate), "%.*s%s%s", (int)folder_length,
                   path, folder_length ? "/" : "", anchor_name);
      if (written <= 0 || (uint32_t)written >= sizeof(candidate) ||
          !vkr_depot_ignore_match(rule->anchor, candidate, false_v)) {
        continue;
      }
      bool8_t member = strcmp(rest, anchor_name) == 0;
      for (uint32_t m = 0u; m < rule->member_count && !member; ++m) {
        member = vkr_depot_ignore_match(rule->members[m], rest, false_v);
      }
      if (member && folder_length < capacity) {
        *out_rule = rule;
        snprintf(out_folder, capacity, "%.*s", (int)folder_length, path);
        return true_v;
      }
    }
  }
  return false_v;
}

// =============================================================================
// Decisions
// =============================================================================

typedef enum MergeAction {
  MERGE_KEEP = 0,
  MERGE_TAKE,
  MERGE_CONFLICT,
} MergeAction;

typedef struct MergePath {
  const char *path;
  const VkrDepotIndexEntry *base;
  const VkrDepotIndexEntry *mine;
  const VkrDepotIndexEntry *theirs;
  MergeAction action;
  /* Index into the merge's units, or UINT32_MAX. */
  uint32_t unit;
} MergePath;

typedef struct MergeUnit {
  const VkrDepotMergeRule *rule;
  char folder[VKR_DEPOT_PATH_CAPACITY];
  bool8_t mine_changed;
  bool8_t theirs_changed;
  VkrDepotMergeUnitStatus status;
  VkrDepotChangeList changes;
  char output[VKR_DEPOT_PATH_CAPACITY];
} MergeUnit;

typedef struct MergeState {
  VkrDepotWorkspace *workspace;
  const VkrDepotMergeConfig *config;
  VkrDepotFileList base;
  VkrDepotFileList mine;
  VkrDepotFileList theirs;
  MergePath *paths;
  uint32_t path_count;
  MergeUnit *units;
  uint32_t unit_count;
  VkrDepotMergeRules rules;
  char temp_root[VKR_DEPOT_PATH_CAPACITY];
} MergeState;

static bool8_t merge_same(const VkrDepotIndexEntry *a,
                          const VkrDepotIndexEntry *b) {
  if (!a || !b) {
    return a == b;
  }
  return vkr_depot_id_equal(&a->blob, &b->blob);
}

static int merge_path_compare(const void *a, const void *b) {
  return strcmp(((const MergePath *)a)->path, ((const MergePath *)b)->path);
}

/* One entry per path any side holds, sorted, with its decision. */
static bool8_t merge_collect_paths(MergeState *state) {
  const VkrDepotFileList *lists[3] = {&state->base, &state->mine,
                                      &state->theirs};
  uint32_t total = 0u;
  for (uint32_t i = 0u; i < 3u; ++i) {
    total += lists[i]->count;
  }
  state->paths = calloc(Max(total, 1u), sizeof(MergePath));
  if (!state->paths) {
    return false_v;
  }
  for (uint32_t i = 0u; i < 3u; ++i) {
    for (uint32_t k = 0u; k < lists[i]->count; ++k) {
      state->paths[state->path_count++].path = lists[i]->entries[k].path;
    }
  }
  qsort(state->paths, state->path_count, sizeof(MergePath), merge_path_compare);
  uint32_t unique = 0u;
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    if (unique &&
        strcmp(state->paths[unique - 1u].path, state->paths[i].path) == 0) {
      continue;
    }
    state->paths[unique++] = state->paths[i];
  }
  state->path_count = unique;

  for (uint32_t i = 0u; i < state->path_count; ++i) {
    MergePath *entry = &state->paths[i];
    entry->base = vkr_depot_file_list_find(&state->base, entry->path);
    entry->mine = vkr_depot_file_list_find(&state->mine, entry->path);
    entry->theirs = vkr_depot_file_list_find(&state->theirs, entry->path);
    entry->unit = UINT32_MAX;
    if (merge_same(entry->mine, entry->theirs) ||
        merge_same(entry->theirs, entry->base)) {
      entry->action = MERGE_KEEP;
    } else if (merge_same(entry->mine, entry->base)) {
      entry->action = MERGE_TAKE;
    } else {
      entry->action = MERGE_CONFLICT;
    }
  }
  return true_v;
}

/* Groups the paths of units: a unit both sides changed goes to its
   driver whole. */
static bool8_t merge_collect_units(MergeState *state) {
  state->units = calloc(MERGE_UNITS_MAX, sizeof(MergeUnit));
  if (!state->units) {
    return false_v;
  }
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    MergePath *entry = &state->paths[i];
    const bool8_t mine_changed = !merge_same(entry->mine, entry->base);
    const bool8_t theirs_changed = !merge_same(entry->theirs, entry->base);
    const VkrDepotMergeRule *rule = NULL;
    char folder[VKR_DEPOT_PATH_CAPACITY];
    if ((!mine_changed && !theirs_changed) ||
        !vkr_depot_merge_unit_of(&state->rules, entry->path, &rule, folder,
                                 sizeof(folder))) {
      continue;
    }
    uint32_t unit = UINT32_MAX;
    for (uint32_t u = 0u; u < state->unit_count && unit == UINT32_MAX; ++u) {
      if (state->units[u].rule == rule &&
          strcmp(state->units[u].folder, folder) == 0) {
        unit = u;
      }
    }
    if (unit == UINT32_MAX) {
      if (state->unit_count == MERGE_UNITS_MAX) {
        return false_v;
      }
      unit = state->unit_count++;
      state->units[unit].rule = rule;
      snprintf(state->units[unit].folder, sizeof(state->units[unit].folder),
               "%s", folder);
    }
    state->units[unit].mine_changed |= mine_changed;
    state->units[unit].theirs_changed |= theirs_changed;
  }
  /* Every path of a unit both sides changed, changed or not, belongs to
     it; the others keep their own decision. */
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    MergePath *entry = &state->paths[i];
    const VkrDepotMergeRule *rule = NULL;
    char folder[VKR_DEPOT_PATH_CAPACITY];
    if (!vkr_depot_merge_unit_of(&state->rules, entry->path, &rule, folder,
                                 sizeof(folder))) {
      continue;
    }
    for (uint32_t u = 0u; u < state->unit_count; ++u) {
      const MergeUnit *unit = &state->units[u];
      if (unit->rule == rule && unit->mine_changed && unit->theirs_changed &&
          strcmp(unit->folder, folder) == 0) {
        entry->unit = u;
      }
    }
  }
  return true_v;
}

// =============================================================================
// Units through the driver
// =============================================================================

static const char *merge_relative(const char *path, const char *folder) {
  const size_t length = strlen(folder);
  return length ? path + length + 1u : path;
}

/* Writes `entry`'s file into `root` at its path relative to the folder. */
static bool8_t merge_write_side(const VkrDepotStore *store, const char *root,
                                const char *relative,
                                const VkrDepotIndexEntry *entry) {
  if (!entry) {
    return true_v;
  }
  char path[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), root, relative)) {
    return false_v;
  }
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  return vkr_bakery_make_directories(directory) &&
         vkr_depot_write_blob(store, &entry->blob, path);
}

/* Fetches the merge-base files of units that the store lacks. */
static bool8_t merge_fetch_base(MergeState *state) {
  VkrDepotId *missing = malloc(Max(state->path_count, 1u) * sizeof(VkrDepotId));
  uint32_t count = 0u;
  if (!missing) {
    return false_v;
  }
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    const MergePath *entry = &state->paths[i];
    if (entry->unit != UINT32_MAX && entry->base &&
        !vkr_depot_store_has(&state->workspace->store, &entry->base->blob)) {
      missing[count++] = entry->base->blob;
    }
  }
  VkrDepotTransferStats stats = {0};
  const bool8_t ok =
      count == 0u || (state->config->client &&
                      vkr_depot_client_fetch_files(state->config->client,
                                                   missing, count, &stats));
  free(missing);
  return ok;
}

static void merge_run_unit(MergeState *state, uint32_t index,
                           VkrDepotMergeResult *out_result) {
  MergeUnit *unit = &state->units[index];
  unit->status = VKR_DEPOT_MERGE_UNIT_FAILED;
  char root[VKR_DEPOT_PATH_CAPACITY];
  char sides[3][VKR_DEPOT_PATH_CAPACITY];
  static const char *const names[] = {"base", "mine", "theirs"};
  snprintf(root, sizeof(root), "%s/unit-%u", state->temp_root, index);
  bool8_t ok = vkr_bakery_remove_tree(root);
  for (uint32_t s = 0u; ok && s < 3u; ++s) {
    ok = vkr_bakery_path_join(sides[s], sizeof(sides[s]), root, names[s]) &&
         vkr_bakery_make_directories(sides[s]);
  }
  ok = ok &&
       vkr_bakery_path_join(unit->output, sizeof(unit->output), root,
                            "output") &&
       vkr_bakery_make_directories(unit->output);
  char anchor[VKR_DEPOT_PATH_CAPACITY] = {0};
  for (uint32_t i = 0u; ok && i < state->path_count; ++i) {
    const MergePath *entry = &state->paths[i];
    if (entry->unit != index) {
      continue;
    }
    const char *relative = merge_relative(entry->path, unit->folder);
    const char *slash = strrchr(unit->rule->anchor, '/');
    if (strcmp(relative, slash ? slash + 1 : unit->rule->anchor) == 0) {
      snprintf(anchor, sizeof(anchor), "%s", entry->path);
    }
    const VkrDepotIndexEntry *side[3] = {entry->base, entry->mine,
                                         entry->theirs};
    for (uint32_t s = 0u; ok && s < 3u; ++s) {
      ok = merge_write_side(&state->workspace->store, sides[s], relative,
                            side[s]);
    }
  }
  if (!ok || !state->config->driver) {
    return;
  }
  const VkrDepotMergeUnitRequest request = {.driver = unit->rule->driver,
                                            .workspace_root =
                                                state->workspace->root,
                                            .anchor = anchor,
                                            .folder = unit->folder,
                                            .base_root = sides[0],
                                            .mine_root = sides[1],
                                            .theirs_root = sides[2],
                                            .output_root = unit->output};
  VkrDepotChangeList conflicts = {0};
  char detail[256] = {0};
  unit->status =
      state->config->driver(state->config->driver_context, &request,
                            &unit->changes, &conflicts, detail, sizeof(detail));
  vkr_depot_change_list_free(&conflicts);
  if (unit->status == VKR_DEPOT_MERGE_UNIT_MERGED) {
    out_result->units_merged++;
  }
}

// =============================================================================
// Applying the result
// =============================================================================

static bool8_t merge_conflict_add(VkrDepotChangeList *list, const char *path) {
  VkrDepotChangeEntry *entries = realloc(
      list->entries, (size_t)(list->count + 1u) * sizeof(VkrDepotChangeEntry));
  if (!entries) {
    return false_v;
  }
  list->entries = entries;
  const size_t length = strlen(path);
  char *copy = malloc(length + 1u);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, path, length + 1u);
  list->entries[list->count++] =
      (VkrDepotChangeEntry){.path = copy, .change = VKR_DEPOT_CHANGE_MODIFIED};
  list->capacity = list->count;
  return true_v;
}

/* The paths left in conflict: unresolved paths outside units, and the
   anchor of each unit no driver merged. */
static bool8_t merge_list_conflicts(const MergeState *state,
                                    VkrDepotChangeList *out) {
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    const MergePath *entry = &state->paths[i];
    if (entry->unit == UINT32_MAX && entry->action == MERGE_CONFLICT &&
        !merge_conflict_add(out, entry->path)) {
      return false_v;
    }
  }
  for (uint32_t u = 0u; u < state->unit_count; ++u) {
    const MergeUnit *unit = &state->units[u];
    if (!unit->mine_changed || !unit->theirs_changed ||
        unit->status == VKR_DEPOT_MERGE_UNIT_MERGED) {
      continue;
    }
    char anchor[VKR_DEPOT_PATH_CAPACITY];
    const char *slash = strrchr(unit->rule->anchor, '/');
    snprintf(anchor, sizeof(anchor), "%s%s%s", unit->folder,
             unit->folder[0] ? "/" : "",
             slash ? slash + 1 : unit->rule->anchor);
    if (!merge_conflict_add(out, anchor)) {
      return false_v;
    }
  }
  return true_v;
}

/* Makes the working directory's `path` hold `entry` (NULL: absent). */
static bool8_t merge_put(const VkrDepotWorkspace *workspace, const char *path,
                         const VkrDepotIndexEntry *entry) {
  char full[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(full, sizeof(full), workspace->root, path)) {
    return false_v;
  }
  if (!entry) {
    return !vkr_bakery_is_file(full) || vkr_bakery_remove_file(full);
  }
  vkr_bakery_path_parent(directory, sizeof(directory), full);
  return vkr_bakery_make_directories(directory) &&
         vkr_depot_write_blob(&workspace->store, &entry->blob, full);
}

/* Writes a merged unit's files from the driver's output. */
static bool8_t merge_put_unit(const VkrDepotWorkspace *workspace,
                              const MergeUnit *unit) {
  for (uint32_t i = 0u; i < unit->changes.count; ++i) {
    const VkrDepotChangeEntry *change = &unit->changes.entries[i];
    char path[VKR_DEPOT_PATH_CAPACITY];
    char full[VKR_DEPOT_PATH_CAPACITY];
    char source[VKR_DEPOT_PATH_CAPACITY];
    char directory[VKR_DEPOT_PATH_CAPACITY];
    if (!vkr_depot_path_valid(change->path)) {
      return false_v;
    }
    snprintf(path, sizeof(path), "%s%s%s", unit->folder,
             unit->folder[0] ? "/" : "", change->path);
    if (!vkr_bakery_path_join(full, sizeof(full), workspace->root, path)) {
      return false_v;
    }
    if (change->change == VKR_DEPOT_CHANGE_DELETED) {
      if (vkr_bakery_is_file(full) && !vkr_bakery_remove_file(full)) {
        return false_v;
      }
      continue;
    }
    vkr_bakery_path_parent(directory, sizeof(directory), full);
    if (!vkr_bakery_path_join(source, sizeof(source), unit->output,
                              change->path) ||
        !vkr_bakery_make_directories(directory) ||
        !vkr_bakery_clone_or_copy(source, full)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t merge_apply(MergeState *state, VkrDepotMergeResult *out_result) {
  const VkrDepotConflictPolicy policy = state->config->policy;
  for (uint32_t i = 0u; i < state->path_count; ++i) {
    const MergePath *entry = &state->paths[i];
    bool8_t take = entry->action == MERGE_TAKE;
    if (entry->unit != UINT32_MAX) {
      /* In a unit a driver merged, a path takes its own decision and the
         driver's files then replace those it wrote, such as both sides'
         new inventories under one merged scene. A unit no driver merged is
         settled whole, one side's files. */
      if (state->units[entry->unit].status != VKR_DEPOT_MERGE_UNIT_MERGED) {
        take = policy == VKR_DEPOT_CONFLICT_TAKE_THEIRS;
      }
    } else if (entry->action == MERGE_CONFLICT) {
      take = policy == VKR_DEPOT_CONFLICT_TAKE_THEIRS;
    }
    if (take && !merge_same(entry->mine, entry->theirs)) {
      if (!merge_put(state->workspace, entry->path, entry->theirs)) {
        return false_v;
      }
      out_result->taken++;
    }
  }
  for (uint32_t u = 0u; u < state->unit_count; ++u) {
    if (state->units[u].status == VKR_DEPOT_MERGE_UNIT_MERGED &&
        state->units[u].mine_changed && state->units[u].theirs_changed &&
        !merge_put_unit(state->workspace, &state->units[u])) {
      return false_v;
    }
  }
  return true_v;
}

// =============================================================================
// Merge
// =============================================================================

static bool8_t merge_tree_files(const VkrDepotStore *store,
                                const VkrDepotId *commit,
                                VkrDepotFileList *out) {
  MemZero(out, sizeof(*out));
  if (vkr_depot_id_zero(commit)) {
    return true_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit parsed;
  const bool8_t ok = vkr_depot_store_get(store, commit, &bytes, &size) &&
                     vkr_depot_commit_parse(bytes, size, &parsed) &&
                     vkr_depot_tree_files(store, &parsed.tree, out);
  free(bytes);
  return ok;
}

static void merge_rules_load(const VkrDepotWorkspace *workspace,
                             VkrDepotMergeRules *out) {
  MemZero(out, sizeof(*out));
  char path[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_path_join(path, sizeof(path), workspace->root,
                           VKR_DEPOT_MERGE_FILE) &&
      vkr_bakery_is_file(path) &&
      vkr_bakery_read_file(path, 1u << 16, &bytes, &size)) {
    vkr_depot_merge_rules_parse((const char *)bytes, size, out);
  }
  free(bytes);
}

static void merge_state_free(MergeState *state) {
  vkr_depot_file_list_free(&state->base);
  vkr_depot_file_list_free(&state->mine);
  vkr_depot_file_list_free(&state->theirs);
  free(state->paths);
  if (state->units) {
    for (uint32_t u = 0u; u < state->unit_count; ++u) {
      vkr_depot_change_list_free(&state->units[u].changes);
    }
  }
  free(state->units);
  if (state->temp_root[0]) {
    (void)vkr_bakery_remove_tree(state->temp_root);
  }
}

/* Whether the working copy holds local changes; false when the snapshot
   fails. */
static bool8_t merge_clean(VkrDepotWorkspace *workspace, bool8_t *out_clean,
                           char *error, uint32_t capacity) {
  VkrDepotChangeList changes;
  if (!vkr_depot_status(workspace, &changes, error, capacity)) {
    return false_v;
  }
  *out_clean = changes.count == 0u;
  vkr_depot_change_list_free(&changes);
  return true_v;
}

/* Commits the merged working directory with both parents and moves the
   branch to it. */
static bool8_t merge_commit(VkrDepotWorkspace *workspace,
                            const VkrDepotId *theirs, const char *message,
                            VkrDepotId *out_commit, char *error,
                            uint32_t capacity) {
  VkrDepotId tree;
  VkrDepotFileList files = {0};
  if (!vkr_depot_workspace_snapshot(workspace, &tree, &files)) {
    snprintf(error, capacity, "the merged working directory snapshot failed");
    return false_v;
  }
  const VkrDepotId parents[2] = {workspace->base, *theirs};
  if (!vkr_depot_make_commit(&workspace->store, &tree, parents, 2u,
                             message ? message : "Merge", out_commit, error,
                             capacity)) {
    vkr_depot_file_list_free(&files);
    return false_v;
  }
  if (vkr_depot_ref_swap(workspace->store.root, workspace->branch,
                         &workspace->base, out_commit,
                         NULL) != VKR_DEPOT_REF_SWAP_OK) {
    snprintf(error, capacity, "%s moved in the store during the merge",
             workspace->branch);
    vkr_depot_file_list_free(&files);
    return false_v;
  }
  vkr_depot_file_list_free(&workspace->index);
  workspace->index = files;
  workspace->base = *out_commit;
  if (!vkr_depot_workspace_save(workspace)) {
    snprintf(error, capacity, "merged, but the working copy did not save");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_merge(VkrDepotWorkspace *workspace, const VkrDepotId *theirs,
                        const VkrDepotMergeConfig *config,
                        VkrDepotMergeResult *out_result, char *error,
                        uint32_t capacity) {
  MemZero(out_result, sizeof(*out_result));
  out_result->status = VKR_DEPOT_MERGE_FAILED;
  const VkrDepotId mine = workspace->base;
  VkrDepotId base;
  bool8_t clean = false_v;
  if (workspace->sparse) {
    snprintf(error, capacity,
             "a merge needs every file on disk; this working copy fetches "
             "files when opened");
    return false_v;
  }
  if (!merge_clean(workspace, &clean, error, capacity)) {
    return false_v;
  }
  if (!clean) {
    snprintf(error, capacity,
             "commit or undo the local changes before merging");
    return false_v;
  }
  if (!vkr_depot_merge_base(&workspace->store, &mine, theirs, &base)) {
    snprintf(error, capacity, "the history is unreadable");
    return false_v;
  }
  if (vkr_depot_id_equal(&base, theirs) || vkr_depot_id_equal(&mine, theirs)) {
    out_result->status = VKR_DEPOT_MERGE_UP_TO_DATE;
    out_result->commit = mine;
    return true_v;
  }
  if (vkr_depot_id_equal(&base, &mine)) {
    uint32_t kept = 0u;
    if (!vkr_depot_fetch_checkout(workspace, NULL, theirs,
                                  VKR_DEPOT_CONFLICT_FAIL, &kept, NULL, error,
                                  capacity)) {
      return false_v;
    }
    out_result->status = VKR_DEPOT_MERGE_FAST_FORWARD;
    out_result->commit = *theirs;
    return true_v;
  }

  MergeState state = {.workspace = workspace, .config = config};
  bool8_t ok = merge_tree_files(&workspace->store, &base, &state.base) &&
               merge_tree_files(&workspace->store, &mine, &state.mine) &&
               merge_tree_files(&workspace->store, theirs, &state.theirs);
  if (!ok) {
    snprintf(error, capacity, "a merged commit's trees are not in the store");
    merge_state_free(&state);
    return false_v;
  }
  merge_rules_load(workspace, &state.rules);
  ok = merge_collect_paths(&state) && merge_collect_units(&state) &&
       vkr_bakery_path_join(state.temp_root, sizeof(state.temp_root),
                            workspace->root, VKR_DEPOT_META_DIR "/merge") &&
       vkr_bakery_remove_tree(state.temp_root) && merge_fetch_base(&state);
  if (!ok) {
    snprintf(error, capacity,
             "the merge could not prepare its units (out of memory, too many "
             "units, or merge-base files not fetched)");
    merge_state_free(&state);
    return false_v;
  }
  for (uint32_t u = 0u; u < state.unit_count; ++u) {
    if (state.units[u].mine_changed && state.units[u].theirs_changed) {
      merge_run_unit(&state, u, out_result);
    }
  }

  if (!merge_list_conflicts(&state, &out_result->conflicts)) {
    snprintf(error, capacity, "out of memory");
    merge_state_free(&state);
    return false_v;
  }
  if (out_result->conflicts.count &&
      config->policy == VKR_DEPOT_CONFLICT_FAIL) {
    out_result->status = VKR_DEPOT_MERGE_CONFLICTS;
    out_result->commit = mine;
    merge_state_free(&state);
    return true_v;
  }

  ok = merge_apply(&state, out_result);
  merge_state_free(&state);
  if (!ok) {
    snprintf(error, capacity,
             "the merge could not write its result; the working copy holds "
             "part of it, and `switch` back to the branch restores it");
    return false_v;
  }
  if (!merge_commit(workspace, theirs, config->message, &out_result->commit,
                    error, capacity)) {
    return false_v;
  }
  out_result->status = VKR_DEPOT_MERGE_MERGED;
  return true_v;
}

bool8_t vkr_depot_pull_merge(VkrAllocator *allocator,
                             VkrDepotWorkspace *workspace, const char *remote,
                             const VkrDepotMergeConfig *config,
                             VkrDepotPullResult *out_pull,
                             VkrDepotMergeResult *out_merge,
                             VkrDepotConnectStatus *out_status, char *error,
                             uint32_t capacity) {
  MemZero(out_merge, sizeof(*out_merge));
  if (!vkr_depot_pull(allocator, workspace, remote, config->policy, out_pull,
                      out_status, error, capacity)) {
    return false_v;
  }
  if (out_pull->status != VKR_DEPOT_PULL_DIVERGED) {
    return true_v;
  }
  VkrDepotRemoteEntry entry;
  if (!vkr_depot_remote_find(workspace->store.root, remote, &entry)) {
    snprintf(error, capacity, "no remote %s", remote ? remote : "origin");
    return false_v;
  }
  VkrDepotConnection *connection = vkr_depot_connect_remote(
      allocator, workspace, &entry, out_status, error, capacity);
  if (!connection) {
    return false_v;
  }
  char message[VKR_DEPOT_REMOTE_NAME + VKR_DEPOT_BRANCH_NAME + 16u];
  snprintf(message, sizeof(message), "Merge %s/%s", entry.name,
           workspace->branch);
  VkrDepotMergeConfig merge = *config;
  merge.client = vkr_depot_connection_client(connection);
  if (!merge.message) {
    merge.message = message;
  }
  const bool8_t ok = vkr_depot_merge(workspace, &out_pull->commit, &merge,
                                     out_merge, error, capacity);
  vkr_depot_disconnect(connection);
  return ok;
}

// =============================================================================
// The Bakery driver
// =============================================================================

bool8_t vkr_depot_bakery_path(const char *override, char *out,
                              uint32_t capacity) {
  if (override && override[0]) {
    return snprintf(out, capacity, "%s", override) < (int)capacity;
  }
  const char *variable = getenv("VKR_BAKERY");
  if (variable && variable[0]) {
    return snprintf(out, capacity, "%s", variable) < (int)capacity;
  }
  char executable[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_executable_path(executable, sizeof(executable))) {
    return false_v;
  }
  vkr_bakery_path_parent(directory, sizeof(directory), executable);
#if defined(_WIN32)
  const char *name = "vkr_bakery.exe";
#else
  const char *name = "vkr_bakery";
#endif
  /* Beside this executable, or in the build tree's tools/bakery beside
     tools/depot. */
  if (vkr_bakery_path_join(out, capacity, directory, name) &&
      vkr_bakery_is_file(out)) {
    return true_v;
  }
  char tools[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(tools, sizeof(tools), directory);
  char bakery[VKR_DEPOT_PATH_CAPACITY];
  return vkr_bakery_path_join(bakery, sizeof(bakery), tools, "bakery") &&
         vkr_bakery_path_join(out, capacity, bakery, name) &&
         vkr_bakery_is_file(out);
}

/* Appends `text` as a JSON string. */
static uint64_t merge_json_string(char *out, uint64_t at, uint64_t capacity,
                                  const char *text) {
  static const char digits[] = "0123456789abcdef";
  if (at < capacity) {
    out[at] = '"';
  }
  ++at;
  for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
    char escaped[7] = {0};
    if (*c == '"' || *c == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)*c;
    } else if (*c < 0x20u) {
      snprintf(escaped, sizeof(escaped), "\\u00%c%c", digits[*c >> 4],
               digits[*c & 15u]);
    } else {
      escaped[0] = (char)*c;
    }
    for (const char *e = escaped; *e; ++e) {
      if (at < capacity) {
        out[at] = *e;
      }
      ++at;
    }
  }
  if (at < capacity) {
    out[at] = '"';
  }
  return at + 1u;
}

static uint64_t merge_json_raw(char *out, uint64_t at, uint64_t capacity,
                               const char *text) {
  for (const char *c = text; *c; ++c) {
    if (at < capacity) {
      out[at] = *c;
    }
    ++at;
  }
  return at;
}

/* Reads the strings of the array at the reader into `out` with `change`. */
static bool8_t merge_json_paths(VkrJsonReader *reader, VkrAllocator *allocator,
                                VkrDepotChange change,
                                VkrDepotChangeList *out) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != '[') {
    return false_v;
  }
  reader->pos++;
  for (;;) {
    vkr_json_skip_whitespace(reader);
    if (reader->pos < reader->length && reader->data[reader->pos] == ']') {
      return true_v;
    }
    String8 text = {0};
    if (!vkr_json_parse_string_decoded(reader, allocator, &text) ||
        !merge_conflict_add(out, (const char *)text.str)) {
      return false_v;
    }
    out->entries[out->count - 1u].change = change;
    vkr_allocator_free(allocator, text.str, text.length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    vkr_json_skip_whitespace(reader);
    if (reader->pos < reader->length && reader->data[reader->pos] == ',') {
      reader->pos++;
    }
  }
}

VkrDepotMergeUnitStatus vkr_depot_merge_bakery_driver(
    void *context, const VkrDepotMergeUnitRequest *request,
    VkrDepotChangeList *out_changes, VkrDepotChangeList *out_conflicts,
    char *detail, uint32_t capacity) {
  const char *bakery = context;
  const char *operation =
      strcmp(request->driver, "scene") == 0     ? "merge_scene_unit"
      : strcmp(request->driver, "project") == 0 ? "merge_project_document"
                                                : NULL;
  if (!operation || !bakery || !bakery[0]) {
    snprintf(detail, capacity, "no driver %s", request->driver);
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }

  char root[VKR_DEPOT_PATH_CAPACITY];
  char request_path[VKR_DEPOT_PATH_CAPACITY];
  char result_path[VKR_DEPOT_PATH_CAPACITY];
  char workspace[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(root, sizeof(root), request->output_root);
  if (!vkr_bakery_path_join(request_path, sizeof(request_path), root,
                            "request.json") ||
      !vkr_bakery_path_join(result_path, sizeof(result_path), root,
                            "result.json") ||
      !vkr_bakery_path_join(workspace, sizeof(workspace),
                            request->workspace_root, ".vkreditor")) {
    snprintf(detail, capacity, "paths too long");
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }

  const uint64_t json_capacity = 6u * VKR_DEPOT_PATH_CAPACITY * 2u + 512u;
  char *json = malloc(json_capacity);
  if (!json) {
    snprintf(detail, capacity, "out of memory");
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }
  const char *names[] = {"workspace_root", "anchor", "base",
                         "mine",           "theirs", "output"};
  const char *values[] = {
      workspace,          request->anchor,      request->base_root,
      request->mine_root, request->theirs_root, request->output_root};
  uint64_t size = merge_json_raw(json, 0u, json_capacity,
                                 "{\"version\":1,\"read_only\":true,"
                                 "\"operation\":");
  size = merge_json_string(json, size, json_capacity, operation);
  for (uint32_t i = 0u; i < ArrayCount(names); ++i) {
    size = merge_json_raw(json, size, json_capacity, ",");
    size = merge_json_string(json, size, json_capacity, names[i]);
    size = merge_json_raw(json, size, json_capacity, ":");
    size = merge_json_string(json, size, json_capacity, values[i]);
  }
  size = merge_json_raw(json, size, json_capacity, "}\n");
  const bool8_t written =
      size <= json_capacity &&
      vkr_bakery_write_file_atomic(request_path, json, size);
  free(json);
  if (!written) {
    snprintf(detail, capacity, "the driver request could not be written");
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }

  const char *arguments[] = {"project", "--request", request_path, "--result",
                             result_path};
  char output[512];
  int32_t exit_code = -1;
  if (!vkr_platform_process_capture(bakery, arguments, ArrayCount(arguments),
                                    root, output, sizeof(output), &exit_code)) {
    snprintf(detail, capacity, "%s did not run", bakery);
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }

  uint8_t *bytes = NULL;
  uint64_t result_size = 0u;
  if (!vkr_bakery_is_file(result_path) ||
      !vkr_bakery_read_file(result_path, MERGE_RESULT_MAX, &bytes,
                            &result_size)) {
    snprintf(detail, capacity, "%s %s wrote no result (exit %d)", bakery,
             operation, exit_code);
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }
  VkrDepotMergeUnitStatus status = VKR_DEPOT_MERGE_UNIT_FAILED;
  VkrJsonReader reader = vkr_json_reader_create(bytes, result_size);
  /* The decoded paths' storage, for this result only. */
  VkrDMemory memory;
  if (!vkr_dmemory_create(KB(64), MB(16), &memory)) {
    free(bytes);
    snprintf(detail, capacity, "out of memory");
    return VKR_DEPOT_MERGE_UNIT_FAILED;
  }
  VkrAllocator decoder = {.ctx = &memory};
  vkr_dmemory_allocator_create(&decoder);
  VkrAllocator *allocator = &decoder;
  bool8_t merged = false_v;
  if (!vkr_json_find_root_field(&reader, "merged") ||
      !vkr_json_parse_bool(&reader, &merged)) {
    vkr_json_reader_reset(&reader);
    String8 message = {0};
    if (vkr_json_find_root_field(&reader, "error") &&
        vkr_json_parse_string(&reader, &message)) {
      snprintf(detail, capacity, "%s: %.*s", operation, (int)message.length,
               (const char *)message.str);
    } else {
      snprintf(detail, capacity, "%s: no merge result", operation);
    }
  } else if (merged) {
    vkr_json_reader_reset(&reader);
    bool8_t ok = !vkr_json_find_root_field(&reader, "write") ||
                 merge_json_paths(&reader, allocator, VKR_DEPOT_CHANGE_MODIFIED,
                                  out_changes);
    vkr_json_reader_reset(&reader);
    ok = ok && (!vkr_json_find_root_field(&reader, "delete") ||
                merge_json_paths(&reader, allocator, VKR_DEPOT_CHANGE_DELETED,
                                 out_changes));
    status = ok ? VKR_DEPOT_MERGE_UNIT_MERGED : VKR_DEPOT_MERGE_UNIT_FAILED;
    if (!ok) {
      snprintf(detail, capacity, "%s: an unreadable file list", operation);
    }
  } else {
    status = VKR_DEPOT_MERGE_UNIT_CONFLICT;
    snprintf(detail, capacity, "%s: conflicts", operation);
    (void)out_conflicts;
  }
  vkr_dmemory_allocator_destroy(&decoder);
  free(bytes);
  return status;
}

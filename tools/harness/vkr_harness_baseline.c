#include "vkr_harness_json.h"
#include "vkr_harness_runtime.h"

#define VKR_HARNESS_BASELINE_ROOT "tools/baselines"
#define VKR_HARNESS_BASELINE_PLAN_ROOT "build/_artifacts/baseline"

typedef struct VkrHarnessBaselinePlan {
  char source_run[VKR_HARNESS_RELATIVE_PATH_MAX];
  char source_report_sha256[VKR_HARNESS_DIGEST_MAX];
  char source_summary_sha256[VKR_HARNESS_DIGEST_MAX];
  char entries[VKR_HARNESS_RELATIVE_PATH_MAX];
  char entries_sha256[VKR_HARNESS_DIGEST_MAX];
  char generation_sha256[VKR_HARNESS_DIGEST_MAX];
  char prior_generation_sha256[VKR_HARNESS_DIGEST_MAX];
  char profile_id[VKR_HARNESS_ID_MAX];
  char case_id[VKR_HARNESS_ID_MAX];
  char actor[VKR_HARNESS_TEXT_MAX];
  char reason[VKR_HARNESS_TEXT_MAX];
} VkrHarnessBaselinePlan;

static bool8_t
vkr_harness_baseline_json_string(const VkrHarnessJsonDocument *document,
                                 int32_t object, const char *name, char *out,
                                 uint32_t capacity, VkrHarnessError *error) {
  bool8_t duplicate = false_v;
  const int32_t token =
      vkr_harness_json_object_get(document, object, name, &duplicate);
  return token >= 0 && !duplicate &&
         vkr_harness_json_string(document, token, out, capacity, name, error);
}

static bool8_t vkr_harness_baseline_copy(const char *source,
                                         const char *destination,
                                         const char *digest, Arena *arena,
                                         VkrHarnessError *error) {
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  char observed[VKR_HARNESS_DIGEST_MAX];
  Scratch scratch = scratch_create(arena);
  const bool8_t ok =
      vkr_harness_sha256_file(source, observed) &&
      string_equals(observed, digest) &&
      vkr_harness_read_file(source, arena, &bytes, &length) &&
      vkr_harness_atomic_write(destination, bytes, length, error) &&
      vkr_harness_sha256_file(destination, observed) &&
      string_equals(observed, digest);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}

/** A `sha256:`-prefixed 64-hex-digit digest and nothing else. */
static bool8_t vkr_harness_baseline_digest_valid(const char *digest) {
  return string_n_equals(digest, "sha256:", 7u) &&
         string_length(digest) == VKR_HARNESS_DIGEST_MAX - 1u;
}

static bool8_t vkr_harness_baseline_current_generation(
    const char *repo_root, const char *profile_id, const char *case_id,
    Arena *arena, char generation[VKR_HARNESS_DIGEST_MAX],
    VkrHarnessError *error) {
  char current[VKR_HARNESS_PATH_MAX];
  string_format(current, sizeof(current), "%s/%s/%s/%s/current.json", repo_root,
                VKR_HARNESS_BASELINE_ROOT, profile_id, case_id);
  FilePath current_path = vkr_harness_file_path(current);
  if (!file_exists(&current_path)) {
    vkr_harness_error_set(error, "baseline.missing", "current.json",
                          "No accepted baseline exists for %s/%s", profile_id,
                          case_id);
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  /* Only `generation`, which the caller owns, outlives this read. */
  Scratch scratch = scratch_create(arena);
  VkrHarnessJsonDocument *document =
      arena_alloc(arena, sizeof(*document), ARENA_MEMORY_TAG_STRUCT);
  const bool8_t ok =
      document && vkr_harness_read_file(current, arena, &bytes, &length) &&
      vkr_harness_json_parse(document, (const char *)bytes, length, error) &&
      vkr_harness_baseline_json_string(document, 0, "generation_sha256",
                                       generation, VKR_HARNESS_DIGEST_MAX,
                                       error) &&
      vkr_harness_baseline_digest_valid(generation);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRUCT);
  return ok;
}

bool8_t vkr_harness_baseline_current(const char *repo_root,
                                     const char *profile_id,
                                     const char *case_id, Arena *arena,
                                     char generation_root[VKR_HARNESS_PATH_MAX],
                                     VkrHarnessCaptureSummary *summary,
                                     VkrHarnessError *error) {
  char generation[VKR_HARNESS_DIGEST_MAX] = {0};
  if (!repo_root || !profile_id || !case_id || !arena || !generation_root ||
      !summary ||
      !vkr_harness_baseline_current_generation(repo_root, profile_id, case_id,
                                               arena, generation, error)) {
    return false_v;
  }
  string_format(generation_root, VKR_HARNESS_PATH_MAX,
                "%s/%s/%s/%s/generations/%s", repo_root,
                VKR_HARNESS_BASELINE_ROOT, profile_id, case_id,
                generation + 7u);
  char summary_path[VKR_HARNESS_PATH_MAX];
  string_format(summary_path, sizeof(summary_path), "%s/capture-summary.bin",
                generation_root);
  if (!vkr_harness_capture_summary_read(summary_path, arena, summary) ||
      !string_equals(summary->profile_id, profile_id) ||
      !string_equals(summary->case_id, case_id)) {
    vkr_harness_error_set(error, "baseline.invalid", "capture-summary.bin",
                          "Accepted baseline summary is invalid");
    return false_v;
  }
  return true_v;
}

static bool8_t
vkr_harness_baseline_write_plan(const char *path,
                                const VkrHarnessBaselinePlan *plan) {
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(
          &file, string8_create_from_cstr((const uint8_t *)path,
                                          string_length(path)))) {
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  const bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version", 1u) &&
      vkr_harness_json_emit_string(writer, "kind",
                                   "vkr.harness.baseline-plan") &&
      vkr_harness_json_emit_string(writer, "source_run", plan->source_run) &&
      vkr_harness_json_emit_string(writer, "source_report_sha256",
                                   plan->source_report_sha256) &&
      vkr_harness_json_emit_string(writer, "source_summary_sha256",
                                   plan->source_summary_sha256) &&
      vkr_harness_json_emit_string(writer, "entries", plan->entries) &&
      vkr_harness_json_emit_string(writer, "entries_sha256",
                                   plan->entries_sha256) &&
      vkr_harness_json_emit_string(writer, "generation_sha256",
                                   plan->generation_sha256) &&
      vkr_harness_json_emit_string(writer, "prior_generation_sha256",
                                   plan->prior_generation_sha256) &&
      vkr_harness_json_emit_string(writer, "profile_id", plan->profile_id) &&
      vkr_harness_json_emit_string(writer, "case_id", plan->case_id) &&
      vkr_harness_json_emit_string(writer, "actor", plan->actor) &&
      vkr_harness_json_emit_string(writer, "reason", plan->reason) &&
      vkr_json_writer_end_object(writer);
  if (!ok || !vkr_json_file_writer_commit(&file)) {
    vkr_json_file_writer_abort(&file);
    return false_v;
  }
  return true_v;
}

/**
 * Appends one `entries.ndjson` line naming a verified source file and where the
 * generation copy of it belongs.
 */
static bool8_t vkr_harness_baseline_append_entry(
    char *entries, uint64_t capacity, uint64_t *used, const char *source_run,
    const char *source_relative, const char *destination, const char *digest,
    const char *role) {
  const int32_t written = string_format(
      entries + *used, capacity - *used,
      "{\"source\":\"%s/%s\",\"destination\":\"%s\",\"sha256\":\"%s\","
      "\"role\":\"%s\"}\n",
      source_run, source_relative, destination, digest, role);
  if (written <= 0 || *used + (uint64_t)written >= capacity) {
    return false_v;
  }
  *used += (uint64_t)written;
  return true_v;
}

int vkr_harness_baseline_propose(const char *repo_root, const char *from_run,
                                 const char *actor, const char *reason) {
  if (!repo_root || !from_run || !actor || !actor[0] || !reason || !reason[0] ||
      !vkr_harness_path_is_safe_relative(from_run)) {
    vkr_harness_stderr("baseline propose requires safe --from, --actor, and "
                       "--reason values\n");
    return VKR_HARNESS_EXIT_INVALID;
  }
  Arena *arena = arena_create();
  VkrHarnessError error = {0};
  int result = VKR_HARNESS_EXIT_ERROR;
  char source_run[VKR_HARNESS_PATH_MAX];
  string_format(source_run, sizeof(source_run), "%s/%s", repo_root, from_run);
  vkr_harness_path_to_run_root(source_run);
  char snapshot_root[VKR_HARNESS_PATH_MAX];
  string_format(snapshot_root, sizeof(snapshot_root), "%s/%s", repo_root,
                "build/_artifacts/snapshot");
  if (!arena ||
      !vkr_harness_existing_path_is_below(snapshot_root, source_run)) {
    vkr_harness_stderr("--from must name a completed snapshot run\n");
    goto cleanup;
  }
  char summary_path[VKR_HARNESS_PATH_MAX];
  char report_path[VKR_HARNESS_PATH_MAX];
  string_format(summary_path, sizeof(summary_path), "%s/capture-summary.bin",
                source_run);
  string_format(report_path, sizeof(report_path), "%s/report.json", source_run);
  VkrHarnessCaptureSummary summary = {0};
  VkrHarnessBaselinePlan plan = {0};
  if (!vkr_harness_capture_summary_read(summary_path, arena, &summary) ||
      summary.tool != VKR_HARNESS_TOOL_SNAPSHOT ||
      summary.exit_code == VKR_HARNESS_EXIT_ERROR ||
      summary.capture_count == 0u ||
      !vkr_harness_sha256_file(summary_path, plan.source_summary_sha256) ||
      !vkr_harness_sha256_file(report_path, plan.source_report_sha256)) {
    vkr_harness_stderr(
        "Snapshot run is incomplete or its summary is invalid\n");
    goto cleanup;
  }
  string_format(plan.source_run, sizeof(plan.source_run), "%s", from_run);
  vkr_harness_path_to_run_root(plan.source_run);
  string_format(plan.profile_id, sizeof(plan.profile_id), "%s",
                summary.profile_id);
  string_format(plan.case_id, sizeof(plan.case_id), "%s", summary.case_id);
  string_format(plan.actor, sizeof(plan.actor), "%s", actor);
  string_format(plan.reason, sizeof(plan.reason), "%s", reason);
  VkrHarnessError prior_error = {0};
  (void)vkr_harness_baseline_current_generation(
      repo_root, plan.profile_id, plan.case_id, arena,
      plan.prior_generation_sha256, &prior_error);

  char plan_id[64];
  char plan_root[VKR_HARNESS_PATH_MAX];
  char plan_candidate[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_generate_run_id(plan_id)) {
    goto cleanup;
  }
  string_format(plan_candidate, sizeof(plan_candidate), "%s/%s/%s", repo_root,
                VKR_HARNESS_BASELINE_PLAN_ROOT, plan_id);
  if (!vkr_harness_make_directories(plan_candidate, &error) ||
      !vkr_harness_realpath(plan_candidate, plan_root)) {
    goto cleanup;
  }
  /* The plan quotes its inventory by repository-relative path so a reviewer
     reads the same location the accept step will. */
  string_format(plan.entries, sizeof(plan.entries), "%s/%s/entries.ndjson",
                VKR_HARNESS_BASELINE_PLAN_ROOT, plan_id);
  const uint64_t entry_capacity =
      ((uint64_t)summary.artifact_count + 2u) * 1024u;
  char *entries = arena_alloc(arena, entry_capacity, ARENA_MEMORY_TAG_STRING);
  uint64_t used = 0u;
  if (!entries ||
      !vkr_harness_baseline_append_entry(
          entries, entry_capacity, &used, plan.source_run, "report.json",
          "report.json", plan.source_report_sha256, "source.report") ||
      !vkr_harness_baseline_append_entry(
          entries, entry_capacity, &used, plan.source_run,
          "capture-summary.bin", "capture-summary.bin",
          plan.source_summary_sha256, "capture.summary")) {
    goto cleanup;
  }
  for (uint32_t i = 0; i < summary.artifact_count; ++i) {
    const VkrHarnessArtifact *artifact = &summary.artifacts[i];
    if (!string_n_equals(artifact->role, "capture.", 8u) ||
        string_equals(artifact->role, "capture.summary")) {
      continue;
    }
    char absolute[VKR_HARNESS_PATH_MAX];
    char digest[VKR_HARNESS_DIGEST_MAX];
    if (!vkr_harness_path_is_safe_relative(artifact->path) ||
        string_format(absolute, sizeof(absolute), "%s/%s", source_run,
                      artifact->path) <= 0 ||
        !vkr_harness_sha256_file(absolute, digest) ||
        !string_equals(digest, artifact->sha256)) {
      vkr_harness_stderr("Snapshot artifact verification failed: %s\n",
                         artifact->path);
      goto cleanup;
    }
    if (!vkr_harness_baseline_append_entry(
            entries, entry_capacity, &used, plan.source_run, artifact->path,
            artifact->path, artifact->sha256, artifact->role)) {
      goto cleanup;
    }
  }
  char entries_path[VKR_HARNESS_PATH_MAX];
  string_format(entries_path, sizeof(entries_path), "%s/entries.ndjson",
                plan_root);
  if (!vkr_harness_atomic_write(entries_path, entries, used, &error) ||
      !vkr_harness_sha256_file(entries_path, plan.entries_sha256)) {
    goto cleanup;
  }
  VkrHarnessFingerprintField generation_fields[] = {
      {.name = "entries", .value = ""},
      {.name = "summary", .value = ""},
      {.name = "profile", .value = ""},
      {.name = "case", .value = ""},
  };
  string_format(generation_fields[0].value, sizeof(generation_fields[0].value),
                "%s", plan.entries_sha256);
  string_format(generation_fields[1].value, sizeof(generation_fields[1].value),
                "%s", plan.source_summary_sha256);
  string_format(generation_fields[2].value, sizeof(generation_fields[2].value),
                "%s", plan.profile_id);
  string_format(generation_fields[3].value, sizeof(generation_fields[3].value),
                "%s", plan.case_id);
  if (!vkr_harness_fingerprint(generation_fields, ArrayCount(generation_fields),
                               plan.generation_sha256, &error)) {
    goto cleanup;
  }
  char final_plan[VKR_HARNESS_PATH_MAX];
  char plan_digest[VKR_HARNESS_DIGEST_MAX];
  string_format(final_plan, sizeof(final_plan), "%s/plan.json", plan_root);
  if (!vkr_harness_baseline_write_plan(final_plan, &plan) ||
      !vkr_harness_sha256_file(final_plan, plan_digest)) {
    goto cleanup;
  }
  vkr_harness_stdout(
      "{\"status\":\"pass\",\"exit_code\":0,\"plan\":\"%s/%s/plan.json\","
      "\"sha256\":\"%s\",\"generation_sha256\":\"%s\"}\n",
      VKR_HARNESS_BASELINE_PLAN_ROOT, plan_id, plan_digest,
      plan.generation_sha256);
  result = VKR_HARNESS_EXIT_PASS;
cleanup:
  if (result != VKR_HARNESS_EXIT_PASS && error.message[0]) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
  }
  arena_destroy(arena);
  return result;
}

static bool8_t vkr_harness_baseline_plan_parse(const char *path, Arena *arena,
                                               VkrHarnessBaselinePlan *plan,
                                               VkrHarnessError *error) {
  uint8_t *bytes = NULL;
  uint64_t length = 0u;
  VkrHarnessJsonDocument *document =
      arena_alloc(arena, sizeof(*document), ARENA_MEMORY_TAG_STRUCT);
  return document && vkr_harness_read_file(path, arena, &bytes, &length) &&
         vkr_harness_json_parse(document, (const char *)bytes, length, error) &&
         vkr_harness_baseline_json_string(document, 0, "source_run",
                                          plan->source_run,
                                          sizeof(plan->source_run), error) &&
         vkr_harness_baseline_json_string(
             document, 0, "source_report_sha256", plan->source_report_sha256,
             sizeof(plan->source_report_sha256), error) &&
         vkr_harness_baseline_json_string(
             document, 0, "source_summary_sha256", plan->source_summary_sha256,
             sizeof(plan->source_summary_sha256), error) &&
         vkr_harness_baseline_json_string(document, 0, "entries", plan->entries,
                                          sizeof(plan->entries), error) &&
         vkr_harness_baseline_json_string(
             document, 0, "entries_sha256", plan->entries_sha256,
             sizeof(plan->entries_sha256), error) &&
         vkr_harness_baseline_json_string(
             document, 0, "generation_sha256", plan->generation_sha256,
             sizeof(plan->generation_sha256), error) &&
         vkr_harness_baseline_json_string(
             document, 0, "prior_generation_sha256",
             plan->prior_generation_sha256,
             sizeof(plan->prior_generation_sha256), error) &&
         vkr_harness_baseline_json_string(document, 0, "profile_id",
                                          plan->profile_id,
                                          sizeof(plan->profile_id), error) &&
         vkr_harness_baseline_json_string(document, 0, "case_id", plan->case_id,
                                          sizeof(plan->case_id), error) &&
         vkr_harness_baseline_json_string(document, 0, "actor", plan->actor,
                                          sizeof(plan->actor), error) &&
         vkr_harness_baseline_json_string(document, 0, "reason", plan->reason,
                                          sizeof(plan->reason), error);
}

/**
 * A parse document is far too large to place on the stack of a per-line loop,
 * so it is scoped to `arena` and released with the line it described.
 */
static bool8_t vkr_harness_baseline_entry_parse(
    const char *line, uint64_t length, Arena *arena,
    char source[VKR_HARNESS_PATH_MAX],
    char destination[VKR_HARNESS_RELATIVE_PATH_MAX],
    char digest[VKR_HARNESS_DIGEST_MAX], VkrHarnessError *error) {
  Scratch scratch = scratch_create(arena);
  VkrHarnessJsonDocument *document =
      arena_alloc(arena, sizeof(*document), ARENA_MEMORY_TAG_STRUCT);
  const bool8_t ok =
      document && vkr_harness_json_parse(document, line, length, error) &&
      vkr_harness_baseline_json_string(document, 0, "source", source,
                                       VKR_HARNESS_PATH_MAX, error) &&
      vkr_harness_baseline_json_string(document, 0, "destination", destination,
                                       VKR_HARNESS_RELATIVE_PATH_MAX, error) &&
      vkr_harness_baseline_json_string(document, 0, "sha256", digest,
                                       VKR_HARNESS_DIGEST_MAX, error);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRUCT);
  return ok;
}

int vkr_harness_baseline_accept(const char *repo_root, const char *plan_path,
                                const char *confirmation) {
  if (!repo_root || !plan_path || !confirmation ||
      !vkr_harness_path_is_safe_relative(plan_path)) {
    return VKR_HARNESS_EXIT_INVALID;
  }
  Arena *arena = arena_create();
  VkrHarnessError error = {0};
  int result = VKR_HARNESS_EXIT_ERROR;
  char absolute_plan[VKR_HARNESS_PATH_MAX];
  char observed[VKR_HARNESS_DIGEST_MAX];
  if (!arena ||
      !vkr_harness_resolve_existing_path(repo_root, plan_path, absolute_plan,
                                         &error) ||
      !vkr_harness_sha256_file(absolute_plan, observed) ||
      !string_equals(observed, confirmation)) {
    vkr_harness_stderr("Plan confirmation digest does not match\n");
    goto cleanup;
  }
  VkrHarnessBaselinePlan plan = {0};
  if (!vkr_harness_baseline_plan_parse(absolute_plan, arena, &plan, &error)) {
    goto cleanup;
  }
  char current_generation[VKR_HARNESS_DIGEST_MAX] = {0};
  VkrHarnessError missing = {0};
  (void)vkr_harness_baseline_current_generation(repo_root, plan.profile_id,
                                                plan.case_id, arena,
                                                current_generation, &missing);
  if (!string_equals(current_generation, plan.prior_generation_sha256)) {
    vkr_harness_stderr("Accepted baseline changed since proposal\n");
    result = VKR_HARNESS_EXIT_MISSING_BASELINE;
    goto cleanup;
  }
  char entries_path[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_resolve_existing_path(repo_root, plan.entries, entries_path,
                                         &error) ||
      !vkr_harness_sha256_file(entries_path, observed) ||
      !string_equals(observed, plan.entries_sha256)) {
    vkr_harness_stderr("Proposal entry inventory changed\n");
    goto cleanup;
  }
  char baseline_case_root[VKR_HARNESS_PATH_MAX];
  char generation_root[VKR_HARNESS_PATH_MAX];
  string_format(baseline_case_root, sizeof(baseline_case_root), "%s/%s/%s/%s",
                repo_root, VKR_HARNESS_BASELINE_ROOT, plan.profile_id,
                plan.case_id);
  if (!vkr_harness_make_directories(baseline_case_root, &error)) {
    goto cleanup;
  }
  string_format(generation_root, sizeof(generation_root), "%s/generations/%s",
                baseline_case_root, plan.generation_sha256 + 7u);
  char generations[VKR_HARNESS_PATH_MAX];
  string_format(generations, sizeof(generations), "%s/generations",
                baseline_case_root);
  if (!vkr_harness_make_directories(generations, &error)) {
    goto cleanup;
  }
  /* Exclusive creation is the promotion lock: a generation directory that
     already exists was either accepted or abandoned mid-copy, and either way
     this run must not write into it. `current.json` is published only after
     every copy verifies, so an abandoned directory is never trusted evidence —
     it does have to be removed by hand before the same plan can be retried. */
  FilePath generation_path = vkr_harness_file_path(generation_root);
  if (file_create_directory_exclusive(&generation_path) != FILE_ERROR_NONE) {
    vkr_harness_stderr("Immutable baseline generation already exists\n");
    goto cleanup;
  }
  uint8_t *entry_bytes = NULL;
  uint64_t entry_length = 0u;
  if (!vkr_harness_read_file(entries_path, arena, &entry_bytes,
                             &entry_length)) {
    goto cleanup;
  }
  uint64_t cursor = 0u;
  while (cursor < entry_length) {
    uint64_t end = cursor;
    while (end < entry_length && entry_bytes[end] != '\n') {
      end++;
    }
    if (end == cursor) {
      cursor = end + 1u;
      continue;
    }
    char source[VKR_HARNESS_PATH_MAX];
    char destination[VKR_HARNESS_RELATIVE_PATH_MAX];
    char digest[VKR_HARNESS_DIGEST_MAX];
    if (!vkr_harness_baseline_entry_parse((const char *)entry_bytes + cursor,
                                          end - cursor, arena, source,
                                          destination, digest, &error) ||
        !vkr_harness_path_is_safe_relative(source) ||
        !vkr_harness_path_is_safe_relative(destination) ||
        !string_n_equals(source, plan.source_run,
                         string_length(plan.source_run))) {
      goto cleanup;
    }
    char absolute_source[VKR_HARNESS_PATH_MAX];
    char absolute_destination[VKR_HARNESS_PATH_MAX];
    char directory[VKR_HARNESS_PATH_MAX];
    string_format(absolute_source, sizeof(absolute_source), "%s/%s", repo_root,
                  source);
    string_format(absolute_destination, sizeof(absolute_destination), "%s/%s",
                  generation_root, destination);
    if (!vkr_harness_path_parent(absolute_destination, directory) ||
        !vkr_harness_make_directories(directory, &error) ||
        !vkr_harness_baseline_copy(absolute_source, absolute_destination,
                                   digest, arena, &error)) {
      goto cleanup;
    }
    cursor = end + 1u;
  }
  char accepted_at[40];
  char current_path[VKR_HARNESS_PATH_MAX];
  vkr_harness_timestamp_utc(accepted_at);
  string_format(current_path, sizeof(current_path), "%s/current.json",
                baseline_case_root);
  VkrJsonFileWriter current = {0};
  if (!vkr_json_file_writer_begin(
          &current, string8_create_from_cstr((const uint8_t *)current_path,
                                             string_length(current_path)))) {
    goto cleanup;
  }
  VkrJsonWriter *writer = &current.writer;
  const bool8_t current_ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version", 1u) &&
      vkr_harness_json_emit_string(writer, "generation_sha256",
                                   plan.generation_sha256) &&
      vkr_harness_json_emit_string(writer, "prior_generation_sha256",
                                   plan.prior_generation_sha256) &&
      vkr_harness_json_emit_string(writer, "actor", plan.actor) &&
      vkr_harness_json_emit_string(writer, "reason", plan.reason) &&
      vkr_harness_json_emit_string(writer, "accepted_at", accepted_at) &&
      vkr_json_writer_end_object(writer);
  if (!current_ok || !vkr_json_file_writer_commit(&current)) {
    vkr_json_file_writer_abort(&current);
    goto cleanup;
  }
  vkr_harness_stdout(
      "{\"status\":\"pass\",\"exit_code\":0,\"generation_sha256\":\"%s\"}\n",
      plan.generation_sha256);
  result = VKR_HARNESS_EXIT_PASS;
cleanup:
  if (result != VKR_HARNESS_EXIT_PASS && error.message[0]) {
    vkr_harness_stderr("%s: %s\n", error.code, error.message);
  }
  arena_destroy(arena);
  return result;
}

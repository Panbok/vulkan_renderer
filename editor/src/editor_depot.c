#include "editor_depot.h"

#include "editor_install.h"
#include "editor_internal.h"
#include "editor_material.h"
#include "editor_project_store.h"
#include "editor_projects.h"
#include "editor_scripts.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/vkr_vfs.h"
#include "memory/arena.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_json.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_merge.h"
#include "vkr_depot_remote.h"
#include "vkr_depot_service.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_zstd.h"
#include "vkr_net_crypto.h"

/* Servers remembered across runs and probed one by one, since some
   endpoint security passes answers to such probes but drops answers to a
   broadcast. */
#define DEPOT_KNOWN_MAX 16u
/* Shared workspaces on this computer the project hub lists. */
#define DEPOT_PROJECTS_MAX 32u
/* Names of the computers this editor gave access, by identity key. */
#define DEPOT_PEOPLE_MAX 64u
/* Changed paths, conflicts and history rows the window lists. */
#define DEPOT_LIST_MAX 64u
#define DEPOT_HISTORY_MAX 12u
#define DEPOT_REQUESTS_MAX VKR_DEPOT_REQUESTS_MAX
#define DEPOT_ACCESS_MAX 32u
#define DEPOT_PATH 1024u
#define DEPOT_TEXT 256u
/* Seconds between checks of local changes and the server's branch. */
#define DEPOT_STATUS_SECONDS 20.0
/* Seconds between retries while a server has not allowed this computer. */
#define DEPOT_RETRY_SECONDS 3.0
/* Between tries to open a workspace a Get or Share made. */
#define DEPOT_OPEN_RETRY_SECONDS 2.0
#define DEPOT_PROBE_SECONDS 2.0
#define DEPOT_BRANCH "main"

typedef enum DepotJobKind {
  DEPOT_JOB_NONE = 0,
  DEPOT_JOB_STATUS,
  DEPOT_JOB_COMMIT,
  DEPOT_JOB_PUSH,
  DEPOT_JOB_PULL,
  DEPOT_JOB_SYNC,
  DEPOT_JOB_FETCH,
  DEPOT_JOB_CLONE,
  DEPOT_JOB_INIT,
  DEPOT_JOB_SWITCH,
} DepotJobKind;

static const char *const depot_job_names[] = {"",
                                              "Checking",
                                              "Committing",
                                              "Pushing",
                                              "Pulling",
                                              "Syncing",
                                              "Fetching",
                                              "Cloning",
                                              "Making a repository",
                                              "Switching branch"};

/* Branches and commit rows the window lists. */
#define DEPOT_BRANCHES_MAX 16u
#define DEPOT_COMMITS_MAX 8u

typedef struct DepotText {
  char text[DEPOT_TEXT];
} DepotText;

typedef struct DepotHistory {
  char id[VKR_DEPOT_ID_HEX];
  char message[96];
  uint64_t time_us;
} DepotHistory;

typedef struct DepotBranch {
  char name[VKR_DEPOT_BRANCH_NAME];
} DepotBranch;

/* One step on the worker thread. The UI thread fills the inputs, starts it
   and reads the outputs once `done` is set; the worker owns everything in
   between, except `progress`, which both read. */
typedef struct DepotJob {
  VkrThread thread;
  bool8_t running;
  VkrAtomicBool done;
  DepotJobKind kind;
  /* Inputs. */
  char directory[DEPOT_PATH];
  /* The remote a step exchanges with, and its address and key for the
     cooked side branch; empty for none. */
  char remote[VKR_DEPOT_REMOTE_NAME];
  char address[DEPOT_TEXT];
  uint8_t server_key[32];
  /* A Clone that makes a sparse working copy, whose files arrive when
     read. */
  bool8_t sparse;
  char message[DEPOT_TEXT];
  VkrDepotConflictPolicy policy;
  /* Commit: pull and push after it. Init: serve the new repository. Switch:
     make the branch first. */
  bool8_t then_sync;
  bool8_t serve;
  bool8_t create_branch;
  /* Commit: copy the files the project reads from outside it into it
     first, or commit without them. */
  bool8_t adopt;
  bool8_t ignore_untracked;
  char branch[VKR_DEPOT_BRANCH_NAME];
  char source[DEPOT_PATH];
  char project_id[40];
  /* Outputs. */
  bool8_t ok;
  VkrDepotConnectStatus connect;
  char error[DEPOT_TEXT];
  uint32_t change_count;
  DepotText changes[DEPOT_LIST_MAX];
  uint32_t conflict_count;
  DepotText conflicts[DEPOT_LIST_MAX];
  /* Paths a pull, switch or clone wrote, for the scene reload and texture
     cook. */
  bool8_t scene_files_changed;
  bool8_t materials_changed;
  /* A project's Scripts folder changed: native code someone else wrote. */
  bool8_t scripts_changed;
  /* A pull merged both sides, or fast-forwarded. */
  bool8_t merged;
  /* Scenes whose files a pull or switch wrote, for Bakery's pull report;
     an overflow reports every scene. */
  uint32_t changed_scene_count;
  char changed_scenes[DEPOT_LIST_MAX][40];
  /* The first scene the pull report found that does not open. */
  char scene_error[DEPOT_TEXT];
  /* Files the project reads from outside it, which another computer
     lacks (Bakery's untracked_dependencies). */
  uint32_t untracked_count;
  uint64_t untracked_bytes;
  DepotText untracked[DEPOT_LIST_MAX];
  uint32_t textures_cooked;
  uint32_t textures_failed;
  /* Cooked textures the side branch for this computer brought instead. */
  uint32_t textures_fetched;
  VkrDepotId base;
  /* The working copy's branch, its commit in the store and the remote's
     as last fetched, and how far apart they are. */
  char current_branch[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId remote_head;
  bool8_t remote_known;
  uint32_t ahead;
  uint32_t behind;
  uint32_t branch_count;
  DepotBranch branches[DEPOT_BRANCHES_MAX];
  uint32_t outgoing_count;
  DepotHistory outgoing[DEPOT_COMMITS_MAX];
  uint32_t incoming_count;
  DepotHistory incoming[DEPOT_COMMITS_MAX];
  uint32_t history_count;
  DepotHistory history[DEPOT_HISTORY_MAX];
  char opened_project[40];
  VkrDepotProgress progress;
  /* The texture cook that follows a pull, after the depot's phases. */
  VkrAtomicUint32 cook_done;
  VkrAtomicUint32 cook_total;
} DepotJob;

typedef struct DepotKnown {
  char name[VKR_DEPOT_DISCOVERY_TEXT];
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  char address[VKR_NET_ADDRESS_TEXT];
  uint8_t key[32];
} DepotKnown;

typedef struct DepotPerson {
  uint8_t key[32];
  char name[VKR_DEPOT_DISPLAY_NAME];
} DepotPerson;

typedef struct DepotProject {
  char path[DEPOT_PATH];
  char name[96];
  char project_id[40];
  /* Its user let the scripts others committed run here. */
  bool8_t trusted;
} DepotProject;

struct VkrEditorDepot {
  VkrAllocator *allocator;
  /* Asset streaming: the remote content directory's server, installed as
     the VFS remote and overlay until the editor closes. */
  VkrDepotRemote *stream;
  char stream_directory[DEPOT_PATH];
  /* Platform clock seconds when it opened, and how long opening took. */
  float64_t stream_opened_at;
  float64_t stream_open_seconds;
  bool8_t registry_loaded;
  DepotKnown known[DEPOT_KNOWN_MAX];
  uint32_t known_count;
  DepotProject projects[DEPOT_PROJECTS_MAX];
  uint32_t project_count;
  DepotPerson people[DEPOT_PEOPLE_MAX];
  uint32_t people_count;

  /* The open workspace folder and what the depot makes of it. */
  char workspace[DEPOT_PATH];
  char project_id[40];
  char project_name[96];
  /* The workspace is a depot working copy, and this editor serves its
     store (the store holds a server key). */
  bool8_t repository;
  bool8_t serving;
  /* The store's remotes and the one steps use: origin, else the first. */
  VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
  uint32_t remote_count;
  uint32_t selected_remote;
  /* What each remote's last exchange gave, by index. */
  VkrDepotConnectStatus remote_connect[VKR_DEPOT_REMOTES_MAX];
  bool8_t remote_tried[VKR_DEPOT_REMOTES_MAX];
  /* This editor's own server, over its loopback, for the cooked side
     branch of a served repository. */
  char address[DEPOT_TEXT];
  uint8_t server_key[32];

  /* Allocates its networking from memory of its own; the retained
     allocator makes only its thread's handle, on the UI thread. */
  VkrDepotService *service;
  VkrDepotServiceInfo service_info;
  char service_error[DEPOT_TEXT];
  float64_t service_polled;
  VkrDepotAccessRequest requests[DEPOT_REQUESTS_MAX];
  uint32_t request_count;
  VkrDepotAccessEntry access[DEPOT_ACCESS_MAX];
  uint32_t access_count;

  VkrDepotBrowser browser;
  bool8_t browser_open;
  float64_t browser_probed;
  bool8_t browse_wanted;

  DepotJob *job;
  float64_t next_status;
  /* Latest finished results. */
  bool8_t status_known;
  bool8_t connected;
  VkrDepotConnectStatus connect;
  uint32_t change_count;
  /* The textures the last Get or Get latest cooked, took from the cooked
     side branch and failed to cook. */
  uint32_t textures_cooked;
  uint32_t textures_fetched;
  uint32_t textures_failed;
  DepotText changes[DEPOT_LIST_MAX];
  uint32_t conflict_count;
  DepotText conflicts[DEPOT_LIST_MAX];
  uint32_t untracked_count;
  uint64_t untracked_bytes;
  DepotText untracked[DEPOT_LIST_MAX];
  VkrDepotId base;
  VkrDepotId remote_head;
  bool8_t remote_known;
  char branch[VKR_DEPOT_BRANCH_NAME];
  uint32_t ahead;
  uint32_t behind;
  uint32_t branch_count;
  DepotBranch branches[DEPOT_BRANCHES_MAX];
  uint32_t outgoing_count;
  DepotHistory outgoing[DEPOT_COMMITS_MAX];
  uint32_t incoming_count;
  DepotHistory incoming[DEPOT_COMMITS_MAX];
  uint32_t history_count;
  DepotHistory history[DEPOT_HISTORY_MAX];
  char error[DEPOT_TEXT];
  char notice[DEPOT_TEXT];
  /* A scene reload the last pull asked for, sent once the scene is saved. */
  bool8_t reload_pending;
  /* The scripts prompt was dismissed for this run of the editor. */
  bool8_t scripts_prompt_dismissed;
  /* The job to repeat once a server allows this computer. */
  DepotJobKind waiting_kind;
  float64_t retry_at;

  /* Forms. */
  char message[DEPOT_TEXT];
  char init_destination[DEPOT_PATH];
  bool8_t init_serve;
  char new_branch[VKR_DEPOT_BRANCH_NAME];
  char remote_name[VKR_DEPOT_REMOTE_NAME];
  char remote_address[DEPOT_TEXT];
  char remote_key[VKR_DEPOT_ID_HEX];
  bool8_t remote_form_open;
  char find_address[DEPOT_TEXT];
  bool8_t dialog_open;
  char dialog_name[VKR_DEPOT_DISCOVERY_TEXT];
  char dialog_host[VKR_DEPOT_DISCOVERY_TEXT];
  char dialog_address[VKR_NET_ADDRESS_TEXT];
  uint8_t dialog_key[32];
  char dialog_destination[DEPOT_PATH];
  /* The Clone makes a sparse working copy. */
  bool8_t dialog_sparse;
  /* A finished clone the hub opens on its next build. */
  char open_directory[DEPOT_PATH];
  char open_project[40];
  float64_t open_retry_at;
  /* A finished Make repository the editor switches to. */
  bool8_t init_switch;
  float32_t window_scroll;
  float32_t window_height;
};

// =============================================================================
// Text helpers
// =============================================================================

static String8 depot_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

static void depot_key_hex(const uint8_t key[32], char out[VKR_DEPOT_ID_HEX]) {
  VkrDepotId id;
  MemCopy(id.bytes, key, sizeof(id.bytes));
  vkr_depot_id_hex(&id, out);
}

static bool8_t depot_key_parse(const char *hex, uint8_t out[32]) {
  VkrDepotId id;
  if (!hex || string_length(hex) != 64u || !vkr_depot_id_parse(hex, &id)) {
    return false_v;
  }
  MemCopy(out, id.bytes, sizeof(id.bytes));
  return true_v;
}

static bool8_t depot_key_zero(const uint8_t key[32]) {
  for (uint32_t i = 0u; i < 32u; ++i) {
    if (key[i]) {
      return false_v;
    }
  }
  return true_v;
}

/* Copies `text` keeping only characters a file name holds on every
   platform. */
static void depot_folder_name(const char *text, char *out, uint32_t capacity) {
  uint32_t length = 0u;
  for (const char *c = text; *c && length + 1u < capacity; ++c) {
    const char ch = *c;
    const bool8_t bad = ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
                        ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
                        ch == '|' || (unsigned char)ch < 32u;
    out[length++] = bad ? '_' : ch;
  }
  while (length && (out[length - 1u] == ' ' || out[length - 1u] == '.')) {
    --length;
  }
  out[length] = '\0';
  if (!length) {
    string_format(out, capacity, "Project");
  }
}

/* `<home>/VKR Projects/<name>`, made unique with a number when taken. */
static void depot_default_destination(const char *name, char *out,
                                      uint32_t capacity) {
  char folder[96];
  depot_folder_name(name, folder, sizeof(folder));
  char home[DEPOT_PATH];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_HOME, home,
                                   sizeof(home))) {
    string_format(home, sizeof(home), ".");
  }
  char base[DEPOT_PATH];
  string_format(base, sizeof(base), "%s/VKR Projects/%s", home, folder);
  string_format(out, capacity, "%s", base);
  for (uint32_t i = 2u; vkr_bakery_is_directory(out); ++i) {
    string_format(out, capacity, "%s %u", base, i);
  }
}

/* Unix microseconds now, as commits record their time. */
static uint64_t depot_unix_us(void) {
  return (uint64_t)vkr_platform_get_unix_time() * 1000000ull;
}

/* "just now", "5 min ago", "3 h ago" or "2 days ago". */
static void depot_format_age(uint64_t now_us, uint64_t then_us, char *out,
                             uint32_t capacity) {
  const uint64_t seconds =
      now_us > then_us ? (now_us - then_us) / 1000000ull : 0u;
  if (seconds < 60u) {
    string_format(out, capacity, "just now");
  } else if (seconds < 3600u) {
    string_format(out, capacity, "%llu min ago",
                  (unsigned long long)(seconds / 60u));
  } else if (seconds < 86400u) {
    string_format(out, capacity, "%llu h ago",
                  (unsigned long long)(seconds / 3600u));
  } else {
    string_format(out, capacity, "%llu day%s ago",
                  (unsigned long long)(seconds / 86400u),
                  seconds / 86400u == 1u ? "" : "s");
  }
}

static void depot_format_size(uint64_t bytes, char *out, uint32_t capacity) {
  if (bytes >= 1000000000ull) {
    string_format(out, capacity, "%.2f GB", (float64_t)bytes / 1e9);
  } else if (bytes >= 1000000ull) {
    string_format(out, capacity, "%.1f MB", (float64_t)bytes / 1e6);
  } else {
    string_format(out, capacity, "%.0f KB", (float64_t)bytes / 1e3);
  }
}

// =============================================================================
// Registry: <user data>/depot.json
// =============================================================================

/* A string member copied into `out`; false when absent or too long. */
static bool8_t depot_json_text(const VkrBakeryJson *object, const char *key,
                               char *out, uint32_t capacity) {
  String8 value = {0};
  if (!object || !vkr_bakery_json_get_string(object, key, &value) ||
      value.length >= capacity) {
    return false_v;
  }
  MemCopy(out, value.str, value.length);
  out[value.length] = '\0';
  return true_v;
}

static bool8_t depot_registry_path(char *out, uint32_t capacity) {
  char directory[DEPOT_PATH];
  return vkr_bakery_user_data_directory(directory, sizeof(directory)) &&
         string_format(out, capacity, "%s/depot.json", directory) <
             (int32_t)capacity;
}

static void depot_registry_load(VkrEditorDepot *depot) {
  depot->registry_loaded = true_v;
  char path[DEPOT_PATH];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!depot_registry_path(path, sizeof(path)) || !vkr_bakery_is_file(path) ||
      !vkr_bakery_read_file(path, MB(1), &bytes, &size)) {
    return;
  }
  Arena *arena = arena_create(KB(256), KB(64));
  const VkrBakeryJson *root =
      arena ? vkr_bakery_json_parse(arena, bytes, size, 16u, NULL) : NULL;
  const VkrBakeryJson *servers = vkr_bakery_json_get(root, "servers");
  for (const VkrBakeryJson *entry = servers ? servers->first : NULL;
       entry && depot->known_count < DEPOT_KNOWN_MAX; entry = entry->next) {
    DepotKnown *known = &depot->known[depot->known_count];
    char key[80] = {0};
    if (depot_json_text(entry, "key", key, sizeof(key)) &&
        depot_key_parse(key, known->key) &&
        depot_json_text(entry, "address", known->address,
                        sizeof(known->address))) {
      (void)depot_json_text(entry, "name", known->name, sizeof(known->name));
      (void)depot_json_text(entry, "host", known->host, sizeof(known->host));
      depot->known_count++;
    }
  }
  const VkrBakeryJson *people = vkr_bakery_json_get(root, "people");
  for (const VkrBakeryJson *entry = people ? people->first : NULL;
       entry && depot->people_count < DEPOT_PEOPLE_MAX; entry = entry->next) {
    DepotPerson *person = &depot->people[depot->people_count];
    char key[80] = {0};
    if (depot_json_text(entry, "key", key, sizeof(key)) &&
        depot_key_parse(key, person->key) &&
        depot_json_text(entry, "name", person->name, sizeof(person->name))) {
      depot->people_count++;
    }
  }
  const VkrBakeryJson *projects = vkr_bakery_json_get(root, "projects");
  for (const VkrBakeryJson *entry = projects ? projects->first : NULL;
       entry && depot->project_count < DEPOT_PROJECTS_MAX;
       entry = entry->next) {
    DepotProject *project = &depot->projects[depot->project_count];
    if (depot_json_text(entry, "path", project->path, sizeof(project->path)) &&
        vkr_bakery_is_directory(project->path)) {
      (void)depot_json_text(entry, "name", project->name,
                            sizeof(project->name));
      (void)depot_json_text(entry, "project", project->project_id,
                            sizeof(project->project_id));
      (void)vkr_bakery_json_get_bool(entry, "trusted", &project->trusted);
      depot->project_count++;
    }
  }
  if (arena) {
    arena_destroy(arena);
  }
  vkr_heap_free(bytes);
}

static void depot_registry_save(const VkrEditorDepot *depot) {
  char path[DEPOT_PATH];
  if (!depot_registry_path(path, sizeof(path))) {
    return;
  }
  Arena *arena = arena_create(KB(256), KB(64));
  if (!arena) {
    return;
  }
  VkrBakeryJson *root = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, root, "version", vkr_bakery_json_int(arena, 1));
  VkrBakeryJson *servers = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < depot->known_count; ++i) {
    const DepotKnown *known = &depot->known[i];
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(known->key, key);
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, known->name));
    vkr_bakery_json_set(arena, entry, "host",
                        vkr_bakery_json_cstr(arena, known->host));
    vkr_bakery_json_set(arena, entry, "address",
                        vkr_bakery_json_cstr(arena, known->address));
    vkr_bakery_json_set(arena, entry, "key", vkr_bakery_json_cstr(arena, key));
    vkr_bakery_json_append(servers, entry);
  }
  vkr_bakery_json_set(arena, root, "servers", servers);
  VkrBakeryJson *projects = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < depot->project_count; ++i) {
    const DepotProject *project = &depot->projects[i];
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "path",
                        vkr_bakery_json_cstr(arena, project->path));
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, project->name));
    vkr_bakery_json_set(arena, entry, "project",
                        vkr_bakery_json_cstr(arena, project->project_id));
    vkr_bakery_json_set(arena, entry, "trusted",
                        vkr_bakery_json_bool(arena, project->trusted));
    vkr_bakery_json_append(projects, entry);
  }
  vkr_bakery_json_set(arena, root, "projects", projects);
  VkrBakeryJson *people = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < depot->people_count; ++i) {
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(depot->people[i].key, key);
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "key", vkr_bakery_json_cstr(arena, key));
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, depot->people[i].name));
    vkr_bakery_json_append(people, entry);
  }
  vkr_bakery_json_set(arena, root, "people", people);
  String8 text = {0};
  char directory[DEPOT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (vkr_bakery_json_write(arena, root, VKR_BAKERY_JSON_PRETTY, &text) &&
      vkr_bakery_make_directories(directory) &&
      !vkr_bakery_write_file_atomic(path, text.str, text.length)) {
    log_warn("Source control: %s could not be written", path);
  }
  arena_destroy(arena);
}

/* Remembers a server by its key, newest first. */
static void depot_remember_server(VkrEditorDepot *depot, const char *name,
                                  const char *host, const char *address,
                                  const uint8_t key[32]) {
  uint32_t at = depot->known_count;
  for (uint32_t i = 0u; i < depot->known_count; ++i) {
    if (MemCompare(depot->known[i].key, key, 32u) == 0) {
      at = i;
      break;
    }
  }
  DepotKnown entry = {0};
  string_format(entry.name, sizeof(entry.name), "%s", name ? name : "");
  string_format(entry.host, sizeof(entry.host), "%s", host ? host : "");
  string_format(entry.address, sizeof(entry.address), "%s", address);
  MemCopy(entry.key, key, 32u);
  if (at == depot->known_count && at == DEPOT_KNOWN_MAX) {
    at = DEPOT_KNOWN_MAX - 1u;
  } else if (at == depot->known_count) {
    depot->known_count++;
  }
  MemCopy(&depot->known[1], &depot->known[0], at * sizeof(DepotKnown));
  depot->known[0] = entry;
  depot_registry_save(depot);
}

static void depot_remember_project(VkrEditorDepot *depot, const char *path,
                                   const char *name, const char *project_id,
                                   bool8_t trusted) {
  uint32_t at = depot->project_count;
  for (uint32_t i = 0u; i < depot->project_count; ++i) {
    if (string_equals(depot->projects[i].path, path)) {
      at = i;
      break;
    }
  }
  if (at == depot->project_count) {
    if (at == DEPOT_PROJECTS_MAX) {
      return;
    }
    depot->project_count++;
    depot->projects[at].trusted = trusted;
  }
  DepotProject *project = &depot->projects[at];
  string_format(project->path, sizeof(project->path), "%s", path);
  string_format(project->name, sizeof(project->name), "%s", name);
  string_format(project->project_id, sizeof(project->project_id), "%s",
                project_id ? project_id : "");
  depot_registry_save(depot);
}

static DepotProject *depot_project_entry(VkrEditorDepot *depot,
                                         const char *path) {
  for (uint32_t i = 0u; i < depot->project_count; ++i) {
    if (string_equals(depot->projects[i].path, path)) {
      return &depot->projects[i];
    }
  }
  return NULL;
}

static void depot_set_trusted(VkrEditorDepot *depot, const char *path,
                              bool8_t trusted) {
  DepotProject *project = depot_project_entry(depot, path);
  if (project && project->trusted != trusted) {
    project->trusted = trusted;
    depot_registry_save(depot);
  }
}

/* The first 8 hexadecimal digits of a key as `ab12 cd34`: both computers
   show it, so people can tell their own key from an impostor's. */
static void depot_fingerprint(const uint8_t key[32], char out[16]) {
  char hex[VKR_DEPOT_ID_HEX];
  depot_key_hex(key, hex);
  string_format(out, 16u, "%.4s %.4s", hex, hex + 4);
}

/* Whether this editor got a project from, or allowed, this key before. */
static bool8_t depot_key_known(const VkrEditorDepot *depot,
                               const uint8_t key[32]) {
  for (uint32_t i = 0u; i < depot->known_count; ++i) {
    if (MemCompare(depot->known[i].key, key, 32u) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* Remembers the name a key asked with, for the access list. */
static void depot_remember_person(VkrEditorDepot *depot, const uint8_t key[32],
                                  const char *name) {
  if (!name || !name[0]) {
    return;
  }
  uint32_t at = depot->people_count;
  for (uint32_t i = 0u; i < depot->people_count; ++i) {
    if (MemCompare(depot->people[i].key, key, 32u) == 0) {
      at = i;
      break;
    }
  }
  if (at == depot->people_count) {
    if (at == DEPOT_PEOPLE_MAX) {
      return;
    }
    depot->people_count++;
  }
  MemCopy(depot->people[at].key, key, 32u);
  string_format(depot->people[at].name, sizeof(depot->people[at].name), "%s",
                name);
  depot_registry_save(depot);
}

/* `user@computer`, as a client sends it, shown as `user on computer`; a key
   without a name shows its first digits. */
static void depot_person_text(const VkrEditorDepot *depot,
                              const uint8_t key[32], const char *name,
                              char *out, uint32_t capacity) {
  for (uint32_t i = 0u; (!name || !name[0]) && i < depot->people_count; ++i) {
    if (MemCompare(depot->people[i].key, key, 32u) == 0) {
      name = depot->people[i].name;
    }
  }
  if (!name || !name[0]) {
    char hex[VKR_DEPOT_ID_HEX];
    depot_key_hex(key, hex);
    string_format(out, capacity, "Computer %.8s", hex);
    return;
  }
  const char *at = string_find_char(name, '@');
  if (at) {
    string_format(out, capacity, "%.*s on %s", (int)(at - name), name, at + 1);
  } else {
    string_format(out, capacity, "%s", name);
  }
}

// =============================================================================
// Worker steps
// =============================================================================

typedef struct DepotWorker {
  DepotJob *job;
  VkrDMemory memory;
  VkrAllocator allocator;
  VkrDepotWorkspace *workspace;
} DepotWorker;

static VkrDepotConnection *depot_job_connect(DepotWorker *worker,
                                             const VkrDepotStore *store) {
  DepotJob *job = worker->job;
  const VkrDepotConnectConfig config = {
      .address = job->address,
      .server_key = job->server_key,
      .store = store,
      .timeout_ms = 8000u,
      .progress = &job->progress,
  };
  return vkr_depot_connect(&worker->allocator, &config, &job->connect,
                           job->error, sizeof(job->error));
}

static void depot_job_add_text(DepotText *list, uint32_t *count,
                               const char *path) {
  if (*count < DEPOT_LIST_MAX) {
    string_format(list[*count].text, sizeof(list[*count].text), "%s", path);
  }
  (*count)++;
}

static void depot_job_take_changes(DepotJob *job,
                                   const VkrDepotChangeList *changes,
                                   DepotText *list, uint32_t *count) {
  *count = 0u;
  for (uint32_t i = 0u; i < changes->count; ++i) {
    const VkrDepotChangeEntry *entry = &changes->entries[i];
    char text[DEPOT_TEXT];
    string_format(text, sizeof(text), "%c %s",
                  entry->change == VKR_DEPOT_CHANGE_ADDED     ? '+'
                  : entry->change == VKR_DEPOT_CHANGE_DELETED ? '-'
                                                              : '~',
                  entry->path);
    depot_job_add_text(list, count, text);
  }
  (void)job;
}

typedef struct DepotHistoryVisit {
  DepotJob *job;
} DepotHistoryVisit;

static bool8_t depot_history_visit(void *context,
                                   const VkrDepotLogEntry *entry) {
  DepotJob *job = context;
  if (job->history_count >= DEPOT_HISTORY_MAX) {
    return false_v;
  }
  DepotHistory *row = &job->history[job->history_count++];
  vkr_depot_id_hex(&entry->id, row->id);
  const uint32_t length =
      Min(entry->message_size, (uint32_t)sizeof(row->message) - 1u);
  MemCopy(row->message, entry->message, length);
  row->message[length] = '\0';
  for (char *c = row->message; *c; ++c) {
    if (*c == '\n' || *c == '\r') {
      *c = '\0';
      break;
    }
  }
  row->time_us = entry->time_us;
  return true_v;
}

static void depot_job_history(DepotJob *job, const VkrDepotWorkspace *ws) {
  job->history_count = 0u;
  if (!vkr_depot_id_zero(&ws->base)) {
    (void)vkr_depot_log(&ws->store, &ws->base, DEPOT_HISTORY_MAX,
                        depot_history_visit, job);
  }
}

static bool8_t depot_branch_visit(void *context, const char *branch) {
  DepotJob *job = context;
  if (job->branch_count < DEPOT_BRANCHES_MAX) {
    string_format(job->branches[job->branch_count++].name,
                  VKR_DEPOT_BRANCH_NAME, "%s", branch);
  }
  return true_v;
}

typedef struct DepotCommitRows {
  DepotHistory *rows;
  uint32_t *count;
} DepotCommitRows;

static bool8_t depot_commit_row_visit(void *context,
                                      const VkrDepotLogEntry *entry) {
  DepotCommitRows *rows = context;
  if (*rows->count >= DEPOT_COMMITS_MAX) {
    return false_v;
  }
  DepotHistory *row = &rows->rows[(*rows->count)++];
  vkr_depot_id_hex(&entry->id, row->id);
  const uint32_t length =
      Min(entry->message_size, (uint32_t)sizeof(row->message) - 1u);
  MemCopy(row->message, entry->message, length);
  row->message[length] = '\0';
  for (char *c = row->message; *c; ++c) {
    if (*c == '\n' || *c == '\r') {
      *c = '\0';
      break;
    }
  }
  row->time_us = entry->time_us;
  return true_v;
}

/* The branch, the store's branches, and how the branch stands against the
   remote's as last fetched: commits to push and to pull, newest first. */
static void depot_job_summary(DepotJob *job, const VkrDepotWorkspace *ws) {
  string_format(job->current_branch, sizeof(job->current_branch), "%s",
                ws->branch);
  job->branch_count = 0u;
  (void)vkr_depot_ref_list(ws->store.root, depot_branch_visit, job);
  job->ahead = 0u;
  job->behind = 0u;
  job->outgoing_count = 0u;
  job->incoming_count = 0u;
  job->remote_known = false_v;
  VkrDepotId local = {0};
  if (!vkr_depot_ref_read(ws->store.root, ws->branch, &local)) {
    local = ws->base;
  }
  if (!job->remote[0] ||
      !vkr_depot_tracking_read(ws->store.root, job->remote, ws->branch,
                               &job->remote_head)) {
    return;
  }
  job->remote_known = true_v;
  if (!vkr_depot_ahead_behind(&ws->store, &local, &job->remote_head,
                              &job->ahead, &job->behind)) {
    return;
  }
  DepotCommitRows outgoing = {job->outgoing, &job->outgoing_count};
  DepotCommitRows incoming = {job->incoming, &job->incoming_count};
  (void)vkr_depot_log(&ws->store, &local, Min(job->ahead, DEPOT_COMMITS_MAX),
                      depot_commit_row_visit, &outgoing);
  (void)vkr_depot_log(&ws->store, &job->remote_head,
                      Min(job->behind, DEPOT_COMMITS_MAX),
                      depot_commit_row_visit, &incoming);
}

/* Whether a path a pull wrote is part of a scene, its World or the
   project's documents, which a reload must read again. */
static bool8_t depot_scene_path(const char *path) {
  return string_find(path, ".vkreditor/projects/") != NULL;
}

static bool8_t depot_ends_with(const char *text, const char *suffix) {
  const size_t length = string_length(text);
  const size_t suffix_length = string_length(suffix);
  return length >= suffix_length &&
         string_equals(text + length - suffix_length, suffix);
}

typedef struct DepotCookList {
  char (*folders)[DEPOT_PATH];
  uint32_t count;
  uint32_t capacity;
} DepotCookList;

/* Adds the folder of `path` once. */
static void depot_cook_add(DepotCookList *list, const char *root,
                           const char *path) {
  char full[DEPOT_PATH];
  char folder[DEPOT_PATH];
  string_format(full, sizeof(full), "%s/%s", root, path);
  vkr_bakery_path_parent(folder, sizeof(folder), full);
  for (uint32_t i = 0u; i < list->count; ++i) {
    if (string_equals(list->folders[i], folder)) {
      return;
    }
  }
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
    void *grown =
        vkr_heap_realloc(list->folders, capacity * sizeof(*list->folders));
    if (!grown) {
      return;
    }
    list->folders = grown;
    list->capacity = capacity;
  }
  string_format(list->folders[list->count++], DEPOT_PATH, "%s", folder);
}

/* A texture source whose cooked file for this computer is missing. */
static bool8_t depot_needs_cook(const char *root, const char *path) {
  /* Project builds re-cook from their recipes (depot_job_recook); their
     source snapshots are not textures the scenes read. */
  if (string_n_equals(path, ".vkreditor/", 11u)) {
    return false_v;
  }
  if (!depot_ends_with(path, ".png") && !depot_ends_with(path, ".jpg") &&
      !depot_ends_with(path, ".jpeg") && !depot_ends_with(path, ".tga")) {
    return false_v;
  }
  char cooked[DEPOT_PATH];
  string_format(cooked, sizeof(cooked), "%s/%s.vkt", root, path);
  return !vkr_bakery_is_file(cooked);
}

static bool8_t depot_cook_cancelled(void *context) {
  const DepotJob *job = context;
  return vkr_depot_progress_canceled(&job->progress);
}

/* Cooks each folder that holds texture sources without a cooked file for
   this computer: other platforms cook other formats, so cooked textures
   never travel (.vkrdepotignore). One Bakery process at a time. */
static void depot_job_cook(DepotJob *job, const char *root,
                           const VkrDepotFileList *files) {
  DepotCookList list = {0};
  for (uint32_t i = 0u; i < files->count; ++i) {
    if (depot_needs_cook(root, files->entries[i].path)) {
      depot_cook_add(&list, root, files->entries[i].path);
    }
  }
  vkr_atomic_uint32_store(&job->cook_total, list.count,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&job->cook_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  for (uint32_t i = 0u; i < list.count; ++i) {
    if (vkr_depot_progress_canceled(&job->progress)) {
      break;
    }
    const char *arguments[] = {"cook", list.folders[i], "--root", root};
    const VkrPlatformProcessConfig config = {
        .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
        .arguments = arguments,
        .argument_count = ArrayCount(arguments),
        .working_directory = root,
        .timeout_ms = 20u * 60u * 1000u,
        .termination_grace_ms = 250u,
        .terminate_process_tree = true_v,
        .hidden = true_v,
        .is_cancelled = depot_cook_cancelled,
        .cancel_context = job,
    };
    int32_t exit_code = 0;
    bool8_t timed_out = false_v;
    if (vkr_platform_process_run(&config, &exit_code, &timed_out) &&
        exit_code == 0) {
      job->textures_cooked++;
    } else {
      job->textures_failed++;
      log_warn("Source control: textures in %s did not cook (exit %d)",
               list.folders[i], exit_code);
    }
    vkr_atomic_uint32_store(&job->cook_done, i + 1u, VKR_MEMORY_ORDER_RELAXED);
  }
  vkr_heap_free(list.folders);
}

typedef struct DepotPullVisit {
  DepotJob *job;
} DepotPullVisit;

/* Adds the scene id of `path`, `.vkreditor/projects/<id>/scenes/<id>/...`,
   to the scenes the pull report checks. */
static void depot_note_scene(DepotJob *job, const char *path) {
  static const char prefix[] = ".vkreditor/projects/";
  const size_t length = sizeof(prefix) - 1u;
  if (!string_n_equals(path, prefix, length) ||
      string_length(path) < length + 36u + 8u + 36u ||
      !string_n_equals(path + length + 36u, "/scenes/", 8u)) {
    return;
  }
  const char *scene = path + length + 36u + 8u;
  for (uint32_t i = 0u; i < Min(job->changed_scene_count, DEPOT_LIST_MAX);
       ++i) {
    if (string_n_equals(job->changed_scenes[i], scene, 36u)) {
      return;
    }
  }
  if (job->changed_scene_count < DEPOT_LIST_MAX) {
    string_format(job->changed_scenes[job->changed_scene_count], 40u, "%.36s",
                  scene);
  }
  job->changed_scene_count++;
}

static bool8_t depot_pull_visit(void *context, const char *path,
                                VkrDepotChange change) {
  DepotJob *job = context;
  (void)change;
  if (depot_scene_path(path)) {
    job->scene_files_changed = true_v;
    depot_note_scene(job, path);
  }
  if (depot_ends_with(path, ".mt") || depot_ends_with(path, ".mtg")) {
    job->materials_changed = true_v;
  }
  if (string_find(path, "/Scripts/")) {
    job->scripts_changed = true_v;
  }
  return true_v;
}

static void depot_job_recook(DepotJob *job, const char *folder,
                             bool8_t every_scene);

/* Writes the cooked textures this computer's side branch holds for the
   checked-out commit (ADR-123), so the cook that follows makes only the
   rest. A failure only leaves more to cook. */
static void depot_job_fetch_cooked(DepotJob *job, VkrDepotWorkspace *ws,
                                   VkrDepotClient *client) {
  VkrDepotCookedFetch fetched;
  char error[DEPOT_TEXT] = {0};
  if (!vkr_depot_fetch_cooked(ws, client, ws->branch, &fetched, error,
                              sizeof(error))) {
    log_warn("Source control: cooked textures did not arrive: %s", error);
    return;
  }
  job->textures_fetched = fetched.written;
  if (!vkr_depot_id_zero(&fetched.commit)) {
    log_info("Source control: %u cooked textures from %s (%u here already, "
             "%u to cook from changed sources)",
             fetched.written, fetched.branch, fetched.present, fetched.stale);
  }
}

/* Publishes this computer's cooked textures to the side branch, so another
   computer of its GPU family fetches them instead of cooking (ADR-123).
   The cooked cache keeps the snapshot to files that changed. A failure is
   reported and leaves the step's result alone. */
static void depot_job_publish_cooked(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  char saved[DEPOT_TEXT];
  string_format(saved, sizeof(saved), "%s", job->error);
  VkrDepotId tree;
  uint32_t files = 0u;
  char error[DEPOT_TEXT] = {0};
  if (!vkr_depot_cooked_snapshot(ws, &tree, &files, error, sizeof(error))) {
    if (files) {
      log_warn("Source control: cooked textures were not published: %s", error);
    }
    return;
  }
  VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  VkrDepotCookedResult result;
  if (client && vkr_depot_publish_cooked(ws, client, ws->branch, &tree, files,
                                         &result, error, sizeof(error))) {
    if (!result.unchanged) {
      log_info("Source control: published %u cooked textures to %s",
               result.files, result.branch);
    }
  } else {
    log_warn("Source control: cooked textures were not published: %s",
             client ? error : job->error);
  }
  vkr_depot_disconnect(connection);
  string_format(job->error, sizeof(job->error), "%s", saved);
}

/* The texture work that follows files a pull, switch or merge wrote:
   cooked textures from this computer's side branch on the remote, then a
   cook of the rest, published back for others of its GPU family. A sparse
   working copy's textures come cooked from the side branch when read; a
   cook would read every source. */
static void depot_job_after_checkout(DepotWorker *worker,
                                     const VkrDepotFileList *before) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  (void)vkr_depot_file_list_diff(before, &ws->index, depot_pull_visit, job);
  if (ws->sparse) {
    return;
  }
  if (job->address[0]) {
    VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
    VkrDepotClient *client = vkr_depot_connection_client(connection);
    if (client) {
      depot_job_fetch_cooked(job, ws, client);
    }
    vkr_depot_disconnect(connection);
    job->error[0] = '\0';
  }
  depot_job_cook(job, ws->root, &ws->index);
  depot_job_recook(job, ws->root, false_v);
  if (job->textures_cooked && job->address[0]) {
    depot_job_publish_cooked(worker);
  }
}

static VkrDepotFileList depot_index_copy(const VkrDepotWorkspace *ws) {
  VkrDepotFileList copy = {0};
  for (uint32_t i = 0u; i < ws->index.count; ++i) {
    const VkrDepotIndexEntry *entry = &ws->index.entries[i];
    (void)vkr_depot_file_list_add(&copy, entry->path, entry->size,
                                  entry->mtime_ns, &entry->blob);
  }
  return copy;
}

/* Pulls the branch from the remote: a fast-forward, or a merge when both
   sides moved it, whose scene units Bakery merges. Conflicts it could not
   settle under the job's policy stop it with them listed. */
static void depot_job_pull(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  VkrDepotFileList before = depot_index_copy(ws);
  char bakery[DEPOT_PATH];
  string_format(bakery, sizeof(bakery), "%s",
                vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY));
  const VkrDepotMergeConfig config = {
      .driver = vkr_depot_merge_bakery_driver,
      .driver_context = bakery,
      .policy = job->policy,
  };
  VkrDepotPullResult pulled;
  VkrDepotMergeResult merged;
  job->ok = vkr_depot_pull_merge(&worker->allocator, ws, job->remote, &config,
                                 &pulled, &merged, &job->connect, job->error,
                                 sizeof(job->error));
  depot_job_take_changes(
      job, merged.conflicts.count ? &merged.conflicts : &pulled.conflicts,
      job->conflicts, &job->conflict_count);
  if (job->ok && merged.status == VKR_DEPOT_MERGE_CONFLICTS) {
    job->ok = false_v;
    string_format(job->error, sizeof(job->error),
                  "%u files changed here and on %s; choose whose to keep",
                  merged.conflicts.count, job->remote);
  } else if (job->ok && pulled.status == VKR_DEPOT_PULL_DIVERGED) {
    job->merged = merged.status == VKR_DEPOT_MERGE_MERGED;
  } else if (job->ok) {
    job->merged = pulled.status == VKR_DEPOT_PULL_FAST_FORWARD;
  }
  vkr_depot_change_list_free(&pulled.conflicts);
  vkr_depot_change_list_free(&merged.conflicts);
  if (job->ok) {
    /* The policy settled every conflict the pull met. */
    job->conflict_count = 0u;
    depot_job_after_checkout(worker, &before);
  }
  vkr_depot_file_list_free(&before);
}

/* Pushes the branch to the remote, then this computer's cooked textures to
   its side branch there. A sparse working copy holds only the cooked files
   it read: publishing them would drop the rest from the side branch. */
static void depot_job_push(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  VkrDepotPushResult pushed;
  job->ok = vkr_depot_push(&worker->allocator, ws, job->remote, NULL, &pushed,
                           &job->connect, job->error, sizeof(job->error));
  if (job->ok && !ws->sparse && pushed.status == VKR_DEPOT_PUSH_OK) {
    depot_job_publish_cooked(worker);
  }
}

/* Records every local change in the repository; a served repository
   publishes its cooked textures to its own side branch, for the computers
   that clone it. */
static bool8_t depot_project_job(DepotJob *job, const char *folder,
                                 Arena *arena, VkrBakeryJson *request,
                                 const char *what,
                                 const VkrBakeryJson **out_result);
static void depot_first_project(const char *directory, char out[40]);

/* Lists, and with the job's `adopt` copies into the project, the files it
   reads from outside itself; false when the commit should stop for them. */
static bool8_t depot_job_untracked(DepotJob *job, const char *folder) {
  char project_id[40];
  depot_first_project(folder, project_id);
  Arena *arena = project_id[0] ? arena_create(MB(1), KB(256)) : NULL;
  if (!arena) {
    return true_v;
  }
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path),
                "%s/.vkreditor/projects/%s/project.json", folder, project_id);
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "untracked_dependencies"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  vkr_bakery_json_set(arena, request, "legacy_root",
                      vkr_bakery_json_cstr(arena, vkr_content_root()));
  vkr_bakery_json_set(arena, request, "adopt",
                      vkr_bakery_json_bool(arena, job->adopt));
  const VkrBakeryJson *result = NULL;
  bool8_t proceed = true_v;
  if (depot_project_job(job, folder, arena, request,
                        "Looking for files from outside the project",
                        &result)) {
    const VkrBakeryJson *list = vkr_bakery_json_get(result, "untracked");
    int64_t bytes = 0;
    (void)vkr_bakery_json_get_int(result, "untracked_bytes", &bytes);
    job->untracked_bytes = (uint64_t)bytes;
    job->untracked_count = 0u;
    for (const VkrBakeryJson *entry = list ? list->first : NULL; entry;
         entry = entry->next) {
      char path[DEPOT_TEXT] = {0};
      (void)depot_json_text(entry, "path", path, sizeof(path));
      depot_job_add_text(job->untracked, &job->untracked_count, path);
    }
    if (job->untracked_count && !job->adopt) {
      string_format(job->error, sizeof(job->error),
                    "%u files this project uses come from outside it; "
                    "another computer lacks them",
                    job->untracked_count);
      proceed = false_v;
    } else if (job->adopt) {
      job->untracked_count = 0u;
    }
  } else {
    /* An older Bakery or a content root without git: the commit goes on,
       as it did before this check. */
    log_warn("Source control: %s", job->error);
    job->error[0] = '\0';
  }
  arena_destroy(arena);
  return proceed;
}

/* Removes the builds, overlays and inventories no live file names, with
   Bakery's `collect_garbage` rules (ADR-088 revisions stay, recent files
   get its grace period), so the commit does not carry them; pruned files
   stay in history. A failure leaves them and the commit goes on. */
static void depot_job_prune(DepotJob *job, const char *folder) {
  char project_id[40];
  depot_first_project(folder, project_id);
  Arena *arena = project_id[0] ? arena_create(KB(256), KB(64)) : NULL;
  if (!arena) {
    return;
  }
  /* The job names a project; the cleanup covers the whole workspace. */
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path),
                "%s/.vkreditor/projects/%s/project.json", folder, project_id);
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "collect_garbage"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  const VkrBakeryJson *result = NULL;
  if (depot_project_job(job, folder, arena, request,
                        "Removing files no scene uses", &result)) {
    int64_t removed = 0;
    int64_t freed = 0;
    (void)vkr_bakery_json_get_int(result, "removed", &removed);
    (void)vkr_bakery_json_get_int(result, "freed_bytes", &freed);
    if (removed > 0) {
      log_info("Source control: removed %lld unused paths (%.1f MiB) before "
               "the commit",
               (long long)removed, (float64_t)freed / 1048576.0);
    }
  } else {
    log_warn("Source control: %s", job->error);
    job->error[0] = '\0';
  }
  arena_destroy(arena);
}

static void depot_job_commit(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  /* A sparse copy holds only the files it read, so it cannot tell which
     ones no scene uses. */
  if (!ws->sparse) {
    depot_job_prune(job, ws->root);
  }
  if (!job->ignore_untracked && !ws->sparse &&
      !depot_job_untracked(job, ws->root)) {
    return;
  }
  VkrDepotCommitResult committed;
  job->ok = vkr_depot_commit_local(ws, job->message, &committed, job->error,
                                   sizeof(job->error));
  if (job->ok && committed.empty && !job->then_sync) {
    job->ok = false_v;
    string_format(job->error, sizeof(job->error), "Nothing to commit");
    return;
  }
  if (job->ok && job->then_sync && job->remote[0]) {
    depot_job_pull(worker);
    if (job->ok) {
      depot_job_push(worker);
    }
  }
}

static void depot_job_fetch_visit(void *context, const char *branch,
                                  VkrDepotFetchAction action,
                                  const VkrDepotId *commit,
                                  const char *detail) {
  (void)context;
  (void)commit;
  if (action == VKR_DEPOT_FETCH_FAILED) {
    log_warn("Source control: fetching %s failed: %s", branch, detail);
  }
}

static void depot_job_status(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  VkrDepotChangeList changes = {0};
  if (!vkr_depot_status(ws, &changes, job->error, sizeof(job->error))) {
    return;
  }
  depot_job_take_changes(job, &changes, job->changes, &job->change_count);
  vkr_depot_change_list_free(&changes);
  job->ok = true_v;
  /* The remote's branches, so the window shows what there is to pull; an
     unreachable remote leaves the last fetch's view. */
  if (job->remote[0] &&
      !vkr_depot_fetch(&worker->allocator, ws, job->remote, NULL,
                       depot_job_fetch_visit, NULL, &job->connect, job->error,
                       sizeof(job->error))) {
    log_info("Source control: %s: %s", job->remote, job->error);
    job->error[0] = '\0';
  }
}

/* Switches the working copy to another branch, keeping no local change a
   switch would overwrite, and makes the branch first when asked. */
static void depot_job_switch(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  if (job->create_branch &&
      !vkr_depot_branch_create(ws, job->branch, job->error,
                               sizeof(job->error))) {
    return;
  }
  VkrDepotFileList before = depot_index_copy(ws);
  uint32_t kept = 0u;
  VkrDepotChangeList conflicts = {0};
  job->ok = vkr_depot_switch(ws, job->branch, VKR_DEPOT_CONFLICT_FAIL, &kept,
                             &conflicts, job->error, sizeof(job->error));
  depot_job_take_changes(job, &conflicts, job->conflicts, &job->conflict_count);
  vkr_depot_change_list_free(&conflicts);
  if (job->ok) {
    depot_job_after_checkout(worker, &before);
  }
  vkr_depot_file_list_free(&before);
}

/* The id of the first project folder under `<directory>/.vkreditor/projects`.
 */
typedef struct DepotFirstProject {
  char id[40];
} DepotFirstProject;

static bool8_t depot_first_project_visit(void *context, const char *name,
                                         bool8_t directory) {
  DepotFirstProject *first = context;
  if (directory && string_length(name) == 36u) {
    string_format(first->id, sizeof(first->id), "%s", name);
    return false_v;
  }
  return true_v;
}

/* The first project a sparse working copy tracks: its folder may not be on
   disk yet. */
static void depot_first_tracked_project(const VkrDepotWorkspace *ws,
                                        char out[40]) {
  static const char prefix[] = ".vkreditor/projects/";
  for (uint32_t i = 0u; i < ws->index.count; ++i) {
    const char *path = ws->index.entries[i].path;
    if (string_n_equals(path, prefix, sizeof(prefix) - 1u) &&
        string_length(path) > sizeof(prefix) - 1u + 36u &&
        path[sizeof(prefix) - 1u + 36u] == '/') {
      string_format(out, 40u, "%.36s", path + sizeof(prefix) - 1u);
      return;
    }
  }
  out[0] = '\0';
}

static void depot_first_project(const char *directory, char out[40]) {
  char projects[DEPOT_PATH];
  string_format(projects, sizeof(projects), "%s/.vkreditor/projects",
                directory);
  DepotFirstProject first = {0};
  (void)vkr_bakery_list_directory(projects, depot_first_project_visit, &first);
  string_format(out, 40u, "%s", first.id);
}

static void depot_job_clone(DepotWorker *worker) {
  DepotJob *job = worker->job;
  const VkrDepotCloneConfig config = {
      .address = job->address,
      .server_key = job->server_key,
      .directory = job->directory,
      .branch = DEPOT_BRANCH,
      .codec = vkr_depot_zstd_codec(),
      .progress = &job->progress,
      .sparse = job->sparse,
  };
  VkrDepotCloneResult result = {0};
  job->ok = vkr_depot_clone(&worker->allocator, &config, &result, job->error,
                            sizeof(job->error));
  job->connect = result.connect;
  if (!job->ok) {
    return;
  }
  if (result.empty) {
    job->ok = false_v;
    string_format(
        job->error, sizeof(job->error),
        "The project has no files yet; its editor is still sharing it");
    return;
  }
  VkrDepotWorkspace *ws = worker->workspace;
  const bool8_t opened =
      vkr_depot_workspace_open(ws, job->directory, vkr_depot_zstd_codec());
  if (opened && ws->sparse) {
    /* Its files, cooked textures included, arrive when the editor and its
       Bakery jobs read them. */
    depot_job_history(job, ws);
    depot_first_tracked_project(ws, job->opened_project);
    vkr_depot_workspace_close(ws);
    return;
  }
  if (opened) {
    ws->progress = &job->progress;
    VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
    VkrDepotClient *client = vkr_depot_connection_client(connection);
    if (client) {
      depot_job_fetch_cooked(job, ws, client);
    }
    vkr_depot_disconnect(connection);
    job->error[0] = '\0';
    depot_job_cook(job, ws->root, &ws->index);
    depot_job_recook(job, ws->root, true_v);
    if (job->textures_cooked) {
      depot_job_publish_cooked(worker);
    }
    depot_job_history(job, ws);
    vkr_depot_workspace_close(ws);
  }
  depot_first_project(job->directory, job->opened_project);
}

/* Runs one Bakery project operation in the workspace at `folder`: writes
   `request` (to which it adds the version and the workspace root) into the
   workspace's jobs folder, runs it and parses its result into `arena`.
   False with the reason in the job's error; `what` names the step. */
static bool8_t depot_project_job(DepotJob *job, const char *folder,
                                 Arena *arena, VkrBakeryJson *request,
                                 const char *what,
                                 const VkrBakeryJson **out_result) {
  *out_result = NULL;
  char job_id[40];
  char jobs[DEPOT_PATH];
  char request_path[DEPOT_PATH];
  char result_path[DEPOT_PATH];
  char workspace_root[DEPOT_PATH];
  VkrEditorProjectError project_error = {0};
  if (!vkr_editor_project_id_generate(job_id, &project_error)) {
    string_format(job->error, sizeof(job->error), "%s", project_error.message);
    return false_v;
  }
  string_format(workspace_root, sizeof(workspace_root), "%s/.vkreditor",
                folder);
  string_format(jobs, sizeof(jobs), "%s/jobs/%s", workspace_root, job_id);
  string_format(request_path, sizeof(request_path), "%s/request.json", jobs);
  string_format(result_path, sizeof(result_path), "%s/result.json", jobs);
  vkr_bakery_json_set(arena, request, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, request, "read_only",
                      vkr_bakery_json_bool(arena, false_v));
  vkr_bakery_json_set(arena, request, "workspace_root",
                      vkr_bakery_json_cstr(arena, workspace_root));
  String8 text = {0};
  if (!vkr_bakery_make_directories(jobs) ||
      !vkr_bakery_json_write(arena, request, VKR_BAKERY_JSON_PRETTY, &text) ||
      !vkr_bakery_write_file_atomic(request_path, text.str, text.length)) {
    string_format(job->error, sizeof(job->error), "%s: %s could not be written",
                  what, request_path);
    return false_v;
  }
  const char *arguments[] = {"project", "--request", request_path, "--result",
                             result_path};
  const VkrPlatformProcessConfig config = {
      .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .working_directory = vkr_content_root(),
      .timeout_ms = 60u * 60u * 1000u,
      .termination_grace_ms = 2000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = depot_cook_cancelled,
      .cancel_context = job,
  };
  int32_t exit_code = 0;
  bool8_t timed_out = false_v;
  const bool8_t ran = vkr_platform_process_run(&config, &exit_code, &timed_out);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_read_file(result_path, MB(4), &bytes, &size)) {
    *out_result = vkr_bakery_json_parse(arena, bytes, size, 32u, NULL);
  }
  vkr_heap_free(bytes);
  /* The job folder is named by a fresh id, so nothing else uses it. */
  (void)vkr_bakery_remove_tree(jobs);
  if (ran && exit_code == 0 && *out_result) {
    return true_v;
  }
  char reason[DEPOT_TEXT] = {0};
  if (!depot_json_text(*out_result, "error", reason, sizeof(reason))) {
    string_format(reason, sizeof(reason),
                  timed_out ? "timed out" : "the Bakery step failed (exit %d)",
                  exit_code);
  }
  string_format(job->error, sizeof(job->error), "%s: %s", what, reason);
  return false_v;
}

/* Bakery's pull report for the scenes a pull changed (every scene with
   `every_scene`, as after a clone): for each, it re-cooks for this computer
   the textures its imported models' recipes name but lack here, then
   prepares the scene and says whether it opens. Cooked textures never
   travel, since each GPU family samples its own format. The project's own
   assets re-cook after it. A scene that does not open is the step's
   warning; the pull itself stands. */
static void depot_job_recook(DepotJob *job, const char *folder,
                             bool8_t every_scene) {
  char project_id[40];
  depot_first_project(folder, project_id);
  if (!project_id[0] ||
      (!every_scene && !job->changed_scene_count && !job->materials_changed)) {
    return;
  }
  Arena *arena = arena_create(MB(1), KB(256));
  if (!arena) {
    return;
  }
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path),
                "%s/.vkreditor/projects/%s/project.json", folder, project_id);
  vkr_atomic_uint32_store(&job->cook_total, 2u, VKR_MEMORY_ORDER_RELAXED);
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "pull_report"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  if (!every_scene && job->changed_scene_count <= DEPOT_LIST_MAX) {
    VkrBakeryJson *scenes = vkr_bakery_json_array(arena);
    for (uint32_t i = 0u; i < job->changed_scene_count; ++i) {
      vkr_bakery_json_append(
          scenes, vkr_bakery_json_cstr(arena, job->changed_scenes[i]));
    }
    vkr_bakery_json_set(arena, request, "scenes", scenes);
  }
  const VkrBakeryJson *result = NULL;
  if ((every_scene || job->changed_scene_count) &&
      !vkr_depot_progress_canceled(&job->progress)) {
    if (depot_project_job(job, folder, arena, request, "Preparing scenes",
                          &result)) {
      const VkrBakeryJson *reports = vkr_bakery_json_get(result, "scenes");
      int64_t open = 0;
      (void)vkr_bakery_json_get_int(result, "scenes_open", &open);
      log_info("Source control: %lld of %u pulled scenes open", (long long)open,
               reports ? reports->count : 0u);
      for (const VkrBakeryJson *report = reports ? reports->first : NULL;
           report; report = report->next) {
        int64_t recooked = 0;
        int64_t skipped = 0;
        bool8_t opens = true_v;
        bool8_t present = true_v;
        (void)vkr_bakery_json_get_int(report, "recooked", &recooked);
        (void)vkr_bakery_json_get_int(report, "skipped", &skipped);
        (void)vkr_bakery_json_get_bool(report, "opens", &opens);
        (void)vkr_bakery_json_get_bool(report, "present", &present);
        job->textures_cooked += (uint32_t)recooked;
        /* Textures of a build whose mesh source snapshot is gone stay
           uncooked; the scene's other textures are ready. */
        job->textures_failed += (uint32_t)skipped;
        char scene_id[40] = {0};
        char reason[DEPOT_TEXT] = {0};
        (void)depot_json_text(report, "scene_id", scene_id, sizeof(scene_id));
        if (present && !opens) {
          (void)depot_json_text(report, "prepare_error", reason,
                                sizeof(reason));
          log_warn("Source control: scene %s does not open after the pull: "
                   "%s",
                   scene_id, reason);
          if (!job->scene_error[0]) {
            string_format(job->scene_error, sizeof(job->scene_error),
                          "Scene %.8s does not open: %s", scene_id, reason);
          }
        }
      }
    } else {
      job->textures_failed++;
      log_warn("Source control: %s", job->error);
      job->error[0] = '\0';
    }
  }
  vkr_atomic_uint32_store(&job->cook_done, 1u, VKR_MEMORY_ORDER_RELAXED);
  request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "recook_textures"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  if (!vkr_depot_progress_canceled(&job->progress) &&
      !depot_project_job(job, folder, arena, request, "Preparing textures",
                         &result)) {
    job->textures_failed++;
    log_warn("Source control: %s", job->error);
    job->error[0] = '\0';
  }
  vkr_atomic_uint32_store(&job->cook_done, 2u, VKR_MEMORY_ORDER_RELAXED);
  arena_destroy(arena);
}

/* Rebuilds each imported model the share reported as cooked before
   texture recipes existed, in the shared workspace, so another computer
   can cook its textures (rebuild_asset). */
static void depot_job_rebuild_old_models(DepotJob *job,
                                         const VkrBakeryJson *share_result) {
  const VkrBakeryJson *list =
      vkr_bakery_json_get(share_result, "needs_rebuild");
  if (!list || !list->count) {
    return;
  }
  Arena *arena = arena_create(MB(1), KB(256));
  if (!arena) {
    return;
  }
  char projects[DEPOT_PATH];
  string_format(projects, sizeof(projects), "%s/.vkreditor/projects/%s",
                job->directory, job->project_id);
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path), "%s/project.json",
                projects);
  vkr_atomic_uint32_store(&job->cook_total, list->count,
                          VKR_MEMORY_ORDER_RELAXED);
  uint32_t done = 0u;
  for (const VkrBakeryJson *entry = list->first;
       entry && !vkr_depot_progress_canceled(&job->progress);
       entry = entry->next) {
    char scene_id[40] = {0};
    char asset_id[64] = {0};
    if (depot_json_text(entry, "scene", scene_id, sizeof(scene_id)) &&
        depot_json_text(entry, "asset_id", asset_id, sizeof(asset_id))) {
      char scene_path[DEPOT_PATH];
      string_format(scene_path, sizeof(scene_path), "%s/scenes/%s/scene.json",
                    projects, scene_id);
      VkrBakeryJson *request = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, request, "operation",
                          vkr_bakery_json_cstr(arena, "rebuild_asset"));
      vkr_bakery_json_set(arena, request, "project_path",
                          vkr_bakery_json_cstr(arena, project_path));
      vkr_bakery_json_set(arena, request, "scene_id",
                          vkr_bakery_json_cstr(arena, scene_id));
      vkr_bakery_json_set(arena, request, "scene_path",
                          vkr_bakery_json_cstr(arena, scene_path));
      vkr_bakery_json_set(arena, request, "asset_id",
                          vkr_bakery_json_cstr(arena, asset_id));
      vkr_bakery_json_set(arena, request, "texture_tier",
                          vkr_bakery_json_cstr(arena, "final"));
      vkr_bakery_json_set(arena, request, "texture_encode_speed",
                          vkr_bakery_json_cstr(arena, "fast"));
      const VkrBakeryJson *result = NULL;
      if (!depot_project_job(job, job->directory, arena, request,
                             "Rebuilding a model for sharing", &result)) {
        log_warn("Source control: %s", job->error);
        job->textures_failed++;
        job->error[0] = '\0';
      }
    }
    vkr_atomic_uint32_store(&job->cook_done, ++done, VKR_MEMORY_ORDER_RELAXED);
  }
  /* The builds the rebuilds replaced would otherwise travel too. */
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "collect_garbage"));
  /* No other job writes the new workspace yet, so nothing needs the grace
     period that protects builds a running job has not recorded. */
  vkr_bakery_json_set(arena, request, "grace_seconds",
                      vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  const VkrBakeryJson *result = NULL;
  if (!vkr_depot_progress_canceled(&job->progress) &&
      !depot_project_job(job, job->directory, arena, request,
                         "Removing replaced builds", &result)) {
    log_warn("Source control: %s", job->error);
    job->error[0] = '\0';
  } else if (result) {
    int64_t freed = 0;
    (void)vkr_bakery_json_get_int(result, "freed_bytes", &freed);
    log_info("Source control: removed %lld bytes of replaced builds",
             (long long)freed);
  }
  arena_destroy(arena);
}

/* Copies the project's live files and every asset they use into the new
   workspace (Bakery's share_project), rebuilds models cooked before texture
   recipes, makes it a repository and commits it. With `serve`, the store
   gets a server key, which marks it as this editor's to serve. */
static void depot_job_init(DepotWorker *worker) {
  DepotJob *job = worker->job;
  Arena *arena = arena_create(MB(1), KB(256));
  if (!arena) {
    string_format(job->error, sizeof(job->error), "out of memory");
    return;
  }
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path),
                "%s/.vkreditor/projects/%s/project.json", job->source,
                job->project_id);
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "share_project"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  vkr_bakery_json_set(arena, request, "destination",
                      vkr_bakery_json_cstr(arena, job->directory));
  vkr_bakery_json_set(arena, request, "legacy_root",
                      vkr_bakery_json_cstr(arena, vkr_content_root()));
  vkr_depot_progress_phase(&job->progress, VKR_DEPOT_PHASE_SNAPSHOT, 0u, 0u);
  const VkrBakeryJson *result = NULL;
  if (!depot_project_job(job, job->source, arena, request,
                         "Copying the project", &result)) {
    arena_destroy(arena);
    return;
  }
  depot_job_rebuild_old_models(job, result);
  arena_destroy(arena);
  if (vkr_depot_progress_canceled(&job->progress)) {
    string_format(job->error, sizeof(job->error), "canceled");
    return;
  }
  VkrDepotWorkspace *ws = worker->workspace;
  if (!vkr_depot_workspace_create(ws, job->directory, NULL,
                                  vkr_depot_zstd_codec())) {
    string_format(job->error, sizeof(job->error),
                  "%s could not become a repository", job->directory);
    return;
  }
  string_format(ws->branch, sizeof(ws->branch), "%s", DEPOT_BRANCH);
  ws->progress = &job->progress;
  VkrDepotCommitResult committed;
  job->ok = vkr_depot_workspace_save(ws) &&
            vkr_depot_commit_local(ws, job->message, &committed, job->error,
                                   sizeof(job->error));
  vkr_depot_workspace_close(ws);
  if (job->ok && job->serve) {
    char store[DEPOT_PATH];
    VkrNetKeyPair keys;
    string_format(store, sizeof(store), "%s/" VKR_DEPOT_META_DIR "/store",
                  job->directory);
    job->ok = vkr_depot_server_key(store, &keys);
    vkr_net_random(keys.secret_key, sizeof(keys.secret_key));
    if (!job->ok) {
      string_format(job->error, sizeof(job->error), "%s could not be served",
                    job->directory);
    }
  }
}

static void *depot_worker(void *argument) {
  DepotWorker worker = {.job = argument};
  DepotJob *job = worker.job;
  job->ok = false_v;
  job->error[0] = '\0';
  worker.workspace = vkr_heap_alloc_zeroed(1u, sizeof(*worker.workspace));
  const bool8_t memory =
      worker.workspace && vkr_dmemory_create(MB(4), MB(256), &worker.memory);
  if (memory) {
    worker.allocator.ctx = &worker.memory;
    vkr_dmemory_allocator_create(&worker.allocator);
  }
  if (!memory) {
    string_format(job->error, sizeof(job->error), "out of memory");
  } else if (job->kind == DEPOT_JOB_CLONE) {
    depot_job_clone(&worker);
  } else if (job->kind == DEPOT_JOB_INIT) {
    depot_job_init(&worker);
  } else if (!vkr_depot_workspace_open(worker.workspace, job->directory,
                                       vkr_depot_zstd_codec())) {
    string_format(job->error, sizeof(job->error), "No repository at %s",
                  job->directory);
  } else {
    VkrDepotWorkspace *ws = worker.workspace;
    ws->progress = &job->progress;
    switch (job->kind) {
    case DEPOT_JOB_STATUS:
      depot_job_status(&worker);
      break;
    case DEPOT_JOB_COMMIT:
      depot_job_commit(&worker);
      break;
    case DEPOT_JOB_PUSH:
      depot_job_push(&worker);
      break;
    case DEPOT_JOB_PULL:
      depot_job_pull(&worker);
      break;
    case DEPOT_JOB_SYNC:
      depot_job_pull(&worker);
      if (job->ok) {
        depot_job_push(&worker);
      }
      break;
    case DEPOT_JOB_FETCH:
      job->ok = vkr_depot_fetch(&worker.allocator, ws, job->remote, NULL,
                                depot_job_fetch_visit, NULL, &job->connect,
                                job->error, sizeof(job->error));
      break;
    case DEPOT_JOB_SWITCH:
      depot_job_switch(&worker);
      break;
    default:
      break;
    }
    /* What the window shows after any step: the base, the branches, how
       the branch stands against the remote, its history and, after a
       change, what is left to commit. */
    job->base = ws->base;
    depot_job_summary(job, ws);
    depot_job_history(job, ws);
    if (job->kind != DEPOT_JOB_STATUS && job->ok) {
      VkrDepotChangeList changes = {0};
      char error[DEPOT_TEXT];
      if (vkr_depot_status(ws, &changes, error, sizeof(error))) {
        depot_job_take_changes(job, &changes, job->changes, &job->change_count);
      }
      vkr_depot_change_list_free(&changes);
    }
    vkr_depot_workspace_close(ws);
  }
  if (memory) {
    vkr_dmemory_allocator_destroy(&worker.allocator);
  }
  vkr_heap_free(worker.workspace);
  vkr_depot_progress_phase(&job->progress, VKR_DEPOT_PHASE_DONE, 0u, 0u);
  vkr_atomic_bool_store(&job->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

static bool8_t depot_busy(const VkrEditorDepot *depot) {
  return depot->job->running;
}

/* Starts the step the job's inputs describe; the caller filled them. */
static bool8_t depot_start(VkrEditorDepot *depot, DepotJobKind kind) {
  DepotJob *job = depot->job;
  if (job->running) {
    return false_v;
  }
  job->kind = kind;
  job->connect = VKR_DEPOT_CONNECT_OK;
  job->change_count = 0u;
  job->conflict_count = 0u;
  job->history_count = 0u;
  job->remote_known = false_v;
  job->scene_files_changed = false_v;
  job->materials_changed = false_v;
  job->scripts_changed = false_v;
  job->merged = false_v;
  job->changed_scene_count = 0u;
  job->scene_error[0] = '\0';
  job->untracked_count = 0u;
  job->untracked_bytes = 0u;
  job->branch_count = 0u;
  job->outgoing_count = 0u;
  job->incoming_count = 0u;
  job->ahead = 0u;
  job->behind = 0u;
  job->textures_cooked = 0u;
  job->textures_failed = 0u;
  job->textures_fetched = 0u;
  job->opened_project[0] = '\0';
  MemZero(&job->remote_head, sizeof(job->remote_head));
  vkr_depot_progress_reset(&job->progress);
  vkr_atomic_uint32_store(&job->cook_done, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&job->cook_total, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_bool_store(&job->done, false_v, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_thread_create(depot->allocator, &job->thread, depot_worker, job)) {
    return false_v;
  }
  job->running = true_v;
  return true_v;
}

/* The remote steps use: the named one, else the selected one; NULL when
   the repository has none. */
static const VkrDepotRemoteEntry *
depot_remote_entry(const VkrEditorDepot *depot, const char *name) {
  for (uint32_t i = 0u; name && name[0] && i < depot->remote_count; ++i) {
    if (string_equals(depot->remotes[i].name, name)) {
      return &depot->remotes[i];
    }
  }
  if (name && name[0]) {
    return NULL;
  }
  return depot->selected_remote < depot->remote_count
             ? &depot->remotes[depot->selected_remote]
             : NULL;
}

/* Fills the inputs every workspace step needs: the remote it exchanges
   with, or, for a served repository with none, this editor's own server
   for the cooked side branch. */
static void depot_job_inputs(VkrEditorDepot *depot, const char *remote) {
  DepotJob *job = depot->job;
  string_format(job->directory, sizeof(job->directory), "%s", depot->workspace);
  const VkrDepotRemoteEntry *entry = depot_remote_entry(depot, remote);
  job->remote[0] = '\0';
  job->address[0] = '\0';
  MemZero(job->server_key, sizeof(job->server_key));
  if (entry) {
    string_format(job->remote, sizeof(job->remote), "%s", entry->name);
    string_format(job->address, sizeof(job->address), "%s", entry->address);
    MemCopy(job->server_key, entry->key, 32u);
  } else if (depot->serving && depot->address[0]) {
    string_format(job->address, sizeof(job->address), "%s", depot->address);
    MemCopy(job->server_key, depot->server_key, 32u);
  }
}

// =============================================================================
// Server
// =============================================================================

static void depot_store_root(const char *workspace, char *out,
                             uint32_t capacity) {
  string_format(out, capacity, "%s/" VKR_DEPOT_META_DIR "/store", workspace);
}

static void depot_service_stop(VkrEditorDepot *depot) {
  if (depot->service) {
    vkr_depot_service_stop(depot->service);
    depot->service = NULL;
  }
  MemZero(&depot->service_info, sizeof(depot->service_info));
  depot->request_count = 0u;
  depot->access_count = 0u;
}

/* Serves the workspace's store; its own identity is the store's admin. */
static bool8_t depot_service_start(VkrEditorDepot *depot) {
  char store[DEPOT_PATH];
  depot_store_root(depot->workspace, store, sizeof(store));
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  vkr_depot_host_name(host, sizeof(host));
  const VkrDepotServiceConfig config = {
      .store_root = store,
      .name = depot->project_name[0] ? depot->project_name : NULL,
      .host = host,
      .codec = vkr_depot_zstd_codec(),
      .owner_admin = true_v,
  };
  depot->service =
      vkr_depot_service_start(depot->allocator, &config, depot->service_error,
                              sizeof(depot->service_error));
  if (!depot->service) {
    log_warn("Source control: the server did not start: %s",
             depot->service_error);
    return false_v;
  }
  depot->service_error[0] = '\0';
  vkr_depot_service_info(depot->service, &depot->service_info);
  MemCopy(depot->server_key, depot->service_info.server_key, 32u);
  /* This editor's own steps reach its server over the loopback of the
     family it bound, never another server that holds the other family's
     port. */
  string_format(depot->address, sizeof(depot->address),
                depot->service_info.address.family == VKR_NET_ADDRESS_IPV6
                    ? "[::1]:%u"
                    : "127.0.0.1:%u",
                (uint32_t)depot->service_info.address.port);
  log_info("Source control: serving %s on UDP %u%s", depot->project_name,
           (uint32_t)depot->service_info.address.port,
           depot->service_info.discoverable ? "" : " (not discoverable)");
  return true_v;
}

// =============================================================================
// Workspace tracking
// =============================================================================

/* The workspace folder of the open project: the `.vkreditor` root's
   parent. */
static void depot_workspace_folder(const VkrEditorUi *editor, char *out,
                                   uint32_t capacity) {
  const char *root = vkr_editor_projects_workspace_root(editor->projects);
  string_format(out, capacity, "%s", root ? root : "");
  const char *found = string_find(out, "/.vkreditor");
  if (found && string_equals(found, "/.vkreditor")) {
    out[found - out] = '\0';
  } else {
    char parent[DEPOT_PATH];
    vkr_bakery_path_parent(parent, sizeof(parent), out);
    string_format(out, capacity, "%s", parent);
  }
}

static void depot_reset_status(VkrEditorDepot *depot) {
  depot->status_known = false_v;
  depot->connected = false_v;
  depot->change_count = 0u;
  depot->conflict_count = 0u;
  depot->history_count = 0u;
  depot->remote_known = false_v;
  depot->ahead = 0u;
  depot->behind = 0u;
  depot->branch_count = 0u;
  depot->outgoing_count = 0u;
  depot->incoming_count = 0u;
  depot->branch[0] = '\0';
  depot->error[0] = '\0';
  depot->waiting_kind = DEPOT_JOB_NONE;
  MemZero(depot->remote_tried, sizeof(depot->remote_tried));
  MemZero(&depot->base, sizeof(depot->base));
  MemZero(&depot->remote_head, sizeof(depot->remote_head));
}

/* Reads the store's remotes and keeps the selected one by name: origin
   when the selection is gone, else the first. */
static void depot_load_remotes(VkrEditorDepot *depot) {
  char store[DEPOT_PATH];
  char selected[VKR_DEPOT_REMOTE_NAME] = {0};
  if (depot->selected_remote < depot->remote_count) {
    string_format(selected, sizeof(selected), "%s",
                  depot->remotes[depot->selected_remote].name);
  }
  depot_store_root(depot->workspace, store, sizeof(store));
  depot->remote_count = 0u;
  if (!vkr_depot_remotes_load(store, depot->remotes, VKR_DEPOT_REMOTES_MAX,
                              &depot->remote_count)) {
    depot->remote_count = 0u;
  }
  depot->selected_remote = 0u;
  for (uint32_t i = 0u; i < depot->remote_count; ++i) {
    if (selected[0] ? string_equals(depot->remotes[i].name, selected)
                    : string_equals(depot->remotes[i].name, VKR_DEPOT_ORIGIN)) {
      depot->selected_remote = i;
    }
  }
}

/* Reads the open workspace's depot state when it changed: a working copy
   is a repository, and one whose store holds a server key is this editor's
   to serve. */
static void depot_follow_workspace(VkrEditorDepot *depot, VkrEditorUi *editor) {
  char folder[DEPOT_PATH] = {0};
  const struct VkrEditorProject *project =
      vkr_editor_projects_project(editor->projects);
  if (project) {
    depot_workspace_folder(editor, folder, sizeof(folder));
  }
  if (string_equals(folder, depot->workspace)) {
    if (project && !string_equals(depot->project_name,
                                  ((const VkrEditorProject *)project)->name)) {
      string_format(depot->project_name, sizeof(depot->project_name), "%s",
                    ((const VkrEditorProject *)project)->name);
    }
    return;
  }
  depot_service_stop(depot);
  depot_reset_status(depot);
  string_format(depot->workspace, sizeof(depot->workspace), "%s", folder);
  depot->repository = false_v;
  depot->serving = false_v;
  depot->remote_count = 0u;
  depot->selected_remote = 0u;
  depot->address[0] = '\0';
  MemZero(depot->server_key, sizeof(depot->server_key));
  depot->project_id[0] = '\0';
  depot->project_name[0] = '\0';
  depot->init_destination[0] = '\0';
  if (!project) {
    return;
  }
  const VkrEditorProject *open = project;
  string_format(depot->project_id, sizeof(depot->project_id), "%s", open->id);
  string_format(depot->project_name, sizeof(depot->project_name), "%s",
                open->name);
  char meta[DEPOT_PATH];
  string_format(meta, sizeof(meta), "%s/" VKR_DEPOT_META_DIR, folder);
  if (!vkr_bakery_is_directory(meta)) {
    return;
  }
  /* Opening adopts a working copy of the older form: its server becomes
     the remote origin (vkr_depot_workspace.h). */
  VkrDepotWorkspace *ws = vkr_heap_alloc_zeroed(1u, sizeof(*ws));
  if (!ws || !vkr_depot_workspace_open(ws, folder, vkr_depot_zstd_codec())) {
    vkr_heap_free(ws);
    return;
  }
  depot->repository = true_v;
  depot->base = ws->base;
  string_format(depot->branch, sizeof(depot->branch), "%s", ws->branch);
  vkr_depot_workspace_close(ws);
  vkr_heap_free(ws);
  depot_load_remotes(depot);
  char key_path[DEPOT_PATH];
  string_format(key_path, sizeof(key_path),
                "%s/" VKR_DEPOT_META_DIR "/store/server.key", folder);
  if (vkr_bakery_is_file(key_path)) {
    depot->serving = true_v;
    (void)depot_service_start(depot);
  }
  depot_remember_project(depot, folder, depot->project_name, depot->project_id,
                         depot->serving);
  depot->next_status = 0.0;
}

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorDepot *vkr_editor_depot_create(VkrAllocator *allocator) {
  VkrEditorDepot *depot = vkr_allocator_alloc(allocator, sizeof(*depot),
                                              VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!depot) {
    return NULL;
  }
  MemZero(depot, sizeof(*depot));
  depot->allocator = allocator;
  depot->job = vkr_heap_alloc_zeroed(1u, sizeof(*depot->job));
  if (!depot->job) {
    vkr_allocator_free(allocator, depot, sizeof(*depot),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return NULL;
  }
  return depot;
}

static void depot_job_join(VkrEditorDepot *depot) {
  DepotJob *job = depot->job;
  if (!job->running) {
    return;
  }
  (void)vkr_thread_join(job->thread);
  (void)vkr_thread_destroy(depot->allocator, &job->thread);
  job->running = false_v;
}

void vkr_editor_depot_destroy(VkrEditorDepot *depot) {
  if (!depot) {
    return;
  }
  if (depot->job->running) {
    vkr_depot_progress_cancel(&depot->job->progress);
    depot_job_join(depot);
  }
  depot_service_stop(depot);
  if (depot->browser_open) {
    vkr_depot_browser_close(&depot->browser);
  }
  if (depot->stream) {
    /* Reads stop asking before the remote goes. */
    (void)vkr_vfs_set_remote(NULL, NULL, NULL);
    vkr_depot_remote_close(depot->stream);
  }
  vkr_heap_free(depot->job);
  vkr_allocator_free(depot->allocator, depot, sizeof(*depot),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

// =============================================================================
// Finishing steps
// =============================================================================

/* The selected remote's index, or UINT32_MAX for none. */
static uint32_t depot_remote_index(const VkrEditorDepot *depot,
                                   const char *name) {
  for (uint32_t i = 0u; name[0] && i < depot->remote_count; ++i) {
    if (string_equals(depot->remotes[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

static void depot_take_results(VkrEditorDepot *depot) {
  DepotJob *job = depot->job;
  if (job->kind == DEPOT_JOB_STATUS || job->ok) {
    depot->change_count = job->change_count;
    MemCopy(depot->changes, job->changes,
            Min(job->change_count, DEPOT_LIST_MAX) * sizeof(DepotText));
  }
  /* Conflicts stay listed until a step that pulls or switches settles or
     replaces them; a status check knows nothing about them. */
  const bool8_t settles = job->kind == DEPOT_JOB_PULL ||
                          job->kind == DEPOT_JOB_SYNC ||
                          job->kind == DEPOT_JOB_SWITCH ||
                          (job->kind == DEPOT_JOB_COMMIT && job->then_sync);
  if (settles) {
    depot->conflict_count = job->conflict_count;
    MemCopy(depot->conflicts, job->conflicts,
            Min(job->conflict_count, DEPOT_LIST_MAX) * sizeof(DepotText));
  }
  if (job->kind == DEPOT_JOB_COMMIT) {
    depot->untracked_count = job->untracked_count;
    depot->untracked_bytes = job->untracked_bytes;
    MemCopy(depot->untracked, job->untracked,
            Min(job->untracked_count, DEPOT_LIST_MAX) * sizeof(DepotText));
  }
  if (!vkr_depot_id_zero(&job->base)) {
    depot->base = job->base;
  }
  if (job->history_count) {
    depot->history_count = job->history_count;
    MemCopy(depot->history, job->history,
            job->history_count * sizeof(DepotHistory));
  }
  if (job->current_branch[0]) {
    string_format(depot->branch, sizeof(depot->branch), "%s",
                  job->current_branch);
    depot->branch_count = job->branch_count;
    MemCopy(depot->branches, job->branches,
            job->branch_count * sizeof(DepotBranch));
    depot->remote_known = job->remote_known;
    depot->remote_head = job->remote_head;
    depot->ahead = job->ahead;
    depot->behind = job->behind;
    depot->outgoing_count = job->outgoing_count;
    MemCopy(depot->outgoing, job->outgoing,
            job->outgoing_count * sizeof(DepotHistory));
    depot->incoming_count = job->incoming_count;
    MemCopy(depot->incoming, job->incoming,
            job->incoming_count * sizeof(DepotHistory));
  }
  const uint32_t remote = depot_remote_index(depot, job->remote);
  if (remote != UINT32_MAX && job->kind != DEPOT_JOB_COMMIT) {
    depot->remote_connect[remote] = job->connect;
    depot->remote_tried[remote] = true_v;
  }
  depot->connect = job->connect;
  depot->connected = job->connect == VKR_DEPOT_CONNECT_OK && job->remote[0];
  depot->status_known = depot->status_known || job->ok;
}

static void depot_finish(VkrEditorDepot *depot, VkrEditorUi *editor,
                         const VkrSampleUiFrame *frame) {
  DepotJob *job = depot->job;
  if (!job->running ||
      !vkr_atomic_bool_load(&job->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    return;
  }
  depot_job_join(depot);
  const float64_t now = vkr_platform_get_absolute_time();
  depot_take_results(depot);
  const bool8_t denied = job->connect == VKR_DEPOT_CONNECT_DENIED;
  /* A status check keeps the last step's error on screen. */
  if (job->kind != DEPOT_JOB_STATUS) {
    depot->error[0] = '\0';
  }
  depot->waiting_kind = DEPOT_JOB_NONE;
  const bool8_t exchange =
      job->kind == DEPOT_JOB_CLONE || job->kind == DEPOT_JOB_PULL ||
      job->kind == DEPOT_JOB_PUSH || job->kind == DEPOT_JOB_SYNC;
  if (denied && exchange) {
    /* The remote recorded a request; ask again until its owner answers. */
    depot->waiting_kind = job->kind;
    depot->retry_at = now + DEPOT_RETRY_SECONDS;
  } else if (!job->ok && job->kind != DEPOT_JOB_STATUS &&
             !vkr_depot_progress_canceled(&job->progress)) {
    string_format(depot->error, sizeof(depot->error), "%s", job->error);
  }
  if (job->ok && job->scene_error[0]) {
    string_format(depot->error, sizeof(depot->error), "%s", job->scene_error);
  }
  if (job->kind == DEPOT_JOB_PULL || job->kind == DEPOT_JOB_SYNC ||
      job->kind == DEPOT_JOB_CLONE || job->kind == DEPOT_JOB_SWITCH ||
      (job->kind == DEPOT_JOB_COMMIT && job->then_sync)) {
    depot->textures_cooked = job->textures_cooked;
    depot->textures_fetched = job->textures_fetched;
    depot->textures_failed = job->textures_failed;
  }
  const bool8_t wrote_files = job->scene_files_changed ||
                              job->materials_changed || job->scripts_changed;
  if (job->ok && wrote_files && job->kind != DEPOT_JOB_CLONE) {
    depot->reload_pending = job->scene_files_changed;
    if (job->scripts_changed) {
      /* Someone else's native code: it runs once this user trusts it. */
      depot_set_trusted(depot, depot->workspace, false_v);
      vkr_editor_scripts_hold(editor->scripts, true_v);
      depot->scripts_prompt_dismissed = false_v;
    }
    if (job->materials_changed) {
      vkr_editor_material_index_reset(editor->materials);
    }
  }
  switch (job->kind) {
  case DEPOT_JOB_STATUS:
    depot->next_status = now + DEPOT_STATUS_SECONDS;
    break;
  case DEPOT_JOB_COMMIT:
    if (job->ok) {
      depot->message[0] = '\0';
      vkr_editor_toast(editor, VKR_UI_ICON_CHECK_CIRCLE,
                       vkr_ui_theme()->success,
                       job->then_sync ? "Committed and synced" : "Committed");
    }
    depot->next_status = now + 1.0;
    break;
  case DEPOT_JOB_PUSH:
    if (job->ok) {
      vkr_editor_toast(editor, VKR_UI_ICON_ARROW_UP, vkr_ui_theme()->success,
                       "Pushed");
    }
    depot->next_status = now + 1.0;
    break;
  case DEPOT_JOB_PULL:
  case DEPOT_JOB_SYNC:
    if (job->ok) {
      vkr_editor_toast(editor, VKR_UI_ICON_ARROW_DOWN, vkr_ui_theme()->success,
                       job->textures_failed
                           ? "Pulled; some textures did not cook"
                       : job->kind == DEPOT_JOB_SYNC ? "Synced"
                       : job->merged ? "Pulled the latest changes"
                                     : "Already up to date");
    }
    depot->next_status = now + 1.0;
    break;
  case DEPOT_JOB_FETCH:
    depot->next_status = now + DEPOT_STATUS_SECONDS;
    break;
  case DEPOT_JOB_SWITCH:
    if (job->ok) {
      depot->new_branch[0] = '\0';
      vkr_editor_toast(editor, VKR_UI_ICON_GIT_BRANCH, vkr_ui_theme()->success,
                       "Switched branch");
    }
    depot->next_status = now + 1.0;
    break;
  case DEPOT_JOB_CLONE:
    if (job->ok) {
      depot_remember_project(depot, job->directory, depot->dialog_name,
                             job->opened_project, false_v);
      depot_remember_server(depot, depot->dialog_name, depot->dialog_host,
                            job->address, job->server_key);
      string_format(depot->open_directory, sizeof(depot->open_directory), "%s",
                    job->directory);
      string_format(depot->open_project, sizeof(depot->open_project), "%s",
                    job->opened_project);
      depot->dialog_open = false_v;
    }
    break;
  case DEPOT_JOB_INIT:
    if (job->ok) {
      depot->init_switch = true_v;
      depot_remember_project(depot, job->directory, depot->project_name,
                             job->project_id, job->serve);
    }
    break;
  default:
    break;
  }
  (void)frame;
}

/* Saved scene edits before a reload: the reload reads the files the pull
   wrote, and unsaved edits stay until their owner saves or discards. */
static bool8_t depot_journals_saved(const VkrSampleUiFrame *frame) {
  const VkrSceneEditState *journals[2u + VKR_SCENE_ADDITIVE_MAX] = {
      frame->scene ? frame->edits : NULL,
      frame->world ? frame->world_edits : NULL};
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    journals[2u + i] = frame->additive[i] ? frame->additive_edits[i] : NULL;
  }
  for (uint32_t i = 0u; i < ArrayCount(journals); ++i) {
    if (journals[i] && journals[i]->revision != journals[i]->saved_revision) {
      return false_v;
    }
  }
  return true_v;
}

void vkr_editor_depot_update(VkrEditorDepot *depot, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  if (!depot) {
    return;
  }
  if (!depot->registry_loaded) {
    depot_registry_load(depot);
  }
  depot_finish(depot, editor, frame);
  /* A finished clone or share opens its workspace from the hub's thread
     of control, here, once nothing else runs. A switch that cannot happen
     yet, behind unsaved edits or a failed open, tries again every few
     seconds rather than every frame: a sparse working copy connects to its
     server for each try. */
  const float64_t opening = vkr_platform_get_absolute_time();
  if (depot->open_directory[0] && opening >= depot->open_retry_at) {
    if (vkr_editor_projects_open_workspace(editor->projects, editor, frame,
                                           depot->open_directory,
                                           depot->open_project)) {
      depot->open_directory[0] = '\0';
      depot->open_project[0] = '\0';
    } else {
      depot->open_retry_at = opening + DEPOT_OPEN_RETRY_SECONDS;
    }
  }
  if (depot->init_switch && !depot_busy(depot)) {
    depot->init_switch = false_v;
    string_format(depot->open_directory, sizeof(depot->open_directory), "%s",
                  depot->job->directory);
    string_format(depot->open_project, sizeof(depot->open_project), "%s",
                  depot->job->project_id);
  }
  depot_follow_workspace(depot, editor);

  const float64_t now = vkr_platform_get_absolute_time();
  if (depot->service) {
    if (now - depot->service_polled > 0.5) {
      depot->service_polled = now;
      vkr_depot_service_info(depot->service, &depot->service_info);
      depot->request_count =
          Min(vkr_depot_service_requests(depot->service, depot->requests,
                                         DEPOT_REQUESTS_MAX),
              DEPOT_REQUESTS_MAX);
      depot->access_count =
          Min(vkr_depot_service_access(depot->service, depot->access,
                                       DEPOT_ACCESS_MAX),
              DEPOT_ACCESS_MAX);
    }
    if (depot->service_info.failed) {
      string_format(depot->service_error, sizeof(depot->service_error),
                    "The server's socket failed; sharing stopped");
      depot_service_stop(depot);
    }
  }

  if (depot->waiting_kind != DEPOT_JOB_NONE && now >= depot->retry_at &&
      !depot_busy(depot)) {
    const DepotJobKind kind = depot->waiting_kind;
    depot->waiting_kind = DEPOT_JOB_NONE;
    if (kind != DEPOT_JOB_CLONE) {
      char remote[VKR_DEPOT_REMOTE_NAME];
      string_format(remote, sizeof(remote), "%s", depot->job->remote);
      depot_job_inputs(depot, remote);
    }
    (void)depot_start(depot, kind);
  }

  if (depot->repository && !depot_busy(depot) &&
      depot->waiting_kind == DEPOT_JOB_NONE && now >= depot->next_status) {
    depot_load_remotes(depot);
    depot_job_inputs(depot, NULL);
    depot->next_status = now + DEPOT_STATUS_SECONDS;
    (void)depot_start(depot, DEPOT_JOB_STATUS);
  }

  if (depot->reload_pending && frame->scene_edit &&
      frame->scene_edit->action == VKR_SCENE_EDIT_NONE &&
      depot_journals_saved(frame) && !frame->scene_loading) {
    depot->reload_pending = false_v;
    if (frame->scene) {
      frame->scene_edit->action = VKR_SCENE_EDIT_RELOAD;
    }
  }

  /* Discovery runs while the hub, the Get dialog or the window can show
     what it finds. */
  if (depot->browse_wanted && !depot->browser_open) {
    depot->browser_open = vkr_depot_browser_open(&depot->browser, 0u);
    depot->browser_probed = 0.0;
    if (depot->browser_open) {
      for (uint32_t i = 0u; i < depot->known_count; ++i) {
        VkrNetAddress address;
        if (vkr_depot_address(depot->known[i].address, &address)) {
          (void)vkr_depot_browser_add_target(&depot->browser, &address);
        }
      }
    }
  } else if (!depot->browse_wanted && depot->browser_open) {
    vkr_depot_browser_close(&depot->browser);
    depot->browser_open = false_v;
  }
  if (depot->browser_open) {
    if (now - depot->browser_probed > DEPOT_PROBE_SECONDS) {
      depot->browser_probed = now;
      vkr_depot_browser_probe(&depot->browser, true_v);
    }
    (void)vkr_depot_browser_update(&depot->browser);
    /* A remote that stopped answering follows its key to the address
       discovery hears it at, and keeps it in the store. */
    for (uint32_t r = 0u; r < depot->remote_count && !depot_busy(depot); ++r) {
      if (depot->remote_connect[r] != VKR_DEPOT_CONNECT_UNREACHABLE) {
        continue;
      }
      for (uint32_t i = 0u; i < depot->browser.found_count; ++i) {
        const VkrDepotDiscovered *found = &depot->browser.found[i];
        char text[VKR_NET_ADDRESS_TEXT];
        vkr_net_address_format(&found->address, text, sizeof(text));
        if (MemCompare(found->server_key, depot->remotes[r].key, 32u) == 0 &&
            !string_equals(text, depot->remotes[r].address)) {
          VkrDepotRemoteEntry moved = depot->remotes[r];
          string_format(moved.address, sizeof(moved.address), "%s", text);
          char store[DEPOT_PATH];
          depot_store_root(depot->workspace, store, sizeof(store));
          if (vkr_depot_remote_set(store, &moved)) {
            depot->remotes[r] = moved;
            depot->remote_connect[r] = VKR_DEPOT_CONNECT_OK;
            depot->next_status = 0.0;
          }
        }
      }
    }
  }
  depot->browse_wanted = false_v;
}

// =============================================================================
// Actions
// =============================================================================

/* A user's step goes before a status check: the check, which may wait
   seconds for a remote that does not answer, stops and its results are
   dropped. */
static void depot_preempt_status(VkrEditorDepot *depot) {
  DepotJob *job = depot->job;
  if (!job->running || job->kind != DEPOT_JOB_STATUS) {
    return;
  }
  vkr_depot_progress_cancel(&job->progress);
  depot_job_join(depot);
  depot->next_status = vkr_platform_get_absolute_time() + DEPOT_STATUS_SECONDS;
}

/* Whether a step on the open repository may start; false with the reason. */
static bool8_t depot_can_start(VkrEditorDepot *depot, char *error,
                               uint32_t capacity) {
  if (!depot || !depot->repository) {
    string_format(error, capacity, "This project is not a repository");
    return false_v;
  }
  depot_preempt_status(depot);
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  return true_v;
}

/* Starts `kind` with the remote `remote` (NULL: the selected one), which a
   step that exchanges with one needs. */
static bool8_t depot_start_with(VkrEditorDepot *depot, DepotJobKind kind,
                                const char *remote, bool8_t needs_remote,
                                char *error, uint32_t capacity) {
  depot_job_inputs(depot, remote);
  if (needs_remote && !depot->job->remote[0]) {
    string_format(error, capacity,
                  remote && remote[0]
                      ? "No remote %s"
                      : "Add a remote first: another computer's repository",
                  remote ? remote : "");
    return false_v;
  }
  if (!depot_start(depot, kind)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

static VkrDepotConflictPolicy depot_policy(const char *policy) {
  return policy && string_equals(policy, "theirs")
             ? VKR_DEPOT_CONFLICT_TAKE_THEIRS
         : policy && string_equals(policy, "mine")
             ? VKR_DEPOT_CONFLICT_KEEP_MINE
             : VKR_DEPOT_CONFLICT_FAIL;
}

bool8_t vkr_editor_depot_init(VkrEditorDepot *depot, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              const char *destination, bool8_t serve,
                              char *error, uint32_t capacity) {
  if (!depot || depot->repository || !depot->project_id[0]) {
    string_format(error, capacity,
                  "Open a project that is not a repository yet");
    return false_v;
  }
  depot_preempt_status(depot);
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  if (!depot_journals_saved(frame)) {
    string_format(error, capacity,
                  "Save the scene first; the repository takes the saved "
                  "files");
    return false_v;
  }
  DepotJob *job = depot->job;
  if (destination && destination[0]) {
    string_format(job->directory, sizeof(job->directory), "%s", destination);
  } else {
    depot_default_destination(depot->project_name, job->directory,
                              sizeof(job->directory));
  }
  if (vkr_bakery_is_directory(job->directory)) {
    char probe[DEPOT_PATH];
    string_format(probe, sizeof(probe), "%s/.vkreditor", job->directory);
    if (vkr_bakery_is_directory(probe)) {
      string_format(error, capacity, "%s already holds a workspace",
                    job->directory);
      return false_v;
    }
  }
  depot_workspace_folder(editor, job->source, sizeof(job->source));
  string_format(job->project_id, sizeof(job->project_id), "%s",
                depot->project_id);
  string_format(job->message, sizeof(job->message), "Created %s",
                depot->project_name);
  job->serve = serve;
  job->remote[0] = '\0';
  job->address[0] = '\0';
  if (!depot_start(depot, DEPOT_JOB_INIT)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_commit(VkrEditorDepot *depot, const char *message,
                                bool8_t sync, VkrEditorDepotUntracked untracked,
                                char *error, uint32_t capacity) {
  if (!depot_can_start(depot, error, capacity)) {
    return false_v;
  }
  depot->job->adopt = untracked == VKR_EDITOR_DEPOT_UNTRACKED_ADOPT;
  depot->job->ignore_untracked = untracked == VKR_EDITOR_DEPOT_UNTRACKED_IGNORE;
  const char *text = message && message[0] ? message : depot->message;
  string_format(depot->job->message, sizeof(depot->job->message), "%s",
                text[0] ? text : "Changes");
  depot->job->then_sync = sync;
  depot->job->policy = VKR_DEPOT_CONFLICT_FAIL;
  return depot_start_with(depot, DEPOT_JOB_COMMIT, NULL, sync, error, capacity);
}

bool8_t vkr_editor_depot_push(VkrEditorDepot *depot, const char *remote,
                              char *error, uint32_t capacity) {
  return depot_can_start(depot, error, capacity) &&
         depot_start_with(depot, DEPOT_JOB_PUSH, remote, true_v, error,
                          capacity);
}

bool8_t vkr_editor_depot_pull(VkrEditorDepot *depot, const char *remote,
                              const char *policy, char *error,
                              uint32_t capacity) {
  if (!depot_can_start(depot, error, capacity)) {
    return false_v;
  }
  depot->job->policy = depot_policy(policy);
  return depot_start_with(depot, DEPOT_JOB_PULL, remote, true_v, error,
                          capacity);
}

bool8_t vkr_editor_depot_sync(VkrEditorDepot *depot, const char *remote,
                              char *error, uint32_t capacity) {
  if (!depot_can_start(depot, error, capacity)) {
    return false_v;
  }
  depot->job->policy = VKR_DEPOT_CONFLICT_FAIL;
  return depot_start_with(depot, DEPOT_JOB_SYNC, remote, true_v, error,
                          capacity);
}

bool8_t vkr_editor_depot_fetch(VkrEditorDepot *depot, char *error,
                               uint32_t capacity) {
  return depot_can_start(depot, error, capacity) &&
         depot_start_with(depot, DEPOT_JOB_FETCH, NULL, true_v, error,
                          capacity);
}

bool8_t vkr_editor_depot_switch(VkrEditorDepot *depot, const char *branch,
                                bool8_t create, char *error,
                                uint32_t capacity) {
  if (!depot_can_start(depot, error, capacity)) {
    return false_v;
  }
  if (!branch || !vkr_depot_branch_valid(branch)) {
    string_format(error, capacity,
                  "A branch name uses letters, digits, '.', '-' and '_'");
    return false_v;
  }
  string_format(depot->job->branch, sizeof(depot->job->branch), "%s", branch);
  depot->job->create_branch = create;
  return depot_start_with(depot, DEPOT_JOB_SWITCH, NULL, false_v, error,
                          capacity);
}

bool8_t vkr_editor_depot_remote(VkrEditorDepot *depot, const char *name,
                                const char *address, const char *key_hex,
                                char *error, uint32_t capacity) {
  if (!depot || !depot->repository) {
    string_format(error, capacity, "This project is not a repository");
    return false_v;
  }
  char store[DEPOT_PATH];
  depot_store_root(depot->workspace, store, sizeof(store));
  if (!address) {
    if (!vkr_depot_remote_remove(store, name)) {
      string_format(error, capacity, "No remote %s", name ? name : "");
      return false_v;
    }
    depot_load_remotes(depot);
    return true_v;
  }
  VkrDepotRemoteEntry remote = {0};
  VkrNetAddress parsed;
  if (!name || !vkr_depot_remote_name_valid(name)) {
    string_format(error, capacity,
                  "A remote's name uses a-z, 0-9, '-' and '_', at most 32");
    return false_v;
  }
  if (!vkr_depot_address(address, &parsed)) {
    string_format(error, capacity, "'%s' is not an address or a computer name",
                  address);
    return false_v;
  }
  if (!depot_key_parse(key_hex, remote.key)) {
    string_format(error, capacity, "A key is 64 hexadecimal digits");
    return false_v;
  }
  string_format(remote.name, sizeof(remote.name), "%s", name);
  string_format(remote.address, sizeof(remote.address), "%s", address);
  if (!vkr_depot_remote_set(store, &remote)) {
    string_format(error, capacity, "The remote could not be saved");
    return false_v;
  }
  depot_load_remotes(depot);
  for (uint32_t i = 0u; i < depot->remote_count; ++i) {
    if (string_equals(depot->remotes[i].name, name)) {
      depot->selected_remote = i;
      depot->remote_tried[i] = false_v;
    }
  }
  depot->next_status = 0.0;
  return true_v;
}

bool8_t vkr_editor_depot_serve(VkrEditorDepot *depot, bool8_t serve,
                               char *error, uint32_t capacity) {
  if (!depot || !depot->repository) {
    string_format(error, capacity, "This project is not a repository");
    return false_v;
  }
  char store[DEPOT_PATH];
  char key_path[DEPOT_PATH];
  depot_store_root(depot->workspace, store, sizeof(store));
  string_format(key_path, sizeof(key_path), "%s/server.key", store);
  if (!serve) {
    depot_service_stop(depot);
    depot->serving = false_v;
    depot->address[0] = '\0';
    /* The key stays out of the store, so the next open serves nothing;
       computers that cloned keep their copies and can push again once
       it serves with a new key. */
    if (vkr_bakery_is_file(key_path) && !vkr_bakery_remove_file(key_path)) {
      string_format(error, capacity, "The server key could not be removed");
      return false_v;
    }
    return true_v;
  }
  VkrNetKeyPair keys;
  if (!vkr_depot_server_key(store, &keys)) {
    string_format(error, capacity, "The server key could not be made");
    return false_v;
  }
  vkr_net_random(keys.secret_key, sizeof(keys.secret_key));
  depot->serving = true_v;
  if (!depot->service && !depot_service_start(depot)) {
    string_format(error, capacity, "%s", depot->service_error);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_editor_depot_clone(VkrEditorDepot *depot, const char *address,
                               const char *key_hex, const char *name,
                               const char *destination, bool8_t sparse,
                               char *error, uint32_t capacity) {
  if (!depot) {
    return false_v;
  }
  depot_preempt_status(depot);
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  DepotJob *job = depot->job;
  if (!depot_key_parse(key_hex, job->server_key)) {
    string_format(error, capacity, "The server key is 64 hexadecimal digits");
    return false_v;
  }
  string_format(job->address, sizeof(job->address), "%s", address);
  job->sparse = sparse;
  string_format(depot->dialog_name, sizeof(depot->dialog_name), "%s",
                name && name[0] ? name : "Project");
  /* The computer's name, when discovery heard this server. */
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    if (MemCompare(depot->browser.found[i].server_key, job->server_key, 32u) ==
        0) {
      string_format(depot->dialog_host, sizeof(depot->dialog_host), "%s",
                    depot->browser.found[i].host);
    }
  }
  if (destination && destination[0]) {
    string_format(job->directory, sizeof(job->directory), "%s", destination);
  } else {
    depot_default_destination(depot->dialog_name, job->directory,
                              sizeof(job->directory));
  }
  char probe[DEPOT_PATH];
  string_format(probe, sizeof(probe), "%s/.vkreditor", job->directory);
  if (vkr_bakery_is_directory(probe)) {
    string_format(probe, sizeof(probe), "%s/" VKR_DEPOT_META_DIR,
                  job->directory);
    if (!vkr_bakery_is_directory(probe)) {
      string_format(error, capacity, "%s already holds another workspace",
                    job->directory);
      return false_v;
    }
  }
  if (!depot_start(depot, DEPOT_JOB_CLONE)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  /* The hub shows the step on the server's row, however it started. */
  depot->dialog_open = true_v;
  MemCopy(depot->dialog_key, job->server_key, 32u);
  string_format(depot->dialog_address, sizeof(depot->dialog_address), "%s",
                address);
  string_format(depot->dialog_destination, sizeof(depot->dialog_destination),
                "%s", job->directory);
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_allow(VkrEditorDepot *depot, const char *key_hex,
                               const char *role, char *error,
                               uint32_t capacity) {
  uint8_t key[32];
  if (!depot || !depot->service) {
    string_format(error, capacity, "This editor is not sharing a project");
    return false_v;
  }
  if (!depot_key_parse(key_hex, key)) {
    string_format(error, capacity, "A key is 64 hexadecimal digits");
    return false_v;
  }
  const VkrDepotRole value =
      !role || string_equals(role, "write") ? VKR_DEPOT_ROLE_WRITE
      : string_equals(role, "read")         ? VKR_DEPOT_ROLE_READ
                                            : VKR_DEPOT_ROLE_NONE;
  for (uint32_t i = 0u;
       value != VKR_DEPOT_ROLE_NONE && i < depot->request_count; ++i) {
    if (MemCompare(depot->requests[i].key, key, 32u) == 0) {
      depot_remember_person(depot, key, depot->requests[i].name);
    }
  }
  if (!vkr_depot_service_set_role(depot->service, key, value)) {
    string_format(error, capacity, "The access list could not be written");
    return false_v;
  }
  depot->service_polled = 0.0;
  return true_v;
}

bool8_t vkr_editor_depot_find(VkrEditorDepot *depot, const char *address,
                              char *error, uint32_t capacity) {
  VkrNetAddress parsed;
  if (!depot || !vkr_depot_address(address, &parsed)) {
    string_format(error, capacity, "'%s' is not an address or a computer name",
                  address ? address : "");
    return false_v;
  }
  depot->browse_wanted = true_v;
  if (!depot->browser_open) {
    depot->browser_open = vkr_depot_browser_open(&depot->browser, 0u);
  }
  if (!depot->browser_open ||
      !vkr_depot_browser_add_target(&depot->browser, &parsed)) {
    string_format(error, capacity, "No more addresses can be probed");
    return false_v;
  }
  vkr_depot_browser_probe(&depot->browser, false_v);
  return true_v;
}

void vkr_editor_depot_cancel(VkrEditorDepot *depot) {
  if (depot && depot->job->running) {
    vkr_depot_progress_cancel(&depot->job->progress);
  }
  if (depot) {
    depot->waiting_kind = DEPOT_JOB_NONE;
  }
}

bool8_t vkr_editor_depot_scripts_trusted(VkrEditorDepot *depot,
                                         const char *folder) {
  if (!depot || !folder || !folder[0]) {
    return true_v;
  }
  if (!depot->registry_loaded) {
    depot_registry_load(depot);
  }
  char path[DEPOT_PATH];
  string_format(path, sizeof(path), "%s/" VKR_DEPOT_META_DIR, folder);
  if (!vkr_bakery_is_directory(path)) {
    return true_v;
  }
  const DepotProject *project = depot_project_entry(depot, folder);
  if (project) {
    return project->trusted;
  }
  /* A working copy no step of this editor made: trusted only when this
     editor serves it. */
  string_format(path, sizeof(path),
                "%s/" VKR_DEPOT_META_DIR "/store/server.key", folder);
  return vkr_bakery_is_file(path);
}

void vkr_editor_depot_trust_scripts(VkrEditorDepot *depot,
                                    VkrEditorUi *editor) {
  if (!depot) {
    return;
  }
  depot_set_trusted(depot, depot->workspace, true_v);
  vkr_editor_scripts_hold(editor->scripts, false_v);
}

bool8_t vkr_editor_depot_dialog_open(const VkrEditorDepot *depot) {
  return depot && depot->dialog_open;
}

bool8_t vkr_editor_depot_busy(const VkrEditorDepot *depot) {
  return depot && depot->job && depot_busy(depot);
}

// =============================================================================
// Widgets
// =============================================================================

/* A widget at (x, y) points inside its panel, as the Session window lays
   out its rows. */
static VkrUiWidgetConfig depot_widget(float32_t x, float32_t y, float32_t width,
                                      float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_START,
                                 .margin_pt = {y, 0, 0, x}};
  c.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  c.style.max_size_pt = c.style.min_size_pt;
  c.style.font_size_pt = vkr_ui_theme()->font_body;
  c.style.text_color = vkr_ui_theme()->text;
  c.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
  return c;
}

static void depot_label(VkrUiSystem *ui, const char *id, const char *text,
                        float32_t x, float32_t y, float32_t width, Vec4 color) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 22);
  c.style.text_color = color;
  vkr_ui_label(ui, depot_string(id), depot_string(text), &c);
}

static void depot_caption(VkrUiSystem *ui, const char *id, const char *text,
                          float32_t x, float32_t y, float32_t width) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 18);
  c.style.font_size_pt = vkr_ui_theme()->font_caption;
  c.style.text_color = vkr_ui_theme()->text_secondary;
  vkr_ui_label(ui, depot_string(id), depot_string(text), &c);
}

/* Wrapped caption text; returns the height it takes. */
static float32_t depot_paragraph(VkrUiSystem *ui, const char *id,
                                 const char *text, float32_t x, float32_t y,
                                 float32_t width) {
  const float32_t chars_per_line = Max(16.0f, width / 6.4f);
  const float32_t lines =
      (float32_t)((uint32_t)((float32_t)string_length(text) / chars_per_line) +
                  1u);
  const float32_t height = lines * 17.0f + 4.0f;
  VkrUiWidgetConfig c = depot_widget(x, y, width, height);
  c.style.font_size_pt = vkr_ui_theme()->font_caption;
  c.style.text_color = vkr_ui_theme()->text_secondary;
  c.text.layout.word_wrap = true_v;
  c.text.layout.max_width = width - 8;
  vkr_ui_label(ui, depot_string(id), depot_string(text), &c);
  return height;
}

static void depot_heading(VkrEditorUi *editor, VkrUiSystem *ui, const char *id,
                          const char *text, VkrUiIcon icon, float32_t x,
                          float32_t y, float32_t width) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 24);
  c.text.font = editor->heading_font;
  c.style.font_size_pt = vkr_ui_theme()->font_emphasis;
  c.icon = icon;
  c.icon_size_pt = 15.0f;
  c.icon_color = vkr_ui_theme()->accent_hover;
  vkr_ui_label(ui, depot_string(id), depot_string(text), &c);
}

static bool8_t depot_button(VkrEditorUi *editor, VkrUiSystem *ui,
                            const char *id, const char *text, float32_t x,
                            float32_t y, float32_t width, bool8_t primary,
                            bool8_t disabled) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 26);
  if (primary) {
    vkr_editor_primary_style(&c, editor->heading_font);
  } else {
    vkr_editor_action_style(&c, editor->heading_font);
  }
  c.disabled = disabled;
  return vkr_ui_button(ui, depot_string(id), depot_string(text), &c);
}

static void depot_field(VkrUiSystem *ui, const char *id, char *text,
                        uint32_t capacity, float32_t x, float32_t y,
                        float32_t width) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 26);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer buffer = {(uint8_t *)text, (uint32_t)string_length(text),
                                capacity};
  (void)vkr_ui_text_field(ui, depot_string(id), &buffer, &c);
}

/* A bar of `fraction` (negative: unknown) under a caption. */
static void depot_progress_bar(VkrUiSystem *ui, const char *id, float32_t x,
                               float32_t y, float32_t width,
                               float32_t fraction) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig track = depot_widget(x, y, width, 6);
  track.style.padding_pt = (VkrUiEdges){0};
  track.style.background_color = theme->field;
  track.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
  char track_id[64];
  string_format(track_id, sizeof(track_id), "%s.track", id);
  vkr_ui_label(ui, depot_string(track_id), (String8){0}, &track);
  if (fraction > 0.0f) {
    VkrUiWidgetConfig fill =
        depot_widget(x, y, Max(6.0f, width * Min(fraction, 1.0f)), 6);
    fill.style.padding_pt = (VkrUiEdges){0};
    fill.style.background_color = theme->accent;
    fill.style.corner_radius_pt = (Vec4){3, 3, 3, 3};
    vkr_ui_label(ui, depot_string(id), (String8){0}, &fill);
  }
}

/* What the running step does now, and how far it is, as text and a
   fraction (negative when unknown). */
static float32_t depot_job_text(const VkrEditorDepot *depot, char *out,
                                uint32_t capacity) {
  const DepotJob *job = depot->job;
  const VkrDepotProgressView view = vkr_depot_progress_read(&job->progress);
  const uint32_t cook_total =
      vkr_atomic_uint32_load(&job->cook_total, VKR_MEMORY_ORDER_RELAXED);
  const uint32_t cook_done =
      vkr_atomic_uint32_load(&job->cook_done, VKR_MEMORY_ORDER_RELAXED);
  const char *step = depot_job_names[job->kind];
  if (view.canceled) {
    string_format(out, capacity, "%s: stopping", step);
    return -1.0f;
  }
  if (cook_total && view.phase == VKR_DEPOT_PHASE_CHECKOUT) {
    /* The depot's phases are over; the texture cook follows them. */
  }
  if (cook_total && cook_done < cook_total &&
      (view.phase == VKR_DEPOT_PHASE_DONE ||
       view.phase == VKR_DEPOT_PHASE_CHECKOUT || job->kind == DEPOT_JOB_PULL ||
       job->kind == DEPOT_JOB_SYNC || job->kind == DEPOT_JOB_SWITCH ||
       job->kind == DEPOT_JOB_CLONE || job->kind == DEPOT_JOB_INIT)) {
    string_format(
        out, capacity,
        job->kind == DEPOT_JOB_INIT
            ? "%s: rebuilding imported models for other computers %u/%u"
            : "%s: preparing textures for this computer %u/%u",
        step, cook_done, cook_total);
    return (float32_t)cook_done / (float32_t)cook_total;
  }
  char done[32];
  char total[32];
  depot_format_size(view.bytes_done, done, sizeof(done));
  depot_format_size(view.bytes_total, total, sizeof(total));
  const float32_t fraction =
      view.bytes_total
          ? (float32_t)view.bytes_done / (float32_t)view.bytes_total
      : view.files_total
          ? (float32_t)view.files_done / (float32_t)view.files_total
          : -1.0f;
  switch (view.phase) {
  case VKR_DEPOT_PHASE_CONNECTING:
    string_format(out, capacity, "%s: connecting", step);
    return -1.0f;
  case VKR_DEPOT_PHASE_SNAPSHOT:
    if (job->kind == DEPOT_JOB_INIT) {
      string_format(out, capacity, "%s: copying the project and its assets",
                    step);
      return -1.0f;
    }
    string_format(out, capacity, "%s: reading changed files %llu/%llu", step,
                  (unsigned long long)view.files_done,
                  (unsigned long long)view.files_total);
    return fraction;
  case VKR_DEPOT_PHASE_UPLOAD:
    string_format(out, capacity, "%s: sending %s of %s", step, done, total);
    return fraction;
  case VKR_DEPOT_PHASE_FETCH:
    string_format(out, capacity, "%s: receiving %s of %s", step, done, total);
    return fraction;
  case VKR_DEPOT_PHASE_CHECKOUT:
    string_format(out, capacity, "%s: writing files %llu/%llu", step,
                  (unsigned long long)view.files_done,
                  (unsigned long long)view.files_total);
    return fraction;
  default:
    string_format(out, capacity, "%s", step);
    return -1.0f;
  }
}

static bool8_t depot_incoming(const VkrEditorDepot *depot) {
  return depot->remote_known && depot->behind > 0u;
}

static const char *depot_role_name(VkrDepotRole role) {
  return role == VKR_DEPOT_ROLE_ADMIN   ? "owner"
         : role == VKR_DEPOT_ROLE_WRITE ? "can push"
         : role == VKR_DEPOT_ROLE_READ  ? "can fetch"
                                        : "no access";
}

static const VkrDepotRemoteEntry *depot_selected(const VkrEditorDepot *depot) {
  return depot->selected_remote < depot->remote_count
             ? &depot->remotes[depot->selected_remote]
             : NULL;
}

/* What a remote's last exchange says about reaching it. */
static const char *depot_remote_state(const VkrEditorDepot *depot,
                                      uint32_t index) {
  if (!depot->remote_tried[index]) {
    return "not reached yet";
  }
  switch (depot->remote_connect[index]) {
  case VKR_DEPOT_CONNECT_OK:
    return "reachable";
  case VKR_DEPOT_CONNECT_DENIED:
    return "waiting for its owner's Allow";
  case VKR_DEPOT_CONNECT_UNREACHABLE:
    return "not answering: stopped or blocked";
  case VKR_DEPOT_CONNECT_REFUSED:
    return "refused the connection";
  default:
    return "not reached";
  }
}

static bool8_t depot_found_is_self(const VkrEditorDepot *depot,
                                   const VkrDepotDiscovered *found) {
  return depot->service && MemCompare(found->server_key,
                                      depot->service_info.server_key, 32u) == 0;
}

// =============================================================================
// Top bar
// =============================================================================

void vkr_editor_depot_chip_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 uint32_t column) {
  if (!depot || !depot->project_id[0]) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[96];
  const char *tooltip = "Source control";
  VkrUiIcon icon = VKR_UI_ICON_GIT_BRANCH;
  Vec4 icon_color = theme->text_secondary;
  const char *branch = depot->branch[0] ? depot->branch : DEPOT_BRANCH;
  if (depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS) {
    icon = VKR_UI_ICON_SPINNER;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%s", depot_job_names[depot->job->kind]);
  } else if (!depot->repository) {
    string_format(text, sizeof(text), "Not a repository");
    tooltip = "Make this project a repository to commit, branch and sync it";
  } else if (depot->conflict_count) {
    icon = VKR_UI_ICON_WARNING_FILL;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "%s \xc2\xb7 conflicts", branch);
  } else if (depot->waiting_kind != DEPOT_JOB_NONE) {
    icon = VKR_UI_ICON_CLOCK;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "%s \xc2\xb7 waiting for access", branch);
    tooltip = "The remote's owner has not allowed this computer yet";
  } else if (depot->request_count) {
    icon = VKR_UI_ICON_BELL;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "%s \xc2\xb7 %u asking", branch,
                  depot->request_count);
    tooltip = "Other computers ask to fetch from and push to this repository";
  } else if (depot_incoming(depot)) {
    icon = VKR_UI_ICON_ARROW_DOWN;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%s \xc2\xb7 %u to pull", branch,
                  depot->behind);
    tooltip = "The remote has commits this repository lacks; Pull or Sync";
  } else if (depot->change_count) {
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%s \xc2\xb7 %u change%s", branch,
                  depot->change_count, depot->change_count == 1u ? "" : "s");
    tooltip = "Local changes to commit";
  } else if (depot->ahead) {
    icon = VKR_UI_ICON_ARROW_UP;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%s \xc2\xb7 %u to push", branch,
                  depot->ahead);
    tooltip = "Commits the remote lacks; Push or Sync";
  } else {
    icon_color = depot->status_known ? theme->success : theme->text_disabled;
    string_format(text, sizeof(text), "%s", branch);
    tooltip = depot->remote_count ? "Up to date with the remote"
                                  : "No remote yet: add one to sync";
  }
  VkrUiWidgetConfig chip =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  chip.placement = (VkrUiPlacement){
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_END,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 4.0f, 0.0f, 4.0f},
  };
  chip.style.padding_pt = (VkrUiEdges){3.0f, 10.0f, 3.0f, 8.0f};
  chip.style.background_color = theme->field;
  chip.style.hover_background_color = theme->raised_hover;
  chip.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  chip.style.border_color = theme->border;
  chip.style.corner_radius_pt = (Vec4){11.0f, 11.0f, 11.0f, 11.0f};
  chip.icon = icon;
  chip.icon_size_pt = 13.0f;
  chip.icon_color = icon_color;
  chip.tooltip = depot_string(tooltip);
  if (vkr_ui_button(ui, string8_lit("depot.chip"), depot_string(text), &chip)) {
    VkrEditorWindowState *window = &editor->windows[VKR_EDITOR_WINDOW_DEPOT];
    window->visible = !window->visible;
  }
}

// =============================================================================
// Source Control window
// =============================================================================

static void depot_window_not_repository(VkrEditorDepot *depot,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        VkrUiSystem *ui, float32_t *y,
                                        float32_t width) {
  depot_heading(editor, ui, "init.title", "Make this project a repository",
                VKR_UI_ICON_GIT_BRANCH, 12, *y, width - 24);
  *y += 30;
  *y += depot_paragraph(
      ui, "init.text",
      "A repository keeps the project's history: commit your changes, make "
      "branches, and sync with other computers' repositories. The project "
      "and every asset it uses move to this folder, where you keep "
      "working:",
      12, *y, width - 24);
  *y += 6;
  if (!depot->init_destination[0]) {
    depot_default_destination(depot->project_name, depot->init_destination,
                              sizeof(depot->init_destination));
  }
  depot_field(ui, "init.destination", depot->init_destination,
              sizeof(depot->init_destination), 12, *y, width - 24);
  *y += 34;
  VkrUiWidgetConfig serve = depot_widget(12, *y, width - 24, 22);
  serve.tooltip = string8_lit(
      "Other computers on your network can clone it and push to it while "
      "this editor runs");
  (void)vkr_ui_checkbox(ui, string8_lit("init.serve"),
                        string8_lit("Let others fetch and push"),
                        &depot->init_serve, &serve);
  *y += 30;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "init.go", "Make repository", 12, *y, 170,
                   true_v, depot_busy(depot)) &&
      !vkr_editor_depot_init(depot, editor, frame, depot->init_destination,
                             depot->init_serve, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 40;
}

/* A row of small toggle buttons, one per item, the chosen one primary;
   returns the index clicked, or UINT32_MAX. */
static uint32_t depot_choice_row(VkrEditorUi *editor, VkrUiSystem *ui,
                                 const char *id, const char *const *names,
                                 uint32_t count, uint32_t chosen, float32_t x,
                                 float32_t *y, float32_t width,
                                 bool8_t disabled) {
  uint32_t clicked = UINT32_MAX;
  float32_t cursor = x;
  for (uint32_t i = 0u; i < count; ++i) {
    const float32_t button =
        Min(160.0f, 24.0f + 7.0f * (float32_t)string_length(names[i]));
    if (cursor + button > x + width && cursor > x) {
      cursor = x;
      *y += 32;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    if (depot_button(editor, ui, id, names[i], cursor, *y, button, i == chosen,
                     disabled) &&
        i != chosen) {
      clicked = i;
    }
    (void)vkr_ui_pop_id(ui);
    cursor += button + 8;
  }
  *y += 32;
  return clicked;
}

static void depot_window_branch(VkrEditorDepot *depot, VkrEditorUi *editor,
                                VkrUiSystem *ui, float32_t *y, float32_t width,
                                bool8_t busy) {
  char text[DEPOT_TEXT];
  depot_heading(editor, ui, "repo.title", depot->project_name,
                VKR_UI_ICON_GIT_BRANCH, 12, *y, width - 24);
  *y += 28;
  const VkrDepotRemoteEntry *remote = depot_selected(depot);
  if (remote && depot->remote_known) {
    string_format(text, sizeof(text),
                  "On %s  \xc2\xb7  %u to push, %u to pull against %s/%s",
                  depot->branch, depot->ahead, depot->behind, remote->name,
                  depot->branch);
  } else if (remote) {
    string_format(text, sizeof(text), "On %s  \xc2\xb7  %s has no %s yet",
                  depot->branch, remote->name, depot->branch);
  } else {
    string_format(text, sizeof(text), "On %s  \xc2\xb7  no remote",
                  depot->branch);
  }
  depot_caption(ui, "repo.branch", text, 12, *y, width - 24);
  *y += 24;
  if (depot->branch_count > 1u) {
    const char *names[DEPOT_BRANCHES_MAX];
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0u; i < depot->branch_count; ++i) {
      names[i] = depot->branches[i].name;
      if (string_equals(names[i], depot->branch)) {
        chosen = i;
      }
    }
    const uint32_t clicked =
        depot_choice_row(editor, ui, "branch", names, depot->branch_count,
                         chosen, 12, y, width - 24, busy);
    char error[DEPOT_TEXT] = {0};
    if (clicked != UINT32_MAX &&
        !vkr_editor_depot_switch(depot, names[clicked], false_v, error,
                                 sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  const float32_t field = Min(width - 160.0f, 260.0f);
  depot_field(ui, "branch.new", depot->new_branch, sizeof(depot->new_branch),
              12, *y, field);
  if (!depot->new_branch[0]) {
    VkrUiWidgetConfig hint = depot_widget(18, *y + 3, field - 12, 20);
    hint.style.text_color = vkr_ui_theme()->text_disabled;
    hint.style.font_size_pt = vkr_ui_theme()->font_caption;
    vkr_ui_label(ui, string8_lit("branch.hint"), string8_lit("New branch name"),
                 &hint);
  }
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "branch.create", "New branch", 20 + field, *y,
                   120, false_v, busy || !depot->new_branch[0]) &&
      !vkr_editor_depot_switch(depot, depot->new_branch, true_v, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 38;
}

static void depot_window_sync(VkrEditorDepot *depot, VkrEditorUi *editor,
                              VkrUiSystem *ui, float32_t *y, float32_t width,
                              bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!depot->remote_count) {
    return;
  }
  if (depot->remote_count > 1u) {
    const char *names[VKR_DEPOT_REMOTES_MAX];
    for (uint32_t i = 0u; i < depot->remote_count; ++i) {
      names[i] = depot->remotes[i].name;
    }
    const uint32_t clicked =
        depot_choice_row(editor, ui, "remote.pick", names, depot->remote_count,
                         depot->selected_remote, 12, y, width - 24, busy);
    if (clicked != UINT32_MAX) {
      depot->selected_remote = clicked;
      depot->next_status = 0.0;
    }
  }
  char text[DEPOT_TEXT];
  const VkrDepotRemoteEntry *remote = depot_selected(depot);
  string_format(text, sizeof(text), "%s  \xc2\xb7  %s  \xc2\xb7  %s",
                remote->name, remote->address,
                depot_remote_state(depot, depot->selected_remote));
  VkrUiWidgetConfig state = depot_widget(12, *y, width - 24, 18);
  state.style.font_size_pt = theme->font_caption;
  state.style.text_color =
      depot->remote_tried[depot->selected_remote] &&
              depot->remote_connect[depot->selected_remote] !=
                  VKR_DEPOT_CONNECT_OK
          ? theme->warning
          : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("remote.state"), depot_string(text), &state);
  *y += 24;
  char error[DEPOT_TEXT] = {0};
  const float32_t button = Min(96.0f, (width - 48) / 4.0f);
  if (depot_button(editor, ui, "sync.go", "Sync", 12, *y, button, true_v,
                   busy) &&
      !vkr_editor_depot_sync(depot, NULL, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "sync.pull", "Pull", 20 + button, *y, button,
                   false_v, busy) &&
      !vkr_editor_depot_pull(depot, NULL, "fail", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "sync.push", "Push", 28 + button * 2, *y, button,
                   false_v, busy || !depot->ahead) &&
      !vkr_editor_depot_push(depot, NULL, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "sync.fetch", "Fetch", 36 + button * 3, *y,
                   button, false_v, busy) &&
      !vkr_editor_depot_fetch(depot, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 38;
}

static void depot_window_conflicts(VkrEditorDepot *depot, VkrEditorUi *editor,
                                   VkrUiSystem *ui, float32_t *y,
                                   float32_t width, bool8_t busy) {
  if (!depot->conflict_count) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  string_format(text, sizeof(text), "%u file%s changed here and on the remote",
                depot->conflict_count, depot->conflict_count == 1u ? "" : "s");
  depot_label(ui, "conflict.title", text, 12, *y, width - 24, theme->warning);
  *y += 24;
  /* The end of a path names it: a scene's id and file, not the projects
     folder every path shares. */
  const size_t shown = (size_t)Max(16.0f, (width - 48.0f) / 6.4f);
  for (uint32_t i = 0u; i < Min(depot->conflict_count, 6u); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const char *path = depot->conflicts[i].text;
    char tail[DEPOT_TEXT];
    const size_t length = string_length(path);
    if (length > shown) {
      string_format(tail, sizeof(tail), "\xe2\x80\xa6%s",
                    path + length - shown + 1u);
    } else {
      string_format(tail, sizeof(tail), "%s", path);
    }
    depot_caption(ui, "conflict", tail, 24, *y, width - 36);
    (void)vkr_ui_pop_id(ui);
    *y += 18;
  }
  *y += 4;
  *y += depot_paragraph(ui, "conflict.text",
                        "A scene both sides changed is listed by its scene "
                        "file and settled whole.",
                        12, *y, width - 24);
  *y += 4;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "conflict.theirs", "Use theirs", 12, *y, 120,
                   false_v, busy) &&
      !vkr_editor_depot_pull(depot, NULL, "theirs", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "conflict.mine", "Keep mine", 140, *y, 120,
                   false_v, busy) &&
      !vkr_editor_depot_pull(depot, NULL, "mine", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 38;
}

/* Files the project reads from outside itself, which the last commit
   stopped for: copy them in and commit, or commit without them. */
static void depot_window_untracked(VkrEditorDepot *depot, VkrEditorUi *editor,
                                   VkrUiSystem *ui, float32_t *y,
                                   float32_t width, bool8_t busy) {
  if (!depot->untracked_count) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  char size[32];
  depot_format_size(depot->untracked_bytes, size, sizeof(size));
  string_format(text, sizeof(text), "%u file%s from outside the project (%s)",
                depot->untracked_count, depot->untracked_count == 1u ? "" : "s",
                size);
  depot_label(ui, "untracked.title", text, 12, *y, width - 24, theme->warning);
  *y += 24;
  for (uint32_t i = 0u; i < Min(depot->untracked_count, 6u); ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    depot_caption(ui, "untracked", depot->untracked[i].text, 24, *y,
                  width - 36);
    (void)vkr_ui_pop_id(ui);
    *y += 18;
  }
  if (depot->untracked_count > 6u) {
    string_format(text, sizeof(text), "and %u more",
                  depot->untracked_count - 6u);
    depot_caption(ui, "untracked.more", text, 24, *y, width - 36);
    *y += 18;
  }
  *y += 4;
  *y += depot_paragraph(ui, "untracked.text",
                        "The project reads them from the engine's folder, so "
                        "a clone on another computer would miss them.",
                        12, *y, width - 24);
  *y += 4;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "untracked.adopt", "Copy in and commit", 12, *y,
                   170, true_v, busy) &&
      !vkr_editor_depot_commit(depot, depot->message, false_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_ADOPT, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "untracked.ignore", "Commit without them", 190,
                   *y, 170, false_v, busy) &&
      !vkr_editor_depot_commit(depot, depot->message, false_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_IGNORE, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 38;
}

static void depot_window_changes(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, VkrUiSystem *ui,
                                 float32_t *y, float32_t width, bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  char error[DEPOT_TEXT] = {0};
  if (!depot->status_known && depot_busy(depot)) {
    depot_caption(ui, "changes.reading", "Looking for changes...", 12, *y,
                  width - 24);
    *y += 26;
    return;
  }
  if (!depot->change_count) {
    depot_label(ui, "changes.none", "No changes to commit", 12, *y, width - 24,
                theme->text_secondary);
    *y += 30;
    return;
  }
  string_format(text, sizeof(text), "%u change%s", depot->change_count,
                depot->change_count == 1u ? "" : "s");
  depot_heading(editor, ui, "changes.title", text, VKR_UI_ICON_PLUS_CIRCLE, 12,
                *y, width - 24);
  *y += 28;
  const uint32_t rows = Min(depot->change_count, 8u);
  for (uint32_t i = 0u; i < rows; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    depot_caption(ui, "change", depot->changes[i].text, 24, *y, width - 36);
    (void)vkr_ui_pop_id(ui);
    *y += 18;
  }
  if (depot->change_count > rows) {
    string_format(text, sizeof(text), "and %u more",
                  depot->change_count - rows);
    depot_caption(ui, "changes.more", text, 24, *y, width - 36);
    *y += 18;
  }
  *y += 8;
  depot_field(ui, "message", depot->message, sizeof(depot->message), 12, *y,
              width - 24);
  if (!depot->message[0]) {
    VkrUiWidgetConfig hint = depot_widget(18, *y + 3, width - 40, 20);
    hint.style.text_color = theme->text_disabled;
    hint.style.font_size_pt = theme->font_caption;
    vkr_ui_label(ui, string8_lit("message.hint"),
                 string8_lit("Describe your changes"), &hint);
  }
  *y += 36;
  const bool8_t unsaved = !depot_journals_saved(frame);
  if (depot_button(editor, ui, "commit", "Commit", 12, *y, 120, true_v,
                   busy || unsaved) &&
      !vkr_editor_depot_commit(depot, depot->message, false_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_STOP, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot->remote_count &&
      depot_button(editor, ui, "commit.sync", "Commit & Sync", 140, *y, 140,
                   false_v, busy || unsaved) &&
      !vkr_editor_depot_commit(depot, depot->message, true_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_STOP, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (unsaved) {
    depot_caption(ui, "commit.unsaved", "Save the scene to commit its edits",
                  12, *y + 30, width - 24);
    *y += 20;
  }
  *y += 40;
}

static void depot_commit_rows(VkrUiSystem *ui, const char *id,
                              const char *title, const DepotHistory *rows,
                              uint32_t count, uint32_t total, float32_t *y,
                              float32_t width) {
  if (!total) {
    return;
  }
  char text[DEPOT_TEXT];
  string_format(text, sizeof(text), "%s (%u)", title, total);
  (void)vkr_ui_push_id_u64(ui, (uint64_t)(uintptr_t)id);
  depot_caption(ui, "title", text, 12, *y, width - 24);
  *y += 22;
  const uint64_t now_us = depot_unix_us();
  for (uint32_t i = 0u; i < count; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    depot_label(ui, "row",
                rows[i].message[0] ? rows[i].message : "(no message)", 24, *y,
                width - 140, vkr_ui_theme()->text);
    char when[32];
    depot_format_age(now_us, rows[i].time_us, when, sizeof(when));
    VkrUiWidgetConfig age = depot_widget(width - 112, *y, 100, 22);
    age.style.font_size_pt = vkr_ui_theme()->font_caption;
    age.style.text_color = vkr_ui_theme()->text_secondary;
    vkr_ui_label(ui, string8_lit("age"), depot_string(when), &age);
    (void)vkr_ui_pop_id(ui);
    *y += 22;
  }
  (void)vkr_ui_pop_id(ui);
  *y += 8;
}

static void depot_window_remotes(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 VkrUiSystem *ui, float32_t *y, float32_t width,
                                 bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char error[DEPOT_TEXT] = {0};
  depot_caption(ui, "remotes.title", "Remotes", 12, *y, width - 160);
  if (depot_button(editor, ui, "remotes.add", "Add remote", width - 136, *y - 4,
                   124, !depot->remote_count, false_v)) {
    depot->remote_form_open = !depot->remote_form_open;
  }
  *y += 28;
  for (uint32_t i = 0u; i < depot->remote_count; ++i) {
    const VkrDepotRemoteEntry *remote = &depot->remotes[i];
    (void)vkr_ui_push_id_u64(ui, i);
    char text[DEPOT_TEXT];
    char print[16];
    depot_fingerprint(remote->key, print);
    string_format(text, sizeof(text), "%s  \xc2\xb7  %s  \xc2\xb7  key %s",
                  remote->name, remote->address, print);
    depot_label(ui, "row", text, 24, *y, width - 150, theme->text);
    if (depot_button(editor, ui, "remove", "Remove", width - 112, *y - 2, 100,
                     false_v, busy) &&
        !vkr_editor_depot_remote(depot, remote->name, NULL, NULL, error,
                                 sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 28;
  }
  if (!depot->remote_count && !depot->remote_form_open) {
    *y += depot_paragraph(
        ui, "remotes.none",
        "A remote is another computer's repository of this project. Add "
        "one to push to it and pull from it.",
        12, *y, width - 24);
    *y += 6;
  }
  if (!depot->remote_form_open) {
    return;
  }
  depot->browse_wanted = true_v;
  /* Services found on the network fill the form with one click; their key
     comes from an answer anyone could send, so the owner checks its first
     digits on the other computer. */
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    const VkrDepotDiscovered *found = &depot->browser.found[i];
    if (depot_found_is_self(depot, found)) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, 100u + i);
    char address[VKR_NET_ADDRESS_TEXT];
    char print[16];
    char text[DEPOT_TEXT];
    vkr_net_address_format(&found->address, address, sizeof(address));
    depot_fingerprint(found->server_key, print);
    string_format(text, sizeof(text), "%s on %s  \xc2\xb7  key %s", found->name,
                  found->host, print);
    depot_caption(ui, "found", text, 24, *y + 4, width - 150);
    if (depot_button(editor, ui, "use", "Use", width - 112, *y, 100, false_v,
                     false_v)) {
      string_format(depot->remote_address, sizeof(depot->remote_address), "%s",
                    address);
      depot_key_hex(found->server_key, depot->remote_key);
      if (!depot->remote_name[0]) {
        string_format(depot->remote_name, sizeof(depot->remote_name), "%s",
                      depot->remote_count ? "peer" : VKR_DEPOT_ORIGIN);
      }
    }
    (void)vkr_ui_pop_id(ui);
    *y += 30;
  }
  if (!depot->remote_name[0]) {
    string_format(depot->remote_name, sizeof(depot->remote_name), "%s",
                  depot->remote_count ? "peer" : VKR_DEPOT_ORIGIN);
  }
  static const char *const labels[] = {"Name", "Address", "Key"};
  char *fields[] = {depot->remote_name, depot->remote_address,
                    depot->remote_key};
  const uint32_t sizes[] = {sizeof(depot->remote_name),
                            sizeof(depot->remote_address),
                            sizeof(depot->remote_key)};
  static const char *const ids[] = {"remote.name", "remote.address",
                                    "remote.key"};
  for (uint32_t i = 0u; i < ArrayCount(labels); ++i) {
    depot_caption(ui, labels[i], labels[i], 12, *y + 4, 70);
    depot_field(ui, ids[i], fields[i], sizes[i], 84, *y, width - 96);
    *y += 32;
  }
  if (depot_button(editor, ui, "remote.save", "Add", 12, *y, 100, true_v,
                   !depot->remote_address[0] || !depot->remote_key[0])) {
    if (vkr_editor_depot_remote(depot, depot->remote_name,
                                depot->remote_address, depot->remote_key, error,
                                sizeof(error))) {
      depot->remote_form_open = false_v;
      depot->remote_name[0] = '\0';
      depot->remote_address[0] = '\0';
      depot->remote_key[0] = '\0';
    } else {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  if (depot_button(editor, ui, "remote.cancel", "Cancel", 120, *y, 100, false_v,
                   false_v)) {
    depot->remote_form_open = false_v;
  }
  *y += 40;
}

static void depot_window_server(VkrEditorDepot *depot, VkrEditorUi *editor,
                                VkrUiSystem *ui, float32_t *y,
                                float32_t width) {
  char text[DEPOT_TEXT];
  bool8_t serve = depot->serving;
  VkrUiWidgetConfig toggle = depot_widget(12, *y, width - 24, 22);
  toggle.tooltip = string8_lit(
      "Other computers on your network can clone this repository and push "
      "to it while this editor runs");
  (void)vkr_ui_checkbox(ui, string8_lit("serve"),
                        string8_lit("Let others fetch and push"), &serve,
                        &toggle);
  if (serve != depot->serving) {
    char error[DEPOT_TEXT] = {0};
    if (!vkr_editor_depot_serve(depot, serve, error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  *y += 28;
  if (!depot->serving) {
    return;
  }
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  vkr_depot_host_name(host, sizeof(host));
  if (depot->service) {
    char print[16];
    depot_fingerprint(depot->service_info.server_key, print);
    string_format(text, sizeof(text),
                  "Served by this editor on %s, UDP %u  \xc2\xb7  key %s  "
                  "\xc2\xb7  %u connected",
                  host, (uint32_t)depot->service_info.address.port, print,
                  depot->service_info.peers);
    depot_caption(ui, "server.peers", text, 24, *y, width - 36);
    *y += 22;
    if (!depot->service_info.discoverable) {
      string_format(text, sizeof(text),
                    "Not listed on the network: another server uses its "
                    "port. Others enter %s to find it.",
                    host);
      *y += depot_paragraph(ui, "server.hidden", text, 24, *y, width - 36);
    }
  } else {
    depot_label(ui, "server.error",
                depot->service_error[0] ? depot->service_error
                                        : "The server is not running",
                24, *y, width - 160, vkr_ui_theme()->warning);
    if (depot_button(editor, ui, "server.retry", "Start", width - 112, *y - 4,
                     100, false_v, false_v)) {
      (void)depot_service_start(depot);
    }
    *y += 28;
  }
  *y += 4;
}

static void depot_window_requests(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  VkrUiSystem *ui, float32_t *y,
                                  float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  for (uint32_t i = 0u; i < depot->request_count; ++i) {
    const VkrDepotAccessRequest *request = &depot->requests[i];
    (void)vkr_ui_push_id_u64(ui, i);
    char who[VKR_DEPOT_DISPLAY_NAME + 16];
    depot_person_text(depot, request->key, request->name, who, sizeof(who));
    char text[160];
    string_format(text, sizeof(text), "%s asks to fetch and push", who);
    depot_label(ui, "request", text, 12, *y, width - 24, theme->warning);
    *y += 22;
    char print[16];
    depot_fingerprint(request->key, print);
    string_format(
        text, sizeof(text),
        "Its key starts %s; that computer shows the same while it waits",
        print);
    depot_caption(ui, "request.key", text, 12, *y, width - 24);
    *y += 24;
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(request->key, key);
    char error[DEPOT_TEXT] = {0};
    if (depot_button(editor, ui, "allow", "Allow", 12, *y, 96, true_v,
                     false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "write", error, sizeof(error));
    }
    if (depot_button(editor, ui, "read", "Fetch only", 116, *y, 96, false_v,
                     false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "read", error, sizeof(error));
    }
    if (depot_button(editor, ui, "deny", "Deny", 220, *y, 80, false_v,
                     false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "none", error, sizeof(error));
    }
    if (error[0]) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 34;
  }
}

static void depot_window_access(VkrEditorDepot *depot, VkrEditorUi *editor,
                                VkrUiSystem *ui, float32_t *y,
                                float32_t width) {
  uint32_t shown = 0u;
  for (uint32_t i = 0u; i < depot->access_count; ++i) {
    if (depot->access[i].role != VKR_DEPOT_ROLE_ADMIN) {
      ++shown;
    }
  }
  if (!shown) {
    return;
  }
  depot_caption(ui, "access.title", "Computers with access", 12, *y,
                width - 24);
  *y += 22;
  for (uint32_t i = 0u; i < depot->access_count; ++i) {
    const VkrDepotAccessEntry *entry = &depot->access[i];
    if (entry->role == VKR_DEPOT_ROLE_ADMIN) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(entry->key, key);
    char text[160];
    char who[VKR_DEPOT_DISPLAY_NAME + 16];
    depot_person_text(depot, entry->key, NULL, who, sizeof(who));
    string_format(text, sizeof(text), "%s  \xc2\xb7  %s", who,
                  depot_role_name(entry->role));
    depot_label(ui, "entry", text, 24, *y, width - 150, vkr_ui_theme()->text);
    char error[DEPOT_TEXT] = {0};
    if (depot_button(editor, ui, "remove", "Remove", width - 112, *y - 2, 100,
                     false_v, false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "none", error, sizeof(error));
    }
    (void)vkr_ui_pop_id(ui);
    *y += 28;
  }
  *y += 6;
}

static void depot_window_history(VkrEditorDepot *depot, VkrUiSystem *ui,
                                 float32_t *y, float32_t width) {
  depot_commit_rows(ui, "history", "History", depot->history,
                    depot->history_count, depot->history_count, y, width);
}

void vkr_editor_depot_window_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds) {
  VkrEditorDepot *depot = editor->depot;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / ui->content_scale;
  const float32_t height = bounds.height / ui->content_scale;
  if (!depot || width < 260) {
    return;
  }
  /* Discovery follows a remote that moved to another address. */
  depot->browse_wanted = depot->browse_wanted || depot->remote_count > 0u;
  if (ui->mouse_x >= bounds.x && ui->mouse_x < bounds.x + bounds.width &&
      ui->mouse_y >= bounds.y && ui->mouse_y < bounds.y + bounds.height &&
      !ui->mouse_captured) {
    depot->window_scroll -= ui->mouse_wheel * 40;
  }
  depot->window_scroll = vkr_clamp_f32(
      depot->window_scroll, 0, Max(0.0f, depot->window_height - height));
  VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
  const VkrUiTrack track = {.value = Max(height, depot->window_height),
                            .unit = VKR_UI_TRACK_PX};
  scroll.rows = &track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("depot.layout"), &scroll)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &depot->window_scroll);
  float32_t y = 10;
  const bool8_t busy =
      depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS;
  if (!depot->project_id[0]) {
    depot_label(ui, "none", "Open a project to use source control.", 12, y,
                width - 24, theme->text_secondary);
    y += 30;
  } else if (!depot->repository && !depot_busy(depot)) {
    depot_window_not_repository(depot, editor, frame, ui, &y, width);
  } else if (depot->repository) {
    depot_window_branch(depot, editor, ui, &y, width, busy);
    depot_window_sync(depot, editor, ui, &y, width, busy);
    depot_window_conflicts(depot, editor, ui, &y, width, busy);
    depot_window_untracked(depot, editor, ui, &y, width, busy);
    depot_window_changes(depot, editor, frame, ui, &y, width, busy);
    depot_commit_rows(ui, "outgoing", "To push", depot->outgoing,
                      depot->outgoing_count, depot->ahead, &y, width);
    depot_commit_rows(ui, "incoming", "To pull", depot->incoming,
                      depot->incoming_count, depot->behind, &y, width);
    depot_window_remotes(depot, editor, ui, &y, width, busy);
    depot_window_server(depot, editor, ui, &y, width);
    depot_window_requests(depot, editor, ui, &y, width);
    if (depot->serving) {
      depot_window_access(depot, editor, ui, &y, width);
    }
    if (vkr_editor_scripts_held(editor->scripts)) {
      depot_label(ui, "scripts.held", "Scripts from others are not running", 12,
                  y, width - 160, theme->warning);
      if (depot_button(editor, ui, "scripts.trust", "Run scripts", width - 136,
                       y - 2, 124, false_v, false_v)) {
        vkr_editor_depot_trust_scripts(depot, editor);
      }
      y += 34;
    }
    depot_window_history(depot, ui, &y, width);
  }
  if (busy) {
    char text[DEPOT_TEXT];
    const float32_t fraction = depot_job_text(depot, text, sizeof(text));
    depot_label(ui, "job.text", text, 12, y, width - 120, theme->text);
    if (depot_button(editor, ui, "job.cancel", "Cancel", width - 100, y - 2, 88,
                     false_v, false_v)) {
      vkr_editor_depot_cancel(depot);
    }
    y += 28;
    depot_progress_bar(ui, "job.bar", 12, y, width - 24, fraction);
    y += 18;
  }
  if (depot->error[0]) {
    VkrUiWidgetConfig c = depot_widget(12, y, width - 24, 40);
    c.style.text_color = theme->warning;
    c.style.max_size_pt.y = 60;
    c.text.layout.word_wrap = true_v;
    c.text.layout.max_width = width - 32;
    vkr_ui_label(ui, string8_lit("error"), depot_string(depot->error), &c);
    y += 46;
  }
  depot->window_height = y + 12;
  (void)vkr_ui_scroll_area_end(ui);
}

// =============================================================================
// Overlay: access requests and progress
// =============================================================================

static void depot_overlay_request(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame) {
  if (!depot->request_count ||
      editor->windows[VKR_EDITOR_WINDOW_DEPOT].visible) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t target_width =
      (float32_t)ui->target_width / ui->content_scale;
  const float32_t width = Min(380.0f, target_width - 24.0f);
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement = (VkrUiPlacement){
      .column_span = 1,
      .row_span = 1,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {VKR_EDITOR_NAVIGATION_HEIGHT_PT + 12.0f, 0, 0,
                    target_width - width - 12.0f},
  };
  card.style = vkr_editor_glass_style();
  card.style.min_size_pt = card.style.max_size_pt = (Vec2){width, 104.0f};
  card.presence = VKR_UI_PRESENCE_POPUP;
  if (!vkr_ui_panel_begin(ui, string8_lit("depot.request"), &card)) {
    return;
  }
  const VkrDepotAccessRequest *request = &depot->requests[0];
  char who[VKR_DEPOT_DISPLAY_NAME + 16];
  depot_person_text(depot, request->key, request->name, who, sizeof(who));
  VkrUiWidgetConfig c = depot_widget(12, 10, width - 24, 22);
  c.icon = VKR_UI_ICON_BELL;
  c.icon_size_pt = 13.0f;
  c.icon_color = theme->warning;
  c.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("who"), depot_string(who), &c);
  char text[200];
  char print[16];
  depot_fingerprint(request->key, print);
  string_format(text, sizeof(text),
                "asks to fetch and push %s  \xc2\xb7  key %s",
                depot->project_name, print);
  depot_caption(ui, "text", text, 34, 34, width - 46);
  char key[VKR_DEPOT_ID_HEX];
  depot_key_hex(request->key, key);
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "allow", "Allow", 12, 64, 100, true_v,
                   false_v)) {
    (void)vkr_editor_depot_allow(depot, key, "write", error, sizeof(error));
  }
  if (depot_button(editor, ui, "read", "Fetch only", 120, 64, 100, false_v,
                   false_v)) {
    (void)vkr_editor_depot_allow(depot, key, "read", error, sizeof(error));
  }
  if (depot_button(editor, ui, "deny", "Deny", 228, 64, 80, false_v, false_v)) {
    (void)vkr_editor_depot_allow(depot, key, "none", error, sizeof(error));
  }
  if (error[0]) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Scripts are native code: a project's scripts from another computer wait
   for this user's trust before they build and run. */
static void depot_overlay_scripts(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame) {
  if (!vkr_editor_scripts_held(editor->scripts) ||
      depot->scripts_prompt_dismissed) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t target_width =
      (float32_t)ui->target_width / ui->content_scale;
  const float32_t width = Min(400.0f, target_width - 24.0f);
  const bool8_t below_request =
      depot->request_count && !editor->windows[VKR_EDITOR_WINDOW_DEPOT].visible;
  const float32_t top =
      VKR_EDITOR_NAVIGATION_HEIGHT_PT + 12.0f + (below_request ? 116.0f : 0.0f);
  VkrUiPanelConfig card = vkr_ui_panel_config_default();
  card.placement = (VkrUiPlacement){
      .column_span = 1,
      .row_span = 1,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {top, 0, 0, target_width - width - 12.0f},
  };
  card.style = vkr_editor_glass_style();
  card.style.min_size_pt = card.style.max_size_pt = (Vec2){width, 122.0f};
  card.presence = VKR_UI_PRESENCE_POPUP;
  if (!vkr_ui_panel_begin(ui, string8_lit("depot.scripts"), &card)) {
    return;
  }
  VkrUiWidgetConfig c = depot_widget(12, 10, width - 24, 22);
  c.icon = VKR_UI_ICON_CODE;
  c.icon_size_pt = 13.0f;
  c.icon_color = theme->warning;
  c.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("title"),
               string8_lit("Scripts from another computer"), &c);
  (void)depot_paragraph(ui, "text",
                        "This project's scripts run as programs on this "
                        "computer. Run them only if you trust the people "
                        "you share it with.",
                        34, 34, width - 46);
  if (depot_button(editor, ui, "trust", "Run scripts", 12, 84, 130, true_v,
                   false_v)) {
    vkr_editor_depot_trust_scripts(depot, editor);
  }
  if (depot_button(editor, ui, "later", "Not now", 150, 84, 100, false_v,
                   false_v)) {
    depot->scripts_prompt_dismissed = true_v;
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_depot_overlay_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  if (!depot) {
    return;
  }
  depot_overlay_request(depot, editor, frame);
  depot_overlay_scripts(depot, editor, frame);
}

// =============================================================================
// Project hub
// =============================================================================

/* A raised rounded panel behind one row of the hub, as project cards
   are drawn. */
static void depot_card(VkrUiSystem *ui, const char *id, float32_t x,
                       float32_t y, float32_t width, float32_t height) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig card = depot_widget(x, y, width, height);
  card.style.background_color = theme->raised;
  card.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  card.style.border_color = theme->border;
  card.style.corner_radius_pt = (Vec4){8, 8, 8, 8};
  vkr_ui_label(ui, depot_string(id), (String8){0}, &card);
}

static void depot_row_title(VkrUiSystem *ui, const char *text, VkrUiIcon icon,
                            float32_t x, float32_t y, float32_t width) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 22);
  c.style.font_size_pt = vkr_ui_theme()->font_emphasis;
  c.icon = icon;
  c.icon_size_pt = 15.0f;
  c.icon_color = vkr_ui_theme()->accent_hover;
  vkr_ui_label(ui, string8_lit("title"), depot_string(text), &c);
}

/* The expanded row of the server the user chose to get. */
static float32_t depot_hub_dialog(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  VkrUiSystem *ui, float32_t x, float32_t y,
                                  float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t top = y;
  char error[DEPOT_TEXT] = {0};
  const bool8_t cloning =
      depot_busy(depot) && depot->job->kind == DEPOT_JOB_CLONE;
  const bool8_t waiting = depot->waiting_kind == DEPOT_JOB_CLONE;
  if (!cloning && !waiting) {
    depot_caption(ui, "get.folder", "Folder on this computer", x + 12, y,
                  width - 24);
    y += 20;
    depot_field(ui, "get.destination", depot->dialog_destination,
                sizeof(depot->dialog_destination), x + 12, y, width - 24);
    y += 34;
    VkrUiWidgetConfig sparse = depot_widget(x + 12, y, width - 24, 22);
    sparse.tooltip = string8_lit(
        "Download each file when a scene or tool first reads it, instead "
        "of the whole project now");
    (void)vkr_ui_checkbox(ui, string8_lit("get.sparse"),
                          string8_lit("Fetch files when opened"),
                          &depot->dialog_sparse, &sparse);
    y += 28;
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(depot->dialog_key, key);
    if (depot_button(editor, ui, "get.go", "Clone", x + 12, y, 140, true_v,
                     depot_busy(depot)) &&
        !vkr_editor_depot_clone(depot, depot->dialog_address, key,
                                depot->dialog_name, depot->dialog_destination,
                                depot->dialog_sparse, error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    if (depot_button(editor, ui, "get.cancel", "Cancel", x + 160, y, 100,
                     false_v, false_v)) {
      depot->dialog_open = false_v;
      depot->error[0] = '\0';
    }
    y += 36;
  } else {
    char text[DEPOT_TEXT];
    float32_t fraction = -1.0f;
    if (waiting) {
      char print[16] = "";
      VkrNetKeyPair identity;
      if (vkr_depot_identity(&identity, true_v)) {
        depot_fingerprint(identity.public_key, print);
        vkr_net_random(identity.secret_key, sizeof(identity.secret_key));
      }
      string_format(
          text, sizeof(text), "Waiting for %s to allow this computer, key %s",
          depot->dialog_host[0] ? depot->dialog_host : "the other editor",
          print);
    } else {
      fraction = depot_job_text(depot, text, sizeof(text));
    }
    VkrUiWidgetConfig c = depot_widget(x + 12, y, width - 130, 22);
    c.style.text_color = waiting ? theme->warning : theme->text;
    c.icon = waiting ? VKR_UI_ICON_CLOCK : VKR_UI_ICON_SPINNER;
    c.icon_size_pt = 13.0f;
    c.icon_color = waiting ? theme->warning : theme->accent_hover;
    vkr_ui_label(ui, string8_lit("get.state"), depot_string(text), &c);
    if (depot_button(editor, ui, "get.stop", "Cancel", x + width - 108, y - 2,
                     96, false_v, false_v)) {
      vkr_editor_depot_cancel(depot);
      depot->dialog_open = false_v;
    }
    y += 30;
    if (!waiting) {
      depot_progress_bar(ui, "get.bar", x + 12, y, width - 24, fraction);
      y += 16;
    }
  }
  if (depot->error[0]) {
    depot_label(ui, "get.error", depot->error, x + 12, y, width - 24,
                theme->warning);
    y += 26;
  }
  return y - top;
}

float32_t vkr_editor_depot_hub_build(VkrEditorDepot *depot, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame, float32_t x,
                                     float32_t y, float32_t width) {
  if (!depot) {
    return 0.0f;
  }
  if (!depot->registry_loaded) {
    depot_registry_load(depot);
  }
  depot->browse_wanted = true_v;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t top = y;
  char text[DEPOT_TEXT];

  /* Repositories on this computer, other than the open one. */
  uint32_t local = 0u;
  for (uint32_t i = 0u; i < depot->project_count; ++i) {
    local += !string_equals(depot->projects[i].path, depot->workspace);
  }
  if (local) {
    depot_heading(editor, ui, "hub.local", "Repositories on this computer",
                  VKR_UI_ICON_HARD_DRIVES, x, y, width);
    y += 30;
    for (uint32_t i = 0u; i < depot->project_count; ++i) {
      const DepotProject *project = &depot->projects[i];
      if (string_equals(project->path, depot->workspace)) {
        continue;
      }
      (void)vkr_ui_push_id_u64(ui, 1000u + i);
      depot_card(ui, "card", x, y, width, 52);
      depot_row_title(ui, project->name, VKR_UI_ICON_PROJECT, x + 12, y + 6,
                      width - 150);
      depot_caption(ui, "path", project->path, x + 36, y + 28, width - 170);
      if (depot_button(editor, ui, "open", "Open", x + width - 112, y + 13, 100,
                       false_v, depot_busy(depot))) {
        string_format(depot->open_directory, sizeof(depot->open_directory),
                      "%s", project->path);
        string_format(depot->open_project, sizeof(depot->open_project), "%s",
                      project->project_id);
      }
      (void)vkr_ui_pop_id(ui);
      y += 60;
    }
    y += 8;
  }

  /* Repositories other computers serve, to clone. */
  depot_heading(editor, ui, "hub.network", "Clone from your network",
                VKR_UI_ICON_CLOUD, x, y, width);
  y += 30;
  uint32_t shown = 0u;
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    const VkrDepotDiscovered *found = &depot->browser.found[i];
    if (depot_found_is_self(depot, found)) {
      continue;
    }
    ++shown;
    (void)vkr_ui_push_id_u64(ui, 2000u + i);
    char address[VKR_NET_ADDRESS_TEXT];
    vkr_net_address_format(&found->address, address, sizeof(address));
    depot_card(ui, "card", x, y, width, 52);
    depot_row_title(ui, found->name, VKR_UI_ICON_CLOUD, x + 12, y + 6,
                    width - 150);
    char print[16];
    depot_fingerprint(found->server_key, print);
    string_format(
        text, sizeof(text), "Served by %s  \xc2\xb7  %s  \xc2\xb7  key %s%s",
        found->host, address, print,
        depot_key_known(depot, found->server_key) ? "  \xc2\xb7  used before"
                                                  : "");
    depot_caption(ui, "host", text, x + 36, y + 28, width - 170);
    /* An answer with the name of a server used before but another key:
       discovery is not authenticated, so say it is not that computer. */
    for (uint32_t k = 0u; k < depot->known_count; ++k) {
      if (string_equals(depot->known[k].name, found->name) &&
          !depot_key_known(depot, found->server_key)) {
        VkrUiWidgetConfig warning =
            depot_widget(x + 36, y + 46, width - 170, 18);
        warning.style.font_size_pt = theme->font_caption;
        warning.style.text_color = theme->warning;
        vkr_ui_label(ui, string8_lit("impostor"),
                     string8_lit("Not the computer you cloned this project "
                                 "from before"),
                     &warning);
        y += 16;
        break;
      }
    }
    const bool8_t chosen =
        depot->dialog_open &&
        MemCompare(depot->dialog_key, found->server_key, 32u) == 0;
    if (!chosen && depot_button(editor, ui, "get", "Clone", x + width - 112,
                                y + 13, 100, true_v, depot_busy(depot))) {
      depot->dialog_open = true_v;
      depot->error[0] = '\0';
      string_format(depot->dialog_name, sizeof(depot->dialog_name), "%s",
                    found->name);
      string_format(depot->dialog_host, sizeof(depot->dialog_host), "%s",
                    found->host);
      string_format(depot->dialog_address, sizeof(depot->dialog_address), "%s",
                    address);
      MemCopy(depot->dialog_key, found->server_key, 32u);
      depot_default_destination(found->name, depot->dialog_destination,
                                sizeof(depot->dialog_destination));
    }
    y += 60;
    if (chosen) {
      y += depot_hub_dialog(depot, editor, ui, x + 24, y, width - 24) + 8;
    }
    (void)vkr_ui_pop_id(ui);
  }
  /* A Clone started by address, whose server discovery has not heard. */
  bool8_t dialog_listed = false_v;
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    dialog_listed =
        dialog_listed || MemCompare(depot->browser.found[i].server_key,
                                    depot->dialog_key, 32u) == 0;
  }
  if (depot->dialog_open && !dialog_listed) {
    ++shown;
    depot_card(ui, "dialog.card", x, y, width, 52);
    depot_row_title(ui, depot->dialog_name, VKR_UI_ICON_CLOUD, x + 12, y + 6,
                    width - 150);
    string_format(text, sizeof(text), "Served at %s", depot->dialog_address);
    depot_caption(ui, "dialog.host", text, x + 36, y + 28, width - 170);
    y += 60;
    y += depot_hub_dialog(depot, editor, ui, x + 24, y, width - 24) + 8;
  }
  if (!shown) {
    depot_caption(ui, "hub.empty",
                  "Looking for repositories served on this network...", x + 12,
                  y, width - 24);
    y += 26;
  }
  /* A server whose answers a broadcast does not bring back: endpoint
     security on some computers drops them, but not a direct probe. */
  depot_caption(ui, "hub.find.label",
                "Not listed? Enter its computer name or "
                "address:",
                x + 12, y + 6, width - 24);
  y += 28;
  const float32_t field_width = Min(width - 140.0f, 360.0f);
  depot_field(ui, "hub.find", depot->find_address, sizeof(depot->find_address),
              x + 12, y, field_width);
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "hub.find.go", "Find", x + 20 + field_width, y,
                   100, false_v, !depot->find_address[0])) {
    if (vkr_editor_depot_find(depot, depot->find_address, error,
                              sizeof(error))) {
      depot->error[0] = '\0';
    } else {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  y += 34;
  if (depot->error[0] && !depot->dialog_open) {
    depot_label(ui, "hub.error", depot->error, x + 12, y, width - 24,
                theme->warning);
    y += 26;
  }
  return y - top;
}

// =============================================================================
// Status for agents
// =============================================================================

/* The open workspace's sparse working copy and the remote that fetches its
   files (vkr_editor_depot_workspace_sparse). The VFS remote is one per
   process, so this is too. */
typedef struct DepotSparseWorkspace {
  VkrDMemory memory;
  VkrAllocator allocator;
  bool8_t memory_ready;
  VkrDepotRemote *remote;
  float64_t opened_at;
} DepotSparseWorkspace;

vkr_global DepotSparseWorkspace depot_sparse = {0};

static void depot_sparse_end(void) {
  if (depot_sparse.remote) {
    /* Reads stop asking before the remote goes. */
    (void)vkr_vfs_set_remote(NULL, NULL, NULL);
    (void)vkr_platform_environment_set(VKR_DEPOT_SPARSE_WORKSPACE_VARIABLE, "");
    log_info("Sparse working copy %s closed",
             vkr_depot_remote_directory(depot_sparse.remote));
    vkr_depot_remote_close(depot_sparse.remote);
    depot_sparse.remote = NULL;
  }
  if (depot_sparse.memory_ready) {
    vkr_dmemory_allocator_destroy(&depot_sparse.allocator);
    depot_sparse.memory_ready = false_v;
  }
}

void vkr_editor_depot_workspace_sparse(const char *directory) {
  if (depot_sparse.remote && directory &&
      string_equals(vkr_depot_remote_directory(depot_sparse.remote),
                    directory)) {
    return;
  }
  depot_sparse_end();
  char state[DEPOT_PATH];
  if (!directory || !directory[0]) {
    return;
  }
  string_format(state, sizeof(state), "%s/" VKR_DEPOT_META_DIR "/state",
                directory);
  if (!vkr_bakery_is_file(state)) {
    return;
  }
  VkrDepotWorkspace *ws = vkr_heap_alloc_zeroed(1u, sizeof(*ws));
  char root[DEPOT_PATH] = {0};
  if (ws && vkr_depot_workspace_open(ws, directory, vkr_depot_zstd_codec()) &&
      ws->sparse) {
    string_format(root, sizeof(root), "%s", ws->root);
  }
  if (ws) {
    vkr_depot_workspace_close(ws);
    vkr_heap_free(ws);
  }
  if (!root[0] || !vkr_dmemory_create(MB(1), MB(64), &depot_sparse.memory)) {
    return;
  }
  depot_sparse.allocator = (VkrAllocator){.ctx = &depot_sparse.memory};
  vkr_dmemory_allocator_create(&depot_sparse.allocator);
  depot_sparse.memory_ready = true_v;
  const VkrDepotRemoteConfig config = {.workspace = root,
                                       .codec = vkr_depot_zstd_codec()};
  char error[DEPOT_TEXT];
  depot_sparse.opened_at = vkr_platform_get_absolute_time();
  depot_sparse.remote = vkr_depot_remote_open(&depot_sparse.allocator, &config,
                                              error, sizeof(error));
  if (!depot_sparse.remote) {
    log_warn("Sparse working copy %s cannot fetch its files: %s", root, error);
    depot_sparse_end();
    return;
  }
  if (!vkr_vfs_set_remote(root, vkr_depot_remote_vfs_calls(),
                          depot_sparse.remote) ||
      !vkr_platform_environment_set(VKR_DEPOT_SPARSE_WORKSPACE_VARIABLE,
                                    root)) {
    log_warn("Sparse working copy %s cannot fetch its files", root);
    depot_sparse_end();
    return;
  }
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(depot_sparse.remote, &stats);
  log_info("Sparse working copy %s: %u files fetch when read", root,
           stats.listed);
}

bool8_t vkr_editor_depot_stream(VkrEditorDepot *depot, const char *address,
                                const char *key_hex, const char *directory,
                                const char *branch, bool8_t no_cooked,
                                char *error, uint32_t capacity) {
  uint8_t key[32] = {0};
  if (depot->stream) {
    string_format(error, capacity, "a stream is open already");
    return false_v;
  }
  if (address && !depot_key_parse(key_hex, key)) {
    string_format(error, capacity, "the server key needs 64 hex digits");
    return false_v;
  }
  if (!vkr_bakery_path_absolute(directory, depot->stream_directory,
                                sizeof(depot->stream_directory))) {
    string_format(error, capacity, "the stream directory path is too long");
    return false_v;
  }
  const VkrDepotRemoteConfig config = {
      .workspace = address ? NULL : depot->stream_directory,
      .directory = depot->stream_directory,
      .address = address,
      .server_key = key,
      .branch = branch,
      .codec = vkr_depot_zstd_codec(),
      .no_cooked = no_cooked};
  depot->stream_opened_at = vkr_platform_get_absolute_time();
  depot->stream =
      vkr_depot_remote_open(depot->allocator, &config, error, capacity);
  if (!depot->stream) {
    return false_v;
  }
  /* A working copy opened at a folder inside it streams into its root. */
  string_format(depot->stream_directory, sizeof(depot->stream_directory), "%s",
                vkr_depot_remote_directory(depot->stream));
  depot->stream_open_seconds =
      vkr_platform_get_absolute_time() - depot->stream_opened_at;
  if (!vkr_vfs_set_remote(depot->stream_directory, vkr_depot_remote_vfs_calls(),
                          depot->stream) ||
      !vkr_vfs_set_overlay_root(depot->stream_directory)) {
    (void)vkr_vfs_set_remote(NULL, NULL, NULL);
    vkr_depot_remote_close(depot->stream);
    depot->stream = NULL;
    string_format(error, capacity, "the stream directory path is too long");
    return false_v;
  }
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(depot->stream, &stats);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&stats.commit, hex);
  log_info("Streaming %u files of %.12s%s from %s into %s (listed in %.0f ms)",
           stats.listed, hex,
           vkr_depot_id_zero(&stats.cooked_commit) ? ""
                                                   : " and its cooked branch",
           address ? address : "the working copy's server",
           depot->stream_directory, depot->stream_open_seconds * 1000.0);
  return true_v;
}

VkrBakeryJson *vkr_editor_depot_stream_json(VkrEditorDepot *depot,
                                            Arena *arena) {
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  /* A stream, else the open sparse working copy's remote. */
  VkrDepotRemote *remote = depot->stream ? depot->stream : depot_sparse.remote;
  vkr_bakery_json_set(arena, result, "streaming",
                      vkr_bakery_json_bool(arena, remote != NULL));
  vkr_bakery_json_set(arena, result, "sparse",
                      vkr_bakery_json_bool(arena, !depot->stream && remote));
  if (!remote) {
    return result;
  }
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(remote, &stats);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&stats.commit, hex);
  vkr_bakery_json_set(arena, result, "commit",
                      vkr_bakery_json_cstr(arena, hex));
  vkr_depot_id_hex(&stats.cooked_commit, hex);
  vkr_bakery_json_set(
      arena, result, "cooked_commit",
      vkr_bakery_json_cstr(arena,
                           vkr_depot_id_zero(&stats.cooked_commit) ? "" : hex));
  vkr_bakery_json_set(
      arena, result, "directory",
      vkr_bakery_json_cstr(arena, vkr_depot_remote_directory(remote)));
  vkr_bakery_json_set(arena, result, "listed",
                      vkr_bakery_json_int(arena, stats.listed));
  vkr_bakery_json_set(arena, result, "present",
                      vkr_bakery_json_int(arena, stats.present));
  vkr_bakery_json_set(arena, result, "fetched",
                      vkr_bakery_json_int(arena, stats.fetched));
  vkr_bakery_json_set(arena, result, "failed",
                      vkr_bakery_json_int(arena, stats.failed));
  vkr_bakery_json_set(arena, result, "stored_bytes",
                      vkr_bakery_json_int(arena, (int64_t)stats.stored_bytes));
  vkr_bakery_json_set(arena, result, "reads",
                      vkr_bakery_json_int(arena, stats.reads));
  vkr_bakery_json_set(arena, result, "read_bytes",
                      vkr_bakery_json_int(arena, (int64_t)stats.read_bytes));
  vkr_bakery_json_set(arena, result, "prefetched",
                      vkr_bakery_json_int(arena, stats.prefetched));
  vkr_bakery_json_set(arena, result, "cancelled",
                      vkr_bakery_json_int(arena, stats.cancelled));
  vkr_bakery_json_set(
      arena, result, "wait_ms",
      vkr_bakery_json_float(arena, (float64_t)stats.wait_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "last_fetch_ms",
      vkr_bakery_json_float(arena, (float64_t)stats.last_write_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "last_read_ms",
      vkr_bakery_json_float(arena, (float64_t)stats.last_read_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "open_ms",
      vkr_bakery_json_float(
          arena, depot->stream ? depot->stream_open_seconds * 1000.0 : 0.0));
  vkr_bakery_json_set(
      arena, result, "since_open_ms",
      vkr_bakery_json_float(arena, (vkr_platform_get_absolute_time() -
                                    (depot->stream ? depot->stream_opened_at
                                                   : depot_sparse.opened_at)) *
                                       1000.0));
  return result;
}

VkrBakeryJson *vkr_editor_depot_status_json(VkrEditorDepot *depot,
                                            Arena *arena) {
  static const char *const connects[] = {"ok",     "invalid", "unreachable",
                                         "denied", "refused", "canceled"};
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "repository",
                      vkr_bakery_json_bool(arena, depot->repository));
  vkr_bakery_json_set(arena, result, "serving",
                      vkr_bakery_json_bool(arena, depot->serving));
  vkr_bakery_json_set(arena, result, "branch",
                      vkr_bakery_json_cstr(arena, depot->branch));
  vkr_bakery_json_set(arena, result, "ahead",
                      vkr_bakery_json_int(arena, depot->ahead));
  vkr_bakery_json_set(arena, result, "behind",
                      vkr_bakery_json_int(arena, depot->behind));
  VkrBakeryJson *branches = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < depot->branch_count; ++i) {
    vkr_bakery_json_append(
        branches, vkr_bakery_json_cstr(arena, depot->branches[i].name));
  }
  vkr_bakery_json_set(arena, result, "branches", branches);
  VkrBakeryJson *remotes = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < depot->remote_count; ++i) {
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(depot->remotes[i].key, key);
    vkr_bakery_json_set(arena, row, "name",
                        vkr_bakery_json_cstr(arena, depot->remotes[i].name));
    vkr_bakery_json_set(arena, row, "address",
                        vkr_bakery_json_cstr(arena, depot->remotes[i].address));
    vkr_bakery_json_set(arena, row, "key", vkr_bakery_json_cstr(arena, key));
    vkr_bakery_json_set(
        arena, row, "selected",
        vkr_bakery_json_bool(arena, i == depot->selected_remote));
    vkr_bakery_json_set(
        arena, row, "state",
        vkr_bakery_json_cstr(arena, depot_remote_state(depot, i)));
    vkr_bakery_json_append(remotes, row);
  }
  vkr_bakery_json_set(arena, result, "remotes", remotes);
  vkr_bakery_json_set(arena, result, "workspace",
                      vkr_bakery_json_cstr(arena, depot->workspace));
  vkr_bakery_json_set(arena, result, "project",
                      vkr_bakery_json_cstr(arena, depot->project_name));
  vkr_bakery_json_set(arena, result, "address",
                      vkr_bakery_json_cstr(arena, depot->address));
  char hex[VKR_DEPOT_ID_HEX];
  depot_key_hex(depot->server_key, hex);
  vkr_bakery_json_set(arena, result, "server_key",
                      vkr_bakery_json_cstr(
                          arena, depot_key_zero(depot->server_key) ? "" : hex));
  VkrNetKeyPair identity;
  if (vkr_depot_identity(&identity, true_v)) {
    depot_key_hex(identity.public_key, hex);
    vkr_net_random(identity.secret_key, sizeof(identity.secret_key));
    vkr_bakery_json_set(arena, result, "identity",
                        vkr_bakery_json_cstr(arena, hex));
  }
  vkr_depot_id_hex(&depot->base, hex);
  vkr_bakery_json_set(arena, result, "base", vkr_bakery_json_cstr(arena, hex));
  vkr_depot_id_hex(&depot->remote_head, hex);
  vkr_bakery_json_set(
      arena, result, "remote",
      vkr_bakery_json_cstr(arena, depot->remote_known ? hex : ""));
  vkr_bakery_json_set(arena, result, "incoming",
                      vkr_bakery_json_bool(arena, depot_incoming(depot)));
  vkr_bakery_json_set(arena, result, "connected",
                      vkr_bakery_json_bool(arena, depot->connected));
  vkr_bakery_json_set(
      arena, result, "connect",
      vkr_bakery_json_cstr(arena, connects[Min((uint32_t)depot->connect, 5u)]));
  vkr_bakery_json_set(
      arena, result, "waiting",
      vkr_bakery_json_bool(arena, depot->waiting_kind != DEPOT_JOB_NONE));
  VkrBakeryJson *changes = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < Min(depot->change_count, DEPOT_LIST_MAX); ++i) {
    vkr_bakery_json_append(changes,
                           vkr_bakery_json_cstr(arena, depot->changes[i].text));
  }
  vkr_bakery_json_set(arena, result, "change_count",
                      vkr_bakery_json_int(arena, depot->change_count));
  vkr_bakery_json_set(arena, result, "changes", changes);
  VkrBakeryJson *conflicts = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < Min(depot->conflict_count, DEPOT_LIST_MAX); ++i) {
    vkr_bakery_json_append(
        conflicts, vkr_bakery_json_cstr(arena, depot->conflicts[i].text));
  }
  vkr_bakery_json_set(arena, result, "conflicts", conflicts);
  VkrBakeryJson *untracked = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < Min(depot->untracked_count, DEPOT_LIST_MAX); ++i) {
    vkr_bakery_json_append(
        untracked, vkr_bakery_json_cstr(arena, depot->untracked[i].text));
  }
  vkr_bakery_json_set(arena, result, "untracked", untracked);
  VkrBakeryJson *job = vkr_bakery_json_object(arena);
  const bool8_t busy = depot_busy(depot);
  vkr_bakery_json_set(arena, job, "running", vkr_bakery_json_bool(arena, busy));
  vkr_bakery_json_set(arena, job, "step",
                      vkr_bakery_json_cstr(
                          arena, depot_job_names[busy ? depot->job->kind : 0]));
  if (busy) {
    char text[DEPOT_TEXT];
    (void)depot_job_text(depot, text, sizeof(text));
    vkr_bakery_json_set(arena, job, "progress",
                        vkr_bakery_json_cstr(arena, text));
  }
  vkr_bakery_json_set(arena, result, "job", job);
  VkrBakeryJson *textures = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, textures, "cooked",
                      vkr_bakery_json_int(arena, depot->textures_cooked));
  vkr_bakery_json_set(arena, textures, "fetched",
                      vkr_bakery_json_int(arena, depot->textures_fetched));
  vkr_bakery_json_set(arena, textures, "failed",
                      vkr_bakery_json_int(arena, depot->textures_failed));
  vkr_bakery_json_set(arena, result, "textures", textures);
  vkr_bakery_json_set(arena, result, "error",
                      vkr_bakery_json_cstr(arena, depot->error));
  if (depot->service) {
    VkrBakeryJson *server = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, server, "port",
        vkr_bakery_json_int(arena, depot->service_info.address.port));
    vkr_bakery_json_set(
        arena, server, "discoverable",
        vkr_bakery_json_bool(arena, depot->service_info.discoverable));
    vkr_bakery_json_set(arena, server, "peers",
                        vkr_bakery_json_int(arena, depot->service_info.peers));
    VkrBakeryJson *requests = vkr_bakery_json_array(arena);
    for (uint32_t i = 0u; i < depot->request_count; ++i) {
      VkrBakeryJson *row = vkr_bakery_json_object(arena);
      depot_key_hex(depot->requests[i].key, hex);
      vkr_bakery_json_set(arena, row, "name",
                          vkr_bakery_json_cstr(arena, depot->requests[i].name));
      vkr_bakery_json_set(arena, row, "key", vkr_bakery_json_cstr(arena, hex));
      vkr_bakery_json_append(requests, row);
    }
    vkr_bakery_json_set(arena, server, "requests", requests);
    VkrBakeryJson *access = vkr_bakery_json_array(arena);
    for (uint32_t i = 0u; i < depot->access_count; ++i) {
      VkrBakeryJson *row = vkr_bakery_json_object(arena);
      depot_key_hex(depot->access[i].key, hex);
      vkr_bakery_json_set(arena, row, "key", vkr_bakery_json_cstr(arena, hex));
      vkr_bakery_json_set(
          arena, row, "role",
          vkr_bakery_json_cstr(arena, depot_role_name(depot->access[i].role)));
      vkr_bakery_json_append(access, row);
    }
    vkr_bakery_json_set(arena, server, "access", access);
    vkr_bakery_json_set(arena, result, "server", server);
  }
  VkrBakeryJson *found = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    const VkrDepotDiscovered *entry = &depot->browser.found[i];
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    char address[VKR_NET_ADDRESS_TEXT];
    vkr_net_address_format(&entry->address, address, sizeof(address));
    depot_key_hex(entry->server_key, hex);
    vkr_bakery_json_set(arena, row, "name",
                        vkr_bakery_json_cstr(arena, entry->name));
    vkr_bakery_json_set(arena, row, "host",
                        vkr_bakery_json_cstr(arena, entry->host));
    vkr_bakery_json_set(arena, row, "address",
                        vkr_bakery_json_cstr(arena, address));
    vkr_bakery_json_set(arena, row, "key", vkr_bakery_json_cstr(arena, hex));
    vkr_bakery_json_set(
        arena, row, "self",
        vkr_bakery_json_bool(arena, depot_found_is_self(depot, entry)));
    vkr_bakery_json_append(found, row);
  }
  vkr_bakery_json_set(arena, result, "found", found);
  return result;
}

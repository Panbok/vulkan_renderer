#include "editor_depot.h"

#include "editor_install.h"
#include "editor_internal.h"
#include "editor_material.h"
#include "editor_project_store.h"
#include "editor_projects.h"
#include "editor_scripts.h"
#include "editor_table.h"

#include "containers/vkr_sort.h"
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
/* Commits the History lists: both sides of the branch, newest first. */
#define DEPOT_HISTORY_MAX 200u
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
  DEPOT_JOB_SERVICE,
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
                                              "Switching branch",
                                              "Starting the depot service"};

/* Branches the window lists. */
#define DEPOT_BRANCHES_MAX 16u

typedef struct DepotText {
  char text[DEPOT_TEXT];
} DepotText;

/* Where a listed commit stands against the branch and the remote's. */
typedef enum DepotCommitFlag {
  /* Only the branch has it: a push sends it. */
  DEPOT_COMMIT_TO_PUSH = 1u << 0,
  /* Only the remote's branch has it: a pull brings it. */
  DEPOT_COMMIT_TO_PULL = 1u << 1,
  /* The branch, or the remote's branch as last fetched, points at it. */
  DEPOT_COMMIT_HEAD = 1u << 2,
  DEPOT_COMMIT_REMOTE_HEAD = 1u << 3,
  /* This user's identity made it. */
  DEPOT_COMMIT_MINE = 1u << 4,
} DepotCommitFlag;

typedef struct DepotHistory {
  char id[VKR_DEPOT_ID_HEX];
  /* The message's first line. */
  char message[120];
  uint64_t time_us;
  uint8_t author[32];
  uint32_t parent_count;
  uint32_t flags;
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
  /* The remote a step exchanges with, and its address, key and repository
     for the cooked side branch; empty for none. */
  char remote[VKR_DEPOT_REMOTE_NAME];
  char address[DEPOT_TEXT];
  uint8_t server_key[32];
  uint8_t repository[VKR_DEPOT_REPOSITORY_ID_SIZE];
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
  /* Status: the remote takes no connections, so the check reads its branch
     as last fetched instead of waiting out a try. */
  bool8_t skip_fetch;
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
  /* Commits other computers pushed to this store's branch that the working
     copy has not taken. */
  uint32_t pushed;
  uint32_t branch_count;
  DepotBranch branches[DEPOT_BRANCHES_MAX];
  /* The store the working copy opened, for the History's details. */
  char store_root[DEPOT_PATH];
  /* The branch's commit, and the commits of both sides, newest first. */
  VkrDepotId head;
  uint32_t history_count;
  DepotHistory history[DEPOT_HISTORY_MAX];
  char opened_project[40];
  /* Status: this computer's depot service as it stands (heap, about
     100 KB, kept with the job), and the store's access list. */
  VkrDepotServiceStatus *machine;
  bool8_t machine_read;
  uint32_t access_count;
  VkrDepotAccessEntry access[DEPOT_ACCESS_MAX];
  VkrDepotProgress progress;
  /* The texture cook that follows a pull, after the depot's phases. */
  VkrAtomicUint32 cook_done;
  VkrAtomicUint32 cook_total;
} DepotJob;

/* Changed files the details of one commit list, and its message. */
#define DEPOT_DETAIL_FILES_MAX 512u
#define DEPOT_DETAIL_MESSAGE 2048u

typedef struct DepotDetailFile {
  char path[DEPOT_TEXT];
  VkrDepotChange change;
} DepotDetailFile;

/* The details of the commit the History selected, read on a thread of
   their own, so a long step does not hold them up; commits are immutable,
   so reading the store beside a step is safe. The UI thread fills the
   inputs, starts it and reads the outputs once `done` is set. */
typedef struct DepotInspect {
  VkrThread thread;
  bool8_t running;
  VkrAtomicBool done;
  /* Inputs. */
  char store_root[DEPOT_PATH];
  VkrDepotId commit;
  /* Outputs: the whole message, the parents, and the files that differ
     from the first parent (every file of a first commit). */
  bool8_t ok;
  char error[DEPOT_TEXT];
  char message[DEPOT_DETAIL_MESSAGE];
  VkrDepotId parents[VKR_DEPOT_PARENTS_MAX];
  uint32_t parent_count;
  uint32_t file_count;
  uint32_t added;
  uint32_t modified;
  uint32_t deleted;
  uint32_t listed;
  DepotDetailFile files[DEPOT_DETAIL_FILES_MAX];
} DepotInspect;

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
  /* The workspace is a depot working copy, and this computer's depot
     service serves its store (ADR-134). */
  bool8_t repository;
  bool8_t serving;
  /* The store holds the server key an editor that served it itself left,
     and the service does not serve it. */
  bool8_t served_before;
  /* The store's remotes and the one steps use: origin, else the first. */
  VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
  uint32_t remote_count;
  uint32_t selected_remote;
  /* What each remote's last exchange gave, by index. */
  VkrDepotConnectStatus remote_connect[VKR_DEPOT_REMOTES_MAX];
  bool8_t remote_tried[VKR_DEPOT_REMOTES_MAX];
  /* A peer that said it takes no connections from outside last connected
     to the machine's service from the remote's host, and when (Unix
     microseconds): the remote is reachable only while it connects. */
  bool8_t remote_outbound[VKR_DEPOT_REMOTES_MAX];
  uint64_t remote_seen_us[VKR_DEPOT_REMOTES_MAX];
  /* The machine's service over its loopback, for the cooked side branch of
     a served repository, and the store's repository ID it selects. */
  char address[DEPOT_TEXT];
  uint8_t server_key[32];
  uint8_t repository_id[VKR_DEPOT_REPOSITORY_ID_SIZE];

  /* This computer's depot service as the last status check read it: it
     runs apart from the editor and serves every shared repository. */
  bool8_t machine_known;
  bool8_t machine_running;
  bool8_t machine_outbound_only;
  /* Status checks in a row that found the service stopped while it serves
     the repository, and whether the editor started it once for this
     workspace. */
  uint32_t machine_stopped_checks;
  bool8_t machine_started;
  uint8_t machine_key[32];
  uint16_t machine_port;
  /* Keys connected to it now. */
  uint32_t machine_connected;
  /* Keys it refused for want of a role, but those this user denied. */
  VkrDepotAccessRequest requests[DEPOT_REQUESTS_MAX];
  uint32_t request_count;
  /* Denied in this run of the editor; the service keeps a refused key's
     request until it restarts. */
  uint8_t denied[DEPOT_REQUESTS_MAX][32];
  uint32_t denied_count;
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
  uint32_t pushed;
  uint32_t branch_count;
  DepotBranch branches[DEPOT_BRANCHES_MAX];
  uint32_t history_count;
  DepotHistory history[DEPOT_HISTORY_MAX];
  char store_root[DEPOT_PATH];
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
  /* The repository of that service to clone; zero for its default. */
  uint8_t dialog_repository[VKR_DEPOT_REPOSITORY_ID_SIZE];
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
  /* The window's tab, its tables, the History's split between commits and
     details, and the commit whose details it shows. */
  uint32_t tab;
  VkrEditorTable changes_table;
  /* One per column set the History's width picks (DEPOT_COMMIT_LAYOUTS). */
  VkrEditorTable history_tables[3];
  VkrEditorTable files_table;
  VkrEditorSplit history_split;
  char selected_commit[VKR_DEPOT_ID_HEX];
  /* Made on the first selection. */
  DepotInspect *inspect;
  /* The toolbar's branch and remote lists are open. */
  bool8_t branch_menu_open;
  bool8_t remote_menu_open;
  /* The height the lists and cards under the toolbar took last build. */
  float32_t top_height;
  /* Hover text the UI reads after the build. */
  char remote_tip[DEPOT_TEXT];
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

/* A repository ID in hexadecimal; empty or NULL is the service's default,
   zero. */
static bool8_t
depot_repository_parse(const char *hex,
                       uint8_t out[VKR_DEPOT_REPOSITORY_ID_SIZE]) {
  MemZero(out, VKR_DEPOT_REPOSITORY_ID_SIZE);
  if (!hex || !hex[0]) {
    return true_v;
  }
  if (string_length(hex) != VKR_DEPOT_REPOSITORY_ID_SIZE * 2u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < VKR_DEPOT_REPOSITORY_ID_SIZE * 2u; ++i) {
    const char c = hex[i];
    const int32_t digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
    if (digit < 0) {
      return false_v;
    }
    out[i / 2u] = (uint8_t)(i % 2u ? (out[i / 2u] | digit) : digit << 4);
  }
  return true_v;
}

static void
depot_repository_hex(const uint8_t id[VKR_DEPOT_REPOSITORY_ID_SIZE],
                     char out[VKR_DEPOT_REPOSITORY_ID_SIZE * 2u + 1u]) {
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0u; i < VKR_DEPOT_REPOSITORY_ID_SIZE; ++i) {
    out[i * 2u] = digits[id[i] >> 4];
    out[i * 2u + 1u] = digits[id[i] & 15u];
  }
  out[VKR_DEPOT_REPOSITORY_ID_SIZE * 2u] = '\0';
}

static bool8_t depot_key_zero(const uint8_t key[32]) {
  for (uint32_t i = 0u; i < 32u; ++i) {
    if (key[i]) {
      return false_v;
    }
  }
  return true_v;
}

/* `<projects root>/<name>` (ADR-110), made unique with a number when
   taken; `.` when the projects folder is unknown. */
static void depot_default_destination(const char *name, char *out,
                                      uint32_t capacity) {
  char folder[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  VkrEditorProjectError error = {0};
  if (!vkr_editor_projects_new_folder(NULL, name, folder, &error)) {
    log_warn("Source control: %s", error.message);
    string_format(folder, sizeof(folder), ".");
  }
  string_format(out, capacity, "%s", folder);
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

/* A message from Bakery, cut to `capacity`; empty when absent. */
static void depot_json_message(const VkrBakeryJson *object, const char *key,
                               char *out, uint32_t capacity) {
  String8 value = {0};
  out[0] = '\0';
  if (object && vkr_bakery_json_get_string(object, key, &value)) {
    const uint64_t length = Min(value.length, (uint64_t)capacity - 1u);
    MemCopy(out, value.str, length);
    out[length] = '\0';
  }
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
      .repository = job->repository,
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

/* The worker's view of a history walk: the user's key marks their own
   commits. */
typedef struct DepotHistoryWalk {
  DepotJob *job;
  uint8_t self_key[32];
  bool8_t self_known;
} DepotHistoryWalk;

static bool8_t depot_history_visit(void *context,
                                   const VkrDepotLogEntry *entry) {
  DepotHistoryWalk *walk = context;
  DepotJob *job = walk->job;
  if (job->history_count >= DEPOT_HISTORY_MAX) {
    return false_v;
  }
  DepotHistory *row = &job->history[job->history_count++];
  vkr_depot_id_hex(&entry->id, row->id);
  uint32_t length =
      Min(entry->message_size, (uint32_t)sizeof(row->message) - 1u);
  for (uint32_t i = 0u; i < length; ++i) {
    if (entry->message[i] == '\n' || entry->message[i] == '\r') {
      length = i;
      break;
    }
  }
  MemCopy(row->message, entry->message, length);
  row->message[length] = '\0';
  row->time_us = entry->time_us;
  MemCopy(row->author, entry->author, sizeof(row->author));
  row->parent_count = entry->parent_count;
  row->flags = 0u;
  /* Bit 0: the branch reaches it; bit 1: the remote's branch does. */
  if (job->remote_known && entry->reach == 1u) {
    row->flags |= DEPOT_COMMIT_TO_PUSH;
  }
  if (job->remote_known && entry->reach == 2u) {
    row->flags |= DEPOT_COMMIT_TO_PULL;
  }
  if (vkr_depot_id_equal(&entry->id, &job->head)) {
    row->flags |= DEPOT_COMMIT_HEAD;
  }
  if (job->remote_known && vkr_depot_id_equal(&entry->id, &job->remote_head)) {
    row->flags |= DEPOT_COMMIT_REMOTE_HEAD;
  }
  if (walk->self_known &&
      MemCompare(entry->author, walk->self_key, sizeof(walk->self_key)) == 0) {
    row->flags |= DEPOT_COMMIT_MINE;
  }
  return true_v;
}

/* The commits of the branch and of the remote's branch as last fetched,
   merged lines included, newest first. Without depot_job_summary before
   it (a clone), the branch is the working copy's base. */
static void depot_job_history(DepotJob *job, const VkrDepotWorkspace *ws) {
  job->history_count = 0u;
  if (vkr_depot_id_zero(&job->head)) {
    job->head = ws->base;
  }
  DepotHistoryWalk walk = {.job = job};
  VkrNetKeyPair identity;
  if (vkr_depot_identity(&identity, false_v)) {
    MemCopy(walk.self_key, identity.public_key, sizeof(walk.self_key));
    walk.self_known = true_v;
    vkr_net_random(identity.secret_key, sizeof(identity.secret_key));
  }
  const VkrDepotId tips[] = {job->head, job->remote_head};
  (void)vkr_depot_log_graph(&ws->store, tips, job->remote_known ? 2u : 1u,
                            DEPOT_HISTORY_MAX, depot_history_visit, &walk);
}

static bool8_t depot_branch_visit(void *context, const char *branch) {
  DepotJob *job = context;
  if (job->branch_count < DEPOT_BRANCHES_MAX) {
    string_format(job->branches[job->branch_count++].name,
                  VKR_DEPOT_BRANCH_NAME, "%s", branch);
  }
  return true_v;
}

/* The branch, the store's branches, and how the branch stands against the
   remote's as last fetched: how many commits to push and to pull. */
static void depot_job_summary(DepotJob *job, const VkrDepotWorkspace *ws) {
  string_format(job->current_branch, sizeof(job->current_branch), "%s",
                ws->branch);
  job->branch_count = 0u;
  (void)vkr_depot_ref_list(ws->store.root, depot_branch_visit, job);
  job->ahead = 0u;
  job->behind = 0u;
  job->pushed = 0u;
  job->remote_known = false_v;
  if (!vkr_depot_ref_read(ws->store.root, ws->branch, &job->head)) {
    job->head = ws->base;
  }
  if (!vkr_depot_id_equal(&job->head, &ws->base)) {
    uint32_t unused = 0u;
    (void)vkr_depot_ahead_behind(&ws->store, &ws->base, &job->head, &unused,
                                 &job->pushed);
  }
  if (!job->remote[0] ||
      !vkr_depot_tracking_read(ws->store.root, job->remote, ws->branch,
                               &job->remote_head)) {
    return;
  }
  job->remote_known = true_v;
  (void)vkr_depot_ahead_behind(&ws->store, &job->head, &job->remote_head,
                               &job->ahead, &job->behind);
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

/* Other computers push to a served store, which moves its branch past the
   working copy's base; merges the branch into the working copy, as
   `vkr_depot merge <branch>` does. True when there was nothing to take or
   the merge settled; false with the error, or the conflicts listed. */
static bool8_t depot_job_take_pushed(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  VkrDepotId branch_at;
  if (ws->sparse ||
      !vkr_depot_ref_read(ws->store.root, ws->branch, &branch_at) ||
      vkr_depot_id_equal(&branch_at, &ws->base)) {
    job->ok = true_v;
    return true_v;
  }
  VkrDepotFileList before = depot_index_copy(ws);
  char bakery[DEPOT_PATH];
  string_format(bakery, sizeof(bakery), "%s",
                vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY));
  char message[VKR_DEPOT_BRANCH_NAME + 8u];
  string_format(message, sizeof(message), "Merge %s", ws->branch);
  const VkrDepotMergeConfig config = {
      .driver = vkr_depot_merge_bakery_driver,
      .driver_context = bakery,
      .policy = job->policy,
      .message = message,
  };
  VkrDepotMergeResult merged;
  job->ok = vkr_depot_merge(ws, &branch_at, &config, &merged, job->error,
                            sizeof(job->error));
  depot_job_take_changes(job, &merged.conflicts, job->conflicts,
                         &job->conflict_count);
  if (job->ok && merged.status == VKR_DEPOT_MERGE_CONFLICTS) {
    job->ok = false_v;
    string_format(job->error, sizeof(job->error),
                  "%u files changed here and in commits pushed to this "
                  "repository; choose whose to keep",
                  merged.conflicts.count);
  } else if (job->ok) {
    job->merged = merged.status != VKR_DEPOT_MERGE_UP_TO_DATE;
  }
  vkr_depot_change_list_free(&merged.conflicts);
  if (job->ok) {
    job->conflict_count = 0u;
    depot_job_after_checkout(worker, &before);
  }
  vkr_depot_file_list_free(&before);
  return job->ok;
}

/* Pulls the branch from the remote: a fast-forward, or a merge when both
   sides moved it, whose scene units Bakery merges. Conflicts it could not
   settle under the job's policy stop it with them listed. Commits pushed to
   this computer's store come first; a served copy without a remote takes
   only those. */
static void depot_job_pull(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  if (!depot_job_take_pushed(worker) || !job->remote[0]) {
    return;
  }
  const bool8_t took_pushed = job->merged;
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
    job->merged = took_pushed || merged.status == VKR_DEPOT_MERGE_MERGED;
  } else if (job->ok) {
    job->merged = took_pushed || pulled.status == VKR_DEPOT_PULL_FAST_FORWARD;
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
static void depot_job_rebuild_old_models(DepotJob *job, const char *directory,
                                         const char *project_id,
                                         const VkrBakeryJson *share_result,
                                         int64_t grace_seconds);
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
    /* A model cooked without texture recipes leaves another computer
       unable to cook its textures: rebuild it before it is committed. */
    depot_job_rebuild_old_models(job, folder, project_id, result, -1);
    if (job->untracked_count && !job->adopt && !job->ignore_untracked) {
      string_format(job->error, sizeof(job->error),
                    "%u files this project uses come from outside it; "
                    "another computer lacks them",
                    job->untracked_count);
      proceed = false_v;
    } else if (job->adopt || job->ignore_untracked) {
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
  if (!ws->sparse && !depot_job_untracked(job, ws->root)) {
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
  if (job->remote[0] && !job->skip_fetch &&
      !vkr_depot_fetch(&worker->allocator, ws, job->remote, NULL,
                       depot_job_fetch_visit, NULL, &job->connect, job->error,
                       sizeof(job->error))) {
    log_info("Source control: %s: %s", job->remote, job->error);
    job->error[0] = '\0';
  }
  /* The machine's service answers discovery on loopback at once when it
     runs. */
  job->machine_read =
      job->machine && vkr_depot_service_status(job->machine, 300u);
  job->access_count =
      vkr_depot_access_list(ws->store.root, job->access, DEPOT_ACCESS_MAX);
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
      .repository = job->repository,
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
          depot_json_message(report, "prepare_error", reason, sizeof(reason));
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

/* Rebuilds each imported model Bakery reported as cooked without texture
   recipes (`needs_rebuild` of share_project or untracked_dependencies) in
   the workspace at `directory`, so another computer can cook its textures
   (rebuild_asset), then removes the builds the rebuilds replaced after
   `grace_seconds` (-1 for collect_garbage's default). */
static void depot_job_rebuild_old_models(DepotJob *job, const char *directory,
                                         const char *project_id,
                                         const VkrBakeryJson *share_result,
                                         int64_t grace_seconds) {
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
                directory, project_id);
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
      if (!depot_project_job(job, directory, arena, request,
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
  if (grace_seconds >= 0) {
    vkr_bakery_json_set(arena, request, "grace_seconds",
                        vkr_bakery_json_int(arena, grace_seconds));
  }
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  const VkrBakeryJson *result = NULL;
  if (!vkr_depot_progress_canceled(&job->progress) &&
      !depot_project_job(job, directory, arena, request,
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
   recipes, makes it a repository and commits it. With `serve`, the
   machine's depot service serves it once the step finishes. */
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
  /* No other job writes the new workspace yet, so nothing needs the grace
     period that protects builds a running job has not recorded. */
  depot_job_rebuild_old_models(job, job->directory, job->project_id, result, 0);
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
}

/* Installs and starts this computer's depot service with the vkr_depot
   beside the editor: `service install` keeps the settings it finds, so
   the stores it serves and its outbound-only choice stay. */
static void depot_job_service(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotServicePaths paths;
  char log[DEPOT_PATH];
  if (!vkr_depot_service_paths(&paths) ||
      !vkr_bakery_make_directories(paths.folder) ||
      !vkr_bakery_path_join(log, sizeof(log), paths.folder, "install.log")) {
    string_format(job->error, sizeof(job->error),
                  "The depot service's folder could not be made");
    return;
  }
  const char *arguments[] = {"service", "install"};
  const VkrPlatformProcessConfig config = {
      .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_DEPOT),
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path = log,
      .stderr_path = log,
      .timeout_ms = 120000u,
      .hidden = true_v,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  job->ok = vkr_platform_process_run(&config, &exit_code, &timed_out) &&
            !timed_out && exit_code == 0;
  if (!job->ok) {
    /* The command's last line says why. */
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    char reason[DEPOT_TEXT] = {0};
    if (vkr_bakery_read_file(log, KB(64), &bytes, &size) && size) {
      uint64_t end = size;
      while (end && (bytes[end - 1u] == '\n' || bytes[end - 1u] == '\r')) {
        --end;
      }
      uint64_t start = end;
      while (start && bytes[start - 1u] != '\n') {
        --start;
      }
      const uint64_t length = Min(end - start, (uint64_t)sizeof(reason) - 1u);
      MemCopy(reason, bytes + start, length);
      reason[length] = '\0';
    }
    vkr_heap_free(bytes);
    string_format(job->error, sizeof(job->error),
                  "The depot service did not start%s%s", reason[0] ? ": " : "",
                  reason);
  }
  (void)vkr_bakery_remove_file(log);
}

// =============================================================================
// Commit details
// =============================================================================

static bool8_t depot_inspect_visit(void *context, const char *path,
                                   VkrDepotChange change) {
  DepotInspect *inspect = context;
  inspect->file_count += 1u;
  inspect->added += change == VKR_DEPOT_CHANGE_ADDED;
  inspect->modified += change == VKR_DEPOT_CHANGE_MODIFIED;
  inspect->deleted += change == VKR_DEPOT_CHANGE_DELETED;
  if (inspect->listed < DEPOT_DETAIL_FILES_MAX) {
    DepotDetailFile *file = &inspect->files[inspect->listed++];
    string_format(file->path, sizeof(file->path), "%s", path);
    file->change = change;
  }
  return true_v;
}

/* The whole message and the parents of the one commit vkr_depot_log
   visits. */
static bool8_t depot_inspect_commit(void *context,
                                    const VkrDepotLogEntry *entry) {
  DepotInspect *inspect = context;
  const uint32_t length =
      Min(entry->message_size, (uint32_t)sizeof(inspect->message) - 1u);
  MemCopy(inspect->message, entry->message, length);
  inspect->message[length] = '\0';
  inspect->parent_count = Min(entry->parent_count, VKR_DEPOT_PARENTS_MAX);
  MemCopy(inspect->parents, entry->parents,
          inspect->parent_count * sizeof(VkrDepotId));
  return false_v;
}

static void *depot_inspect_worker(void *argument) {
  DepotInspect *inspect = argument;
  inspect->ok = false_v;
  inspect->error[0] = '\0';
  inspect->message[0] = '\0';
  inspect->parent_count = 0u;
  inspect->file_count = 0u;
  inspect->added = inspect->modified = inspect->deleted = 0u;
  inspect->listed = 0u;
  VkrDepotStore store;
  if (!vkr_depot_store_open(&store, inspect->store_root,
                            vkr_depot_zstd_codec()) ||
      vkr_depot_log(&store, &inspect->commit, 1u, depot_inspect_commit,
                    inspect) != 1u) {
    string_format(inspect->error, sizeof(inspect->error),
                  "This repository does not hold the commit");
  } else if (!vkr_depot_commit_changes(&store, &inspect->commit,
                                       depot_inspect_visit, inspect)) {
    string_format(inspect->error, sizeof(inspect->error),
                  "This repository does not hold the commit's files");
  } else {
    inspect->ok = true_v;
  }
  vkr_atomic_bool_store(&inspect->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
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
  } else if (job->kind == DEPOT_JOB_SERVICE) {
    depot_job_service(&worker);
  } else if (!vkr_depot_workspace_open(worker.workspace, job->directory,
                                       vkr_depot_zstd_codec())) {
    string_format(job->error, sizeof(job->error), "No repository at %s",
                  job->directory);
  } else {
    VkrDepotWorkspace *ws = worker.workspace;
    ws->progress = &job->progress;
    string_format(job->store_root, sizeof(job->store_root), "%s",
                  ws->store.root);
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
      if (job->ok && job->remote[0]) {
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
  MemZero(&job->head, sizeof(job->head));
  job->ahead = 0u;
  job->behind = 0u;
  job->textures_cooked = 0u;
  job->textures_failed = 0u;
  job->textures_fetched = 0u;
  job->opened_project[0] = '\0';
  job->store_root[0] = '\0';
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
   with, or, for a served repository with none, the machine's service for
   the cooked side branch. */
static void depot_job_inputs(VkrEditorDepot *depot, const char *remote) {
  DepotJob *job = depot->job;
  string_format(job->directory, sizeof(job->directory), "%s", depot->workspace);
  const VkrDepotRemoteEntry *entry = depot_remote_entry(depot, remote);
  job->remote[0] = '\0';
  job->address[0] = '\0';
  job->skip_fetch = false_v;
  MemZero(job->server_key, sizeof(job->server_key));
  MemZero(job->repository, sizeof(job->repository));
  if (entry) {
    string_format(job->remote, sizeof(job->remote), "%s", entry->name);
    string_format(job->address, sizeof(job->address), "%s", entry->address);
    MemCopy(job->server_key, entry->key, 32u);
    MemCopy(job->repository, entry->repository, sizeof(job->repository));
  } else if (depot->serving && depot->address[0]) {
    string_format(job->address, sizeof(job->address), "%s", depot->address);
    MemCopy(job->server_key, depot->server_key, 32u);
    MemCopy(job->repository, depot->repository_id, sizeof(job->repository));
  }
}

// =============================================================================
// Server
// =============================================================================

/* The store the working copy's `store-root` names, which a clone with
   `--store` keeps outside it; its own folder's store without one. */
static void depot_store_root(const char *workspace, char *out,
                             uint32_t capacity) {
  char link[DEPOT_PATH];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  string_format(link, sizeof(link), "%s/" VKR_DEPOT_META_DIR "/store-root",
                workspace);
  if (vkr_bakery_is_file(link) &&
      vkr_bakery_read_file(link, DEPOT_PATH - 1u, &bytes, &size)) {
    while (size && (bytes[size - 1u] == '\n' || bytes[size - 1u] == '\r')) {
      --size;
    }
  }
  if (size) {
    string_format(out, capacity, "%.*s", (int32_t)size, (const char *)bytes);
  } else {
    string_format(out, capacity, "%s/" VKR_DEPOT_META_DIR "/store", workspace);
  }
  vkr_heap_free(bytes);
}

/* Forgets what the last status check read of the machine's service. */
static void depot_machine_reset(VkrEditorDepot *depot) {
  depot->machine_known = false_v;
  depot->machine_running = false_v;
  depot->machine_outbound_only = false_v;
  depot->machine_stopped_checks = 0u;
  depot->machine_started = false_v;
  MemZero(depot->machine_key, sizeof(depot->machine_key));
  depot->machine_port = 0u;
  depot->machine_connected = 0u;
  depot->request_count = 0u;
  depot->access_count = 0u;
  depot->address[0] = '\0';
  MemZero(depot->server_key, sizeof(depot->server_key));
}

/* Reads the machine service's settings: whether it serves `store`. */
static bool8_t depot_machine_serves(const char *store) {
  VkrDepotServicePaths paths;
  VkrDepotServiceSettings *settings =
      vkr_heap_alloc_zeroed(1u, sizeof(*settings));
  const bool8_t serves = settings && vkr_depot_service_paths(&paths) &&
                         vkr_depot_service_settings_load(&paths, settings) &&
                         vkr_depot_service_find(settings, store) >= 0;
  vkr_heap_free(settings);
  return serves;
}

static bool8_t depot_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static bool8_t depot_machine_name_taken(const VkrDepotServiceSettings *settings,
                                        const char *name) {
  for (uint32_t i = 0u; i < settings->store_count; ++i) {
    if (string_equals(settings->stores[i].name, name)) {
      return true_v;
    }
  }
  return false_v;
}

/* The name discovery lists the project under: the project's name in the
   letters a store name takes, numbered when another store has it. */
static void depot_machine_name(const VkrDepotServiceSettings *settings,
                               const char *project, char *out,
                               uint32_t capacity) {
  char base[VKR_DEPOT_BRANCH_NAME - 4u];
  uint32_t length = 0u;
  for (const char *c = project; *c && length + 1u < sizeof(base); ++c) {
    base[length++] = depot_name_char(*c) ? *c : '-';
  }
  base[length] = '\0';
  if (!length) {
    string_format(base, sizeof(base), "project");
  } else if (base[0] == '.') {
    base[0] = '-';
  }
  string_format(out, capacity, "%s", base);
  for (uint32_t n = 2u; n < 100u && depot_machine_name_taken(settings, out);
       ++n) {
    string_format(out, capacity, "%s-%u", base, n);
  }
}

/* Adds `store` to the stores the machine's service serves, or removes it;
   a running service follows within a second. */
static bool8_t depot_machine_set(const char *store, const char *project,
                                 bool8_t serve, char *error,
                                 uint32_t capacity) {
  VkrDepotServicePaths paths;
  VkrDepotServiceSettings *settings =
      vkr_heap_alloc_zeroed(1u, sizeof(*settings));
  if (!settings || !vkr_depot_service_paths(&paths) ||
      !vkr_depot_service_settings_load(&paths, settings)) {
    vkr_heap_free(settings);
    string_format(error, capacity,
                  "The depot service's settings could not be read");
    return false_v;
  }
  const bool8_t listed = vkr_depot_service_find(settings, store) >= 0;
  char message[DEPOT_TEXT] = {0};
  bool8_t ok = true_v;
  if (serve && !listed) {
    char name[VKR_DEPOT_BRANCH_NAME];
    depot_machine_name(settings, project, name, sizeof(name));
    ok = vkr_depot_service_add(&paths, settings, store, name, message,
                               sizeof(message));
  } else if (!serve && listed) {
    ok = vkr_depot_service_remove(&paths, settings, store, message,
                                  sizeof(message));
  }
  vkr_heap_free(settings);
  if (!ok) {
    string_format(error, capacity, "%s", message);
  } else if (message[0]) {
    log_info("Source control: depot service: %s", message);
  }
  return ok;
}

static bool8_t depot_same_host(const VkrNetAddress *a, const VkrNetAddress *b) {
  VkrNetAddress x = *a;
  VkrNetAddress y = *b;
  x.port = 0u;
  y.port = 0u;
  return vkr_net_address_equal(&x, &y);
}

/* Marks each remote at a numeric address that an outbound-only peer of the
   machine's service last connected from; a host name is not looked up on
   this thread. */
static void depot_match_outbound(VkrEditorDepot *depot,
                                 const VkrDepotServiceStatus *machine) {
  for (uint32_t r = 0u; r < depot->remote_count; ++r) {
    depot->remote_outbound[r] = false_v;
    depot->remote_seen_us[r] = 0u;
    VkrNetAddress host;
    if (!vkr_net_address_parse(depot->remotes[r].address, 0u, &host)) {
      continue;
    }
    for (uint32_t i = 0u; i < machine->known_peer_count; ++i) {
      const VkrDepotServerPeer *peer = &machine->known_peers[i];
      if (peer->outbound_only && depot_same_host(&peer->address, &host) &&
          peer->last_seen_us > depot->remote_seen_us[r]) {
        depot->remote_outbound[r] = true_v;
        depot->remote_seen_us[r] = peer->last_seen_us;
      }
    }
  }
}

/* Takes what a status check read of the machine's service: whether it
   runs and serves the store, its loopback address for the cooked side
   branch, and the keys asking for access. */
static void depot_take_machine(VkrEditorDepot *depot,
                               const VkrDepotServiceStatus *machine) {
  char store[DEPOT_PATH];
  depot_store_root(depot->workspace, store, sizeof(store));
  depot->machine_known = true_v;
  depot->machine_running = machine->running;
  depot->machine_outbound_only = machine->settings.outbound_only;
  depot->serving = vkr_depot_service_find(&machine->settings, store) >= 0;
  MemCopy(depot->machine_key,
          machine->has_key ? machine->key : machine->answer.server_key, 32u);
  depot->machine_port = machine->running ? machine->answer.address.port : 0u;
  depot->machine_connected = 0u;
  for (uint32_t i = 0u; i < machine->known_peer_count; ++i) {
    depot->machine_connected += machine->known_peers[i].connections ? 1u : 0u;
  }
  /* The service publishes every 5 s, so a key just allowed may still be
     listed; one with a role in the store's access list asks no more. */
  depot->request_count = 0u;
  for (uint32_t i = 0u; i < machine->request_count; ++i) {
    const uint8_t *key = machine->requests[i].key;
    bool8_t answered = false_v;
    for (uint32_t d = 0u; d < depot->denied_count; ++d) {
      answered = answered || MemCompare(depot->denied[d], key, 32u) == 0;
    }
    for (uint32_t a = 0u; a < depot->access_count; ++a) {
      answered = answered || MemCompare(depot->access[a].key, key, 32u) == 0;
    }
    if (!answered && depot->request_count < DEPOT_REQUESTS_MAX) {
      depot->requests[depot->request_count++] = machine->requests[i];
    }
  }
  depot_match_outbound(depot, machine);
  depot->address[0] = '\0';
  MemZero(depot->server_key, sizeof(depot->server_key));
  if (depot->serving && machine->running) {
    vkr_net_address_format(&machine->answer.address, depot->address,
                           sizeof(depot->address));
    MemCopy(depot->server_key, depot->machine_key, 32u);
  }
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
  depot->pushed = 0u;
  depot->branch_count = 0u;
  depot->branch[0] = '\0';
  depot->error[0] = '\0';
  depot->store_root[0] = '\0';
  depot->selected_commit[0] = '\0';
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
   is a repository, which the machine's depot service may serve. */
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
  depot_machine_reset(depot);
  depot_reset_status(depot);
  string_format(depot->workspace, sizeof(depot->workspace), "%s", folder);
  depot->repository = false_v;
  depot->serving = false_v;
  depot->served_before = false_v;
  depot->remote_count = 0u;
  depot->selected_remote = 0u;
  MemZero(depot->repository_id, sizeof(depot->repository_id));
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
  char store[DEPOT_PATH];
  char key_path[DEPOT_PATH];
  depot_store_root(folder, store, sizeof(store));
  string_format(key_path, sizeof(key_path), "%s/server.key", store);
  (void)vkr_depot_repository_id(store, depot->repository_id, false_v);
  depot->serving = depot_machine_serves(store);
  /* Editors before the machine's service served a store that held a
     server key; the window offers to share it again. */
  depot->served_before = !depot->serving && vkr_bakery_is_file(key_path);
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
  if (depot->job) {
    depot->job->machine =
        vkr_heap_alloc_zeroed(1u, sizeof(*depot->job->machine));
  }
  if (!depot->job || !depot->job->machine) {
    vkr_heap_free(depot->job);
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
  if (depot->inspect) {
    if (depot->inspect->running) {
      (void)vkr_thread_join(depot->inspect->thread);
      (void)vkr_thread_destroy(depot->allocator, &depot->inspect->thread);
    }
    vkr_heap_free(depot->inspect);
  }
  if (depot->browser_open) {
    vkr_depot_browser_close(&depot->browser);
  }
  if (depot->stream) {
    /* Reads stop asking before the remote goes. */
    (void)vkr_vfs_set_remote(NULL, NULL, NULL);
    vkr_depot_remote_close(depot->stream);
  }
  vkr_heap_free(depot->job->machine);
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
  if (job->store_root[0]) {
    string_format(depot->store_root, sizeof(depot->store_root), "%s",
                  job->store_root);
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
    depot->pushed = job->pushed;
  }
  /* A status check that skipped the remote learned nothing about it. */
  const uint32_t remote = depot_remote_index(depot, job->remote);
  if (remote != UINT32_MAX && job->kind != DEPOT_JOB_COMMIT &&
      !job->skip_fetch) {
    depot->remote_connect[remote] = job->connect;
    depot->remote_tried[remote] = true_v;
  }
  if (!job->skip_fetch) {
    depot->connect = job->connect;
    depot->connected = job->connect == VKR_DEPOT_CONNECT_OK && job->remote[0];
  }
  depot->status_known = depot->status_known || job->ok;
  if (job->kind == DEPOT_JOB_STATUS && job->ok) {
    depot->access_count = Min(job->access_count, DEPOT_ACCESS_MAX);
    MemCopy(depot->access, job->access,
            depot->access_count * sizeof(VkrDepotAccessEntry));
    if (job->machine_read) {
      depot_take_machine(depot, job->machine);
    }
  }
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
    /* A remote that takes no connections fails by timing out; say why. */
    const uint32_t tried = depot_remote_index(depot, job->remote);
    if (tried != UINT32_MAX && depot->remote_outbound[tried] &&
        job->connect == VKR_DEPOT_CONNECT_UNREACHABLE) {
      string_format(depot->error, sizeof(depot->error),
                    "%s takes no connections from other computers. Its "
                    "depot service brings its commits to the computers it "
                    "syncs with and takes theirs while it is connected.",
                    job->remote);
    } else {
      string_format(depot->error, sizeof(depot->error), "%s", job->error);
    }
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
    /* A served repository needs the service, and a service that exited
       runs again only at the next logon: opening the project starts it,
       once, and a start that fails leaves the Start button. Install stops
       a running service, so a second check soon after confirms a missed
       loopback answer. */
    if (job->ok && job->machine_read) {
      const bool8_t stopped = depot->serving && !depot->machine_running;
      depot->machine_stopped_checks =
          stopped ? depot->machine_stopped_checks + 1u : 0u;
    }
    if (depot->machine_stopped_checks == 1u && !depot->machine_started) {
      depot->next_status = now + 2.0;
    } else if (depot->machine_stopped_checks >= 2u && !depot->machine_started) {
      depot->machine_started = true_v;
      char error[DEPOT_TEXT] = {0};
      if (!vkr_editor_depot_start_service(depot, error, sizeof(error))) {
        string_format(depot->error, sizeof(depot->error), "%s", error);
      }
    }
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
      char store[DEPOT_PATH];
      char error[DEPOT_TEXT] = {0};
      depot_store_root(job->directory, store, sizeof(store));
      if (job->serve && !depot_machine_set(store, depot->project_name, true_v,
                                           error, sizeof(error))) {
        string_format(depot->error, sizeof(depot->error), "%s", error);
      }
      depot_remember_project(depot, job->directory, depot->project_name,
                             job->project_id, job->serve);
    }
    break;
  case DEPOT_JOB_SERVICE:
    if (job->ok) {
      vkr_editor_toast(editor, VKR_UI_ICON_CHECK_CIRCLE,
                       vkr_ui_theme()->success, "The depot service runs");
    }
    depot->next_status = now + 1.0;
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
    depot->job->skip_fetch = depot->selected_remote < depot->remote_count &&
                             depot->remote_outbound[depot->selected_remote];
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
  /* A served store is where others pull from, so its commit is synced. */
  return depot_start_with(depot, DEPOT_JOB_COMMIT, NULL,
                          sync && !depot->serving, error, capacity);
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
  /* A served copy takes the commits others pushed to its store without
     one. */
  return depot_start_with(depot, DEPOT_JOB_PULL, remote, !depot->serving, error,
                          capacity);
}

bool8_t vkr_editor_depot_sync(VkrEditorDepot *depot, const char *remote,
                              char *error, uint32_t capacity) {
  if (!depot_can_start(depot, error, capacity)) {
    return false_v;
  }
  depot->job->policy = VKR_DEPOT_CONFLICT_FAIL;
  return depot_start_with(depot, DEPOT_JOB_SYNC, remote, !depot->serving, error,
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
  depot_store_root(depot->workspace, store, sizeof(store));
  if (!depot_machine_set(store, depot->project_name, serve, error, capacity)) {
    return false_v;
  }
  depot->serving = serve;
  depot->served_before = false_v;
  if (!serve) {
    depot->address[0] = '\0';
    MemZero(depot->server_key, sizeof(depot->server_key));
  }
  /* A service that does not run yet starts; the status check after it
     shows the result. Before the first check, ask it on loopback. */
  bool8_t running = depot->machine_running;
  if (serve && !depot->machine_known) {
    VkrDepotServiceStatus *machine =
        depot->job->running ? NULL : depot->job->machine;
    running = !machine ||
              (vkr_depot_service_status(machine, 300u) && machine->running);
  }
  if (serve && !running &&
      !vkr_editor_depot_start_service(depot, error, capacity)) {
    return false_v;
  }
  depot->next_status = 0.0;
  return true_v;
}

bool8_t vkr_editor_depot_start_service(VkrEditorDepot *depot, char *error,
                                       uint32_t capacity) {
  if (!depot) {
    return false_v;
  }
  depot_preempt_status(depot);
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  if (!depot_start(depot, DEPOT_JOB_SERVICE)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_clone(VkrEditorDepot *depot, const char *address,
                               const char *key_hex, const char *repository_hex,
                               const char *name, const char *destination,
                               bool8_t sparse, char *error, uint32_t capacity) {
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
  if (!depot_repository_parse(repository_hex, job->repository)) {
    string_format(error, capacity, "A repository ID is 32 hexadecimal digits");
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
  MemCopy(depot->dialog_repository, job->repository,
          sizeof(depot->dialog_repository));
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
  if (!depot || !depot->serving) {
    string_format(error, capacity, "This repository is not shared");
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
  /* The service rereads the store's access list within a second. */
  char store[DEPOT_PATH];
  depot_store_root(depot->workspace, store, sizeof(store));
  if (!vkr_depot_access_set(store, key, value)) {
    string_format(error, capacity, "The access list could not be written");
    return false_v;
  }
  if (value == VKR_DEPOT_ROLE_NONE &&
      depot->denied_count < DEPOT_REQUESTS_MAX) {
    MemCopy(depot->denied[depot->denied_count++], key, 32u);
  }
  for (uint32_t i = 0u; i < depot->request_count; ++i) {
    if (MemCompare(depot->requests[i].key, key, 32u) == 0) {
      MemCopy(&depot->requests[i], &depot->requests[i + 1u],
              (depot->request_count - i - 1u) * sizeof(depot->requests[0]));
      depot->request_count -= 1u;
      break;
    }
  }
  depot->access_count =
      vkr_depot_access_list(store, depot->access, DEPOT_ACCESS_MAX);
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

/* Commits Pull would bring: the remote's, as last fetched, and those other
   computers pushed to this store. */
static uint32_t depot_incoming(const VkrEditorDepot *depot) {
  return (depot->remote_known ? depot->behind : 0u) + depot->pushed;
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

/* Whether the remote is an outbound-only machine this one cannot reach,
   known before any try: its state says so instead of a failure. */
static bool8_t depot_remote_outbound(const VkrEditorDepot *depot,
                                     uint32_t index) {
  return depot->remote_outbound[index] &&
         (!depot->remote_tried[index] ||
          depot->remote_connect[index] != VKR_DEPOT_CONNECT_OK);
}

/* What a remote's last exchange says about reaching it. */
static const char *depot_remote_state(const VkrEditorDepot *depot,
                                      uint32_t index) {
  if (depot_remote_outbound(depot, index)) {
    return "reachable only when it connects";
  }
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
  return depot->machine_known &&
         MemCompare(found->server_key, depot->machine_key, 32u) == 0;
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
                  depot_incoming(depot));
    tooltip = depot->pushed
                  ? "Other computers pushed commits this working copy lacks; "
                    "Pull"
                  : "The remote has commits this repository lacks; Pull or "
                    "Sync";
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

/* The window's parts, top down: the toolbar, the branch and remote lists
   while open, the cards that need the user (a running step, an error,
   conflicts, files from outside the project, access requests, held
   scripts), the tabs, and the shown tab, which fills the rest. */
#define DEPOT_PAD 12.0f
#define DEPOT_TOOLBAR_PT 44.0f
#define DEPOT_TABS_PT 34.0f
#define DEPOT_COMMIT_BOX_PT 84.0f
/* The History lays its details beside the commits from this width, else
   under them. */
#define DEPOT_SIDE_BY_SIDE_PT 640.0f

typedef enum DepotTab {
  DEPOT_TAB_CHANGES = 0,
  DEPOT_TAB_HISTORY,
  DEPOT_TAB_REMOTES,
  DEPOT_TAB_SHARING,
  DEPOT_TAB_COUNT,
} DepotTab;

static VkrUiPlacement depot_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){
      .column = column, .row = row, .column_span = 1u, .row_span = 1u};
}

/* A clipped panel filling `placement`, whose children place themselves by
   margin from its top left, as depot_widget does. */
static bool8_t depot_pane_begin(VkrUiSystem *ui, const char *id,
                                VkrUiPlacement placement, Vec4 background) {
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement = placement;
  pane.columns = &fill;
  pane.column_count = 1u;
  pane.rows = &fill;
  pane.row_count = 1u;
  pane.style.gap_pt = 0.0f;
  pane.style.background_color = background;
  pane.clip_children = true_v;
  return vkr_ui_panel_begin(ui, depot_string(id), &pane);
}

/* The width a one-line button needs for `text`, with an icon when asked. */
static float32_t depot_text_pt(const char *text, bool8_t icon) {
  return 24.0f + 7.0f * (float32_t)string_length(text) + (icon ? 20.0f : 0.0f);
}

static bool8_t depot_icon_button(VkrEditorUi *editor, VkrUiSystem *ui,
                                 const char *id, const char *text,
                                 VkrUiIcon icon, float32_t x, float32_t y,
                                 float32_t width, bool8_t primary,
                                 bool8_t disabled, const char *tooltip) {
  VkrUiWidgetConfig c = depot_widget(x, y, width, 28);
  if (primary) {
    vkr_editor_primary_style(&c, editor->heading_font);
  } else {
    vkr_editor_action_style(&c, editor->heading_font);
  }
  c.icon = icon;
  c.icon_size_pt = 14.0f;
  c.disabled = disabled;
  if (tooltip) {
    c.tooltip = depot_string(tooltip);
  }
  return vkr_ui_button(ui, depot_string(id), depot_string(text), &c);
}

/* A small rounded tag, such as a branch a commit heads; returns its
   width. */
static float32_t depot_badge(VkrUiSystem *ui, const char *id, const char *text,
                             float32_t x, float32_t y, Vec4 color) {
  const float32_t width = 14.0f + 6.4f * (float32_t)string_length(text);
  VkrUiWidgetConfig c = depot_widget(x, y, width, 18);
  c.style.font_size_pt = vkr_ui_theme()->font_caption;
  c.style.text_color = color;
  c.style.background_color = vkr_ui_color_alpha(color, 0.14f);
  c.style.corner_radius_pt = (Vec4){9.0f, 9.0f, 9.0f, 9.0f};
  c.style.padding_pt = (VkrUiEdges){1.0f, 7.0f, 1.0f, 7.0f};
  vkr_ui_label(ui, depot_string(id), depot_string(text), &c);
  return width;
}

/* A card behind an alert, marked on its left by `accent`. */
static void depot_alert_card(VkrUiSystem *ui, const char *id, float32_t y,
                             float32_t width, float32_t height, Vec4 accent) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig card =
      depot_widget(DEPOT_PAD, y, width - 2 * DEPOT_PAD, height);
  card.style.padding_pt = (VkrUiEdges){0};
  card.style.background_color = vkr_ui_color_mix(theme->raised, accent, 0.08f);
  card.style.corner_radius_pt = (Vec4){6.0f, 6.0f, 6.0f, 6.0f};
  vkr_ui_label(ui, depot_string(id), (String8){0}, &card);
  VkrUiWidgetConfig mark = depot_widget(DEPOT_PAD, y, 3.0f, height);
  mark.style.padding_pt = (VkrUiEdges){0};
  mark.style.background_color = accent;
  mark.style.corner_radius_pt = (Vec4){1.5f, 1.5f, 1.5f, 1.5f};
  char mark_id[64];
  string_format(mark_id, sizeof(mark_id), "%s.mark", id);
  vkr_ui_label(ui, depot_string(mark_id), (String8){0}, &mark);
}

/* A path's last `shown` characters: a scene's id and file name, not the
   projects folder every path shares. */
static void depot_path_tail(const char *path, size_t shown, char *out,
                            uint32_t capacity) {
  const size_t length = string_length(path);
  if (length > shown && shown > 1u) {
    string_format(out, capacity, "\xe2\x80\xa6%s", path + length - shown + 1u);
  } else {
    string_format(out, capacity, "%s", path);
  }
}

/* "24 Oct 2026, 22:30" in this computer's time zone as it stands now. */
static void depot_format_date(uint64_t time_us, bool8_t with_time, char *out,
                              uint32_t capacity) {
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
  const VkrTime now = vkr_platform_get_local_time();
  const int64_t local = (int64_t)(time_us / 1000000ull) + now.gmtoff;
  const int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
  const int64_t seconds = local - days * 86400;
  /* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2);
  if (with_time) {
    string_format(out, capacity, "%lld %s %lld, %02lld:%02lld", (long long)day,
                  months[month - 1], (long long)year,
                  (long long)(seconds / 3600), (long long)(seconds / 60 % 60));
  } else {
    string_format(out, capacity, "%lld %s %lld", (long long)day,
                  months[month - 1], (long long)year);
  }
}

/* A commit's time as the History lists it: how long ago within a week,
   else its date. */
static void depot_format_when(uint64_t now_us, uint64_t time_us, char *out,
                              uint32_t capacity) {
  if (now_us > time_us && now_us - time_us >= 7ull * 86400ull * 1000000ull) {
    depot_format_date(time_us, false_v, out, capacity);
  } else {
    depot_format_age(now_us, time_us, out, capacity);
  }
}

static void depot_author_text(const VkrEditorDepot *depot,
                              const DepotHistory *row, char *out,
                              uint32_t capacity) {
  if (row->flags & DEPOT_COMMIT_MINE) {
    string_format(out, capacity, "You");
  } else {
    depot_person_text(depot, row->author, NULL, out, capacity);
  }
}

/* The table key of a commit: its id's first 16 digits. */
static uint64_t depot_commit_key(const char *id) {
  char head[17];
  string_format(head, sizeof(head), "%.16s", id);
  return string_parse_u64_hex(head, NULL);
}

static const DepotHistory *depot_history_find(const VkrEditorDepot *depot,
                                              const char *id) {
  for (uint32_t i = 0u; i < depot->history_count; ++i) {
    if (string_equals(depot->history[i].id, id)) {
      return &depot->history[i];
    }
  }
  return NULL;
}

/* Starts reading the selected commit's details when they are neither shown
   nor being read, and takes finished ones. */
static void depot_inspect_update(VkrEditorDepot *depot) {
  DepotInspect *inspect = depot->inspect;
  if (inspect && inspect->running &&
      vkr_atomic_bool_load(&inspect->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)vkr_thread_join(inspect->thread);
    (void)vkr_thread_destroy(depot->allocator, &inspect->thread);
    inspect->running = false_v;
  }
  VkrDepotId wanted;
  if (!depot->selected_commit[0] || !depot->store_root[0] ||
      !vkr_depot_id_parse(depot->selected_commit, &wanted)) {
    return;
  }
  if (!inspect) {
    inspect = depot->inspect = vkr_heap_alloc_zeroed(1u, sizeof(*inspect));
    if (!inspect) {
      return;
    }
  }
  if (inspect->running ||
      (vkr_depot_id_equal(&inspect->commit, &wanted) &&
       string_equals(inspect->store_root, depot->store_root))) {
    return;
  }
  inspect->commit = wanted;
  string_format(inspect->store_root, sizeof(inspect->store_root), "%s",
                depot->store_root);
  inspect->ok = false_v;
  inspect->error[0] = '\0';
  vkr_atomic_bool_store(&inspect->done, false_v, VKR_MEMORY_ORDER_RELAXED);
  inspect->running = vkr_thread_create(depot->allocator, &inspect->thread,
                                       depot_inspect_worker, inspect);
}

/* The details shown for the selected commit; NULL while they are read. */
static const DepotInspect *depot_inspected(const VkrEditorDepot *depot) {
  const DepotInspect *inspect = depot->inspect;
  VkrDepotId wanted;
  if (!inspect || inspect->running || !depot->selected_commit[0] ||
      !vkr_depot_id_parse(depot->selected_commit, &wanted) ||
      !vkr_depot_id_equal(&inspect->commit, &wanted)) {
    return NULL;
  }
  return inspect;
}

// -----------------------------------------------------------------------------
// Toolbar
// -----------------------------------------------------------------------------

/* The colour of a remote's state: reachable, waiting or failing. */
static Vec4 depot_remote_color(const VkrEditorDepot *depot, uint32_t index) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (depot_remote_outbound(depot, index)) {
    return theme->info;
  }
  if (!depot->remote_tried[index]) {
    return theme->text_disabled;
  }
  return depot->remote_connect[index] == VKR_DEPOT_CONNECT_OK ? theme->success
         : depot->remote_connect[index] == VKR_DEPOT_CONNECT_DENIED
             ? theme->warning
             : theme->error;
}

static void depot_toolbar(VkrEditorDepot *depot, VkrEditorUi *editor,
                          VkrUiSystem *ui, float32_t width, bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t y = 8.0f;
  float32_t x = DEPOT_PAD;
  char error[DEPOT_TEXT] = {0};
  /* The branch, opening its list. */
  const char *branch = depot->branch[0] ? depot->branch : DEPOT_BRANCH;
  const float32_t branch_pt =
      Min(depot_text_pt(branch, true_v) + 16.0f, 200.0f);
  VkrUiWidgetConfig picker = depot_widget(x, y, branch_pt, 28);
  vkr_editor_action_style(&picker, editor->heading_font);
  picker.icon = VKR_UI_ICON_GIT_BRANCH;
  picker.icon_size_pt = 14.0f;
  picker.icon_color = theme->accent_hover;
  picker.trailing_icon = depot->branch_menu_open ? VKR_UI_ICON_CHEVRON_UP
                                                 : VKR_UI_ICON_CHEVRON_DOWN;
  picker.trailing_icon_size_pt = 11.0f;
  picker.tooltip = string8_lit("The branch: switch to another or make one");
  if (vkr_ui_button(ui, string8_lit("bar.branch"), depot_string(branch),
                    &picker)) {
    depot->branch_menu_open = !depot->branch_menu_open;
    depot->remote_menu_open = false_v;
  }
  x += branch_pt + 8.0f;
  char pull[32];
  string_format(pull, sizeof(pull), depot_incoming(depot) ? "Pull %u" : "Pull",
                depot_incoming(depot));
  const float32_t pull_pt = depot_text_pt(pull, true_v);
  if (!depot->remote_count) {
    if (depot_icon_button(editor, ui, "bar.add", "Add a remote to sync",
                          VKR_UI_ICON_ADD, width - DEPOT_PAD - 190.0f, y,
                          190.0f, false_v, false_v,
                          "A remote is another computer's repository of this "
                          "project")) {
      depot->tab = DEPOT_TAB_REMOTES;
      depot->remote_form_open = true_v;
    }
    /* Other computers push to a served store; Pull takes their commits. */
    if (depot->pushed &&
        depot_icon_button(editor, ui, "bar.pull", pull, VKR_UI_ICON_ARROW_DOWN,
                          width - DEPOT_PAD - 196.0f - pull_pt, y, pull_pt,
                          false_v, busy,
                          "Take the commits other computers pushed to this "
                          "repository") &&
        !vkr_editor_depot_pull(depot, NULL, "fail", error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    return;
  }
  /* The remote steps use, its state as a coloured cloud. */
  const VkrDepotRemoteEntry *remote = depot_selected(depot);
  const uint32_t selected = depot->selected_remote;
  const float32_t remote_pt =
      Min(depot_text_pt(remote->name, true_v) + 16.0f, 180.0f);
  VkrUiWidgetConfig chooser = depot_widget(x, y, remote_pt, 28);
  vkr_editor_action_style(&chooser, editor->heading_font);
  chooser.icon = VKR_UI_ICON_CLOUD;
  chooser.icon_size_pt = 14.0f;
  chooser.icon_color = depot_remote_color(depot, selected);
  chooser.trailing_icon = depot->remote_menu_open ? VKR_UI_ICON_CHEVRON_UP
                                                  : VKR_UI_ICON_CHEVRON_DOWN;
  chooser.trailing_icon_size_pt = 11.0f;
  string_format(depot->remote_tip, sizeof(depot->remote_tip), "%s at %s: %s",
                remote->name, remote->address,
                depot_remote_state(depot, selected));
  chooser.tooltip = depot_string(depot->remote_tip);
  if (vkr_ui_button(ui, string8_lit("bar.remote"), depot_string(remote->name),
                    &chooser)) {
    depot->remote_menu_open = !depot->remote_menu_open;
    depot->branch_menu_open = false_v;
  }
  x += remote_pt + 8.0f;
  /* Sync, Push, Pull and Fetch from the right; Fetch keeps only its icon
     when the row is short of room. */
  char push[32];
  string_format(push, sizeof(push), depot->ahead ? "Push %u" : "Push",
                depot->ahead);
  const float32_t sync_pt = depot_text_pt("Sync", false_v) + 8.0f;
  const float32_t push_pt = depot_text_pt(push, true_v);
  const float32_t room = width - DEPOT_PAD - x - sync_pt - push_pt - pull_pt;
  const bool8_t fetch_text = room > depot_text_pt("Fetch", true_v) + 24.0f;
  const float32_t fetch_pt =
      fetch_text ? depot_text_pt("Fetch", true_v) : 34.0f;
  float32_t right = width - DEPOT_PAD - sync_pt;
  if (depot_icon_button(editor, ui, "bar.sync", "Sync", VKR_UI_ICON_NONE, right,
                        y, sync_pt, true_v, busy,
                        "Pull, then push the branch") &&
      !vkr_editor_depot_sync(depot, NULL, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  right -= push_pt + 6.0f;
  if (depot_icon_button(editor, ui, "bar.push", push, VKR_UI_ICON_ARROW_UP,
                        right, y, push_pt, false_v, busy || !depot->ahead,
                        "Send this branch's commits to the remote") &&
      !vkr_editor_depot_push(depot, NULL, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  right -= pull_pt + 6.0f;
  if (depot_icon_button(editor, ui, "bar.pull", pull, VKR_UI_ICON_ARROW_DOWN,
                        right, y, pull_pt, false_v, busy,
                        "Bring the remote's commits into this branch") &&
      !vkr_editor_depot_pull(depot, NULL, "fail", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  right -= fetch_pt + 6.0f;
  if (right > x &&
      depot_icon_button(editor, ui, "bar.fetch", fetch_text ? "Fetch" : "",
                        VKR_UI_ICON_REFRESH, right, y, fetch_pt, false_v, busy,
                        "Look for the remote's new commits without taking "
                        "them") &&
      !vkr_editor_depot_fetch(depot, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
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
    const float32_t button = Min(180.0f, depot_text_pt(names[i], false_v));
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
    cursor += button + 6;
  }
  *y += 32;
  return clicked;
}

/* The open branch list: the branches to switch to and a field for a new
   one. */
static void depot_branch_strip(VkrEditorDepot *depot, VkrEditorUi *editor,
                               VkrUiSystem *ui, float32_t *y, float32_t width,
                               bool8_t busy) {
  char error[DEPOT_TEXT] = {0};
  const float32_t top = *y;
  float32_t row = top + 10.0f;
  const char *names[DEPOT_BRANCHES_MAX];
  uint32_t chosen = UINT32_MAX;
  for (uint32_t i = 0u; i < depot->branch_count; ++i) {
    names[i] = depot->branches[i].name;
    if (string_equals(names[i], depot->branch)) {
      chosen = i;
    }
  }
  /* The card goes first, under the rows, so its height counts the rows
     depot_choice_row wraps the branches into. */
  float32_t wrapped = 0.0f;
  float32_t cursor = DEPOT_PAD + 12.0f;
  for (uint32_t i = 0u; i < depot->branch_count; ++i) {
    const float32_t button = Min(180.0f, depot_text_pt(names[i], false_v));
    if (cursor + button > width - DEPOT_PAD - 12.0f &&
        cursor > DEPOT_PAD + 12.0f) {
      cursor = DEPOT_PAD + 12.0f;
      wrapped += 32.0f;
    }
    cursor += button + 6.0f;
  }
  const float32_t height =
      10.0f + (depot->branch_count ? wrapped + 32.0f : 0.0f) + 38.0f;
  depot_alert_card(ui, "branches.card", top, width, height,
                   vkr_ui_theme()->accent);
  if (depot->branch_count) {
    const uint32_t clicked = depot_choice_row(
        editor, ui, "branch", names, depot->branch_count, chosen,
        DEPOT_PAD + 12.0f, &row, width - 2 * DEPOT_PAD - 24.0f, busy);
    if (clicked != UINT32_MAX) {
      if (vkr_editor_depot_switch(depot, names[clicked], false_v, error,
                                  sizeof(error))) {
        depot->branch_menu_open = false_v;
      } else {
        string_format(depot->error, sizeof(depot->error), "%s", error);
      }
    }
  }
  const float32_t field = Min(width - 2 * DEPOT_PAD - 160.0f, 280.0f);
  depot_field(ui, "branch.new", depot->new_branch, sizeof(depot->new_branch),
              DEPOT_PAD + 12.0f, row, field);
  if (!depot->new_branch[0]) {
    VkrUiWidgetConfig hint =
        depot_widget(DEPOT_PAD + 18.0f, row + 3, field - 12, 20);
    hint.style.text_color = vkr_ui_theme()->text_disabled;
    hint.style.font_size_pt = vkr_ui_theme()->font_caption;
    vkr_ui_label(ui, string8_lit("branch.hint"),
                 string8_lit("New branch from this one"), &hint);
  }
  if (depot_button(editor, ui, "branch.create", "Make branch",
                   DEPOT_PAD + 20.0f + field, row, 120, false_v,
                   busy || !depot->new_branch[0])) {
    if (vkr_editor_depot_switch(depot, depot->new_branch, true_v, error,
                                sizeof(error))) {
      depot->branch_menu_open = false_v;
    } else {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  *y = top + height + 8.0f;
}

/* The open remote list: the remote steps use, its state, and the way to
   the Remotes tab. */
static void depot_remote_strip(VkrEditorDepot *depot, VkrEditorUi *editor,
                               VkrUiSystem *ui, float32_t *y, float32_t width,
                               bool8_t busy) {
  const float32_t top = *y;
  const float32_t height = depot->remote_count > 1u ? 92.0f : 60.0f;
  depot_alert_card(ui, "remotes.card", top, width, height,
                   vkr_ui_theme()->accent);
  float32_t row = top + 10.0f;
  if (depot->remote_count > 1u) {
    const char *names[VKR_DEPOT_REMOTES_MAX];
    for (uint32_t i = 0u; i < depot->remote_count; ++i) {
      names[i] = depot->remotes[i].name;
    }
    const uint32_t clicked =
        depot_choice_row(editor, ui, "remote.pick", names, depot->remote_count,
                         depot->selected_remote, DEPOT_PAD + 12.0f, &row,
                         width - 2 * DEPOT_PAD - 24.0f, busy);
    if (clicked != UINT32_MAX) {
      depot->selected_remote = clicked;
      depot->next_status = 0.0;
    }
  }
  const VkrDepotRemoteEntry *remote = depot_selected(depot);
  const uint32_t selected = depot->selected_remote;
  char text[DEPOT_TEXT];
  char seen[48] = {0};
  if (depot_remote_outbound(depot, selected)) {
    const uint64_t now_s = (uint64_t)vkr_platform_get_unix_time();
    const uint64_t seen_s = depot->remote_seen_us[selected] / 1000000u;
    string_format(
        seen, sizeof(seen), ", last %llu min ago",
        (unsigned long long)(now_s > seen_s ? (now_s - seen_s) / 60u : 0u));
  }
  if (depot->remote_known) {
    string_format(text, sizeof(text),
                  "%s  \xc2\xb7  %s%s  \xc2\xb7  %u to push, %u to pull",
                  remote->address, depot_remote_state(depot, selected), seen,
                  depot->ahead, depot->behind);
  } else {
    string_format(text, sizeof(text), "%s  \xc2\xb7  %s%s  \xc2\xb7  no %s yet",
                  remote->address, depot_remote_state(depot, selected), seen,
                  depot->branch);
  }
  VkrUiWidgetConfig state = depot_widget(DEPOT_PAD + 12.0f, row + 5,
                                         width - 2 * DEPOT_PAD - 160.0f, 18);
  state.style.font_size_pt = vkr_ui_theme()->font_caption;
  state.style.text_color = depot_remote_color(depot, selected);
  vkr_ui_label(ui, string8_lit("remote.state"), depot_string(text), &state);
  if (depot_button(editor, ui, "remote.manage", "Manage remotes",
                   width - DEPOT_PAD - 140.0f, row, 128, false_v, false_v)) {
    depot->tab = DEPOT_TAB_REMOTES;
    depot->remote_menu_open = false_v;
  }
  *y = top + height + 8.0f;
}

// -----------------------------------------------------------------------------
// Alerts
// -----------------------------------------------------------------------------

static void depot_alert_progress(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 VkrUiSystem *ui, float32_t *y,
                                 float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  const float32_t fraction = depot_job_text(depot, text, sizeof(text));
  depot_alert_card(ui, "job.card", *y, width, 54.0f, theme->accent);
  VkrUiWidgetConfig label = depot_widget(DEPOT_PAD + 12.0f, *y + 7,
                                         width - 2 * DEPOT_PAD - 130.0f, 22);
  label.icon = VKR_UI_ICON_SPINNER;
  label.icon_size_pt = 13.0f;
  label.icon_color = theme->accent_hover;
  vkr_ui_label(ui, string8_lit("job.text"), depot_string(text), &label);
  if (depot_button(editor, ui, "job.cancel", "Cancel",
                   width - DEPOT_PAD - 100.0f, *y + 6, 88, false_v, false_v)) {
    vkr_editor_depot_cancel(depot);
  }
  depot_progress_bar(ui, "job.bar", DEPOT_PAD + 14.0f, *y + 38,
                     width - 2 * DEPOT_PAD - 28.0f, fraction);
  *y += 62.0f;
}

static void depot_alert_error(VkrEditorDepot *depot, VkrUiSystem *ui,
                              float32_t *y, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t text_pt = width - 2 * DEPOT_PAD - 70.0f;
  const float32_t lines =
      (float32_t)((uint32_t)((float32_t)string_length(depot->error) /
                             Max(16.0f, text_pt / 6.8f)) +
                  1u);
  const float32_t height = Max(36.0f, lines * 18.0f + 16.0f);
  depot_alert_card(ui, "error.card", *y, width, height, theme->warning);
  VkrUiWidgetConfig c =
      depot_widget(DEPOT_PAD + 12.0f, *y + 7, text_pt, height - 12.0f);
  c.style.text_color = theme->text;
  c.text.layout.word_wrap = true_v;
  c.text.layout.max_width = text_pt - 26.0f;
  c.icon = VKR_UI_ICON_WARNING_FILL;
  c.icon_size_pt = 13.0f;
  c.icon_color = theme->warning;
  vkr_ui_label(ui, string8_lit("error"), depot_string(depot->error), &c);
  VkrUiWidgetConfig close =
      depot_widget(width - DEPOT_PAD - 34.0f, *y + 5, 26, 26);
  vkr_editor_ghost_style(&close);
  close.icon = VKR_UI_ICON_CLOSE;
  close.icon_size_pt = 12.0f;
  close.icon_color = theme->text_secondary;
  close.tooltip = string8_lit("Dismiss");
  if (vkr_ui_button(ui, string8_lit("error.close"), (String8){0}, &close)) {
    depot->error[0] = '\0';
  }
  *y += height + 8.0f;
}

/* Paths under a title, at most four, then "and N more". */
static float32_t depot_alert_paths(VkrUiSystem *ui, const char *id,
                                   const DepotText *paths, uint32_t count,
                                   float32_t y, float32_t width) {
  const size_t shown =
      (size_t)Max(16.0f, (width - 2 * DEPOT_PAD - 40.0f) / 6.4f);
  const uint32_t rows = Min(count, 4u);
  (void)vkr_ui_push_id_u64(ui, (uint64_t)(uintptr_t)id);
  for (uint32_t i = 0u; i < rows; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    char tail[DEPOT_TEXT];
    depot_path_tail(paths[i].text, shown, tail, sizeof(tail));
    depot_caption(ui, "path", tail, DEPOT_PAD + 24.0f, y,
                  width - 2 * DEPOT_PAD - 36.0f);
    (void)vkr_ui_pop_id(ui);
    y += 18.0f;
  }
  if (count > rows) {
    char text[64];
    string_format(text, sizeof(text), "and %u more", count - rows);
    depot_caption(ui, "more", text, DEPOT_PAD + 24.0f, y,
                  width - 2 * DEPOT_PAD - 36.0f);
    y += 18.0f;
  }
  (void)vkr_ui_pop_id(ui);
  return y;
}

static float32_t depot_paths_pt(uint32_t count) {
  return 18.0f * (float32_t)(Min(count, 4u) + (count > 4u ? 1u : 0u));
}

static void depot_alert_conflicts(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  VkrUiSystem *ui, float32_t *y,
                                  float32_t width, bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t height =
      30.0f + depot_paths_pt(depot->conflict_count) + 26.0f + 40.0f;
  depot_alert_card(ui, "conflict.card", *y, width, height, theme->warning);
  char text[DEPOT_TEXT];
  string_format(text, sizeof(text), "%u file%s changed here and on the remote",
                depot->conflict_count, depot->conflict_count == 1u ? "" : "s");
  VkrUiWidgetConfig title = depot_widget(DEPOT_PAD + 12.0f, *y + 6,
                                         width - 2 * DEPOT_PAD - 24.0f, 22);
  title.text.font = editor->heading_font;
  title.icon = VKR_UI_ICON_WARNING_FILL;
  title.icon_size_pt = 13.0f;
  title.icon_color = theme->warning;
  vkr_ui_label(ui, string8_lit("conflict.title"), depot_string(text), &title);
  float32_t row = depot_alert_paths(ui, "conflicts", depot->conflicts,
                                    depot->conflict_count, *y + 30.0f, width);
  depot_caption(ui, "conflict.text",
                "A scene both sides changed is settled whole: pick a side.",
                DEPOT_PAD + 12.0f, row + 4.0f, width - 2 * DEPOT_PAD - 24.0f);
  row += 26.0f;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "conflict.theirs", "Use theirs",
                   DEPOT_PAD + 12.0f, row, 120, false_v, busy) &&
      !vkr_editor_depot_pull(depot, NULL, "theirs", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "conflict.mine", "Keep mine", DEPOT_PAD + 140.0f,
                   row, 120, false_v, busy) &&
      !vkr_editor_depot_pull(depot, NULL, "mine", error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += height + 8.0f;
}

/* Files the project reads from outside itself, which the last commit
   stopped for: copy them in and commit, or commit without them. */
static void depot_alert_untracked(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  VkrUiSystem *ui, float32_t *y,
                                  float32_t width, bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t height =
      30.0f + depot_paths_pt(depot->untracked_count) + 26.0f + 40.0f;
  depot_alert_card(ui, "untracked.card", *y, width, height, theme->warning);
  char text[DEPOT_TEXT];
  char size[32];
  depot_format_size(depot->untracked_bytes, size, sizeof(size));
  string_format(text, sizeof(text), "%u file%s from outside the project (%s)",
                depot->untracked_count, depot->untracked_count == 1u ? "" : "s",
                size);
  VkrUiWidgetConfig title = depot_widget(DEPOT_PAD + 12.0f, *y + 6,
                                         width - 2 * DEPOT_PAD - 24.0f, 22);
  title.text.font = editor->heading_font;
  title.icon = VKR_UI_ICON_WARNING_FILL;
  title.icon_size_pt = 13.0f;
  title.icon_color = theme->warning;
  vkr_ui_label(ui, string8_lit("untracked.title"), depot_string(text), &title);
  float32_t row = depot_alert_paths(ui, "untracked", depot->untracked,
                                    depot->untracked_count, *y + 30.0f, width);
  depot_caption(ui, "untracked.text",
                "The project reads them from the engine's folder, so a clone "
                "on another computer would miss them.",
                DEPOT_PAD + 12.0f, row + 4.0f, width - 2 * DEPOT_PAD - 24.0f);
  row += 26.0f;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "untracked.adopt", "Copy in and commit",
                   DEPOT_PAD + 12.0f, row, 170, true_v, busy) &&
      !vkr_editor_depot_commit(depot, depot->message, false_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_ADOPT, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (depot_button(editor, ui, "untracked.ignore", "Commit without them",
                   DEPOT_PAD + 190.0f, row, 170, false_v, busy) &&
      !vkr_editor_depot_commit(depot, depot->message, false_v,
                               VKR_EDITOR_DEPOT_UNTRACKED_IGNORE, error,
                               sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += height + 8.0f;
}

static void depot_alert_requests(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 VkrUiSystem *ui, float32_t *y,
                                 float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  for (uint32_t i = 0u; i < depot->request_count; ++i) {
    const VkrDepotAccessRequest *request = &depot->requests[i];
    (void)vkr_ui_push_id_u64(ui, i);
    depot_alert_card(ui, "request.card", *y, width, 84.0f, theme->accent);
    char who[VKR_DEPOT_DISPLAY_NAME + 16];
    depot_person_text(depot, request->key, request->name, who, sizeof(who));
    char text[160];
    string_format(text, sizeof(text), "%s asks to fetch and push", who);
    VkrUiWidgetConfig title = depot_widget(DEPOT_PAD + 12.0f, *y + 6,
                                           width - 2 * DEPOT_PAD - 24.0f, 22);
    title.text.font = editor->heading_font;
    title.icon = VKR_UI_ICON_BELL;
    title.icon_size_pt = 13.0f;
    title.icon_color = theme->accent_hover;
    vkr_ui_label(ui, string8_lit("request"), depot_string(text), &title);
    char print[16];
    depot_fingerprint(request->key, print);
    string_format(text, sizeof(text),
                  "Its key starts %s; that computer shows the same while it "
                  "waits",
                  print);
    depot_caption(ui, "request.key", text, DEPOT_PAD + 12.0f, *y + 28,
                  width - 2 * DEPOT_PAD - 24.0f);
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(request->key, key);
    char error[DEPOT_TEXT] = {0};
    const float32_t row = *y + 50.0f;
    if (depot_button(editor, ui, "allow", "Allow", DEPOT_PAD + 12.0f, row, 96,
                     true_v, false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "write", error, sizeof(error));
    }
    if (depot_button(editor, ui, "read", "Fetch only", DEPOT_PAD + 116.0f, row,
                     104, false_v, false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "read", error, sizeof(error));
    }
    if (depot_button(editor, ui, "deny", "Deny", DEPOT_PAD + 228.0f, row, 80,
                     false_v, false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "none", error, sizeof(error));
    }
    if (error[0]) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 92.0f;
  }
}

static void depot_alert_line(VkrEditorUi *editor, VkrUiSystem *ui,
                             const char *id, const char *text, VkrUiIcon icon,
                             const char *action, float32_t *y, float32_t width,
                             bool8_t *out_clicked) {
  const VkrUiTheme *theme = vkr_ui_theme();
  depot_alert_card(ui, id, *y, width, 40.0f, theme->warning);
  VkrUiWidgetConfig label = depot_widget(DEPOT_PAD + 12.0f, *y + 9,
                                         width - 2 * DEPOT_PAD - 170.0f, 22);
  label.icon = icon;
  label.icon_size_pt = 13.0f;
  label.icon_color = theme->warning;
  char label_id[64];
  string_format(label_id, sizeof(label_id), "%s.text", id);
  vkr_ui_label(ui, depot_string(label_id), depot_string(text), &label);
  if (action) {
    char button_id[64];
    string_format(button_id, sizeof(button_id), "%s.go", id);
    *out_clicked =
        depot_button(editor, ui, button_id, action, width - DEPOT_PAD - 136.0f,
                     *y + 6, 124, false_v, false_v);
  }
  *y += 48.0f;
}

/* The lists the toolbar opened and the cards that need the user, from
   `*y` down; `*y` ends below them. */
static void depot_top(VkrEditorDepot *depot, VkrEditorUi *editor,
                      VkrUiSystem *ui, float32_t *y, float32_t width,
                      bool8_t busy) {
  if (depot->branch_menu_open) {
    depot_branch_strip(depot, editor, ui, y, width, busy);
  }
  if (depot->remote_menu_open && depot->remote_count) {
    depot_remote_strip(depot, editor, ui, y, width, busy);
  }
  if (busy) {
    depot_alert_progress(depot, editor, ui, y, width);
  }
  if (depot->error[0]) {
    depot_alert_error(depot, ui, y, width);
  }
  if (depot->conflict_count) {
    depot_alert_conflicts(depot, editor, ui, y, width, busy);
  }
  if (depot->untracked_count) {
    depot_alert_untracked(depot, editor, ui, y, width, busy);
  }
  depot_alert_requests(depot, editor, ui, y, width);
  if (depot->waiting_kind != DEPOT_JOB_NONE) {
    const VkrDepotRemoteEntry *remote = depot_selected(depot);
    char text[DEPOT_TEXT];
    string_format(text, sizeof(text),
                  "Waiting for %s's owner to allow this computer",
                  remote ? remote->name : "the remote");
    depot_alert_line(editor, ui, "waiting", text, VKR_UI_ICON_CLOCK, NULL, y,
                     width, NULL);
  }
  if (vkr_editor_scripts_held(editor->scripts)) {
    bool8_t trust = false_v;
    depot_alert_line(editor, ui, "scripts",
                     "Scripts from others are not running", VKR_UI_ICON_LOCK,
                     "Run scripts", y, width, &trust);
    if (trust) {
      vkr_editor_depot_trust_scripts(depot, editor);
    }
  }
}

// -----------------------------------------------------------------------------
// Tabs
// -----------------------------------------------------------------------------

/* The tab row: each tab an icon, a name and a count, the shown one in the
   heading font over an accent underline, on a rule across the window. */
static void depot_tabs(VkrEditorDepot *depot, VkrEditorUi *editor,
                       VkrUiSystem *ui, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig rule = depot_widget(0, DEPOT_TABS_PT - 1.0f, width, 1);
  rule.style.padding_pt = (VkrUiEdges){0};
  rule.style.background_color = theme->separator;
  vkr_ui_label(ui, string8_lit("tabs.rule"), (String8){0}, &rule);
  uint32_t access = 0u;
  for (uint32_t i = 0u; i < depot->access_count; ++i) {
    access += depot->access[i].role != VKR_DEPOT_ROLE_ADMIN;
  }
  char labels[DEPOT_TAB_COUNT][48];
  string_format(labels[DEPOT_TAB_CHANGES], sizeof(labels[0]),
                depot->change_count ? "Changes  %u" : "Changes",
                depot->change_count);
  string_format(labels[DEPOT_TAB_HISTORY], sizeof(labels[0]), "History");
  string_format(labels[DEPOT_TAB_REMOTES], sizeof(labels[0]),
                depot->remote_count ? "Remotes  %u" : "Remotes",
                depot->remote_count);
  string_format(labels[DEPOT_TAB_SHARING], sizeof(labels[0]),
                depot->serving && access ? "Sharing  %u" : "Sharing", access);
  static const VkrUiIcon icons[] = {VKR_UI_ICON_PENCIL_LINE, VKR_UI_ICON_CLOCK,
                                    VKR_UI_ICON_CLOUD, VKR_UI_ICON_WORLD};
  static const char *const tips[] = {
      "Local changes to commit", "The commits of this branch and its remote",
      "Other computers' repositories to sync with",
      "Let other computers fetch from and push to this repository"};
  float32_t x = DEPOT_PAD - 4.0f;
  for (uint32_t i = 0u; i < DEPOT_TAB_COUNT; ++i) {
    const bool8_t active = depot->tab == i;
    const float32_t tab_pt = depot_text_pt(labels[i], true_v);
    VkrUiWidgetConfig tab = depot_widget(x, 2.0f, tab_pt, DEPOT_TABS_PT - 3.0f);
    tab.style.font_size_pt = theme->font_body;
    tab.text.font = active ? editor->heading_font : VKR_FONT_HANDLE_INVALID;
    tab.style.padding_pt = (VkrUiEdges){4.0f, 12.0f, 4.0f, 10.0f};
    tab.style.corner_radius_pt =
        (Vec4){theme->radius, theme->radius, 0.0f, 0.0f};
    tab.style.background_color = theme->panel;
    tab.style.hover_background_color =
        active ? theme->panel
               : vkr_ui_color_mix(theme->panel, theme->raised, 0.6f);
    tab.style.text_color = active ? theme->text : theme->text_secondary;
    tab.style.border_pt =
        active ? (VkrUiEdges){0.0f, 0.0f, 2.0f, 0.0f} : (VkrUiEdges){0};
    tab.style.border_color = theme->accent;
    tab.icon = icons[i];
    tab.icon_size_pt = 14.0f;
    tab.icon_color = active ? theme->accent_hover : theme->text_secondary;
    if (i == DEPOT_TAB_SHARING && depot->request_count) {
      tab.icon = VKR_UI_ICON_BELL;
      tab.icon_color = theme->warning;
    }
    tab.tooltip = depot_string(tips[i]);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("tab"), depot_string(labels[i]), &tab) &&
        depot->tab != i) {
      depot->tab = i;
      depot->window_scroll = 0.0f;
    }
    (void)vkr_ui_pop_id(ui);
    x += tab_pt + 2.0f;
  }
}

// -----------------------------------------------------------------------------
// File tables
// -----------------------------------------------------------------------------

typedef struct DepotFileView {
  const char *path;
  VkrDepotChange change;
} DepotFileView;

/* A row the file table sorts: its texts by column and its place. */
typedef struct DepotFileSort {
  const char *text;
  const char *then;
  uint32_t index;
} DepotFileSort;

static int32_t depot_file_sort_compare(const void *lhs, const void *rhs) {
  const DepotFileSort *a = lhs;
  const DepotFileSort *b = rhs;
  int32_t order = string_compare(a->text, b->text);
  if (!order) {
    order = string_compare(a->then, b->then);
  }
  return order ? order : (a->index < b->index ? -1 : a->index > b->index);
}

static const char *depot_change_name(VkrDepotChange change) {
  return change == VKR_DEPOT_CHANGE_ADDED     ? "Added"
         : change == VKR_DEPOT_CHANGE_DELETED ? "Deleted"
                                              : "Modified";
}

/* Changed files: what changed, the file's name and its folder; sortable by
   each. A `compact` table, for a narrow pane, shows the change as its icon
   and the folder after the name in one column. */
static void depot_files_table(VkrUiSystem *ui, const char *id,
                              VkrEditorTable *table, const DepotFileView *files,
                              uint32_t count, VkrUiPlacement placement,
                              bool8_t compact) {
  const VkrUiTheme *theme = vkr_ui_theme();
  static const VkrEditorTableColumn wide[] = {
      {.title = "Change", .width_pt = 92.0f, .min_pt = 70.0f},
      {.title = "File", .width_pt = 220.0f, .min_pt = 80.0f},
      {.title = "Folder", .width_pt = 0.0f, .min_pt = 80.0f}};
  static const VkrEditorTableColumn narrow[] = {
      {.title = "",
       .width_pt = 30.0f,
       .min_pt = 30.0f,
       .tooltip = "Added, modified or deleted"},
      {.title = "File", .width_pt = 0.0f, .min_pt = 80.0f}};
  const VkrEditorTableColumn *columns = compact ? narrow : wide;
  const uint32_t column_count = compact ? ArrayCount(narrow) : ArrayCount(wide);
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, id, table, columns, column_count, count,
                              placement, &sorted, &first, &end)) {
    return;
  }
  /* Rows in the table's order: the given one, or by the sorted column. */
  DepotFileSort order[DEPOT_DETAIL_FILES_MAX];
  count = Min(count, DEPOT_DETAIL_FILES_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    const char *slash = string_get_last_char_occurrence(files[i].path, '/');
    const char *name = slash ? slash + 1 : files[i].path;
    order[i] = (DepotFileSort){.text = files[i].path, .then = "", .index = i};
    if (table->sort_column == 0u) {
      order[i].text = depot_change_name(files[i].change);
      order[i].then = files[i].path;
    } else if (table->sort_column == 1u) {
      order[i].text = name;
      order[i].then = files[i].path;
    }
  }
  if (table->sort_column != VKR_EDITOR_TABLE_NONE) {
    vkr_sort(order, count, sizeof(order[0]), depot_file_sort_compare);
    if (table->sort_descending) {
      for (uint32_t i = 0u; i < count / 2u; ++i) {
        const DepotFileSort swap = order[i];
        order[i] = order[count - 1u - i];
        order[count - 1u - i] = swap;
      }
    }
  }
  for (uint32_t r = first; r < end && r < count; ++r) {
    const DepotFileView *file = &files[order[r].index];
    const char *slash = string_get_last_char_occurrence(file->path, '/');
    const char *name = slash ? slash + 1 : file->path;
    char folder[DEPOT_TEXT];
    if (slash) {
      string_format(folder, sizeof(folder), "%.*s", (int)(slash - file->path),
                    file->path);
    } else {
      folder[0] = '\0';
    }
    char both[DEPOT_TEXT * 2];
    string_format(both, sizeof(both), folder[0] ? "%s   %s" : "%s", name,
                  folder);
    const Vec4 color = file->change == VKR_DEPOT_CHANGE_ADDED ? theme->success
                       : file->change == VKR_DEPOT_CHANGE_DELETED
                           ? theme->error
                           : theme->warning;
    const VkrUiIcon change =
        file->change == VKR_DEPOT_CHANGE_ADDED     ? VKR_UI_ICON_PLUS_CIRCLE
        : file->change == VKR_DEPOT_CHANGE_DELETED ? VKR_UI_ICON_MINUS
                                                   : VKR_UI_ICON_PENCIL_LINE;
    const String8 wide_cells[] = {depot_string(depot_change_name(file->change)),
                                  depot_string(name), depot_string(folder)};
    const String8 narrow_cells[] = {(String8){0}, depot_string(both)};
    const Vec4 colors[] = {color, {0}, theme->text_secondary};
    const VkrUiIcon icons[] = {change, VKR_UI_ICON_FILE, VKR_UI_ICON_NONE};
    /* The path lives past the build, as the hover text must. */
    const VkrEditorTableRow row = {.key = order[r].index,
                                   .cells = compact ? narrow_cells : wide_cells,
                                   .cell_colors = colors,
                                   .cell_icons = icons,
                                   .tooltip = depot_string(file->path)};
    (void)vkr_editor_table_row(ui, table, columns, column_count, r, &row);
  }
  vkr_editor_table_end(ui);
}

/* An empty tab's message, centred in its pane. */
static void depot_empty(VkrUiSystem *ui, const char *id, VkrUiIcon icon,
                        const char *title, const char *text, float32_t y,
                        float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig mark = depot_widget(width / 2.0f - 16.0f, y, 32, 32);
  mark.icon = icon;
  mark.icon_size_pt = 26.0f;
  mark.icon_color = theme->text_disabled;
  char mark_id[64];
  string_format(mark_id, sizeof(mark_id), "%s.icon", id);
  vkr_ui_label(ui, depot_string(mark_id), (String8){0}, &mark);
  VkrUiWidgetConfig heading = depot_widget(DEPOT_PAD, y + 40, width - 24, 22);
  heading.center = true_v;
  heading.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, depot_string(id), depot_string(title), &heading);
  if (text) {
    VkrUiWidgetConfig body = depot_widget(DEPOT_PAD, y + 64, width - 24, 18);
    body.center = true_v;
    body.style.font_size_pt = theme->font_caption;
    body.style.text_color = theme->text_disabled;
    char body_id[64];
    string_format(body_id, sizeof(body_id), "%s.text", id);
    vkr_ui_label(ui, depot_string(body_id), depot_string(text), &body);
  }
}

// -----------------------------------------------------------------------------
// Changes
// -----------------------------------------------------------------------------

static void depot_tab_changes(VkrEditorDepot *depot, VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, VkrUiSystem *ui,
                              VkrUiPlacement placement, float32_t width,
                              bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t any = depot->change_count > 0u;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      fill,
      {.value = any ? DEPOT_COMMIT_BOX_PT : 0.0f, .unit = VKR_UI_TRACK_PX}};
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement = placement;
  pane.columns = &fill;
  pane.column_count = 1u;
  pane.rows = rows;
  pane.row_count = ArrayCount(rows);
  pane.style.gap_pt = 0.0f;
  pane.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("changes"), &pane)) {
    return;
  }
  if (!any) {
    if (depot_pane_begin(ui, "changes.empty", depot_cell(0u, 0u), (Vec4){0})) {
      if (!depot->status_known && depot_busy(depot)) {
        depot_empty(ui, "changes.reading", VKR_UI_ICON_SPINNER,
                    "Looking for changes\xe2\x80\xa6", NULL, 40.0f, width);
      } else {
        depot_empty(
            ui, "changes.none", VKR_UI_ICON_CHECK_CIRCLE, "No local changes",
            "Saved edits to the project's files show here", 40.0f, width);
      }
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
    return;
  }
  DepotFileView files[DEPOT_LIST_MAX];
  const uint32_t count = Min(depot->change_count, DEPOT_LIST_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    const char *text = depot->changes[i].text;
    files[i].change = text[0] == '+'   ? VKR_DEPOT_CHANGE_ADDED
                      : text[0] == '-' ? VKR_DEPOT_CHANGE_DELETED
                                       : VKR_DEPOT_CHANGE_MODIFIED;
    files[i].path = text[0] && text[1] ? text + 2 : text;
  }
  VkrUiPlacement table = depot_cell(0u, 0u);
  table.margin_pt = (VkrUiEdges){8.0f, DEPOT_PAD, 4.0f, DEPOT_PAD};
  depot_files_table(ui, "changes.files", &depot->changes_table, files, count,
                    table, false_v);
  if (depot_pane_begin(ui, "commit.box", depot_cell(0u, 1u), theme->panel)) {
    float32_t y = 6.0f;
    if (depot->change_count > count) {
      char more[64];
      string_format(more, sizeof(more), "Showing %u of %u changed files", count,
                    depot->change_count);
      depot_caption(ui, "changes.more", more, DEPOT_PAD, y - 4.0f,
                    width - 2 * DEPOT_PAD);
    }
    y += 10.0f;
    depot_field(ui, "message", depot->message, sizeof(depot->message),
                DEPOT_PAD, y, width - 2 * DEPOT_PAD);
    if (!depot->message[0]) {
      VkrUiWidgetConfig hint =
          depot_widget(DEPOT_PAD + 6.0f, y + 3, width - 40, 20);
      hint.style.text_color = theme->text_disabled;
      hint.style.font_size_pt = theme->font_caption;
      vkr_ui_label(ui, string8_lit("message.hint"),
                   string8_lit("Describe your changes"), &hint);
    }
    y += 34.0f;
    char error[DEPOT_TEXT] = {0};
    const bool8_t unsaved = !depot_journals_saved(frame);
    if (depot_button(editor, ui, "commit", "Commit", DEPOT_PAD, y, 110, true_v,
                     busy || unsaved) &&
        !vkr_editor_depot_commit(depot, depot->message, false_v,
                                 VKR_EDITOR_DEPOT_UNTRACKED_STOP, error,
                                 sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    float32_t x = DEPOT_PAD + 118.0f;
    if (depot->remote_count) {
      if (depot_button(editor, ui, "commit.sync", "Commit & Sync", x, y, 130,
                       false_v, busy || unsaved) &&
          !vkr_editor_depot_commit(depot, depot->message, true_v,
                                   VKR_EDITOR_DEPOT_UNTRACKED_STOP, error,
                                   sizeof(error))) {
        string_format(depot->error, sizeof(depot->error), "%s", error);
      }
      x += 138.0f;
    }
    if (unsaved) {
      VkrUiWidgetConfig note =
          depot_widget(x + 6.0f, y + 4, width - x - DEPOT_PAD - 6.0f, 20);
      note.style.font_size_pt = theme->font_caption;
      note.style.text_color = theme->warning;
      note.icon = VKR_UI_ICON_SAVE;
      note.icon_size_pt = 12.0f;
      note.icon_color = theme->warning;
      vkr_ui_label(ui, string8_lit("commit.unsaved"),
                   string8_lit("Save the scene to commit its edits"), &note);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

// -----------------------------------------------------------------------------
// History
// -----------------------------------------------------------------------------

/* A commit row the History sorts: one text or number by column. */
typedef struct DepotCommitSort {
  char text[VKR_DEPOT_DISPLAY_NAME + 16];
  uint64_t number;
  uint32_t index;
} DepotCommitSort;

static int32_t depot_commit_sort_compare(const void *lhs, const void *rhs) {
  const DepotCommitSort *a = lhs;
  const DepotCommitSort *b = rhs;
  if (a->number != b->number) {
    return a->number < b->number ? -1 : 1;
  }
  const int32_t order = string_compare(a->text, b->text);
  return order ? order : (a->index < b->index ? -1 : a->index > b->index);
}

enum {
  DEPOT_COMMIT_COLUMN_MARK = 0,
  DEPOT_COMMIT_COLUMN_MESSAGE,
  DEPOT_COMMIT_COLUMN_AUTHOR,
  DEPOT_COMMIT_COLUMN_DATE,
  DEPOT_COMMIT_COLUMN_ID,
  DEPOT_COMMIT_COLUMN_COUNT,
};

/* The History's columns by the room it has: every column from 600
   points, without the id from 460, and without the author below that, so
   the message keeps its room. */
static const VkrEditorTableColumn depot_commit_columns[] = {
    {.title = "",
     .width_pt = 30.0f,
     .min_pt = 30.0f,
     .tooltip = "Up: to push. Down: to pull. Branch: a merge"},
    {.title = "Message", .width_pt = 0.0f, .min_pt = 120.0f},
    {.title = "Author", .width_pt = 130.0f, .min_pt = 60.0f},
    {.title = "Date", .width_pt = 96.0f, .min_pt = 60.0f, .numeric = true_v},
    {.title = "Commit", .width_pt = 76.0f, .min_pt = 60.0f}};
static const struct {
  float32_t min_pt;
  uint32_t count;
  uint32_t columns[DEPOT_COMMIT_COLUMN_COUNT];
} DEPOT_COMMIT_LAYOUTS[] = {
    {600.0f,
     5u,
     {DEPOT_COMMIT_COLUMN_MARK, DEPOT_COMMIT_COLUMN_MESSAGE,
      DEPOT_COMMIT_COLUMN_AUTHOR, DEPOT_COMMIT_COLUMN_DATE,
      DEPOT_COMMIT_COLUMN_ID}},
    {460.0f,
     4u,
     {DEPOT_COMMIT_COLUMN_MARK, DEPOT_COMMIT_COLUMN_MESSAGE,
      DEPOT_COMMIT_COLUMN_AUTHOR, DEPOT_COMMIT_COLUMN_DATE}},
    {0.0f,
     3u,
     {DEPOT_COMMIT_COLUMN_MARK, DEPOT_COMMIT_COLUMN_MESSAGE,
      DEPOT_COMMIT_COLUMN_DATE}}};

/* The commits of the branch and of its remote, newest first: a mark for
   those to push, to pull and merges, the message, the author, when, and
   the short id; a click shows the commit's details. */
static void depot_commit_table(VkrEditorDepot *depot, VkrUiSystem *ui,
                               VkrUiPlacement placement, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  uint32_t layout = 0u;
  while (layout + 1u < ArrayCount(DEPOT_COMMIT_LAYOUTS) &&
         width < DEPOT_COMMIT_LAYOUTS[layout].min_pt) {
    ++layout;
  }
  const uint32_t *shown = DEPOT_COMMIT_LAYOUTS[layout].columns;
  const uint32_t column_count = DEPOT_COMMIT_LAYOUTS[layout].count;
  VkrEditorTableColumn columns[DEPOT_COMMIT_COLUMN_COUNT];
  for (uint32_t c = 0u; c < column_count; ++c) {
    columns[c] = depot_commit_columns[shown[c]];
  }
  VkrEditorTable *table = &depot->history_tables[layout];
  table->selected_key = depot_commit_key(depot->selected_commit);
  const uint32_t count = depot->history_count;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "history.commits", table, columns,
                              column_count, count, placement, &sorted, &first,
                              &end)) {
    return;
  }
  /* The walk's order is newest first; another column sorts a copy. */
  const uint32_t sort = table->sort_column < column_count
                            ? shown[table->sort_column]
                            : VKR_EDITOR_TABLE_NONE;
  DepotCommitSort order[DEPOT_HISTORY_MAX];
  for (uint32_t i = 0u; i < count; ++i) {
    const DepotHistory *row = &depot->history[i];
    order[i] = (DepotCommitSort){.index = i};
    switch (sort) {
    case DEPOT_COMMIT_COLUMN_MESSAGE:
      string_format(order[i].text, sizeof(order[i].text), "%s", row->message);
      break;
    case DEPOT_COMMIT_COLUMN_AUTHOR:
      depot_author_text(depot, row, order[i].text, sizeof(order[i].text));
      break;
    case DEPOT_COMMIT_COLUMN_DATE:
      order[i].number = row->time_us;
      break;
    case DEPOT_COMMIT_COLUMN_ID:
      string_format(order[i].text, sizeof(order[i].text), "%s", row->id);
      break;
    case DEPOT_COMMIT_COLUMN_MARK:
      order[i].number =
          row->flags & (DEPOT_COMMIT_TO_PUSH | DEPOT_COMMIT_TO_PULL);
      break;
    default:
      break;
    }
  }
  if (sort != VKR_EDITOR_TABLE_NONE) {
    vkr_sort(order, count, sizeof(order[0]), depot_commit_sort_compare);
    if (table->sort_descending) {
      for (uint32_t i = 0u; i < count / 2u; ++i) {
        const DepotCommitSort swap = order[i];
        order[i] = order[count - 1u - i];
        order[count - 1u - i] = swap;
      }
    }
  }
  const uint64_t now_us = depot_unix_us();
  for (uint32_t r = first; r < end && r < count; ++r) {
    const DepotHistory *row = &depot->history[order[r].index];
    char author[VKR_DEPOT_DISPLAY_NAME + 16];
    char when[32];
    char id[9];
    depot_author_text(depot, row, author, sizeof(author));
    depot_format_when(now_us, row->time_us, when, sizeof(when));
    string_format(id, sizeof(id), "%.8s", row->id);
    VkrUiIcon mark =
        row->parent_count > 1u ? VKR_UI_ICON_GIT_BRANCH : VKR_UI_ICON_DOT;
    Vec4 mark_color =
        row->parent_count > 1u ? theme->text_secondary : theme->text_disabled;
    if (row->flags & DEPOT_COMMIT_TO_PUSH) {
      mark = VKR_UI_ICON_ARROW_UP;
      mark_color = theme->success;
    } else if (row->flags & DEPOT_COMMIT_TO_PULL) {
      mark = VKR_UI_ICON_ARROW_DOWN;
      mark_color = theme->accent_hover;
    }
    String8 cells[DEPOT_COMMIT_COLUMN_COUNT];
    Vec4 colors[DEPOT_COMMIT_COLUMN_COUNT];
    VkrUiIcon icons[DEPOT_COMMIT_COLUMN_COUNT];
    for (uint32_t c = 0u; c < column_count; ++c) {
      icons[c] = VKR_UI_ICON_NONE;
      colors[c] = theme->text_secondary;
      switch (shown[c]) {
      case DEPOT_COMMIT_COLUMN_MARK:
        cells[c] = (String8){0};
        icons[c] = mark;
        colors[c] = mark_color;
        break;
      case DEPOT_COMMIT_COLUMN_MESSAGE:
        cells[c] =
            depot_string(row->message[0] ? row->message : "(no message)");
        colors[c] = (Vec4){0};
        break;
      case DEPOT_COMMIT_COLUMN_AUTHOR:
        cells[c] = depot_string(author);
        break;
      case DEPOT_COMMIT_COLUMN_DATE:
        cells[c] = depot_string(when);
        break;
      default:
        cells[c] = depot_string(id);
        colors[c] = theme->text_disabled;
        break;
      }
    }
    /* The hover text lives past the build: the stored message. */
    VkrEditorTableRow view = {.key = depot_commit_key(row->id),
                              .cells = cells,
                              .cell_colors = colors,
                              .cell_icons = icons,
                              .tooltip = depot_string(row->message)};
    /* Commits only the remote has are not here yet. */
    if (row->flags & DEPOT_COMMIT_TO_PULL) {
      view.text_color = theme->text_secondary;
    }
    if (vkr_editor_table_row(ui, table, columns, column_count, r, &view)) {
      string_format(depot->selected_commit, sizeof(depot->selected_commit),
                    "%s", row->id);
    }
  }
  vkr_editor_table_end(ui);
}

/* One labelled line of the details: the label in a column of its own. */
static void depot_detail_line(VkrUiSystem *ui, const char *id,
                              const char *label, const char *value, float32_t y,
                              float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char label_id[64];
  string_format(label_id, sizeof(label_id), "%s.label", id);
  depot_caption(ui, label_id, label, DEPOT_PAD, y + 2, 64);
  VkrUiWidgetConfig c = depot_widget(DEPOT_PAD + 66.0f, y, width - 90.0f, 22);
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = theme->text;
  vkr_ui_label(ui, depot_string(id), depot_string(value), &c);
}

/* The selected commit: its message, branches, author, date, id, parents
   and the files it changed against its first parent. */
static void depot_commit_details(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 VkrUiSystem *ui, VkrUiPlacement placement,
                                 float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const DepotHistory *row = depot_history_find(depot, depot->selected_commit);
  const DepotInspect *inspect = depot_inspected(depot);
  /* The body of the message: what follows its first line. */
  const char *body = NULL;
  if (inspect && inspect->ok) {
    body = string_find_char(inspect->message, '\n');
    while (body && (*body == '\n' || *body == '\r')) {
      ++body;
    }
    if (body && !body[0]) {
      body = NULL;
    }
  }
  const float32_t chars_per_line = Max(16.0f, (width - 2 * DEPOT_PAD) / 6.4f);
  const float32_t body_pt =
      body ? (float32_t)((uint32_t)((float32_t)string_length(body) /
                                    chars_per_line) +
                         1u) *
                     17.0f +
                 8.0f
           : 0.0f;
  /* The title wraps to at most three lines; the badges take a row only
     when there are any. */
  const char *title_text =
      row && row->message[0] ? row->message : "(no message)";
  const float32_t title_lines = Min(
      3.0f, (float32_t)((uint32_t)((float32_t)string_length(title_text) /
                                   Max(12.0f, (width - 2 * DEPOT_PAD) / 7.8f)) +
                        1u));
  const float32_t title_pt = title_lines * 20.0f + 6.0f;
  const uint32_t badge_flags = DEPOT_COMMIT_HEAD | DEPOT_COMMIT_REMOTE_HEAD |
                               DEPOT_COMMIT_TO_PUSH | DEPOT_COMMIT_TO_PULL;
  const bool8_t badges =
      row && ((row->flags & badge_flags) || row->parent_count > 1u);
  const float32_t meta_pt = row ? 10.0f + title_pt + body_pt +
                                      (badges ? 28.0f : 4.0f) + 4.0f * 24.0f +
                                      30.0f
                                : 0.0f;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {{.value = meta_pt, .unit = VKR_UI_TRACK_PX}, fill};
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement = placement;
  pane.columns = &fill;
  pane.column_count = 1u;
  pane.rows = rows;
  pane.row_count = ArrayCount(rows);
  pane.style.gap_pt = 0.0f;
  pane.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("history.details"), &pane)) {
    return;
  }
  if (!row) {
    if (depot_pane_begin(ui, "details.none", depot_cell(0u, 1u), (Vec4){0})) {
      depot_empty(ui, "details.empty", VKR_UI_ICON_GIT_BRANCH,
                  "Select a commit", "Its message, author and files show here",
                  40.0f, width);
      (void)vkr_ui_panel_end(ui);
    }
    (void)vkr_ui_panel_end(ui);
    return;
  }
  if (depot_pane_begin(ui, "details.meta", depot_cell(0u, 0u), (Vec4){0})) {
    float32_t y = 10.0f;
    VkrUiWidgetConfig title =
        depot_widget(DEPOT_PAD, y, width - 2 * DEPOT_PAD, title_pt - 4.0f);
    title.text.font = editor->heading_font;
    title.style.font_size_pt = theme->font_emphasis;
    title.text.layout.word_wrap = true_v;
    title.text.layout.max_width = width - 2 * DEPOT_PAD - 8.0f;
    vkr_ui_label(ui, string8_lit("details.title"), depot_string(title_text),
                 &title);
    y += title_pt;
    if (body) {
      y += depot_paragraph(ui, "details.body", body, DEPOT_PAD, y,
                           width - 2 * DEPOT_PAD);
      y += 4.0f;
    }
    /* Where it stands: the branches at it, and what a push or pull does. */
    float32_t x = DEPOT_PAD;
    const VkrDepotRemoteEntry *remote = depot_selected(depot);
    char text[DEPOT_TEXT];
    if (row->flags & DEPOT_COMMIT_HEAD) {
      x += depot_badge(ui, "badge.head", depot->branch, x, y + 2,
                       theme->accent_hover) +
           6.0f;
    }
    if ((row->flags & DEPOT_COMMIT_REMOTE_HEAD) && remote) {
      string_format(text, sizeof(text), "%s/%s", remote->name, depot->branch);
      x += depot_badge(ui, "badge.remote", text, x, y + 2, theme->info) + 6.0f;
    }
    if (row->flags & DEPOT_COMMIT_TO_PUSH) {
      x += depot_badge(ui, "badge.push", "to push", x, y + 2, theme->success) +
           6.0f;
    }
    if (row->flags & DEPOT_COMMIT_TO_PULL) {
      x += depot_badge(ui, "badge.pull", "to pull", x, y + 2,
                       theme->accent_hover) +
           6.0f;
    }
    if (row->parent_count > 1u) {
      x += depot_badge(ui, "badge.merge", "merge", x, y + 2,
                       theme->text_secondary) +
           6.0f;
    }
    y += badges ? 28.0f : 4.0f;
    /* A computer without a name shows its key already. */
    char author[VKR_DEPOT_DISPLAY_NAME + 16];
    char print[16];
    depot_author_text(depot, row, author, sizeof(author));
    depot_fingerprint(row->author, print);
    if (string_find(author, "Computer ") == author) {
      string_format(text, sizeof(text), "%s", author);
    } else {
      string_format(text, sizeof(text), "%s  \xc2\xb7  key %s", author, print);
    }
    depot_detail_line(ui, "details.author", "Author", text, y, width);
    y += 24.0f;
    char date[48];
    char age[32];
    depot_format_date(row->time_us, true_v, date, sizeof(date));
    depot_format_age(depot_unix_us(), row->time_us, age, sizeof(age));
    /* The age goes where the date leaves room for it. */
    string_format(text, sizeof(text), "%s  \xc2\xb7  %s", date, age);
    if (90.0f + 6.6f * (float32_t)string_length(text) > width) {
      string_format(text, sizeof(text), "%s", date);
    }
    depot_detail_line(ui, "details.date", "Date", text, y, width);
    y += 24.0f;
    string_format(text, sizeof(text), "%.16s", row->id);
    depot_detail_line(ui, "details.id", "Commit", text, y, width);
    VkrUiWidgetConfig copy =
        depot_widget(DEPOT_PAD + 66.0f + 150.0f, y - 2, 26, 24);
    vkr_editor_ghost_style(&copy);
    copy.icon = VKR_UI_ICON_COPY;
    copy.icon_size_pt = 12.0f;
    copy.icon_color = theme->text_secondary;
    copy.tooltip = string8_lit("Copy the whole id");
    if (vkr_ui_button(ui, string8_lit("details.copy"), (String8){0}, &copy)) {
      (void)vkr_platform_clipboard_write_text((const uint8_t *)row->id,
                                              (uint32_t)string_length(row->id));
    }
    y += 24.0f;
    /* Parents, each a link to it when the History lists it. */
    depot_caption(ui, "details.parents.label",
                  row->parent_count > 1u ? "Parents" : "Parent", DEPOT_PAD,
                  y + 2, 64);
    if (!inspect) {
      depot_caption(ui, "details.parents", "\xe2\x80\xa6", DEPOT_PAD + 66.0f,
                    y + 2, 60);
    } else if (!inspect->parent_count) {
      depot_caption(ui, "details.parents", "none: the first commit",
                    DEPOT_PAD + 66.0f, y + 2, width - 90.0f);
    }
    x = DEPOT_PAD + 66.0f;
    for (uint32_t i = 0u; inspect && i < inspect->parent_count; ++i) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&inspect->parents[i], hex);
      char short_id[9];
      string_format(short_id, sizeof(short_id), "%.8s", hex);
      const bool8_t listed = depot_history_find(depot, hex) != NULL;
      VkrUiWidgetConfig link = depot_widget(x - 4.0f, y, 78, 22);
      vkr_editor_ghost_style(&link);
      link.leading = true_v;
      link.style.padding_pt = (VkrUiEdges){2.0f, 4.0f, 2.0f, 4.0f};
      link.style.font_size_pt = theme->font_caption;
      link.style.text_color =
          listed ? theme->accent_hover : theme->text_secondary;
      link.disabled = !listed;
      link.tooltip = listed ? string8_lit("Show this commit")
                            : string8_lit("Older than the commits listed");
      (void)vkr_ui_push_id_u64(ui, i);
      if (vkr_ui_button(ui, string8_lit("details.parent"),
                        depot_string(short_id), &link)) {
        string_format(depot->selected_commit, sizeof(depot->selected_commit),
                      "%s", hex);
      }
      (void)vkr_ui_pop_id(ui);
      x += 82.0f;
    }
    y += 30.0f;
    /* The files' summary over their table. */
    if (!inspect) {
      depot_caption(ui, "details.files", "Reading the commit\xe2\x80\xa6",
                    DEPOT_PAD, y, width - 2 * DEPOT_PAD);
    } else if (!inspect->ok) {
      depot_label(ui, "details.files", inspect->error, DEPOT_PAD, y - 2,
                  width - 2 * DEPOT_PAD, theme->warning);
    } else {
      string_format(text, sizeof(text), "%u file%s changed",
                    inspect->file_count, inspect->file_count == 1u ? "" : "s");
      const float32_t files_pt = 20.0f + 7.6f * (float32_t)string_length(text);
      VkrUiWidgetConfig files = depot_widget(DEPOT_PAD, y - 2, files_pt, 22);
      files.text.font = editor->heading_font;
      vkr_ui_label(ui, string8_lit("details.files"), depot_string(text),
                   &files);
      x = DEPOT_PAD + files_pt + 4.0f;
      /* The counts by kind, in words where they fit. */
      const bool8_t words = width - x > 250.0f;
      const struct {
        uint32_t count;
        const char *format;
        Vec4 color;
      } parts[] = {
          {inspect->added, words ? "+%u added" : "+%u", theme->success},
          {inspect->modified, words ? "~%u modified" : "~%u", theme->warning},
          {inspect->deleted, words ? "-%u deleted" : "-%u", theme->error}};
      for (uint32_t i = 0u; i < ArrayCount(parts); ++i) {
        if (!parts[i].count) {
          continue;
        }
        string_format(text, sizeof(text), parts[i].format, parts[i].count);
        const float32_t part_pt = 12.0f + 6.6f * (float32_t)string_length(text);
        if (x + part_pt > width - DEPOT_PAD) {
          break;
        }
        VkrUiWidgetConfig part = depot_widget(x, y, part_pt, 18);
        part.style.font_size_pt = theme->font_caption;
        part.style.text_color = parts[i].color;
        (void)vkr_ui_push_id_u64(ui, i);
        vkr_ui_label(ui, string8_lit("details.part"), depot_string(text),
                     &part);
        (void)vkr_ui_pop_id(ui);
        x += part_pt + 2.0f;
      }
    }
    (void)vkr_ui_panel_end(ui);
  }
  if (inspect && inspect->ok && inspect->listed) {
    DepotFileView files[DEPOT_DETAIL_FILES_MAX];
    for (uint32_t i = 0u; i < inspect->listed; ++i) {
      files[i] = (DepotFileView){.path = inspect->files[i].path,
                                 .change = inspect->files[i].change};
    }
    VkrUiPlacement table = depot_cell(0u, 1u);
    table.margin_pt = (VkrUiEdges){0.0f, DEPOT_PAD, 10.0f, DEPOT_PAD};
    depot_files_table(ui, "details.table", &depot->files_table, files,
                      inspect->listed, table, true_v);
  }
  (void)vkr_ui_panel_end(ui);
}

static void depot_tab_history(VkrEditorDepot *depot, VkrEditorUi *editor,
                              VkrUiSystem *ui, VkrUiPlacement placement,
                              float32_t width, float32_t height) {
  if (!depot->history_count) {
    if (depot_pane_begin(ui, "history.empty", placement, (Vec4){0})) {
      depot_empty(ui, "history.none", VKR_UI_ICON_CLOCK, "No commits yet",
                  "Commit your changes to start the history", 40.0f, width);
      (void)vkr_ui_panel_end(ui);
    }
    return;
  }
  /* The newest commit is shown until another is chosen; a selection the
     history lost (a switch, a pull) goes back to it. */
  if (!depot_history_find(depot, depot->selected_commit)) {
    string_format(depot->selected_commit, sizeof(depot->selected_commit), "%s",
                  depot->history[0].id);
  }
  VkrEditorSplit *split = &depot->history_split;
  if (split->max_ratio <= 0.0f) {
    *split =
        (VkrEditorSplit){.ratio = 0.6f, .min_ratio = 0.35f, .max_ratio = 0.78f};
  }
  const bool8_t side = width >= DEPOT_SIDE_BY_SIDE_PT;
  const VkrUiTrack tracks[] = {
      {.value = split->ratio, .unit = VKR_UI_TRACK_FR},
      {.value = 6.0f, .unit = VKR_UI_TRACK_PX},
      {.value = 1.0f - split->ratio, .unit = VKR_UI_TRACK_FR}};
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig pane = vkr_ui_panel_config_default();
  pane.placement = placement;
  pane.columns = side ? tracks : &fill;
  pane.column_count = side ? ArrayCount(tracks) : 1u;
  pane.rows = side ? &fill : tracks;
  pane.row_count = side ? 1u : ArrayCount(tracks);
  pane.style.gap_pt = 0.0f;
  pane.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("history"), &pane)) {
    return;
  }
  VkrUiPlacement commits = depot_cell(0u, 0u);
  commits.margin_pt = side ? (VkrUiEdges){8.0f, 0.0f, 10.0f, DEPOT_PAD}
                           : (VkrUiEdges){8.0f, DEPOT_PAD, 0.0f, DEPOT_PAD};
  depot_commit_table(depot, ui, commits,
                     (side ? width * split->ratio : width) - 2 * DEPOT_PAD);
  vkr_editor_split_bar(ui, "history.split", split,
                       side ? depot_cell(1u, 0u) : depot_cell(0u, 1u), !side,
                       side ? width : height);
  const float32_t details_pt =
      side ? width * (1.0f - split->ratio) - 6.0f : width;
  depot_commit_details(depot, editor, ui,
                       side ? depot_cell(2u, 0u) : depot_cell(0u, 2u),
                       details_pt);
  (void)vkr_ui_panel_end(ui);
}

// -----------------------------------------------------------------------------
// Remotes
// -----------------------------------------------------------------------------

static void depot_tab_remotes(VkrEditorDepot *depot, VkrEditorUi *editor,
                              VkrUiSystem *ui, float32_t *y, float32_t width,
                              bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char error[DEPOT_TEXT] = {0};
  *y += 4.0f;
  if (!depot->remote_count && !depot->remote_form_open) {
    depot_empty(ui, "remotes.none", VKR_UI_ICON_CLOUD, "No remotes yet",
                "A remote is another computer's repository of this project",
                *y + 20.0f, width);
    *y += 110.0f;
    if (depot_button(editor, ui, "remotes.add", "Add remote",
                     width / 2.0f - 60.0f, *y, 120, true_v, false_v)) {
      depot->remote_form_open = true_v;
    }
    *y += 40.0f;
    return;
  }
  for (uint32_t i = 0u; i < depot->remote_count; ++i) {
    const VkrDepotRemoteEntry *remote = &depot->remotes[i];
    const bool8_t chosen = i == depot->selected_remote;
    (void)vkr_ui_push_id_u64(ui, i);
    depot_alert_card(ui, "card", *y, width, 56.0f,
                     chosen ? theme->accent : theme->border);
    VkrUiWidgetConfig name = depot_widget(DEPOT_PAD + 12.0f, *y + 7, 260, 22);
    name.text.font = editor->heading_font;
    name.icon = VKR_UI_ICON_CLOUD;
    name.icon_size_pt = 14.0f;
    name.icon_color = depot_remote_color(depot, i);
    vkr_ui_label(ui, string8_lit("name"), depot_string(remote->name), &name);
    if (chosen) {
      depot_badge(ui, "chosen", "syncs",
                  DEPOT_PAD + 26.0f +
                      7.4f * (float32_t)string_length(remote->name) + 24.0f,
                  *y + 9, theme->accent_hover);
    }
    char text[DEPOT_TEXT];
    char print[16];
    depot_fingerprint(remote->key, print);
    string_format(text, sizeof(text), "%s  \xc2\xb7  key %s  \xc2\xb7  %s",
                  remote->address, print, depot_remote_state(depot, i));
    depot_caption(ui, "detail", text, DEPOT_PAD + 12.0f, *y + 31,
                  width - 2 * DEPOT_PAD - 220.0f);
    if (!chosen &&
        depot_button(editor, ui, "use", "Sync with it",
                     width - DEPOT_PAD - 220.0f, *y + 14, 112, false_v, busy)) {
      depot->selected_remote = i;
      depot->next_status = 0.0;
    }
    if (depot_button(editor, ui, "remove", "Remove", width - DEPOT_PAD - 100.0f,
                     *y + 14, 88, false_v, busy) &&
        !vkr_editor_depot_remote(depot, remote->name, NULL, NULL, error,
                                 sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 64.0f;
  }
  if (!depot->remote_form_open) {
    if (depot_icon_button(editor, ui, "remotes.add", "Add remote",
                          VKR_UI_ICON_ADD, DEPOT_PAD, *y + 4.0f, 130, false_v,
                          false_v, NULL)) {
      depot->remote_form_open = true_v;
    }
    *y += 40.0f;
    return;
  }
  depot->browse_wanted = true_v;
  *y += 6.0f;
  VkrUiWidgetConfig heading = depot_widget(DEPOT_PAD, *y, 300, 22);
  heading.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("remote.form"), string8_lit("Add a remote"),
               &heading);
  *y += 28.0f;
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
    VkrUiWidgetConfig row = depot_widget(DEPOT_PAD, *y + 3, width - 150, 22);
    row.style.font_size_pt = theme->font_caption;
    row.style.text_color = theme->text;
    row.icon = VKR_UI_ICON_WORLD;
    row.icon_size_pt = 12.0f;
    row.icon_color = theme->text_secondary;
    vkr_ui_label(ui, string8_lit("found"), depot_string(text), &row);
    if (depot_button(editor, ui, "use", "Use", width - DEPOT_PAD - 88.0f, *y,
                     88, false_v, false_v)) {
      string_format(depot->remote_address, sizeof(depot->remote_address), "%s",
                    address);
      depot_key_hex(found->server_key, depot->remote_key);
      if (!depot->remote_name[0]) {
        string_format(depot->remote_name, sizeof(depot->remote_name), "%s",
                      depot->remote_count ? "peer" : VKR_DEPOT_ORIGIN);
      }
    }
    (void)vkr_ui_pop_id(ui);
    *y += 32.0f;
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
    depot_caption(ui, labels[i], labels[i], DEPOT_PAD, *y + 4, 70);
    depot_field(ui, ids[i], fields[i], sizes[i], DEPOT_PAD + 72.0f, *y,
                width - 2 * DEPOT_PAD - 72.0f);
    *y += 32.0f;
  }
  if (depot_button(editor, ui, "remote.save", "Add", DEPOT_PAD + 72.0f, *y, 100,
                   true_v,
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
  if (depot_button(editor, ui, "remote.cancel", "Cancel", DEPOT_PAD + 180.0f,
                   *y, 100, false_v, false_v)) {
    depot->remote_form_open = false_v;
  }
  *y += 40.0f;
}

// -----------------------------------------------------------------------------
// Sharing
// -----------------------------------------------------------------------------

static void depot_tab_sharing(VkrEditorDepot *depot, VkrEditorUi *editor,
                              VkrUiSystem *ui, float32_t *y, float32_t width,
                              bool8_t busy) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  *y += 8.0f;
  bool8_t serve = depot->serving;
  VkrUiWidgetConfig toggle = depot_widget(DEPOT_PAD, *y, width - 24, 22);
  toggle.tooltip = string8_lit(
      "This computer's depot service serves the repository, so other "
      "computers on your network can clone it and push to it, also while "
      "the editor is closed");
  (void)vkr_ui_checkbox(ui, string8_lit("serve"),
                        string8_lit("Let others fetch and push"), &serve,
                        &toggle);
  if (serve != depot->serving) {
    char error[DEPOT_TEXT] = {0};
    if (!vkr_editor_depot_serve(depot, serve, error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
  }
  *y += 30.0f;
  if (!depot->serving) {
    *y += depot_paragraph(
        ui, "server.off",
        depot->served_before
            ? "An older editor served this repository while it ran. Turn "
              "this on to serve it from this computer's depot service."
            : "Other computers can clone this repository and push to it "
              "while this computer's depot service serves it.",
        DEPOT_PAD + 26.0f, *y, width - 2 * DEPOT_PAD - 26.0f);
    return;
  }
  if (!depot->machine_known) {
    depot_caption(ui, "server.checking",
                  "Checking this computer's depot service\xe2\x80\xa6",
                  DEPOT_PAD + 26.0f, *y, width - 60);
    *y += 26.0f;
    return;
  }
  if (!depot->machine_running) {
    depot_label(ui, "server.stopped",
                "This computer's depot service is not running",
                DEPOT_PAD + 26.0f, *y, width - 180, theme->warning);
    if (depot_button(editor, ui, "server.start", "Start",
                     width - DEPOT_PAD - 100.0f, *y - 4, 88, false_v, busy)) {
      char error[DEPOT_TEXT] = {0};
      if (!vkr_editor_depot_start_service(depot, error, sizeof(error))) {
        string_format(depot->error, sizeof(depot->error), "%s", error);
      }
    }
    *y += 32.0f;
    return;
  }
  char host[VKR_DEPOT_DISCOVERY_TEXT];
  char print[16];
  vkr_depot_host_name(host, sizeof(host));
  depot_fingerprint(depot->machine_key, print);
  depot_detail_line(ui, "server.host", "Served by", host, *y, width);
  *y += 24.0f;
  string_format(text, sizeof(text), "%u", (uint32_t)depot->machine_port);
  depot_detail_line(ui, "server.port", "Port", text, *y, width);
  *y += 24.0f;
  depot_detail_line(ui, "server.key", "Key", print, *y, width);
  *y += 24.0f;
  string_format(text, sizeof(text), "%u connected now",
                depot->machine_connected);
  depot_detail_line(ui, "server.peers", "Others", text, *y, width);
  *y += 28.0f;
  if (depot->machine_outbound_only) {
    *y += depot_paragraph(
        ui, "server.outbound",
        "This computer takes no connections from others. Its service "
        "fetches and pushes through the remotes that have an interval, and "
        "others reach it only while it is connected to them.",
        DEPOT_PAD, *y, width - 2 * DEPOT_PAD);
    *y += 6.0f;
  }
  uint32_t shown = 0u;
  for (uint32_t i = 0u; i < depot->access_count; ++i) {
    shown += depot->access[i].role != VKR_DEPOT_ROLE_ADMIN;
  }
  VkrUiWidgetConfig heading = depot_widget(DEPOT_PAD, *y + 4, 300, 22);
  heading.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("access.title"),
               string8_lit("Computers with access"), &heading);
  *y += 32.0f;
  if (!shown) {
    depot_caption(ui, "access.none",
                  "None yet: a computer that asks shows a card above",
                  DEPOT_PAD, *y, width - 2 * DEPOT_PAD);
    *y += 26.0f;
    return;
  }
  for (uint32_t i = 0u; i < depot->access_count; ++i) {
    const VkrDepotAccessEntry *entry = &depot->access[i];
    if (entry->role == VKR_DEPOT_ROLE_ADMIN) {
      continue;
    }
    (void)vkr_ui_push_id_u64(ui, i);
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(entry->key, key);
    char who[VKR_DEPOT_DISPLAY_NAME + 16];
    depot_person_text(depot, entry->key, NULL, who, sizeof(who));
    depot_alert_card(ui, "entry.card", *y, width, 40.0f, theme->border);
    depot_label(ui, "entry", who, DEPOT_PAD + 12.0f, *y + 9, width - 260,
                theme->text);
    depot_badge(ui, "role", depot_role_name(entry->role),
                width - DEPOT_PAD - 200.0f, *y + 11, theme->text_secondary);
    char error[DEPOT_TEXT] = {0};
    if (depot_button(editor, ui, "remove", "Remove", width - DEPOT_PAD - 100.0f,
                     *y + 6, 88, false_v, false_v)) {
      (void)vkr_editor_depot_allow(depot, key, "none", error, sizeof(error));
    }
    (void)vkr_ui_pop_id(ui);
    *y += 48.0f;
  }
}

// -----------------------------------------------------------------------------
// Window
// -----------------------------------------------------------------------------

/* A tab whose content is a column of rows that scrolls as one. */
static void depot_scroll_tab(VkrEditorDepot *depot, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame,
                             VkrUiPlacement placement, VkrUiRect bounds,
                             float32_t width, float32_t height, bool8_t busy) {
  VkrUiSystem *ui = frame->ui;
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
  scroll.placement = placement;
  scroll.rows = &track;
  scroll.row_count = 1;
  scroll.clip_children = true_v;
  if (!vkr_ui_scroll_area_begin(ui, string8_lit("depot.scroll"), &scroll)) {
    return;
  }
  (void)vkr_ui_scroll_area_offset(ui, &depot->window_scroll);
  float32_t y = 0.0f;
  if (depot->tab == DEPOT_TAB_REMOTES) {
    depot_tab_remotes(depot, editor, ui, &y, width, busy);
  } else {
    depot_tab_sharing(depot, editor, ui, &y, width, busy);
  }
  depot->window_height = y + 12.0f;
  (void)vkr_ui_scroll_area_end(ui);
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
  depot_inspect_update(depot);
  const bool8_t busy =
      depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS;
  if (!depot->project_id[0] || !depot->repository) {
    /* No repository yet: the form that makes one, or what is under way. */
    if (!depot_pane_begin(ui, "depot.layout", depot_cell(0u, 0u), (Vec4){0})) {
      return;
    }
    float32_t y = 10;
    if (!depot->project_id[0]) {
      depot_empty(ui, "none", VKR_UI_ICON_PROJECT, "No project open",
                  "Open a project to use source control", 40.0f, width);
    } else if (!depot_busy(depot)) {
      depot_window_not_repository(depot, editor, frame, ui, &y, width);
    } else if (busy) {
      depot_alert_progress(depot, editor, ui, &y, width);
    }
    if (depot->error[0]) {
      depot_alert_error(depot, ui, &y, width);
    }
    (void)vkr_ui_panel_end(ui);
    return;
  }
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = DEPOT_TOOLBAR_PT, .unit = VKR_UI_TRACK_PX},
      {.value = depot->top_height, .unit = VKR_UI_TRACK_PX},
      {.value = DEPOT_TABS_PT, .unit = VKR_UI_TRACK_PX},
      fill};
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.columns = &fill;
  body.column_count = 1u;
  body.rows = rows;
  body.row_count = ArrayCount(rows);
  body.style.gap_pt = 0.0f;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("depot.layout"), &body)) {
    return;
  }
  if (depot_pane_begin(ui, "depot.toolbar", depot_cell(0u, 0u),
                       theme->header)) {
    depot_toolbar(depot, editor, ui, width, busy);
    (void)vkr_ui_panel_end(ui);
  }
  if (depot_pane_begin(ui, "depot.top", depot_cell(0u, 1u), (Vec4){0})) {
    float32_t y = 8.0f;
    depot_top(depot, editor, ui, &y, width, busy);
    /* The row takes this height from the next build. */
    depot->top_height = y > 8.0f ? y : 0.0f;
    (void)vkr_ui_panel_end(ui);
  }
  if (depot_pane_begin(ui, "depot.tabs", depot_cell(0u, 2u), (Vec4){0})) {
    depot_tabs(depot, editor, ui, width);
    (void)vkr_ui_panel_end(ui);
  }
  const float32_t content_pt =
      Max(80.0f, height - DEPOT_TOOLBAR_PT - depot->top_height - DEPOT_TABS_PT);
  VkrUiRect content = bounds;
  content.y += (DEPOT_TOOLBAR_PT + depot->top_height + DEPOT_TABS_PT) *
               ui->content_scale;
  content.height = content_pt * ui->content_scale;
  switch (depot->tab) {
  case DEPOT_TAB_CHANGES:
    depot_tab_changes(depot, editor, frame, ui, depot_cell(0u, 3u), width,
                      busy);
    break;
  case DEPOT_TAB_HISTORY:
    depot_tab_history(depot, editor, ui, depot_cell(0u, 3u), width, content_pt);
    break;
  default:
    depot_scroll_tab(depot, editor, frame, depot_cell(0u, 3u), content, width,
                     content_pt, busy);
    break;
  }
  (void)vkr_ui_panel_end(ui);
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

static float32_t depot_hub_dialog(VkrEditorDepot *depot, VkrEditorUi *editor,
                                  VkrUiSystem *ui, float32_t x, float32_t y,
                                  float32_t width);

/* One repository a service on the network serves, with Clone; returns its
   height. */
static float32_t depot_hub_found(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 VkrUiSystem *ui,
                                 const VkrDepotDiscovered *found,
                                 const char *name, const uint8_t *repository,
                                 float32_t x, float32_t y, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t top = y;
  char text[DEPOT_TEXT];
  char address[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&found->address, address, sizeof(address));
  depot_card(ui, "card", x, y, width, 52);
  depot_row_title(ui, name, VKR_UI_ICON_CLOUD, x + 12, y + 6, width - 150);
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
      VkrUiWidgetConfig warning = depot_widget(x + 36, y + 46, width - 170, 18);
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
      MemCompare(depot->dialog_key, found->server_key, 32u) == 0 &&
      MemCompare(depot->dialog_repository, repository,
                 sizeof(depot->dialog_repository)) == 0;
  if (!chosen && depot_button(editor, ui, "get", "Clone", x + width - 112,
                              y + 13, 100, true_v, depot_busy(depot))) {
    depot->dialog_open = true_v;
    depot->error[0] = '\0';
    string_format(depot->dialog_name, sizeof(depot->dialog_name), "%s", name);
    string_format(depot->dialog_host, sizeof(depot->dialog_host), "%s",
                  found->host);
    string_format(depot->dialog_address, sizeof(depot->dialog_address), "%s",
                  address);
    MemCopy(depot->dialog_key, found->server_key, 32u);
    MemCopy(depot->dialog_repository, repository,
            sizeof(depot->dialog_repository));
    depot_default_destination(name, depot->dialog_destination,
                              sizeof(depot->dialog_destination));
  }
  y += 60;
  if (chosen) {
    y += depot_hub_dialog(depot, editor, ui, x + 24, y, width - 24) + 8;
  }
  return y - top;
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
    char repository[VKR_DEPOT_REPOSITORY_ID_SIZE * 2u + 1u];
    depot_key_hex(depot->dialog_key, key);
    depot_repository_hex(depot->dialog_repository, repository);
    if (depot_button(editor, ui, "get.go", "Clone", x + 12, y, 140, true_v,
                     depot_busy(depot)) &&
        !vkr_editor_depot_clone(depot, depot->dialog_address, key, repository,
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
  /* A service of version 2 lists each repository it serves; one of
     version 1 serves one, its default. */
  static const uint8_t default_repository[VKR_DEPOT_REPOSITORY_ID_SIZE] = {0};
  for (uint32_t i = 0u; depot->browser_open && i < depot->browser.found_count;
       ++i) {
    const VkrDepotDiscovered *found = &depot->browser.found[i];
    if (depot_found_is_self(depot, found)) {
      continue;
    }
    const uint32_t rows = Max(found->repository_count, 1u);
    for (uint32_t r = 0u; r < rows; ++r) {
      const VkrDepotDiscoveredRepository *listed =
          found->repository_count ? &found->repositories[r] : NULL;
      const char *name = listed ? listed->name : found->name;
      const uint8_t *repository = listed ? listed->id : default_repository;
      ++shown;
      (void)vkr_ui_push_id_u64(ui, 2000u + i * 64u + r);
      y += depot_hub_found(depot, editor, ui, found, name, repository, x, y,
                           width);
      (void)vkr_ui_pop_id(ui);
    }
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
  vkr_bakery_json_set(arena, result, "pushed",
                      vkr_bakery_json_int(arena, depot->pushed));
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
    vkr_bakery_json_set(arena, row, "outbound_only",
                        vkr_bakery_json_bool(arena, depot->remote_outbound[i]));
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
                      vkr_bakery_json_bool(arena, depot_incoming(depot) > 0u));
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
  /* The History's newest commits, as the window lists them. */
  VkrBakeryJson *history = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < Min(depot->history_count, 20u); ++i) {
    const DepotHistory *commit = &depot->history[i];
    VkrBakeryJson *row = vkr_bakery_json_object(arena);
    char author[VKR_DEPOT_ID_HEX];
    depot_key_hex(commit->author, author);
    vkr_bakery_json_set(arena, row, "id",
                        vkr_bakery_json_cstr(arena, commit->id));
    vkr_bakery_json_set(arena, row, "message",
                        vkr_bakery_json_cstr(arena, commit->message));
    vkr_bakery_json_set(arena, row, "author",
                        vkr_bakery_json_cstr(arena, author));
    vkr_bakery_json_set(arena, row, "time_us",
                        vkr_bakery_json_int(arena, (int64_t)commit->time_us));
    vkr_bakery_json_set(arena, row, "parents",
                        vkr_bakery_json_int(arena, commit->parent_count));
    vkr_bakery_json_set(
        arena, row, "to_push",
        vkr_bakery_json_bool(arena,
                             (commit->flags & DEPOT_COMMIT_TO_PUSH) != 0u));
    vkr_bakery_json_set(
        arena, row, "to_pull",
        vkr_bakery_json_bool(arena,
                             (commit->flags & DEPOT_COMMIT_TO_PULL) != 0u));
    vkr_bakery_json_append(history, row);
  }
  vkr_bakery_json_set(arena, result, "history", history);
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
  if (depot->serving) {
    VkrBakeryJson *server = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, server, "known",
                        vkr_bakery_json_bool(arena, depot->machine_known));
    vkr_bakery_json_set(arena, server, "running",
                        vkr_bakery_json_bool(arena, depot->machine_running));
    vkr_bakery_json_set(arena, server, "port",
                        vkr_bakery_json_int(arena, depot->machine_port));
    vkr_bakery_json_set(
        arena, server, "outbound_only",
        vkr_bakery_json_bool(arena, depot->machine_outbound_only));
    vkr_bakery_json_set(arena, server, "peers",
                        vkr_bakery_json_int(arena, depot->machine_connected));
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
    VkrBakeryJson *repositories = vkr_bakery_json_array(arena);
    for (uint32_t r = 0u; r < entry->repository_count; ++r) {
      VkrBakeryJson *listed = vkr_bakery_json_object(arena);
      char id[VKR_DEPOT_REPOSITORY_ID_SIZE * 2u + 1u];
      depot_repository_hex(entry->repositories[r].id, id);
      vkr_bakery_json_set(arena, listed, "id", vkr_bakery_json_cstr(arena, id));
      vkr_bakery_json_set(
          arena, listed, "name",
          vkr_bakery_json_cstr(arena, entry->repositories[r].name));
      vkr_bakery_json_append(repositories, listed);
    }
    vkr_bakery_json_set(arena, row, "repositories", repositories);
    vkr_bakery_json_append(found, row);
  }
  vkr_bakery_json_set(arena, result, "found", found);
  return result;
}

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
#define DEPOT_PROBE_SECONDS 2.0
#define DEPOT_BRANCH "main"

typedef enum DepotRole {
  DEPOT_ROLE_NONE = 0,
  /* This editor serves the workspace's store. */
  DEPOT_ROLE_SHARER,
  /* The workspace is a copy of a project another editor serves. */
  DEPOT_ROLE_MEMBER,
} DepotRole;

typedef enum DepotJobKind {
  DEPOT_JOB_NONE = 0,
  DEPOT_JOB_STATUS,
  DEPOT_JOB_SUBMIT,
  DEPOT_JOB_PULL,
  DEPOT_JOB_CLONE,
  DEPOT_JOB_SHARE,
} DepotJobKind;

static const char *const depot_job_names[] = {
    "",       "Checking", "Submitting", "Getting latest", "Getting project",
    "Sharing"};

typedef struct DepotText {
  char text[DEPOT_TEXT];
} DepotText;

typedef struct DepotHistory {
  char id[VKR_DEPOT_ID_HEX];
  char message[96];
  uint64_t time_us;
} DepotHistory;

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
  char address[DEPOT_TEXT];
  uint8_t server_key[32];
  bool8_t save_remote;
  char message[DEPOT_TEXT];
  VkrDepotConflictPolicy policy;
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
  /* Paths a pull or clone wrote, for the scene reload and texture cook. */
  bool8_t scene_files_changed;
  bool8_t materials_changed;
  /* A project's Scripts folder changed: native code someone else wrote. */
  bool8_t scripts_changed;
  uint32_t textures_cooked;
  uint32_t textures_failed;
  /* Cooked textures the side branch for this computer brought instead. */
  uint32_t textures_fetched;
  VkrDepotId base;
  VkrDepotId remote_head;
  bool8_t remote_known;
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
  DepotRole role;
  char remote[DEPOT_TEXT];
  uint8_t server_key[32];
  /* The address jobs use: the server's loopback port when sharing. */
  char address[DEPOT_TEXT];

  VkrDepotService *service;
  /* The server's own memory: it buffers object chunks of up to 1 MiB for
     every connection, which the editor's retained allocator cannot hold. */
  VkrDMemory service_memory;
  VkrAllocator service_allocator;
  bool8_t service_memory_ready;
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
  VkrDepotId base;
  VkrDepotId remote_head;
  bool8_t remote_known;
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
  char share_destination[DEPOT_PATH];
  char find_address[DEPOT_TEXT];
  bool8_t dialog_open;
  char dialog_name[VKR_DEPOT_DISCOVERY_TEXT];
  char dialog_host[VKR_DEPOT_DISCOVERY_TEXT];
  char dialog_address[VKR_NET_ADDRESS_TEXT];
  uint8_t dialog_key[32];
  char dialog_destination[DEPOT_PATH];
  /* A finished clone the hub opens on its next build. */
  char open_directory[DEPOT_PATH];
  char open_project[40];
  /* A finished share the editor switches to. */
  bool8_t share_switch;
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
    log_warn("Project sharing: %s could not be written", path);
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

/* The server's commit of the branch; false with the connection's error. */
static bool8_t depot_job_remote_head(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotConnection *connection =
      depot_job_connect(worker, &worker->workspace->store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  bool8_t exists = false_v;
  const bool8_t ok =
      client && vkr_depot_find_ref(client, worker->workspace->branch,
                                   &job->remote_head, &exists);
  if (ok && !exists) {
    MemZero(&job->remote_head, sizeof(job->remote_head));
  }
  job->remote_known = ok;
  vkr_depot_disconnect(connection);
  return ok;
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
      log_warn("Project sharing: textures in %s did not cook (exit %d)",
               list.folders[i], exit_code);
    }
    vkr_atomic_uint32_store(&job->cook_done, i + 1u, VKR_MEMORY_ORDER_RELAXED);
  }
  vkr_heap_free(list.folders);
}

typedef struct DepotPullVisit {
  DepotJob *job;
} DepotPullVisit;

static bool8_t depot_pull_visit(void *context, const char *path,
                                VkrDepotChange change) {
  DepotJob *job = context;
  (void)change;
  if (depot_scene_path(path)) {
    job->scene_files_changed = true_v;
  }
  if (depot_ends_with(path, ".mt") || depot_ends_with(path, ".mtg")) {
    job->materials_changed = true_v;
  }
  if (string_find(path, "/Scripts/")) {
    job->scripts_changed = true_v;
  }
  return true_v;
}

static void depot_job_recook(DepotJob *job, const char *folder);

/* Writes the cooked textures this computer's side branch holds for the
   checked-out commit (ADR-123), so the cook that follows makes only the
   rest. A failure only leaves more to cook. */
static void depot_job_fetch_cooked(DepotJob *job, VkrDepotWorkspace *ws,
                                   VkrDepotClient *client) {
  VkrDepotCookedFetch fetched;
  char error[DEPOT_TEXT] = {0};
  if (!vkr_depot_fetch_cooked(ws, client, ws->branch, &fetched, error,
                              sizeof(error))) {
    log_warn("Project sharing: cooked textures did not arrive: %s", error);
    return;
  }
  job->textures_fetched = fetched.written;
  if (!vkr_depot_id_zero(&fetched.commit)) {
    log_info("Project sharing: %u cooked textures from %s (%u here already, "
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
      log_warn("Project sharing: cooked textures were not published: %s",
               error);
    }
    return;
  }
  VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  VkrDepotCookedResult result;
  if (client && vkr_depot_publish_cooked(ws, client, ws->branch, &tree, files,
                                         &result, error, sizeof(error))) {
    if (!result.unchanged) {
      log_info("Project sharing: published %u cooked textures to %s",
               result.files, result.branch);
    }
  } else {
    log_warn("Project sharing: cooked textures were not published: %s",
             client ? error : job->error);
  }
  vkr_depot_disconnect(connection);
  string_format(job->error, sizeof(job->error), "%s", saved);
}

/* Fetches the server's commit and checks it out; cooks the textures it
   brought and notes whether the scene must reload. */
static void depot_job_pull(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  VkrDepotFileList before = {0};
  for (uint32_t i = 0u; i < ws->index.count; ++i) {
    const VkrDepotIndexEntry *entry = &ws->index.entries[i];
    (void)vkr_depot_file_list_add(&before, entry->path, entry->size,
                                  entry->mtime_ns, &entry->blob);
  }
  VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  bool8_t exists = false_v;
  if (client &&
      vkr_depot_find_ref(client, ws->branch, &job->remote_head, &exists)) {
    job->remote_known = true_v;
    if (!exists || vkr_depot_id_equal(&job->remote_head, &ws->base)) {
      job->ok = true_v;
    } else {
      uint32_t kept = 0u;
      VkrDepotChangeList conflicts = {0};
      job->ok = vkr_depot_fetch_checkout(ws, client, &job->remote_head,
                                         job->policy, &kept, &conflicts,
                                         job->error, sizeof(job->error));
      depot_job_take_changes(job, &conflicts, job->conflicts,
                             &job->conflict_count);
      vkr_depot_change_list_free(&conflicts);
    }
  }
  if (job->ok && client) {
    depot_job_fetch_cooked(job, ws, client);
  }
  vkr_depot_disconnect(connection);
  if (job->ok) {
    (void)vkr_depot_file_list_diff(&before, &ws->index, depot_pull_visit, job);
    depot_job_cook(job, ws->root, &ws->index);
    depot_job_recook(job, ws->root);
    if (job->textures_cooked) {
      depot_job_publish_cooked(worker);
    }
  }
  vkr_depot_file_list_free(&before);
}

/* Commits every local change on top of the server's newest commit. A
   newer commit there is checked out first, keeping local changes; a path
   changed on both sides stops with the conflicts listed. */
static void depot_job_submit(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  if (job->save_remote) {
    string_format(ws->remote, sizeof(ws->remote), "%s", job->address);
    MemCopy(ws->server_key, job->server_key, 32u);
    if (!ws->branch[0]) {
      string_format(ws->branch, sizeof(ws->branch), "%s", DEPOT_BRANCH);
    }
    if (!vkr_depot_workspace_save(ws)) {
      string_format(job->error, sizeof(job->error),
                    "The working copy's state could not be saved");
      return;
    }
  }
  if (!depot_job_remote_head(worker)) {
    return;
  }
  if (!vkr_depot_id_zero(&job->remote_head) &&
      !vkr_depot_id_equal(&job->remote_head, &ws->base)) {
    depot_job_pull(worker);
    if (!job->ok) {
      return;
    }
    job->ok = false_v;
  }
  /* A snapshot of a large change outlasts a connection's idle time, so it
     runs before the upload connects. */
  VkrDepotPendingCommit pending = {0};
  if (!vkr_depot_commit_snapshot(ws, &ws->base, job->message, &pending,
                                 job->error, sizeof(job->error))) {
    return;
  }
  VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  const VkrDepotId parent = ws->base;
  if (client) {
    job->ok = vkr_depot_commit_push(ws, client, ws->branch, &parent,
                                    &job->remote_head, &pending, job->error,
                                    sizeof(job->error));
    if (job->ok) {
      job->remote_head = pending.commit;
    }
  }
  vkr_depot_pending_commit_free(&pending);
  vkr_depot_disconnect(connection);
  /* The new commit's cooked textures, for others of this GPU family. */
  if (job->ok) {
    depot_job_publish_cooked(worker);
  }
}

static void depot_job_status(DepotWorker *worker) {
  DepotJob *job = worker->job;
  VkrDepotWorkspace *ws = worker->workspace;
  /* A server that moved: discovery found its key at a new address. */
  if (job->save_remote && !string_equals(ws->remote, job->address)) {
    string_format(ws->remote, sizeof(ws->remote), "%s", job->address);
    (void)vkr_depot_workspace_save(ws);
  }
  VkrDepotChangeList changes = {0};
  if (!vkr_depot_status(ws, &changes, job->error, sizeof(job->error))) {
    return;
  }
  depot_job_take_changes(job, &changes, job->changes, &job->change_count);
  vkr_depot_change_list_free(&changes);
  job->ok = true_v;
  if (job->address[0]) {
    (void)depot_job_remote_head(worker);
  }
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
  if (vkr_depot_workspace_open(ws, job->directory, vkr_depot_zstd_codec())) {
    ws->progress = &job->progress;
    VkrDepotConnection *connection = depot_job_connect(worker, &ws->store);
    VkrDepotClient *client = vkr_depot_connection_client(connection);
    if (client) {
      depot_job_fetch_cooked(job, ws, client);
    }
    vkr_depot_disconnect(connection);
    job->error[0] = '\0';
    depot_job_cook(job, ws->root, &ws->index);
    depot_job_recook(job, ws->root);
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

/* Cooks, for this computer, the textures every scene's imported models and
   the project's own assets name in their recipes but lack here: cooked
   textures never travel, since each GPU family samples its own format
   (recook_textures). Imported models cooked before recipes existed are
   rebuilt once where they are shared, not here. */
static void depot_job_recook(DepotJob *job, const char *folder) {
  char projects[DEPOT_PATH];
  string_format(projects, sizeof(projects), "%s/.vkreditor/projects", folder);
  char project_id[40];
  depot_first_project(folder, project_id);
  if (!project_id[0]) {
    return;
  }
  Arena *arena = arena_create(MB(1), KB(256));
  if (!arena) {
    return;
  }
  char project_path[DEPOT_PATH];
  string_format(project_path, sizeof(project_path), "%s/%s/project.json",
                projects, project_id);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  const VkrBakeryJson *project =
      vkr_bakery_read_file(project_path, MB(8), &bytes, &size)
          ? vkr_bakery_json_parse(arena, bytes, size, 32u, NULL)
          : NULL;
  vkr_heap_free(bytes);
  const VkrBakeryJson *scenes = vkr_bakery_json_get(project, "scenes");
  const uint32_t total = (scenes ? scenes->count : 0u) + 1u;
  vkr_atomic_uint32_store(&job->cook_total, total, VKR_MEMORY_ORDER_RELAXED);
  uint32_t done = 0u;
  for (const VkrBakeryJson *scene = scenes ? scenes->first : NULL;
       scene && !vkr_depot_progress_canceled(&job->progress);
       scene = scene->next) {
    char scene_id[40] = {0};
    char relative[DEPOT_PATH] = {0};
    if (depot_json_text(scene, "id", scene_id, sizeof(scene_id)) &&
        depot_json_text(scene, "path", relative, sizeof(relative))) {
      char scene_path[DEPOT_PATH];
      string_format(scene_path, sizeof(scene_path), "%s/%s/%s", projects,
                    project_id, relative);
      VkrBakeryJson *request = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, request, "operation",
                          vkr_bakery_json_cstr(arena, "recook_textures"));
      vkr_bakery_json_set(arena, request, "project_path",
                          vkr_bakery_json_cstr(arena, project_path));
      vkr_bakery_json_set(arena, request, "scene_id",
                          vkr_bakery_json_cstr(arena, scene_id));
      vkr_bakery_json_set(arena, request, "scene_path",
                          vkr_bakery_json_cstr(arena, scene_path));
      const VkrBakeryJson *result = NULL;
      if (depot_project_job(job, folder, arena, request, "Preparing textures",
                            &result)) {
        int64_t recooked = 0;
        (void)vkr_bakery_json_get_int(result, "recooked", &recooked);
        job->textures_cooked += (uint32_t)recooked;
      } else {
        job->textures_failed++;
        log_warn("Project sharing: %s", job->error);
        job->error[0] = '\0';
      }
    }
    vkr_atomic_uint32_store(&job->cook_done, ++done, VKR_MEMORY_ORDER_RELAXED);
  }
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, "recook_textures"));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, project_path));
  const VkrBakeryJson *result = NULL;
  if (!vkr_depot_progress_canceled(&job->progress) &&
      !depot_project_job(job, folder, arena, request, "Preparing textures",
                         &result)) {
    job->textures_failed++;
    log_warn("Project sharing: %s", job->error);
    job->error[0] = '\0';
  }
  vkr_atomic_uint32_store(&job->cook_done, total, VKR_MEMORY_ORDER_RELAXED);
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
        log_warn("Project sharing: %s", job->error);
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
    log_warn("Project sharing: %s", job->error);
    job->error[0] = '\0';
  } else if (result) {
    int64_t freed = 0;
    (void)vkr_bakery_json_get_int(result, "freed_bytes", &freed);
    log_info("Project sharing: removed %lld bytes of replaced builds",
             (long long)freed);
  }
  arena_destroy(arena);
}

/* Copies the project's live files and every asset they use into the new
   workspace (Bakery's share_project), rebuilds models cooked before texture
   recipes, then makes it a working copy whose store the server serves. The
   first commit follows once the server runs. */
static void depot_job_share(DepotWorker *worker) {
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
                  "%s could not become a working copy", job->directory);
    return;
  }
  string_format(ws->branch, sizeof(ws->branch), "%s", DEPOT_BRANCH);
  job->ok = vkr_depot_workspace_save(ws);
  vkr_depot_workspace_close(ws);
  /* The store's server key marks the workspace as this editor's to
     serve. */
  char store[DEPOT_PATH];
  VkrNetKeyPair keys;
  string_format(store, sizeof(store), "%s/" VKR_DEPOT_META_DIR "/store",
                job->directory);
  job->ok = job->ok && vkr_depot_server_key(store, &keys);
  vkr_net_random(keys.secret_key, sizeof(keys.secret_key));
  if (!job->ok) {
    string_format(job->error, sizeof(job->error),
                  "%s could not become a working copy", job->directory);
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
  } else if (job->kind == DEPOT_JOB_SHARE) {
    depot_job_share(&worker);
  } else if (!vkr_depot_workspace_open(worker.workspace, job->directory,
                                       vkr_depot_zstd_codec())) {
    string_format(job->error, sizeof(job->error), "No working copy at %s",
                  job->directory);
  } else {
    VkrDepotWorkspace *ws = worker.workspace;
    ws->progress = &job->progress;
    if (job->kind == DEPOT_JOB_STATUS) {
      depot_job_status(&worker);
    } else if (job->kind == DEPOT_JOB_SUBMIT) {
      depot_job_submit(&worker);
    } else if (job->kind == DEPOT_JOB_PULL) {
      depot_job_pull(&worker);
    }
    /* What the window shows after any step: the base, its history and,
       after a change, what is left to submit. */
    job->base = ws->base;
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

/* Fills the inputs every workspace step needs. */
static void depot_job_inputs(VkrEditorDepot *depot) {
  DepotJob *job = depot->job;
  string_format(job->directory, sizeof(job->directory), "%s", depot->workspace);
  string_format(job->address, sizeof(job->address), "%s", depot->address);
  MemCopy(job->server_key, depot->server_key, 32u);
  job->save_remote = false_v;
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
  if (depot->service_memory_ready) {
    vkr_dmemory_allocator_destroy(&depot->service_allocator);
    depot->service_memory_ready = false_v;
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
  if (!vkr_dmemory_create(MB(8), GB(1), &depot->service_memory)) {
    string_format(depot->service_error, sizeof(depot->service_error),
                  "out of memory");
    return false_v;
  }
  depot->service_allocator = (VkrAllocator){.ctx = &depot->service_memory};
  vkr_dmemory_allocator_create(&depot->service_allocator);
  depot->service_memory_ready = true_v;
  depot->service = vkr_depot_service_start(&depot->service_allocator, &config,
                                           depot->service_error,
                                           sizeof(depot->service_error));
  if (!depot->service) {
    vkr_dmemory_allocator_destroy(&depot->service_allocator);
    depot->service_memory_ready = false_v;
    log_warn("Project sharing: the server did not start: %s",
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
  log_info("Project sharing: serving %s on UDP %u%s", depot->project_name,
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
  depot->error[0] = '\0';
  depot->waiting_kind = DEPOT_JOB_NONE;
  MemZero(&depot->base, sizeof(depot->base));
  MemZero(&depot->remote_head, sizeof(depot->remote_head));
}

/* Reads the open workspace's depot state when it changed: a working copy
   whose store holds a server key is this editor's to serve. */
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
  depot->role = DEPOT_ROLE_NONE;
  depot->address[0] = '\0';
  depot->remote[0] = '\0';
  MemZero(depot->server_key, sizeof(depot->server_key));
  depot->project_id[0] = '\0';
  depot->project_name[0] = '\0';
  depot->share_destination[0] = '\0';
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
  VkrDepotWorkspace *ws = vkr_heap_alloc_zeroed(1u, sizeof(*ws));
  if (!ws || !vkr_depot_workspace_open(ws, folder, vkr_depot_zstd_codec())) {
    vkr_heap_free(ws);
    return;
  }
  string_format(depot->remote, sizeof(depot->remote), "%s", ws->remote);
  MemCopy(depot->server_key, ws->server_key, 32u);
  depot->base = ws->base;
  vkr_depot_workspace_close(ws);
  vkr_heap_free(ws);
  char key_path[DEPOT_PATH];
  string_format(key_path, sizeof(key_path),
                "%s/" VKR_DEPOT_META_DIR "/store/server.key", folder);
  if (vkr_bakery_is_file(key_path)) {
    depot->role = DEPOT_ROLE_SHARER;
    (void)depot_service_start(depot);
  } else if (depot->remote[0]) {
    depot->role = DEPOT_ROLE_MEMBER;
    string_format(depot->address, sizeof(depot->address), "%s", depot->remote);
  }
  depot_remember_project(depot, folder, depot->project_name, depot->project_id,
                         depot->role == DEPOT_ROLE_SHARER);
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

static void depot_take_results(VkrEditorDepot *depot) {
  DepotJob *job = depot->job;
  if (job->kind == DEPOT_JOB_STATUS || job->ok) {
    depot->change_count = job->change_count;
    MemCopy(depot->changes, job->changes,
            Min(job->change_count, DEPOT_LIST_MAX) * sizeof(DepotText));
  }
  depot->conflict_count = job->conflict_count;
  MemCopy(depot->conflicts, job->conflicts,
          Min(job->conflict_count, DEPOT_LIST_MAX) * sizeof(DepotText));
  if (!vkr_depot_id_zero(&job->base)) {
    depot->base = job->base;
  }
  if (job->history_count) {
    depot->history_count = job->history_count;
    MemCopy(depot->history, job->history,
            job->history_count * sizeof(DepotHistory));
  }
  if (job->remote_known) {
    depot->remote_known = true_v;
    depot->remote_head = job->remote_head;
  }
  depot->connect = job->connect;
  depot->connected = job->remote_known;
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
  depot->error[0] = '\0';
  depot->waiting_kind = DEPOT_JOB_NONE;
  if (denied && (job->kind == DEPOT_JOB_CLONE || job->kind == DEPOT_JOB_PULL ||
                 job->kind == DEPOT_JOB_SUBMIT)) {
    /* The server recorded a request; ask again until its owner answers. */
    depot->waiting_kind = job->kind;
    depot->retry_at = now + DEPOT_RETRY_SECONDS;
  } else if (!job->ok && job->kind != DEPOT_JOB_STATUS &&
             !vkr_depot_progress_canceled(&job->progress)) {
    string_format(depot->error, sizeof(depot->error), "%s", job->error);
  }
  if (job->kind == DEPOT_JOB_PULL || job->kind == DEPOT_JOB_CLONE) {
    depot->textures_cooked = job->textures_cooked;
    depot->textures_fetched = job->textures_fetched;
    depot->textures_failed = job->textures_failed;
  }
  switch (job->kind) {
  case DEPOT_JOB_STATUS:
    depot->next_status = now + DEPOT_STATUS_SECONDS;
    break;
  case DEPOT_JOB_SUBMIT:
    if (job->ok) {
      depot->message[0] = '\0';
      string_format(depot->notice, sizeof(depot->notice), "Submitted");
      vkr_editor_toast(editor, VKR_UI_ICON_CHECK_CIRCLE,
                       vkr_ui_theme()->success, "Changes submitted");
    }
    depot->next_status = now + DEPOT_STATUS_SECONDS;
    break;
  case DEPOT_JOB_PULL:
    if (job->ok) {
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
      vkr_editor_toast(editor, VKR_UI_ICON_ARROW_DOWN, vkr_ui_theme()->success,
                       job->textures_failed ? "Got the latest; some textures "
                                              "did not cook"
                                            : "Got the latest changes");
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
  case DEPOT_JOB_SHARE:
    if (job->ok) {
      depot->share_switch = true_v;
      depot_remember_project(depot, job->directory, depot->project_name,
                             job->project_id, true_v);
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
     of control, here, once nothing else runs. */
  if (depot->open_directory[0]) {
    if (vkr_editor_projects_open_workspace(editor->projects, editor, frame,
                                           depot->open_directory,
                                           depot->open_project)) {
      depot->open_directory[0] = '\0';
      depot->open_project[0] = '\0';
    }
  }
  if (depot->share_switch && !depot_busy(depot)) {
    depot->share_switch = false_v;
    string_format(depot->open_directory, sizeof(depot->open_directory), "%s",
                  depot->job->directory);
    string_format(depot->open_project, sizeof(depot->open_project), "%s",
                  depot->job->project_id);
    /* The project now lives in the new workspace; the first commit runs
       once the editor serves it there. */
    string_format(depot->message, sizeof(depot->message), "Shared %s",
                  depot->project_name);
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

  /* The first commit of a project shared a moment ago. */
  if (depot->role == DEPOT_ROLE_SHARER && depot->service &&
      vkr_depot_id_zero(&depot->base) && !depot_busy(depot) &&
      !depot->error[0]) {
    depot_job_inputs(depot);
    depot->job->save_remote = true_v;
    string_format(depot->job->message, sizeof(depot->job->message), "Shared %s",
                  depot->project_name);
    (void)depot_start(depot, DEPOT_JOB_SUBMIT);
  }

  if (depot->waiting_kind != DEPOT_JOB_NONE && now >= depot->retry_at &&
      !depot_busy(depot)) {
    const DepotJobKind kind = depot->waiting_kind;
    depot->waiting_kind = DEPOT_JOB_NONE;
    if (kind != DEPOT_JOB_CLONE) {
      depot_job_inputs(depot);
    }
    (void)depot_start(depot, kind);
  }

  if (depot->role != DEPOT_ROLE_NONE && !depot_busy(depot) &&
      depot->waiting_kind == DEPOT_JOB_NONE && now >= depot->next_status) {
    depot_job_inputs(depot);
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
    /* A member whose server moved to another address follows its key. */
    if (depot->role == DEPOT_ROLE_MEMBER &&
        depot->connect == VKR_DEPOT_CONNECT_UNREACHABLE) {
      for (uint32_t i = 0u; i < depot->browser.found_count; ++i) {
        const VkrDepotDiscovered *found = &depot->browser.found[i];
        char text[VKR_NET_ADDRESS_TEXT];
        vkr_net_address_format(&found->address, text, sizeof(text));
        if (MemCompare(found->server_key, depot->server_key, 32u) == 0 &&
            !string_equals(text, depot->address) && !depot_busy(depot)) {
          string_format(depot->address, sizeof(depot->address), "%s", text);
          depot_job_inputs(depot);
          depot->job->save_remote = true_v;
          depot->connect = VKR_DEPOT_CONNECT_OK;
          (void)depot_start(depot, DEPOT_JOB_STATUS);
        }
      }
    }
  }
  depot->browse_wanted = false_v;
}

// =============================================================================
// Actions
// =============================================================================

bool8_t vkr_editor_depot_share(VkrEditorDepot *depot, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               const char *destination, char *error,
                               uint32_t capacity) {
  if (!depot || depot->role != DEPOT_ROLE_NONE || !depot->project_id[0]) {
    string_format(error, capacity, "Open a project that is not shared yet");
    return false_v;
  }
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  if (!depot_journals_saved(frame)) {
    string_format(error, capacity,
                  "Save the scene first; Share copies the saved "
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
  if (!depot_start(depot, DEPOT_JOB_SHARE)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_submit(VkrEditorDepot *depot, const char *message,
                                char *error, uint32_t capacity) {
  if (!depot || depot->role == DEPOT_ROLE_NONE) {
    string_format(error, capacity, "This project is not shared");
    return false_v;
  }
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  depot_job_inputs(depot);
  const char *text = message && message[0] ? message : depot->message;
  if (!text[0]) {
    text = "Changes";
  }
  string_format(depot->job->message, sizeof(depot->job->message), "%s", text);
  if (!depot_start(depot, DEPOT_JOB_SUBMIT)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_get_latest(VkrEditorDepot *depot, const char *policy,
                                    char *error, uint32_t capacity) {
  if (!depot || depot->role == DEPOT_ROLE_NONE) {
    string_format(error, capacity, "This project is not shared");
    return false_v;
  }
  if (depot_busy(depot)) {
    string_format(error, capacity, "Wait for the current step to finish");
    return false_v;
  }
  depot_job_inputs(depot);
  depot->job->policy =
      policy && string_equals(policy, "theirs") ? VKR_DEPOT_CONFLICT_TAKE_THEIRS
      : policy && string_equals(policy, "mine") ? VKR_DEPOT_CONFLICT_KEEP_MINE
                                                : VKR_DEPOT_CONFLICT_FAIL;
  if (!depot_start(depot, DEPOT_JOB_PULL)) {
    string_format(error, capacity, "The step could not start");
    return false_v;
  }
  depot->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_depot_clone(VkrEditorDepot *depot, const char *address,
                               const char *key_hex, const char *name,
                               const char *destination, char *error,
                               uint32_t capacity) {
  if (!depot) {
    return false_v;
  }
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
       job->kind == DEPOT_JOB_CLONE || job->kind == DEPOT_JOB_SHARE)) {
    string_format(
        out, capacity,
        job->kind == DEPOT_JOB_SHARE
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
    if (job->kind == DEPOT_JOB_SHARE) {
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
  return depot->remote_known && !vkr_depot_id_zero(&depot->remote_head) &&
         !vkr_depot_id_equal(&depot->remote_head, &depot->base);
}

static const char *depot_role_name(VkrDepotRole role) {
  return role == VKR_DEPOT_ROLE_ADMIN   ? "owner"
         : role == VKR_DEPOT_ROLE_WRITE ? "can edit"
         : role == VKR_DEPOT_ROLE_READ  ? "can view"
                                        : "no access";
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
  const char *tooltip = "Project sharing";
  VkrUiIcon icon = VKR_UI_ICON_CLOUD;
  Vec4 icon_color = theme->text_secondary;
  if (depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS) {
    icon = VKR_UI_ICON_SPINNER;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%s", depot_job_names[depot->job->kind]);
  } else if (depot->role == DEPOT_ROLE_NONE) {
    string_format(text, sizeof(text), "Share");
    tooltip = "Share this project with other computers on your network";
  } else if (depot->waiting_kind != DEPOT_JOB_NONE) {
    icon = VKR_UI_ICON_CLOCK;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "Waiting for access");
    tooltip = "The sharing computer has not allowed this one yet";
  } else if (depot->request_count) {
    icon = VKR_UI_ICON_BELL;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "%u asking", depot->request_count);
    tooltip = "Other computers ask to work on this project";
  } else if (depot->conflict_count) {
    icon = VKR_UI_ICON_WARNING_FILL;
    icon_color = theme->warning;
    string_format(text, sizeof(text), "Conflicts");
  } else if (depot_incoming(depot)) {
    icon = VKR_UI_ICON_ARROW_DOWN;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "New changes");
    tooltip = "Others submitted changes; Get latest brings them here";
  } else if (depot->change_count) {
    icon = VKR_UI_ICON_ARROW_UP;
    icon_color = theme->accent_hover;
    string_format(text, sizeof(text), "%u change%s", depot->change_count,
                  depot->change_count == 1u ? "" : "s");
    tooltip = "Changes on this computer that others do not have yet";
  } else if (depot->role == DEPOT_ROLE_MEMBER && depot->status_known &&
             !depot->connected) {
    icon_color = theme->text_disabled;
    string_format(text, sizeof(text), "Offline");
    tooltip = "The sharing computer does not answer";
  } else if (depot->role == DEPOT_ROLE_SHARER && depot->service) {
    icon = VKR_UI_ICON_CHECK_CIRCLE;
    icon_color = theme->success;
    string_format(text, sizeof(text), "Shared");
  } else {
    icon = VKR_UI_ICON_CHECK_CIRCLE;
    icon_color = depot->status_known ? theme->success : theme->text_disabled;
    string_format(text, sizeof(text), "Synced");
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
// Project sharing window
// =============================================================================

static void depot_window_not_shared(VkrEditorDepot *depot, VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiSystem *ui, float32_t *y,
                                    float32_t width) {
  depot_heading(editor, ui, "share.title", "Share this project",
                VKR_UI_ICON_CLOUD, 12, *y, width - 24);
  *y += 30;
  *y += depot_paragraph(
      ui, "share.text",
      "Other computers on your network can then get it, and you exchange "
      "changes from here. It is served while this editor is open. The "
      "project and every asset it uses move to this folder, where you keep "
      "working:",
      12, *y, width - 24);
  *y += 6;
  if (!depot->share_destination[0]) {
    depot_default_destination(depot->project_name, depot->share_destination,
                              sizeof(depot->share_destination));
  }
  depot_field(ui, "share.destination", depot->share_destination,
              sizeof(depot->share_destination), 12, *y, width - 24);
  *y += 36;
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "share.go", "Share on network", 12, *y, 170,
                   true_v, depot_busy(depot)) &&
      !vkr_editor_depot_share(depot, editor, frame, depot->share_destination,
                              error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  *y += 36;
  *y += depot_paragraph(ui, "share.firewall",
                        "The first time, your computer may ask whether VKR "
                        "Editor may use the network. Choose Allow.",
                        12, *y, width - 24);
  *y += 8;
}

static void depot_window_server(VkrEditorDepot *depot, VkrEditorUi *editor,
                                VkrUiSystem *ui, float32_t *y,
                                float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  if (depot->role == DEPOT_ROLE_SHARER) {
    if (depot->service) {
      depot_heading(editor, ui, "server.state", depot->project_name,
                    VKR_UI_ICON_CLOUD, 12, *y, width - 24);
      *y += 26;
      char host[VKR_DEPOT_DISCOVERY_TEXT];
      vkr_depot_host_name(host, sizeof(host));
      string_format(text, sizeof(text),
                    "Shared from this computer (%s)  \xc2\xb7  %u connected",
                    host, depot->service_info.peers);
      depot_caption(ui, "server.peers", text, 12, *y, width - 24);
      *y += 22;
      if (!depot->service_info.discoverable) {
        string_format(
            text, sizeof(text),
            "Not listed on the network: another server uses its port. "
            "Others enter %s to find it.",
            host);
        *y += depot_paragraph(ui, "server.hidden", text, 12, *y, width - 24);
      }
      *y += 4;
    } else {
      depot_heading(editor, ui, "server.state", "Not shared right now",
                    VKR_UI_ICON_WARNING_FILL, 12, *y, width - 24);
      *y += 26;
      depot_caption(ui, "server.error",
                    depot->service_error[0] ? depot->service_error
                                            : "The server is not running",
                    12, *y, width - 160);
      if (depot_button(editor, ui, "server.retry", "Share again", width - 136,
                       *y - 4, 124, false_v, false_v)) {
        (void)depot_service_start(depot);
      }
      *y += 28;
    }
    return;
  }
  const char *server = depot->remote;
  for (uint32_t i = 0u; i < depot->known_count; ++i) {
    if (MemCompare(depot->known[i].key, depot->server_key, 32u) == 0 &&
        depot->known[i].host[0]) {
      server = depot->known[i].host;
      break;
    }
  }
  depot_heading(editor, ui, "server.state", depot->project_name,
                VKR_UI_ICON_CLOUD, 12, *y, width - 24);
  *y += 26;
  const char *state =
      depot->waiting_kind != DEPOT_JOB_NONE
          ? "waiting for that computer to allow this one"
      : !depot->status_known ? "checking..."
      : depot->connected     ? "connected"
      : depot->connect == VKR_DEPOT_CONNECT_UNREACHABLE
          ? "not answering: its editor may be closed, or a firewall blocks it"
          : "not connected";
  string_format(text, sizeof(text), "Shared by %s  \xc2\xb7  %s", server,
                state);
  VkrUiWidgetConfig c = depot_widget(12, *y, width - 24, 18);
  c.style.font_size_pt = theme->font_caption;
  c.style.text_color = depot->waiting_kind != DEPOT_JOB_NONE
                           ? theme->warning
                           : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("server.connection"), depot_string(text), &c);
  *y += 24;
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
    string_format(text, sizeof(text), "%s asks to work on this project", who);
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
    if (depot_button(editor, ui, "read", "View only", 116, *y, 96, false_v,
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

static void depot_window_changes(VkrEditorDepot *depot, VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, VkrUiSystem *ui,
                                 float32_t *y, float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[DEPOT_TEXT];
  char error[DEPOT_TEXT] = {0};
  const bool8_t busy =
      depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS;

  /* Incoming first: submitting takes the newest commit anyway. */
  if (depot_incoming(depot)) {
    depot_heading(editor, ui, "incoming.title", "New changes from others",
                  VKR_UI_ICON_ARROW_DOWN, 12, *y, width - 160);
    if (depot_button(editor, ui, "incoming.get", "Get latest", width - 136, *y,
                     124, true_v, busy) &&
        !vkr_editor_depot_get_latest(depot, "fail", error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    *y += 36;
  }

  if (depot->conflict_count) {
    string_format(
        text, sizeof(text), "%u file%s changed here and by someone else",
        depot->conflict_count, depot->conflict_count == 1u ? "" : "s");
    depot_label(ui, "conflict.title", text, 12, *y, width - 24, theme->warning);
    *y += 24;
    for (uint32_t i = 0u; i < Min(depot->conflict_count, 6u); ++i) {
      (void)vkr_ui_push_id_u64(ui, i);
      depot_caption(ui, "conflict", depot->conflicts[i].text, 24, *y,
                    width - 36);
      (void)vkr_ui_pop_id(ui);
      *y += 18;
    }
    *y += 6;
    if (depot_button(editor, ui, "conflict.theirs", "Use theirs", 12, *y, 120,
                     false_v, busy) &&
        !vkr_editor_depot_get_latest(depot, "theirs", error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    if (depot_button(editor, ui, "conflict.mine", "Keep mine", 140, *y, 120,
                     false_v, busy) &&
        !vkr_editor_depot_get_latest(depot, "mine", error, sizeof(error))) {
      string_format(depot->error, sizeof(depot->error), "%s", error);
    }
    *y += 38;
  }

  if (!depot->status_known && depot_busy(depot)) {
    depot_caption(ui, "changes.reading", "Looking for changes...", 12, *y,
                  width - 24);
    *y += 26;
    return;
  }
  if (!depot->change_count) {
    depot_label(ui, "changes.none",
                depot_incoming(depot) ? "No changes of yours to submit"
                                      : "Up to date. No changes to submit.",
                12, *y, width - 24, theme->text_secondary);
    *y += 30;
    return;
  }
  string_format(text, sizeof(text), "%u change%s to submit",
                depot->change_count, depot->change_count == 1u ? "" : "s");
  depot_heading(editor, ui, "changes.title", text, VKR_UI_ICON_ARROW_UP, 12, *y,
                width - 24);
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
  if (depot_button(editor, ui, "submit", "Submit", 12, *y, 140, true_v,
                   busy || unsaved || depot->role == DEPOT_ROLE_NONE) &&
      !vkr_editor_depot_submit(depot, depot->message, error, sizeof(error))) {
    string_format(depot->error, sizeof(depot->error), "%s", error);
  }
  if (unsaved) {
    depot_caption(ui, "submit.unsaved", "Save the scene to submit its edits",
                  164, *y + 4, width - 176);
  }
  *y += 40;
}

static void depot_window_history(VkrEditorDepot *depot, VkrUiSystem *ui,
                                 float32_t *y, float32_t width) {
  if (!depot->history_count) {
    return;
  }
  depot_caption(ui, "history.title", "History", 12, *y, width - 24);
  *y += 22;
  const uint64_t now_us = depot_unix_us();
  for (uint32_t i = 0u; i < depot->history_count; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    depot_label(ui, "row", depot->history[i].message, 24, *y, width - 140,
                vkr_ui_theme()->text);
    char when[32];
    depot_format_age(now_us, depot->history[i].time_us, when, sizeof(when));
    VkrUiWidgetConfig age = depot_widget(width - 112, *y, 100, 22);
    age.style.font_size_pt = vkr_ui_theme()->font_caption;
    age.style.text_color = vkr_ui_theme()->text_secondary;
    vkr_ui_label(ui, string8_lit("age"), depot_string(when), &age);
    (void)vkr_ui_pop_id(ui);
    *y += 22;
  }
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
  depot->browse_wanted = depot->role == DEPOT_ROLE_MEMBER;
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
  if (!depot->project_id[0]) {
    depot_label(ui, "none", "Open a project to share it.", 12, y, width - 24,
                theme->text_secondary);
    y += 30;
  } else if (depot->role == DEPOT_ROLE_NONE && !depot_busy(depot)) {
    depot_window_not_shared(depot, editor, frame, ui, &y, width);
  } else {
    depot_window_server(depot, editor, ui, &y, width);
    depot_window_requests(depot, editor, ui, &y, width);
    if (depot->role != DEPOT_ROLE_NONE) {
      depot_window_changes(depot, editor, frame, ui, &y, width);
    }
    if (depot->role == DEPOT_ROLE_SHARER) {
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
  if (depot_busy(depot) && depot->job->kind != DEPOT_JOB_STATUS) {
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
  string_format(text, sizeof(text), "asks to work on %s  \xc2\xb7  key %s",
                depot->project_name, print);
  depot_caption(ui, "text", text, 34, 34, width - 46);
  char key[VKR_DEPOT_ID_HEX];
  depot_key_hex(request->key, key);
  char error[DEPOT_TEXT] = {0};
  if (depot_button(editor, ui, "allow", "Allow", 12, 64, 100, true_v,
                   false_v)) {
    (void)vkr_editor_depot_allow(depot, key, "write", error, sizeof(error));
  }
  if (depot_button(editor, ui, "read", "View only", 120, 64, 100, false_v,
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

static bool8_t depot_found_is_self(const VkrEditorDepot *depot,
                                   const VkrDepotDiscovered *found) {
  return depot->service && MemCompare(found->server_key,
                                      depot->service_info.server_key, 32u) == 0;
}

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
    char key[VKR_DEPOT_ID_HEX];
    depot_key_hex(depot->dialog_key, key);
    if (depot_button(editor, ui, "get.go", "Get project", x + 12, y, 140,
                     true_v, depot_busy(depot)) &&
        !vkr_editor_depot_clone(depot, depot->dialog_address, key,
                                depot->dialog_name, depot->dialog_destination,
                                error, sizeof(error))) {
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

  /* Shared workspaces on this computer, other than the open one. */
  uint32_t local = 0u;
  for (uint32_t i = 0u; i < depot->project_count; ++i) {
    local += !string_equals(depot->projects[i].path, depot->workspace);
  }
  if (local) {
    depot_heading(editor, ui, "hub.local", "Shared projects on this computer",
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

  /* Projects other editors share. */
  depot_heading(editor, ui, "hub.network", "On your network", VKR_UI_ICON_CLOUD,
                x, y, width);
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
        text, sizeof(text), "Shared by %s  \xc2\xb7  %s  \xc2\xb7  key %s%s",
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
                     string8_lit("Not the computer you got this project from "
                                 "before"),
                     &warning);
        y += 16;
        break;
      }
    }
    const bool8_t chosen =
        depot->dialog_open &&
        MemCompare(depot->dialog_key, found->server_key, 32u) == 0;
    if (!chosen && depot_button(editor, ui, "get", "Get", x + width - 112,
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
  /* A Get started by address, whose server discovery has not heard. */
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
    string_format(text, sizeof(text), "Shared at %s", depot->dialog_address);
    depot_caption(ui, "dialog.host", text, x + 36, y + 28, width - 170);
    y += 60;
    y += depot_hub_dialog(depot, editor, ui, x + 24, y, width - 24) + 8;
  }
  if (!shown) {
    depot_caption(ui, "hub.empty",
                  "Looking for projects shared on this network...", x + 12, y,
                  width - 24);
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

bool8_t vkr_editor_depot_stream(VkrEditorDepot *depot, const char *address,
                                const char *key_hex, const char *directory,
                                const char *branch, bool8_t no_cooked,
                                char *error, uint32_t capacity) {
  uint8_t key[32];
  if (depot->stream) {
    string_format(error, capacity, "a stream is open already");
    return false_v;
  }
  if (!depot_key_parse(key_hex, key)) {
    string_format(error, capacity, "the server key needs 64 hex digits");
    return false_v;
  }
  if (!vkr_bakery_path_absolute(directory, depot->stream_directory,
                                sizeof(depot->stream_directory))) {
    string_format(error, capacity, "the stream directory path is too long");
    return false_v;
  }
  const VkrDepotRemoteConfig config = {.directory = depot->stream_directory,
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
  depot->stream_open_seconds =
      vkr_platform_get_absolute_time() - depot->stream_opened_at;
  if (!vkr_vfs_set_remote(depot->stream_directory, vkr_depot_remote_fetch,
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
  log_info(
      "Streaming %u files of %.12s%s from %s into %s (listed in %.0f ms)",
      stats.listed, hex,
      vkr_depot_id_zero(&stats.cooked_commit) ? "" : " and its cooked branch",
      address, depot->stream_directory, depot->stream_open_seconds * 1000.0);
  return true_v;
}

VkrBakeryJson *vkr_editor_depot_stream_json(VkrEditorDepot *depot,
                                            Arena *arena) {
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "streaming",
                      vkr_bakery_json_bool(arena, depot->stream != NULL));
  if (!depot->stream) {
    return result;
  }
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(depot->stream, &stats);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&stats.commit, hex);
  vkr_bakery_json_set(arena, result, "commit",
                      vkr_bakery_json_cstr(arena, hex));
  vkr_depot_id_hex(&stats.cooked_commit, hex);
  vkr_bakery_json_set(
      arena, result, "cooked_commit",
      vkr_bakery_json_cstr(arena,
                           vkr_depot_id_zero(&stats.cooked_commit) ? "" : hex));
  vkr_bakery_json_set(arena, result, "directory",
                      vkr_bakery_json_cstr(arena, depot->stream_directory));
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
  vkr_bakery_json_set(
      arena, result, "wait_ms",
      vkr_bakery_json_float(arena, (float64_t)stats.wait_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "last_fetch_ms",
      vkr_bakery_json_float(arena, (float64_t)stats.last_write_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "open_ms",
      vkr_bakery_json_float(arena, depot->stream_open_seconds * 1000.0));
  vkr_bakery_json_set(
      arena, result, "since_open_ms",
      vkr_bakery_json_float(
          arena, (vkr_platform_get_absolute_time() - depot->stream_opened_at) *
                     1000.0));
  return result;
}

VkrBakeryJson *vkr_editor_depot_status_json(VkrEditorDepot *depot,
                                            Arena *arena) {
  static const char *const roles[] = {"none", "sharer", "member"};
  static const char *const connects[] = {"ok",     "invalid", "unreachable",
                                         "denied", "refused", "canceled"};
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "role",
                      vkr_bakery_json_cstr(arena, roles[depot->role]));
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

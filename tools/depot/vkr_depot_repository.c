#include "vkr_depot_repository.h"

#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_protocol.h"
#include "vkr_depot_store.h"
#include "vkr_net_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a swap waits for the store's lock: 200 tries 10 ms apart. A
   holder keeps it only to read and write one small file. */
#define REPOSITORY_LOCK_TRIES 200u
#define REPOSITORY_LOCK_WAIT_MS 10u
#define REPOSITORY_LOCK_NAME 64u
/* `tracking/<remote>/<branch>` and the like, with the terminator. */
#define REPOSITORY_RELATIVE_MAX                                                \
  (16u + VKR_DEPOT_REMOTE_NAME + VKR_DEPOT_BRANCH_NAME)
/* One line of `remotes`: name, address, key and repository in hex, interval,
   four spaces and a newline. */
#define REPOSITORY_REMOTE_LINE                                                 \
  (VKR_DEPOT_REMOTE_NAME + VKR_DEPOT_REMOTE_ADDRESS + 64u + 32u + 16u)

// =============================================================================
// Paths and the store's lock
// =============================================================================

/* `<store>/<folder>/<name>`; `name` NULL for the folder itself. */
static bool8_t repository_path(const char *store_root, const char *folder,
                               const char *name, char *out, uint32_t capacity) {
  char relative[REPOSITORY_RELATIVE_MAX];
  const int written =
      name ? snprintf(relative, sizeof(relative), "%s/%s", folder, name)
           : snprintf(relative, sizeof(relative), "%s", folder);
  return written > 0 && (uint32_t)written < sizeof(relative) &&
         vkr_bakery_path_join(out, capacity, store_root, relative);
}

/* `tracking/<remote>`, or false for an invalid remote name. */
static bool8_t repository_tracking_folder(const char *remote,
                                          char out[REPOSITORY_RELATIVE_MAX]) {
  return vkr_depot_remote_name_valid(remote) &&
         snprintf(out, REPOSITORY_RELATIVE_MAX, "tracking/%s", remote) > 0;
}

/* The lock's name, the same for every spelling of the store's path that
   resolves to one absolute path: Windows names a mutex by it, and POSIX
   keeps the lock file `<store>/<name>.lock`. Case folds, as on the file
   systems of macOS and Windows. */
static bool8_t repository_lock_name(const char *store_root,
                                    char out[REPOSITORY_LOCK_NAME]) {
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_absolute(store_root, absolute, sizeof(absolute))) {
    return false_v;
  }
  const size_t length = strlen(absolute);
  for (size_t i = 0u; i < length; ++i) {
    const char c = absolute[i];
    if (c == '\\') {
      absolute[i] = '/';
    } else if (c >= 'A' && c <= 'Z') {
      absolute[i] = (char)(c - 'A' + 'a');
    }
  }
  while (strlen(absolute) > 1u && absolute[strlen(absolute) - 1u] == '/') {
    absolute[strlen(absolute) - 1u] = '\0';
  }

  VkrDepotId id;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_of(absolute, strlen(absolute), &id);
  vkr_depot_id_hex(&id, hex);
  snprintf(out, REPOSITORY_LOCK_NAME, "vkr-depot-refs-%.16s", hex);
  return true_v;
}

static bool8_t repository_lock(const char *store_root,
                               VkrPlatformProcessLock *out_lock) {
  char name[REPOSITORY_LOCK_NAME];
  if (!repository_lock_name(store_root, name) ||
      !vkr_bakery_make_directories(store_root)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < REPOSITORY_LOCK_TRIES; ++i) {
    if (vkr_platform_process_lock_acquire(name, store_root, out_lock)) {
      return true_v;
    }
    vkr_platform_sleep(REPOSITORY_LOCK_WAIT_MS);
  }
  return false_v;
}

// =============================================================================
// Named commits: branches and tracking branches
// =============================================================================

static bool8_t repository_id_read(const char *path, VkrDepotId *out_commit) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_is_file(path) ||
      !vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == VKR_DEPOT_ID_SIZE;
  if (ok) {
    memcpy(out_commit->bytes, bytes, VKR_DEPOT_ID_SIZE);
  }
  free(bytes);
  return ok;
}

/* Writes `commit` to `<store>/<folder>/<name>`, or removes the file when
   `commit` is zero; the caller holds the store's lock. */
static bool8_t repository_id_write(const char *store_root, const char *folder,
                                   const char *name, const VkrDepotId *commit) {
  char directory[VKR_DEPOT_PATH_CAPACITY];
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!repository_path(store_root, folder, NULL, directory,
                       sizeof(directory)) ||
      !repository_path(store_root, folder, name, path, sizeof(path))) {
    return false_v;
  }
  if (vkr_depot_id_zero(commit)) {
    return !vkr_bakery_is_file(path) || vkr_bakery_remove_file(path);
  }
  return vkr_bakery_make_directories(directory) &&
         vkr_bakery_write_file_atomic(path, commit->bytes, VKR_DEPOT_ID_SIZE);
}

bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  return vkr_depot_branch_valid(branch) &&
         repository_path(store_root, "refs", branch, path, sizeof(path)) &&
         repository_id_read(path, out_commit);
}

VkrDepotRefSwap vkr_depot_ref_swap(const char *store_root, const char *branch,
                                   const VkrDepotId *expected,
                                   const VkrDepotId *commit,
                                   VkrDepotId *out_current) {
  VkrDepotId current = {0};
  if (out_current) {
    MemZero(out_current, sizeof(*out_current));
  }
  if (!vkr_depot_branch_valid(branch)) {
    return VKR_DEPOT_REF_SWAP_FAILED;
  }

  VkrPlatformProcessLock lock;
  if (!repository_lock(store_root, &lock)) {
    return VKR_DEPOT_REF_SWAP_BUSY;
  }

  VkrDepotRefSwap result = VKR_DEPOT_REF_SWAP_OK;
  const bool8_t exists = vkr_depot_ref_read(store_root, branch, &current);
  if ((exists && !vkr_depot_id_equal(&current, expected)) ||
      (!exists && !vkr_depot_id_zero(expected))) {
    result = VKR_DEPOT_REF_SWAP_MOVED;
  } else if (!repository_id_write(store_root, "refs", branch, commit)) {
    result = VKR_DEPOT_REF_SWAP_FAILED;
  } else {
    current = *commit;
  }

  vkr_platform_process_lock_release(&lock);
  if (out_current) {
    *out_current = current;
  }
  return result;
}

bool8_t vkr_depot_tracking_read(const char *store_root, const char *remote,
                                const char *branch, VkrDepotId *out_commit) {
  char folder[REPOSITORY_RELATIVE_MAX];
  char path[VKR_DEPOT_PATH_CAPACITY];
  return vkr_depot_branch_valid(branch) &&
         repository_tracking_folder(remote, folder) &&
         repository_path(store_root, folder, branch, path, sizeof(path)) &&
         repository_id_read(path, out_commit);
}

bool8_t vkr_depot_tracking_set(const char *store_root, const char *remote,
                               const char *branch, const VkrDepotId *commit) {
  char folder[REPOSITORY_RELATIVE_MAX];
  if (!vkr_depot_branch_valid(branch) ||
      !repository_tracking_folder(remote, folder)) {
    return false_v;
  }
  VkrPlatformProcessLock lock;
  if (!repository_lock(store_root, &lock)) {
    return false_v;
  }
  const bool8_t ok = repository_id_write(store_root, folder, branch, commit);
  vkr_platform_process_lock_release(&lock);
  return ok;
}

typedef struct NameListVisit {
  VkrDepotBranchVisitor visit;
  void *context;
  uint32_t count;
  bool8_t stopped;
} NameListVisit;

static bool8_t repository_name_visit(void *context, const char *name,
                                     bool8_t is_directory) {
  NameListVisit *walk = context;
  if (is_directory || !vkr_depot_branch_valid(name)) {
    return true_v;
  }
  if (walk->count == VKR_DEPOT_REPOSITORY_BRANCHES_MAX ||
      !walk->visit(walk->context, name)) {
    walk->stopped = true_v;
    return false_v;
  }
  walk->count++;
  return true_v;
}

/* Visits each valid branch name stored under `<store>/<folder>`. */
static bool8_t repository_list(const char *store_root, const char *folder,
                               VkrDepotBranchVisitor visit, void *context) {
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!repository_path(store_root, folder, NULL, directory,
                       sizeof(directory))) {
    return false_v;
  }
  if (!vkr_bakery_is_directory(directory)) {
    return true_v;
  }

  NameListVisit walk = {.visit = visit, .context = context};
  return vkr_bakery_list_directory(directory, repository_name_visit, &walk) ||
         walk.stopped;
}

bool8_t vkr_depot_ref_list(const char *store_root, VkrDepotBranchVisitor visit,
                           void *context) {
  return repository_list(store_root, "refs", visit, context);
}

bool8_t vkr_depot_tracking_list(const char *store_root, const char *remote,
                                VkrDepotBranchVisitor visit, void *context) {
  char folder[REPOSITORY_RELATIVE_MAX];
  return repository_tracking_folder(remote, folder) &&
         repository_list(store_root, folder, visit, context);
}

// =============================================================================
// Repository ID and served key
// =============================================================================

bool8_t vkr_depot_repository_id(const char *store_root,
                                uint8_t out[VKR_DEPOT_REPOSITORY_ID_SIZE],
                                bool8_t create) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), store_root, "repository")) {
    return false_v;
  }

  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (vkr_bakery_is_file(path) &&
      vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    const bool8_t ok = size == VKR_DEPOT_REPOSITORY_ID_SIZE;
    if (ok) {
      memcpy(out, bytes, VKR_DEPOT_REPOSITORY_ID_SIZE);
    }
    free(bytes);
    return ok;
  }
  if (!create) {
    return false_v;
  }

  /* Two processes making the ID at once could each write one; the lock
     makes the second read the first's. */
  VkrPlatformProcessLock lock;
  if (!repository_lock(store_root, &lock)) {
    return false_v;
  }
  bool8_t ok = false_v;
  if (vkr_bakery_is_file(path)) {
    vkr_platform_process_lock_release(&lock);
    return vkr_depot_repository_id(store_root, out, false_v);
  }
  vkr_net_random(out, VKR_DEPOT_REPOSITORY_ID_SIZE);
  ok = vkr_bakery_write_file_atomic(path, out, VKR_DEPOT_REPOSITORY_ID_SIZE);
  vkr_platform_process_lock_release(&lock);
  return ok;
}

bool8_t vkr_depot_repository_served_key(const char *store_root,
                                        uint8_t out_public[32]) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_path_join(path, sizeof(path), store_root, "server.key") ||
      !vkr_bakery_is_file(path) ||
      !vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == VKR_NET_KEY_SIZE;
  if (ok) {
    VkrNetKeyPair keys;
    vkr_net_keypair_from_secret(bytes, &keys);
    memcpy(out_public, keys.public_key, 32u);
    vkr_net_random(keys.secret_key, sizeof(keys.secret_key));
  }
  vkr_net_random(bytes, size);
  free(bytes);
  return ok;
}

// =============================================================================
// Remotes
// =============================================================================

bool8_t vkr_depot_remote_name_valid(const char *name) {
  const size_t size = name ? strlen(name) : 0u;
  if (size == 0u || size >= VKR_DEPOT_REMOTE_NAME) {
    return false_v;
  }
  for (size_t i = 0u; i < size; ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_')) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t repository_hex_parse(const char *hex, uint8_t *out,
                                    uint32_t size) {
  if (strlen(hex) != (size_t)size * 2u) {
    return false_v;
  }
  for (uint32_t i = 0u; i < size; ++i) {
    uint32_t value = 0u;
    for (uint32_t k = 0u; k < 2u; ++k) {
      const char c = hex[i * 2u + k];
      uint32_t digit = 0u;
      if (c >= '0' && c <= '9') {
        digit = (uint32_t)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = (uint32_t)(c - 'a' + 10);
      } else {
        return false_v;
      }
      value = value * 16u + digit;
    }
    out[i] = (uint8_t)value;
  }
  return true_v;
}

static void repository_hex_format(const uint8_t *bytes, uint32_t size,
                                  char *out) {
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0u; i < size; ++i) {
    out[i * 2u] = digits[bytes[i] >> 4];
    out[i * 2u + 1u] = digits[bytes[i] & 15u];
  }
  out[size * 2u] = '\0';
}

/* Parses `name address key repository interval`; false for a torn or
   hand-broken line, which the list skips. */
static bool8_t repository_remote_parse(char *line, VkrDepotRemoteEntry *out) {
  char *fields[5] = {NULL};
  uint32_t count = 0u;
  for (char *cursor = line; *cursor && count < ArrayCount(fields);) {
    while (*cursor == ' ') {
      *cursor++ = '\0';
    }
    if (!*cursor) {
      break;
    }
    fields[count++] = cursor;
    while (*cursor && *cursor != ' ') {
      ++cursor;
    }
  }
  if (count != ArrayCount(fields) || !vkr_depot_remote_name_valid(fields[0]) ||
      strlen(fields[1]) >= VKR_DEPOT_REMOTE_ADDRESS ||
      !repository_hex_parse(fields[2], out->key, 32u) ||
      !repository_hex_parse(fields[3], out->repository,
                            VKR_DEPOT_REPOSITORY_ID_SIZE)) {
    return false_v;
  }
  snprintf(out->name, sizeof(out->name), "%s", fields[0]);
  snprintf(out->address, sizeof(out->address), "%s", fields[1]);
  out->interval_seconds = (uint32_t)strtoul(fields[4], NULL, 10);
  return true_v;
}

bool8_t vkr_depot_remotes_load(const char *store_root,
                               VkrDepotRemoteEntry *out_remotes,
                               uint32_t capacity, uint32_t *out_count) {
  *out_count = 0u;
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), store_root, "remotes")) {
    return false_v;
  }
  if (!vkr_bakery_is_file(path)) {
    return true_v;
  }

  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(
          path, (uint64_t)VKR_DEPOT_REMOTES_MAX * REPOSITORY_REMOTE_LINE,
          &bytes, &size)) {
    return false_v;
  }
  char *text = malloc(size + 1u);
  if (!text) {
    free(bytes);
    return false_v;
  }
  MemCopy(text, bytes, size);
  text[size] = '\0';
  free(bytes);

  char *line = text;
  while (line && *line && *out_count < capacity) {
    char *next = strchr(line, '\n');
    if (next) {
      *next++ = '\0';
    }
    VkrDepotRemoteEntry remote = {0};
    if (line[0] != '#' && repository_remote_parse(line, &remote)) {
      out_remotes[(*out_count)++] = remote;
    }
    line = next;
  }
  free(text);
  return true_v;
}

bool8_t vkr_depot_remote_find(const char *store_root, const char *name,
                              VkrDepotRemoteEntry *out_remote) {
  VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
  uint32_t count = 0u;
  if (!vkr_depot_remotes_load(store_root, remotes, ArrayCount(remotes),
                              &count)) {
    return false_v;
  }
  const char *wanted = name ? name : VKR_DEPOT_ORIGIN;
  for (uint32_t i = 0u; i < count; ++i) {
    if (strcmp(remotes[i].name, wanted) == 0) {
      *out_remote = remotes[i];
      return true_v;
    }
  }
  if (!name && count == 1u) {
    *out_remote = remotes[0];
    return true_v;
  }
  return false_v;
}

/* Writes the list whole; the caller holds the store's lock. */
static bool8_t repository_remotes_save(const char *store_root,
                                       const VkrDepotRemoteEntry *remotes,
                                       uint32_t count) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(path, sizeof(path), store_root, "remotes")) {
    return false_v;
  }
  char *text = malloc((size_t)Max(count, 1u) * REPOSITORY_REMOTE_LINE);
  if (!text) {
    return false_v;
  }
  uint64_t size = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    char key[65];
    char repository[VKR_DEPOT_REPOSITORY_ID_SIZE * 2u + 1u];
    repository_hex_format(remotes[i].key, 32u, key);
    repository_hex_format(remotes[i].repository, VKR_DEPOT_REPOSITORY_ID_SIZE,
                          repository);
    size += (uint64_t)snprintf(text + size, REPOSITORY_REMOTE_LINE,
                               "%s %s %s %s %u\n", remotes[i].name,
                               remotes[i].address, key, repository,
                               remotes[i].interval_seconds);
  }
  const bool8_t ok = vkr_bakery_write_file_atomic(path, text, size);
  free(text);
  return ok;
}

bool8_t vkr_depot_remote_set(const char *store_root,
                             const VkrDepotRemoteEntry *remote) {
  if (!vkr_depot_remote_name_valid(remote->name) || !remote->address[0] ||
      strchr(remote->address, ' ') || strchr(remote->address, '\n')) {
    return false_v;
  }
  VkrPlatformProcessLock lock;
  if (!repository_lock(store_root, &lock)) {
    return false_v;
  }

  VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
  uint32_t count = 0u;
  bool8_t ok =
      vkr_depot_remotes_load(store_root, remotes, ArrayCount(remotes), &count);
  uint32_t index = count;
  for (uint32_t i = 0u; ok && i < count; ++i) {
    if (strcmp(remotes[i].name, remote->name) == 0) {
      index = i;
    }
  }
  if (ok && index == ArrayCount(remotes)) {
    ok = false_v;
  }
  if (ok) {
    remotes[index] = *remote;
    ok = repository_remotes_save(store_root, remotes,
                                 index == count ? count + 1u : count);
  }
  vkr_platform_process_lock_release(&lock);
  return ok;
}

bool8_t vkr_depot_remote_remove(const char *store_root, const char *name) {
  char folder[REPOSITORY_RELATIVE_MAX];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!repository_tracking_folder(name, folder) ||
      !repository_path(store_root, folder, NULL, directory,
                       sizeof(directory))) {
    return false_v;
  }
  VkrPlatformProcessLock lock;
  if (!repository_lock(store_root, &lock)) {
    return false_v;
  }

  VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
  uint32_t count = 0u;
  bool8_t found = false_v;
  bool8_t ok =
      vkr_depot_remotes_load(store_root, remotes, ArrayCount(remotes), &count);
  for (uint32_t i = 0u; ok && i < count; ++i) {
    if (strcmp(remotes[i].name, name) == 0) {
      MemCopy(&remotes[i], &remotes[i + 1u],
              (size_t)(count - i - 1u) * sizeof(remotes[0]));
      --count;
      found = true_v;
      break;
    }
  }
  ok = ok && found && repository_remotes_save(store_root, remotes, count) &&
       vkr_bakery_remove_tree(directory);
  vkr_platform_process_lock_release(&lock);
  return ok;
}

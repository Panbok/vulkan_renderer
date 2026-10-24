#include "vkr_depot_repository.h"

#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_protocol.h"
#include "vkr_depot_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a swap waits for the store's lock: 200 tries 10 ms apart. A
   holder keeps it only to read and write one 32-byte file. */
#define REPOSITORY_LOCK_TRIES 200u
#define REPOSITORY_LOCK_WAIT_MS 10u
#define REPOSITORY_LOCK_NAME 64u

static bool8_t repository_ref_path(const char *store_root, const char *branch,
                                   char *out, uint32_t capacity) {
  char relative[VKR_DEPOT_BRANCH_NAME + 8u];
  snprintf(relative, sizeof(relative), "refs/%s", branch);
  return vkr_bakery_path_join(out, capacity, store_root, relative);
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

bool8_t vkr_depot_ref_read(const char *store_root, const char *branch,
                           VkrDepotId *out_commit) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_depot_branch_valid(branch) ||
      !repository_ref_path(store_root, branch, path, sizeof(path)) ||
      !vkr_bakery_is_file(path)) {
    return false_v;
  }

  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_bakery_read_file(path, 64u, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok = size == VKR_DEPOT_ID_SIZE;
  if (ok) {
    memcpy(out_commit->bytes, bytes, VKR_DEPOT_ID_SIZE);
  }
  free(bytes);
  return ok;
}

VkrDepotRefSwap vkr_depot_ref_swap(const char *store_root, const char *branch,
                                   const VkrDepotId *expected,
                                   const VkrDepotId *commit,
                                   VkrDepotId *out_current) {
  VkrDepotId current = {0};
  if (out_current) {
    MemZero(out_current, sizeof(*out_current));
  }
  char directory[VKR_DEPOT_PATH_CAPACITY];
  char path[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_depot_branch_valid(branch) ||
      !vkr_bakery_path_join(directory, sizeof(directory), store_root, "refs") ||
      !repository_ref_path(store_root, branch, path, sizeof(path))) {
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
  } else if (!vkr_bakery_make_directories(directory) ||
             !vkr_bakery_write_file_atomic(path, commit->bytes,
                                           VKR_DEPOT_ID_SIZE)) {
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

typedef struct RefListVisit {
  VkrDepotBranchVisitor visit;
  void *context;
  uint32_t count;
  bool8_t stopped;
} RefListVisit;

static bool8_t repository_ref_name_visit(void *context, const char *name,
                                         bool8_t is_directory) {
  RefListVisit *walk = context;
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

bool8_t vkr_depot_ref_list(const char *store_root, VkrDepotBranchVisitor visit,
                           void *context) {
  char directory[VKR_DEPOT_PATH_CAPACITY];
  if (!vkr_bakery_path_join(directory, sizeof(directory), store_root, "refs")) {
    return false_v;
  }
  if (!vkr_bakery_is_directory(directory)) {
    return true_v;
  }

  RefListVisit walk = {.visit = visit, .context = context};
  return vkr_bakery_list_directory(directory, repository_ref_name_visit,
                                   &walk) ||
         walk.stopped;
}

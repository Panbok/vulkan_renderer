#include "vkr_depot_remote.h"

#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_client.h"
#include "vkr_depot_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Branches the server lists, with room for many session branches. */
#define REMOTE_REFS_MAX 1024u
/* Requests one transfer takes at most. */
#define REMOTE_BATCH_MAX 256u

typedef enum RemoteFileState {
  REMOTE_FILE_ABSENT = 0,
  REMOTE_FILE_QUEUED,
  REMOTE_FILE_PRESENT,
  REMOTE_FILE_FAILED,
} RemoteFileState;

struct VkrDepotRemote {
  VkrAllocator *allocator;
  char directory[VKR_DEPOT_PATH_CAPACITY];
  VkrDepotStore store;
  /* The worker's alone once it runs. Between transfers nothing pumps it, so
     the transport's idle timeout may close it; a failed transfer then
     reconnects once and tries again. */
  VkrDepotConnection *connection;
  char address[VKR_DEPOT_REMOTE_MAX];
  uint8_t server_key[32];
  /* Every listed file, sorted by path, and its state. */
  VkrDepotFileList files;
  uint8_t *states;

  VkrThread thread;
  VkrMutex mutex;
  /* Wakes the worker; and the callers when a transfer ends. */
  VkrCondVar work;
  VkrCondVar done;
  uint32_t *queue;
  uint32_t queue_count;
  bool8_t stopping;
  /* When opening ended, on the platform clock. */
  uint64_t opened_us;
  VkrDepotRemoteStats stats;
};

static uint64_t remote_now_us(void) {
  return (uint64_t)(vkr_platform_get_absolute_time() * 1000000.0);
}

/* The newest commit of `name`; false when the server has no such branch. */
static bool8_t remote_find_ref(const VkrDepotRef *refs, uint32_t count,
                               const char *name, VkrDepotId *out) {
  for (uint32_t i = 0u; i < count; ++i) {
    if (strcmp(refs[i].name, name) == 0) {
      *out = refs[i].commit;
      return true_v;
    }
  }
  return false_v;
}

/* Fetches the trees of `commit` and lists its files into `out_files`. */
static bool8_t remote_list(VkrDepotRemote *remote, VkrDepotClient *client,
                           const VkrDepotId *commit,
                           VkrDepotFileList *out_files, char *error,
                           uint32_t capacity) {
  if (!vkr_depot_client_fetch_trees(client, commit, NULL)) {
    snprintf(error, capacity, "the trees did not arrive: %s",
             vkr_depot_client_error(client));
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit parsed;
  const bool8_t ok =
      vkr_depot_store_get(&remote->store, commit, &bytes, &size) &&
      vkr_depot_commit_parse(bytes, size, &parsed) &&
      vkr_depot_tree_files(&remote->store, &parsed.tree, out_files);
  free(bytes);
  if (!ok) {
    snprintf(error, capacity, "the commit's file list could not be read");
  }
  return ok;
}

/* A file is present when it lies under the directory with its listed size
   and its blob is in the store, as this remote left it. */
static bool8_t remote_file_present(const VkrDepotRemote *remote,
                                   const VkrDepotIndexEntry *entry) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  VkrBakeryStat stat;
  return vkr_bakery_path_join(path, sizeof(path), remote->directory,
                              entry->path) &&
         vkr_bakery_stat(path, &stat) && stat.exists && !stat.is_directory &&
         stat.size == entry->size &&
         vkr_depot_store_has(&remote->store, &entry->blob);
}

/* Downloads `count` blobs, reconnecting once when the transfer fails. */
static bool8_t remote_transfer(VkrDepotRemote *remote, const VkrDepotId *blobs,
                               uint32_t count,
                               VkrDepotTransferStats *out_transfer) {
  for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
    VkrDepotClient *client = vkr_depot_connection_client(remote->connection);
    if (client &&
        vkr_depot_client_fetch_files(client, blobs, count, out_transfer)) {
      return true_v;
    }
    if (client) {
      fprintf(stderr, "remote content: a transfer failed: %s\n",
              vkr_depot_client_error(client));
    }
    vkr_depot_disconnect(remote->connection);
    const VkrDepotConnectConfig connect = {.address = remote->address,
                                           .server_key = remote->server_key,
                                           .store = &remote->store};
    char error[256];
    remote->connection = vkr_depot_connect(remote->allocator, &connect, NULL,
                                           error, sizeof(error));
    if (!remote->connection) {
      fprintf(stderr, "remote content: reconnecting failed: %s\n", error);
      return false_v;
    }
  }
  return false_v;
}

static void *remote_worker(void *argument) {
  VkrDepotRemote *remote = argument;
  uint32_t batch[REMOTE_BATCH_MAX];
  VkrDepotId blobs[REMOTE_BATCH_MAX];
  for (;;) {
    vkr_mutex_lock(remote->mutex);
    while (!remote->stopping && remote->queue_count == 0u) {
      vkr_cond_wait(remote->work, remote->mutex);
    }
    if (remote->stopping) {
      vkr_mutex_unlock(remote->mutex);
      break;
    }
    const uint32_t count = Min(remote->queue_count, REMOTE_BATCH_MAX);
    MemCopy(batch, remote->queue, count * sizeof(batch[0]));
    remote->queue_count -= count;
    MemCopy(remote->queue, remote->queue + count,
            remote->queue_count * sizeof(remote->queue[0]));
    vkr_mutex_unlock(remote->mutex);

    uint32_t missing = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
      const VkrDepotId *blob = &remote->files.entries[batch[i]].blob;
      if (!vkr_depot_store_has(&remote->store, blob)) {
        blobs[missing++] = *blob;
      }
    }
    VkrDepotTransferStats transfer = {0};
    if (missing) {
      (void)remote_transfer(remote, blobs, missing, &transfer);
    }
    uint8_t written[REMOTE_BATCH_MAX];
    for (uint32_t i = 0u; i < count; ++i) {
      const VkrDepotIndexEntry *entry = &remote->files.entries[batch[i]];
      char path[VKR_DEPOT_PATH_CAPACITY];
      written[i] = vkr_depot_store_has(&remote->store, &entry->blob) &&
                   vkr_bakery_path_join(path, sizeof(path), remote->directory,
                                        entry->path) &&
                   vkr_depot_write_blob(&remote->store, &entry->blob, path);
    }

    vkr_mutex_lock(remote->mutex);
    for (uint32_t i = 0u; i < count; ++i) {
      remote->states[batch[i]] =
          written[i] ? REMOTE_FILE_PRESENT : REMOTE_FILE_FAILED;
      remote->stats.fetched += written[i];
      remote->stats.failed += !written[i];
    }
    remote->stats.stored_bytes += transfer.stored_bytes;
    remote->stats.last_write_us = remote_now_us() - remote->opened_us;
    vkr_cond_broadcast(remote->done);
    vkr_mutex_unlock(remote->mutex);
  }
  return NULL;
}

static void remote_free(VkrDepotRemote *remote) {
  vkr_depot_disconnect(remote->connection);
  vkr_depot_file_list_free(&remote->files);
  free(remote->states);
  free(remote->queue);
  if (remote->mutex) {
    vkr_mutex_destroy(remote->allocator, &remote->mutex);
  }
  if (remote->work) {
    vkr_cond_destroy(remote->allocator, &remote->work);
  }
  if (remote->done) {
    vkr_cond_destroy(remote->allocator, &remote->done);
  }
  free(remote);
}

VkrDepotRemote *vkr_depot_remote_open(VkrAllocator *allocator,
                                      const VkrDepotRemoteConfig *config,
                                      char *error, uint32_t capacity) {
  VkrDepotRemote *remote = calloc(1u, sizeof(VkrDepotRemote));
  VkrDepotRef *refs = calloc(REMOTE_REFS_MAX, sizeof(VkrDepotRef));
  VkrDepotFileList cooked = {0};
  if (!remote || !refs) {
    snprintf(error, capacity, "out of memory");
    goto fail;
  }
  remote->allocator = allocator;
  const char *branch = config->branch ? config->branch : "main";
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  snprintf(remote->directory, sizeof(remote->directory), "%s",
           config->directory);
  if (config->store_root) {
    snprintf(store_root, sizeof(store_root), "%s", config->store_root);
  } else if (!vkr_bakery_path_join(store_root, sizeof(store_root),
                                   config->directory, ".vkrdepot-stream")) {
    snprintf(error, capacity, "the directory path is too long");
    goto fail;
  }
  if (!vkr_bakery_make_directories(config->directory) ||
      !vkr_depot_store_open(&remote->store, store_root, config->codec)) {
    snprintf(error, capacity, "the store at %s could not open", store_root);
    goto fail;
  }

  snprintf(remote->address, sizeof(remote->address), "%s", config->address);
  MemCopy(remote->server_key, config->server_key, sizeof(remote->server_key));
  const VkrDepotConnectConfig connect = {.address = config->address,
                                         .server_key = config->server_key,
                                         .store = &remote->store};
  remote->connection =
      vkr_depot_connect(allocator, &connect, NULL, error, capacity);
  if (!remote->connection) {
    goto fail;
  }
  VkrDepotClient *client = vkr_depot_connection_client(remote->connection);
  uint32_t ref_count = 0u;
  if (!vkr_depot_client_refs(client, refs, REMOTE_REFS_MAX, &ref_count)) {
    snprintf(error, capacity, "the branches did not arrive: %s",
             vkr_depot_client_error(client));
    goto fail;
  }
  if (!remote_find_ref(refs, ref_count, branch, &remote->stats.commit)) {
    snprintf(error, capacity, "the server has no branch %s", branch);
    goto fail;
  }
  if (!remote_list(remote, client, &remote->stats.commit, &remote->files, error,
                   capacity)) {
    goto fail;
  }
  vkr_depot_file_list_sort(&remote->files);
  /* Platform files the project's history leaves out: each joins the list
     unless the main commit lists the same path. */
  char cooked_branch[VKR_DEPOT_BRANCH_NAME];
  vkr_depot_cooked_branch(branch, cooked_branch, sizeof(cooked_branch));
  if (!config->no_cooked && remote_find_ref(refs, ref_count, cooked_branch,
                                            &remote->stats.cooked_commit)) {
    if (!remote_list(remote, client, &remote->stats.cooked_commit, &cooked,
                     error, capacity)) {
      goto fail;
    }
    VkrDepotFileList joined = {0};
    for (uint32_t i = 0u; i < cooked.count; ++i) {
      const VkrDepotIndexEntry *entry = &cooked.entries[i];
      if (!vkr_depot_file_list_find(&remote->files, entry->path) &&
          !vkr_depot_file_list_add(&joined, entry->path, entry->size,
                                   entry->mtime_ns, &entry->blob)) {
        vkr_depot_file_list_free(&joined);
        snprintf(error, capacity, "out of memory");
        goto fail;
      }
    }
    for (uint32_t i = 0u; i < joined.count; ++i) {
      const VkrDepotIndexEntry *entry = &joined.entries[i];
      if (!vkr_depot_file_list_add(&remote->files, entry->path, entry->size,
                                   entry->mtime_ns, &entry->blob)) {
        vkr_depot_file_list_free(&joined);
        snprintf(error, capacity, "out of memory");
        goto fail;
      }
    }
    vkr_depot_file_list_free(&joined);
    vkr_depot_file_list_sort(&remote->files);
  }

  remote->states = calloc(remote->files.count + 1u, 1u);
  remote->queue = calloc(remote->files.count + 1u, sizeof(uint32_t));
  if (!remote->states || !remote->queue) {
    snprintf(error, capacity, "out of memory");
    goto fail;
  }
  remote->stats.listed = remote->files.count;
  for (uint32_t i = 0u; i < remote->files.count; ++i) {
    if (remote_file_present(remote, &remote->files.entries[i])) {
      remote->states[i] = REMOTE_FILE_PRESENT;
      remote->stats.present += 1u;
    }
  }
  remote->opened_us = remote_now_us();
  if (!vkr_mutex_create(allocator, &remote->mutex) ||
      !vkr_cond_create(allocator, &remote->work) ||
      !vkr_cond_create(allocator, &remote->done) ||
      !vkr_thread_create(allocator, &remote->thread, remote_worker, remote)) {
    snprintf(error, capacity, "the worker thread did not start");
    goto fail;
  }
  vkr_depot_file_list_free(&cooked);
  free(refs);
  return remote;

fail:
  vkr_depot_file_list_free(&cooked);
  free(refs);
  if (remote) {
    remote_free(remote);
  }
  return NULL;
}

bool8_t vkr_depot_remote_fetch(void *context, const char *relative) {
  VkrDepotRemote *remote = context;
  const VkrDepotIndexEntry *entry =
      remote ? vkr_depot_file_list_find(&remote->files, relative) : NULL;
  if (!entry) {
    return false_v;
  }
  const uint32_t index = (uint32_t)(entry - remote->files.entries);
  vkr_mutex_lock(remote->mutex);
  uint8_t state = remote->states[index];
  if (state == REMOTE_FILE_ABSENT) {
    remote->states[index] = REMOTE_FILE_QUEUED;
    remote->queue[remote->queue_count++] = index;
    vkr_cond_signal(remote->work);
    state = REMOTE_FILE_QUEUED;
  }
  if (state == REMOTE_FILE_QUEUED) {
    const uint64_t start = remote_now_us();
    while (remote->states[index] == REMOTE_FILE_QUEUED && !remote->stopping) {
      vkr_cond_wait(remote->done, remote->mutex);
    }
    remote->stats.wait_us += remote_now_us() - start;
    state = remote->states[index];
  }
  vkr_mutex_unlock(remote->mutex);
  return state == REMOTE_FILE_PRESENT;
}

void vkr_depot_remote_stats(VkrDepotRemote *remote,
                            VkrDepotRemoteStats *out_stats) {
  vkr_mutex_lock(remote->mutex);
  *out_stats = remote->stats;
  vkr_mutex_unlock(remote->mutex);
}

void vkr_depot_remote_close(VkrDepotRemote *remote) {
  if (!remote) {
    return;
  }
  vkr_mutex_lock(remote->mutex);
  remote->stopping = true_v;
  vkr_cond_broadcast(remote->work);
  vkr_cond_broadcast(remote->done);
  vkr_mutex_unlock(remote->mutex);
  (void)vkr_thread_join(remote->thread);
  (void)vkr_thread_destroy(remote->allocator, &remote->thread);
  remote_free(remote);
}

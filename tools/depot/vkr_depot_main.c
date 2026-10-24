/* vkr_depot: a versioned, content-addressed asset depot over the VKR network
 * protocol (docs/proposals/network-protocol.md, "Depot").
 *
 *   vkr_depot keygen [--identity FILE]
 *   vkr_depot init STORE
 *   vkr_depot serve STORE [--bind ADDR] [--open] [--name NAME]
 *                   [--discovery-port PORT]
 *   vkr_depot discover [--to ADDR] [--timeout MS] [--discovery-port PORT]
 *   vkr_depot mirror STORE PEER-ADDR PEER-KEY [--local ADDR] [--branch NAME]
 *                    [--watch SECONDS]
 *   vkr_depot access STORE KEY none|read|write|admin
 *   vkr_depot clone ADDR SERVER-KEY DIR [--branch NAME] [--store DIR]
 *                   [--repository ID]
 *   vkr_depot status [DIR]
 *   vkr_depot commit [DIR] -m MESSAGE [--push] [--remote NAME]
 *   vkr_depot push|fetch [DIR] [--remote NAME] [--branch NAME]
 *   vkr_depot pull [DIR] [--remote NAME] [--take-theirs|--keep-mine]
 *                  [--bakery PATH]
 *   vkr_depot merge BRANCH|REMOTE/BRANCH [--dir DIR] [--take-theirs|
 *                   --keep-mine] [--bakery PATH]
 *   vkr_depot remote [list|add NAME ADDR KEY [--interval SECONDS]|remove
 *                    NAME] [--dir DIR|--store STORE]
 *   vkr_depot sync STORE [--remote NAME]
 *   vkr_depot branch [NAME [--delete [--force]]] [--dir DIR]
 *   vkr_depot switch NAME [--dir DIR] [--take-theirs|--keep-mine]
 *   vkr_depot refs [DIR]
 *   vkr_depot log [DIR] [--count N]
 *   vkr_depot lock PATH [--dir DIR]
 *   vkr_depot unlock PATH [--dir DIR] [--force]
 *   vkr_depot locks [DIR]
 *
 * A working copy's store is its repository (ADR-133):
 * `commit` snapshots the directory and moves the branch there, with no
 * network; `push` uploads the commits a remote lacks and moves its branch
 * when that is a fast-forward, `fetch` brings the remotes' branches in and
 * `pull` fetches and fast-forwards the working copy's branch, or merges when
 * both sides moved it (vkr_depot_merge.h; units of `.vkrdepotmerge` go to
 * `vkr_bakery`, found beside this program, through `$VKR_BAKERY` or
 * `--bakery`). `commit
 * --push` commits and pushes, or, when the push is refused, leaves the
 * branch and the working copy as they were, as `commit` did before
 * repositories. A remote defaults to `origin`, the server a clone came from.
 * `sync` does for a served store, with no working directory, what `mirror`
 * does for two servers, through one of the store's remotes, and the
 * machine's service runs it for each remote with an `--interval`. `serve` also
 * answers LAN discovery (vkr_depot_discovery.h), and `mirror` keeps the
 * branches of the local server and a peer server equal. A running
 * server applies `access` within a second; a key it refused waits there as a
 * request. */

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_client.h"
#include "vkr_depot_discovery.h"
#include "vkr_depot_merge.h"
#include "vkr_depot_remote.h"
#include "vkr_depot_repository.h"
#include "vkr_depot_server.h"
#include "vkr_depot_service.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_workspace.h"
#include "vkr_depot_zstd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkrAllocator depot_allocator;
static VkrDMemory depot_memory;

static void depot_print_key(const uint8_t key[32]) {
  for (uint32_t i = 0u; i < 32u; ++i) {
    printf("%02x", key[i]);
  }
}

/* A key is 64 lowercase hex digits, the same form as an object ID. */
static bool8_t depot_parse_key(const char *hex, uint8_t out[32]) {
  VkrDepotId key;
  if (!vkr_depot_id_parse(hex, &key)) {
    return false_v;
  }
  memcpy(out, key.bytes, 32u);
  return true_v;
}

/* A repository ID: VKR_DEPOT_REPOSITORY_ID_SIZE bytes in hexadecimal. */
static bool8_t
depot_parse_repository(const char *hex,
                       uint8_t out[VKR_DEPOT_REPOSITORY_ID_SIZE]) {
  if (strlen(hex) != VKR_DEPOT_REPOSITORY_ID_SIZE * 2u) {
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

// =============================================================================
// Connections
// =============================================================================

/* Connects to the working copy's server; NULL after printing why. */
static VkrDepotConnection *depot_connect(const VkrDepotWorkspace *workspace) {
  char error[256];
  const VkrDepotConnectConfig config = {.address = workspace->remote,
                                        .server_key = workspace->server_key,
                                        .store = &workspace->store};
  VkrDepotConnection *connection =
      vkr_depot_connect(&depot_allocator, &config, NULL, error, sizeof(error));
  if (!connection) {
    fprintf(stderr, "%s\n", error);
  }
  return connection;
}

// =============================================================================
// Commands
// =============================================================================

static int depot_keygen(void) {
  VkrNetKeyPair keys;
  if (!vkr_depot_identity(&keys, true_v)) {
    fprintf(stderr, "could not create the identity key\n");
    return 1;
  }
  printf("identity ");
  depot_print_key(keys.public_key);
  printf("\n");
  return 0;
}

static int depot_init(const char *store_root) {
  VkrNetKeyPair keys;
  VkrDepotStore store;
  if (!vkr_depot_store_open(&store, store_root, vkr_depot_zstd_codec()) ||
      !vkr_depot_server_key(store_root, &keys)) {
    fprintf(stderr, "could not create the store at %s\n", store_root);
    return 1;
  }
  printf("store %s\nserver key ", store_root);
  depot_print_key(keys.public_key);
  printf("\n");
  return 0;
}

static void depot_serve_accept(void *context, const VkrNetAddress *address,
                               const uint8_t key[32], const char *name,
                               bool8_t accepted) {
  (void)context;
  (void)name;
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(address, text, sizeof(text));
  printf("%s %s key ", accepted ? "accepted" : "refused", text);
  depot_print_key(key);
  printf("\n");
  fflush(stdout);
}

/* Serves on the service thread until the socket fails; a changed access
   file applies within a second. */
static int depot_serve(const char *store_root, const char *bind,
                       bool8_t open_access, const char *name,
                       uint16_t discovery_port) {
  const VkrDepotServiceConfig config = {.store_root = store_root,
                                        .bind = bind,
                                        .port_attempts = 1u,
                                        .name = name,
                                        .discovery_port = discovery_port,
                                        .codec = vkr_depot_zstd_codec(),
                                        .open_access = open_access,
                                        .on_accept = depot_serve_accept};
  char error[256];
  VkrDepotService *service =
      vkr_depot_service_start(&depot_allocator, &config, error, sizeof(error));
  if (!service) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  VkrDepotServiceInfo info;
  vkr_depot_service_info(service, &info);
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&info.address, text, sizeof(text));
  printf("serving %s on %s key ", store_root, text);
  depot_print_key(info.server_key);
  printf("%s\n", open_access ? " (open access)" : "");
  if (info.discoverable) {
    printf("discoverable as %s on UDP %u\n", info.name, info.discovery_port);
  } else {
    printf("not discoverable: UDP %u is taken\n", info.discovery_port);
  }
  fflush(stdout);
  for (;;) {
    vkr_platform_sleep(200u);
    vkr_depot_service_info(service, &info);
    if (info.failed) {
      fprintf(stderr, "the socket failed\n");
      break;
    }
  }
  vkr_depot_service_stop(service);
  return 1;
}

static int depot_discover(const char *to, uint32_t timeout_ms,
                          uint16_t discovery_port) {
  VkrNetAddress target;
  if (to && !vkr_depot_address(to, &target)) {
    fprintf(stderr, "invalid --to address %s\n", to);
    return 1;
  }
  VkrDepotDiscovered found[32];
  const uint32_t count =
      vkr_depot_discover(to ? &target : NULL, discovery_port, timeout_ms, found,
                         ArrayCount(found));
  for (uint32_t i = 0u; i < count; ++i) {
    char text[VKR_NET_ADDRESS_TEXT];
    vkr_net_address_format(&found[i].address, text, sizeof(text));
    printf("%s ", text);
    depot_print_key(found[i].server_key);
    printf(" %s %s\n", found[i].name[0] ? found[i].name : "-",
           found[i].host[0] ? found[i].host : "-");
    /* Each repository, for `clone --repository`. */
    for (uint32_t r = 0u; r < found[i].repository_count; ++r) {
      printf("  repository ");
      for (uint32_t b = 0u; b < VKR_DEPOT_REPOSITORY_ID_SIZE; ++b) {
        printf("%02x", found[i].repositories[r].id[b]);
      }
      printf(" %s\n", found[i].repositories[r].name);
    }
  }
  if (!count) {
    fprintf(stderr, "no depot server answered within %u ms\n", timeout_ms);
  }
  return count ? 0 : 1;
}

static void depot_mirror_print(void *context, const char *branch,
                               VkrDepotMirrorAction action,
                               const VkrDepotId *commit, const char *detail) {
  static const char *const actions[] = {"same", "to-peer", "from-peer",
                                        "diverged", "failed"};
  uint32_t *failures = context;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(commit, hex);
  printf("%s %s %.12s%s%s\n", actions[action], branch, hex,
         detail[0] ? ": " : "", detail);
  fflush(stdout);
  *failures += action >= VKR_DEPOT_MIRROR_DIVERGED;
}

static int depot_mirror_command(const char *store_root, const char *peer,
                                const char *peer_key_hex, const char *local,
                                const char *branch, uint32_t watch_seconds) {
  uint8_t peer_key[32];
  VkrNetKeyPair local_keys;
  VkrDepotStore store;
  if (!depot_parse_key(peer_key_hex, peer_key) ||
      !vkr_depot_store_open(&store, store_root, vkr_depot_zstd_codec()) ||
      !vkr_depot_server_key(store_root, &local_keys)) {
    fprintf(stderr, "mirror needs STORE PEER-ADDR PEER-KEY\n");
    return 1;
  }
  vkr_net_random(local_keys.secret_key, sizeof(local_keys.secret_key));
  for (;;) {
    char error[256];
    uint32_t failures = 0u;
    if (!vkr_depot_mirror(&depot_allocator, &store, local,
                          local_keys.public_key, peer, peer_key, branch,
                          depot_mirror_print, &failures, error,
                          sizeof(error))) {
      fprintf(stderr, "%s\n", error);
      failures += 1u;
    }
    if (!watch_seconds) {
      return failures ? 1 : 0;
    }
    vkr_platform_sleep((uint64_t)watch_seconds * 1000u);
  }
}

static int depot_access(const char *store_root, const char *key_hex,
                        const char *role_name) {
  uint8_t key[32];
  static const char *const roles[] = {"none", "read", "write", "admin"};
  uint32_t role = UINT32_MAX;
  for (uint32_t i = 0u; i < ArrayCount(roles); ++i) {
    if (strcmp(role_name, roles[i]) == 0) {
      role = i;
    }
  }
  if (!depot_parse_key(key_hex, key) || role == UINT32_MAX ||
      !vkr_depot_access_set(store_root, key, (VkrDepotRole)role)) {
    fprintf(stderr, "access needs STORE KEY none|read|write|admin\n");
    return 1;
  }
  printf("%s ", role_name);
  depot_print_key(key);
  printf("\n");
  return 0;
}

static int depot_clone(const char *address, const char *key_hex,
                       const char *directory, const char *branch,
                       const char *store_root, const char *repository_hex,
                       bool8_t sparse) {
  uint8_t server_key[32];
  uint8_t repository[VKR_DEPOT_REPOSITORY_ID_SIZE] = {0};
  if (!depot_parse_key(key_hex, server_key) ||
      !vkr_depot_branch_valid(branch) ||
      (repository_hex && !depot_parse_repository(repository_hex, repository))) {
    fprintf(stderr, "clone needs ADDR SERVER-KEY DIR [--branch NAME] "
                    "[--repository ID]\n");
    return 1;
  }
  const VkrDepotCloneConfig config = {.address = address,
                                      .server_key = server_key,
                                      .repository = repository,
                                      .directory = directory,
                                      .branch = branch,
                                      .store_root = store_root,
                                      .codec = vkr_depot_zstd_codec(),
                                      .sparse = sparse};
  VkrDepotCloneResult result;
  char error[256];
  if (!vkr_depot_clone(&depot_allocator, &config, &result, error,
                       sizeof(error))) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  if (result.empty) {
    printf("branch %s is empty; the first commit creates it\n", branch);
    return 0;
  }
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&result.commit, hex);
  printf("cloned %s at %.12s: %u files%s\n", branch, hex, result.files,
         sparse ? ", fetched when opened" : "");
  return 0;
}

/* Files a sparse working copy tracks but holds no copy of. */
static uint32_t depot_unfetched(const VkrDepotWorkspace *workspace) {
  uint32_t unfetched = 0u;
  for (uint32_t i = 0u; i < workspace->index.count; ++i) {
    unfetched +=
        workspace->index.entries[i].mtime_ns == VKR_DEPOT_MTIME_UNFETCHED;
  }
  return unfetched;
}

static int depot_status(const char *directory) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotChangeList changes;
  char error[256];
  int result = 1;
  if (vkr_depot_status(&workspace, &changes, error, sizeof(error))) {
    static const char codes[] = {'?', 'A', 'M', 'D'};
    for (uint32_t i = 0u; i < changes.count; ++i) {
      printf("%c %s\n", codes[changes.entries[i].change],
             changes.entries[i].path);
    }
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&workspace.base, hex);
    printf("%u changes on %s at %.12s", changes.count, workspace.branch, hex);
    if (workspace.sparse) {
      printf("; %u of %u files not fetched", depot_unfetched(&workspace),
             workspace.index.count);
    }
    VkrDepotRemoteEntry remote;
    VkrDepotId local;
    VkrDepotId tracked;
    uint32_t ahead = 0u;
    uint32_t behind = 0u;
    if (vkr_depot_remote_find(workspace.store.root, NULL, &remote) &&
        vkr_depot_ref_read(workspace.store.root, workspace.branch, &local) &&
        vkr_depot_tracking_read(workspace.store.root, remote.name,
                                workspace.branch, &tracked) &&
        vkr_depot_ahead_behind(&workspace.store, &local, &tracked, &ahead,
                               &behind)) {
      printf("; %u ahead, %u behind %s/%s", ahead, behind, remote.name,
             workspace.branch);
    }
    printf("\n");
    vkr_depot_change_list_free(&changes);
    result = 0;
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_open(VkrDepotWorkspace *workspace, const char *directory) {
  if (!vkr_depot_workspace_open(workspace, directory, vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  return 0;
}

static VkrDepotFileList depot_index_copy(const VkrDepotFileList *index) {
  VkrDepotFileList copy = {0};
  for (uint32_t i = 0u; i < index->count; ++i) {
    const VkrDepotIndexEntry *entry = &index->entries[i];
    if (!vkr_depot_file_list_add(&copy, entry->path, entry->size,
                                 entry->mtime_ns, &entry->blob)) {
      vkr_depot_file_list_free(&copy);
      copy.count = UINT32_MAX;
      return copy;
    }
  }
  return copy;
}

static void depot_print_push(const VkrDepotPushResult *result,
                             const char *branch, const char *remote) {
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&result->commit, hex);
  if (result->status == VKR_DEPOT_PUSH_UP_TO_DATE) {
    printf("%s on %s is up to date at %.12s\n", branch, remote, hex);
    return;
  }
  printf("pushed %s to %s at %.12s: %u commits, %llu objects, %llu bytes "
         "uploaded\n",
         branch, remote, hex, result->commits,
         (unsigned long long)result->uploaded.objects,
         (unsigned long long)result->uploaded.stored_bytes);
}

/* Commits locally; with `push`, pushes too, and puts the branch, base and
   index back when the push is refused. */
static int depot_commit(const char *directory, const char *message,
                        bool8_t push, const char *remote) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  const VkrDepotId base = workspace.base;
  VkrDepotFileList index = depot_index_copy(&workspace.index);
  char error[256];
  VkrDepotCommitResult committed;
  int result = 1;
  if (index.count == UINT32_MAX) {
    fprintf(stderr, "out of memory\n");
  } else if (!vkr_depot_commit_local(&workspace, message, &committed, error,
                                     sizeof(error))) {
    fprintf(stderr, "%s\n", error);
  } else if (committed.empty && !push) {
    printf("nothing to commit on %s\n", workspace.branch);
    result = 0;
  } else if (!push) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&committed.commit, hex);
    printf("committed %s at %.12s: %u files\n", workspace.branch, hex,
           committed.files);
    result = 0;
  } else {
    VkrDepotPushResult pushed;
    if (vkr_depot_push(&depot_allocator, &workspace, remote, NULL, &pushed,
                       NULL, error, sizeof(error))) {
      depot_print_push(&pushed, workspace.branch,
                       remote ? remote : VKR_DEPOT_ORIGIN);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
      if (!committed.empty &&
          vkr_depot_ref_swap(workspace.store.root, workspace.branch,
                             &committed.commit, &base,
                             NULL) == VKR_DEPOT_REF_SWAP_OK) {
        vkr_depot_file_list_free(&workspace.index);
        workspace.index = index;
        MemZero(&index, sizeof(index));
        workspace.base = base;
        if (vkr_depot_workspace_save(&workspace)) {
          fprintf(stderr, "nothing was committed; the changes stay local\n");
        }
      }
    }
  }
  if (index.count != UINT32_MAX) {
    vkr_depot_file_list_free(&index);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_push_command(const char *directory, const char *remote,
                              const char *branch) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  VkrDepotPushResult pushed;
  char error[256];
  int result = 1;
  if (vkr_depot_push(&depot_allocator, &workspace, remote, branch, &pushed,
                     NULL, error, sizeof(error))) {
    depot_print_push(&pushed, branch ? branch : workspace.branch,
                     remote ? remote : VKR_DEPOT_ORIGIN);
    result = 0;
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static void depot_fetch_print(void *context, const char *branch,
                              VkrDepotFetchAction action,
                              const VkrDepotId *commit, const char *detail) {
  static const char *const actions[] = {"same", "new", "moved", "failed"};
  const char *remote = context;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(commit, hex);
  printf("%s %s/%s %.12s%s%s\n", actions[action], remote, branch, hex,
         detail[0] ? ": " : "", detail);
}

static int depot_fetch_command(const char *directory, const char *remote,
                               const char *branch) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  char error[256];
  VkrDepotRemoteEntry entry = {0};
  (void)vkr_depot_remote_find(workspace.store.root, remote, &entry);
  const int result =
      vkr_depot_fetch(&depot_allocator, &workspace, remote, branch,
                      depot_fetch_print, entry.name, NULL, error, sizeof(error))
          ? 0
          : 1;
  if (result) {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

/* Publishes the working copy's cooked platform files to its branch's
   cooked side branch for this computer. */
static int depot_cooked(const char *directory) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  /* The snapshot comes first, as for a commit. */
  VkrDepotId tree;
  uint32_t files = 0u;
  char error[256];
  if (!vkr_depot_cooked_snapshot(&workspace, &tree, &files, error,
                                 sizeof(error))) {
    fprintf(stderr, "%s\n", error);
    vkr_depot_workspace_close(&workspace);
    return 1;
  }
  VkrDepotConnection *connection = depot_connect(&workspace);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  if (client) {
    char branch[VKR_DEPOT_BRANCH_NAME];
    snprintf(branch, sizeof(branch), "%s", workspace.branch);
    VkrDepotCookedResult cooked;
    if (vkr_depot_publish_cooked(&workspace, client, branch, &tree, files,
                                 &cooked, error, sizeof(error))) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&cooked.commit, hex);
      printf("%s %s at %.12s: %u files, %llu bytes uploaded\n",
             cooked.unchanged ? "unchanged" : "published", cooked.branch, hex,
             cooked.files, (unsigned long long)cooked.uploaded_bytes);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
    }
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

/* Lists a branch's files into `directory` without fetching them, then
   fetches `path` as a streaming reader would and prints the timing. */
static int depot_stream(const char *address, const char *key_hex,
                        const char *directory, const char *path,
                        const char *branch) {
  uint8_t key[32];
  char absolute[VKR_DEPOT_PATH_CAPACITY];
  if (!depot_parse_key(key_hex, key) ||
      !vkr_bakery_path_absolute(directory, absolute, sizeof(absolute))) {
    fprintf(stderr, "stream needs a 64-digit server key and a directory\n");
    return 1;
  }
  const VkrDepotRemoteConfig config = {.directory = absolute,
                                       .address = address,
                                       .server_key = key,
                                       .branch = branch,
                                       .codec = vkr_depot_zstd_codec()};
  char error[256];
  const float64_t opened_at = vkr_platform_get_absolute_time();
  VkrDepotRemote *remote =
      vkr_depot_remote_open(&depot_allocator, &config, error, sizeof(error));
  if (!remote) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  const float64_t listed_at = vkr_platform_get_absolute_time();
  const bool8_t fetched = vkr_depot_remote_fetch(remote, path);
  const float64_t fetched_at = vkr_platform_get_absolute_time();
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(remote, &stats);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&stats.commit, hex);
  printf(
      "listed %u files of %.12s%s in %.1f ms; %s %s in %.1f ms "
      "(%llu bytes stored)\n",
      stats.listed, hex,
      vkr_depot_id_zero(&stats.cooked_commit) ? "" : " and its cooked branch",
      (listed_at - opened_at) * 1000.0, fetched ? "fetched" : "could not fetch",
      path, (fetched_at - listed_at) * 1000.0,
      (unsigned long long)stats.stored_bytes);
  vkr_depot_remote_close(remote);
  return fetched ? 0 : 1;
}

static void depot_print_conflicts(const VkrDepotChangeList *conflicts,
                                  VkrDepotConflictPolicy policy) {
  static const char *const sides[] = {"conflict", "took theirs", "kept mine"};
  for (uint32_t i = 0u; i < conflicts->count; ++i) {
    printf("%s %s\n", sides[policy], conflicts->entries[i].path);
  }
}

/* Prints a merge's outcome; 0 when it merged, fast-forwarded or had
   nothing to do. */
static int depot_print_merge(const VkrDepotMergeResult *merged,
                             VkrDepotConflictPolicy policy, const char *what) {
  depot_print_conflicts(&merged->conflicts, policy);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&merged->commit, hex);
  if (merged->status == VKR_DEPOT_MERGE_CONFLICTS) {
    fprintf(stderr,
            "%u conflicts with %s; nothing changed. Merge again with "
            "--take-theirs or --keep-mine\n",
            merged->conflicts.count, what);
    return 1;
  }
  if (merged->status == VKR_DEPOT_MERGE_UP_TO_DATE) {
    printf("already holds %s\n", what);
  } else if (merged->status == VKR_DEPOT_MERGE_FAST_FORWARD) {
    printf("fast-forwarded to %s at %.12s\n", what, hex);
  } else {
    printf("merged %s at %.12s: %u files taken, %u units merged\n", what, hex,
           merged->taken, merged->units_merged);
  }
  return 0;
}

static VkrDepotMergeConfig depot_merge_config(const char *bakery_override,
                                              char *bakery, uint32_t capacity,
                                              VkrDepotConflictPolicy policy) {
  const bool8_t found =
      vkr_depot_bakery_path(bakery_override, bakery, capacity);
  return (VkrDepotMergeConfig){.driver =
                                   found ? vkr_depot_merge_bakery_driver : NULL,
                               .driver_context = bakery,
                               .policy = policy};
}

static int depot_pull(const char *directory, const char *remote,
                      VkrDepotConflictPolicy policy,
                      const char *bakery_override) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  char bakery[VKR_DEPOT_PATH_CAPACITY] = {0};
  const VkrDepotMergeConfig config =
      depot_merge_config(bakery_override, bakery, sizeof(bakery), policy);
  VkrDepotPullResult pulled;
  VkrDepotMergeResult merged;
  char error[256];
  const bool8_t ok =
      vkr_depot_pull_merge(&depot_allocator, &workspace, remote, &config,
                           &pulled, &merged, NULL, error, sizeof(error));
  depot_print_conflicts(&pulled.conflicts, policy);
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&pulled.commit, hex);
  char what[VKR_DEPOT_REMOTE_NAME + VKR_DEPOT_BRANCH_NAME + 2u];
  snprintf(what, sizeof(what), "%s/%s", remote ? remote : VKR_DEPOT_ORIGIN,
           workspace.branch);
  int result = 1;
  if (!ok) {
    fprintf(stderr, "%s\n", error);
    if (pulled.conflicts.count) {
      fprintf(stderr, "pull again with --take-theirs or --keep-mine\n");
    }
  } else if (pulled.status == VKR_DEPOT_PULL_UP_TO_DATE) {
    printf("up to date\n");
    result = 0;
  } else if (pulled.status == VKR_DEPOT_PULL_FAST_FORWARD) {
    printf("pulled %s at %.12s; %u local changes kept\n", workspace.branch, hex,
           pulled.kept);
    result = 0;
  } else if (pulled.status == VKR_DEPOT_PULL_AHEAD) {
    printf("%s is ahead of %s; push it\n", workspace.branch, what);
    result = 0;
  } else {
    result = depot_print_merge(&merged, policy, what);
  }
  vkr_depot_change_list_free(&pulled.conflicts);
  vkr_depot_change_list_free(&merged.conflicts);
  vkr_depot_workspace_close(&workspace);
  return result;
}

/* Merges a local branch, or `<remote>/<branch>` as last fetched. */
static int depot_merge_command(const char *directory, const char *name,
                               VkrDepotConflictPolicy policy,
                               const char *bakery_override) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  VkrDepotId theirs;
  const char *slash = strchr(name, '/');
  char remote[VKR_DEPOT_REMOTE_NAME] = {0};
  bool8_t found = false_v;
  if (slash && (size_t)(slash - name) < sizeof(remote)) {
    MemCopy(remote, name, (size_t)(slash - name));
    found = vkr_depot_tracking_read(workspace.store.root, remote, slash + 1,
                                    &theirs);
  } else {
    found = vkr_depot_ref_read(workspace.store.root, name, &theirs);
  }
  int result = 1;
  if (!found) {
    fprintf(stderr, "no branch %s; fetch it first\n", name);
  } else {
    char bakery[VKR_DEPOT_PATH_CAPACITY] = {0};
    VkrDepotMergeConfig config =
        depot_merge_config(bakery_override, bakery, sizeof(bakery), policy);
    char message[VKR_DEPOT_REMOTE_NAME + VKR_DEPOT_BRANCH_NAME + 16u];
    snprintf(message, sizeof(message), "Merge %s", name);
    config.message = message;
    VkrDepotMergeResult merged;
    char error[256];
    if (vkr_depot_merge(&workspace, &theirs, &config, &merged, error,
                        sizeof(error))) {
      result = depot_print_merge(&merged, policy, name);
    } else {
      fprintf(stderr, "%s\n", error);
    }
    vkr_depot_change_list_free(&merged.conflicts);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static void depot_sync_print(void *context, const char *branch,
                             VkrDepotSyncAction action,
                             const VkrDepotId *commit, const char *detail) {
  static const char *const actions[] = {"same", "pulled", "pushed", "diverged",
                                        "failed"};
  (void)context;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(commit, hex);
  printf("%s %s %.12s%s%s\n", actions[action], branch, hex,
         detail[0] ? ": " : "", detail);
  fflush(stdout);
}

/* Syncs a served store with one of its remotes. */
static int depot_sync_command(const char *store_root, const char *remote) {
  char error[256];
  if (!vkr_depot_repository_sync(
          &depot_allocator, store_root, vkr_depot_zstd_codec(), remote,
          depot_sync_print, NULL, NULL, error, sizeof(error))) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  return 0;
}

/* Sets the interval of a remote about to be saved, and keeps the repository
   an earlier remote of that name at the same address and key learned. */
static void depot_remote_keep_repository(const char *store,
                                         VkrDepotRemoteEntry *remote,
                                         uint32_t interval_seconds) {
  remote->interval_seconds = interval_seconds;
  VkrDepotRemoteEntry earlier;
  if (vkr_depot_remote_find(store, remote->name, &earlier) &&
      strcmp(earlier.address, remote->address) == 0 &&
      MemCompare(earlier.key, remote->key, sizeof(remote->key)) == 0) {
    MemCopy(remote->repository, earlier.repository, sizeof(remote->repository));
  }
}

/* The remotes of the working copy at `directory`, or of `store_root` when
   given: a served store has no working directory. */
static int depot_remote_command(const char *directory, const char *store_root,
                                const char *action, const char *name,
                                const char *address, const char *key_hex,
                                uint32_t interval_seconds) {
  VkrDepotWorkspace workspace = {0};
  if (!store_root && depot_open(&workspace, directory)) {
    return 1;
  }
  const char *store = store_root ? store_root : workspace.store.root;
  int result = 1;
  if (!action || strcmp(action, "list") == 0) {
    VkrDepotRemoteEntry remotes[VKR_DEPOT_REMOTES_MAX];
    uint32_t count = 0u;
    if (vkr_depot_remotes_load(store, remotes, ArrayCount(remotes), &count)) {
      for (uint32_t i = 0u; i < count; ++i) {
        printf("%s %s ", remotes[i].name, remotes[i].address);
        depot_print_key(remotes[i].key);
        if (remotes[i].interval_seconds) {
          printf(" every %us", remotes[i].interval_seconds);
        }
        printf("\n");
      }
      result = 0;
    }
  } else if (strcmp(action, "add") == 0 && name && address && key_hex) {
    VkrDepotRemoteEntry remote = {0};
    snprintf(remote.name, sizeof(remote.name), "%s", name);
    snprintf(remote.address, sizeof(remote.address), "%s", address);
    VkrNetAddress parsed;
    if (!vkr_depot_remote_name_valid(name) ||
        !vkr_depot_address(address, &parsed) ||
        !depot_parse_key(key_hex, remote.key)) {
      fprintf(stderr, "remote add needs NAME (a-z, 0-9, - and _) ADDR KEY\n");
    } else {
      depot_remote_keep_repository(store, &remote, interval_seconds);
      if (!vkr_depot_remote_set(store, &remote)) {
        fprintf(stderr, "could not save remote %s\n", name);
      } else {
        printf("remote %s %s", name, address);
        if (interval_seconds) {
          printf(" every %us", interval_seconds);
        }
        printf("\n");
        result = 0;
      }
    }
  } else if (strcmp(action, "remove") == 0 && name) {
    if (vkr_depot_remote_remove(store, name)) {
      printf("removed remote %s\n", name);
      result = 0;
    } else {
      fprintf(stderr, "no remote %s\n", name);
    }
  } else {
    fprintf(stderr, "remote takes list, add NAME ADDR KEY or remove NAME\n");
  }
  if (!store_root) {
    vkr_depot_workspace_close(&workspace);
  }
  return result;
}

typedef struct DepotBranchPrint {
  const VkrDepotWorkspace *workspace;
  const char *remote;
} DepotBranchPrint;

/* A branch with its commit and, when the default remote tracks it, how far
   it is ahead and behind. */
static bool8_t depot_branch_print(void *context, const char *branch) {
  const DepotBranchPrint *print = context;
  const char *store = print->workspace->store.root;
  VkrDepotId commit;
  VkrDepotId tracked;
  if (!vkr_depot_ref_read(store, branch, &commit)) {
    return true_v;
  }
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&commit, hex);
  printf("%c %s %.12s", strcmp(branch, print->workspace->branch) ? ' ' : '*',
         branch, hex);
  uint32_t ahead = 0u;
  uint32_t behind = 0u;
  if (print->remote &&
      vkr_depot_tracking_read(store, print->remote, branch, &tracked) &&
      vkr_depot_ahead_behind(&print->workspace->store, &commit, &tracked,
                             &ahead, &behind)) {
    printf(" [%s/%s: ahead %u, behind %u]", print->remote, branch, ahead,
           behind);
  }
  printf("\n");
  return true_v;
}

static int depot_branch_command(const char *directory, const char *name,
                                bool8_t remove, bool8_t force) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  char error[256];
  int result = 1;
  if (!name) {
    VkrDepotRemoteEntry origin;
    const bool8_t tracked =
        vkr_depot_remote_find(workspace.store.root, NULL, &origin);
    DepotBranchPrint print = {.workspace = &workspace,
                              .remote = tracked ? origin.name : NULL};
    result =
        vkr_depot_ref_list(workspace.store.root, depot_branch_print, &print)
            ? 0
            : 1;
  } else if (remove) {
    if (vkr_depot_branch_delete(&workspace, name, force, error,
                                sizeof(error))) {
      printf("deleted branch %s\n", name);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
    }
  } else if (vkr_depot_branch_create(&workspace, name, error, sizeof(error))) {
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&workspace.base, hex);
    printf("branch %s at %.12s\n", name, hex);
    result = 0;
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_switch(const char *directory, const char *name,
                        VkrDepotConflictPolicy policy) {
  VkrDepotWorkspace workspace;
  if (depot_open(&workspace, directory)) {
    return 1;
  }
  uint32_t kept = 0u;
  VkrDepotChangeList conflicts;
  char error[256];
  const bool8_t ok = vkr_depot_switch(&workspace, name, policy, &kept,
                                      &conflicts, error, sizeof(error));
  static const char *const sides[] = {"conflict", "took theirs", "kept mine"};
  for (uint32_t i = 0u; i < conflicts.count; ++i) {
    printf("%s %s\n", sides[policy], conflicts.entries[i].path);
  }
  if (ok) {
    printf("switched to %s; %u local changes kept\n", name, kept);
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_change_list_free(&conflicts);
  vkr_depot_workspace_close(&workspace);
  return ok ? 0 : 1;
}

static int depot_refs(const char *directory) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  const VkrDepotConnectConfig config = {.address = workspace.remote,
                                        .server_key = workspace.server_key};
  VkrDepotRef refs[VKR_DEPOT_REFS_MAX];
  uint32_t count = 0u;
  char error[256];
  int result = 1;
  if (vkr_depot_branches(&depot_allocator, &config, refs, VKR_DEPOT_REFS_MAX,
                         &count, NULL, error, sizeof(error))) {
    for (uint32_t i = 0u; i < count; ++i) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&refs[i].commit, hex);
      printf("%s %s\n", hex, refs[i].name);
    }
    result = 0;
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static bool8_t depot_print_commit(void *context,
                                  const VkrDepotLogEntry *entry) {
  (void)context;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&entry->id, hex);
  printf("%.12s %llu ", hex, (unsigned long long)(entry->time_us / 1000000u));
  for (uint32_t k = 0u; k < 6u; ++k) {
    printf("%02x", entry->author[k]);
  }
  printf(" %.*s\n", (int)entry->message_size, entry->message);
  return true_v;
}

static int depot_log(const char *directory, uint32_t count) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  (void)vkr_depot_log(&workspace.store, &workspace.base, count,
                      depot_print_commit, NULL);
  vkr_depot_workspace_close(&workspace);
  return 0;
}

static bool8_t depot_print_lock(void *context, const char *path,
                                const uint8_t owner[32], uint64_t time_us) {
  (void)context;
  printf("%s ", path);
  for (uint32_t k = 0u; k < 6u; ++k) {
    printf("%02x", owner[k]);
  }
  printf(" %llu\n", (unsigned long long)(time_us / 1000000u));
  return true_v;
}

static int depot_locks_command(const char *directory, const char *path,
                               bool8_t lock, bool8_t release, bool8_t force) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotConnection *connection = depot_connect(&workspace);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  if (client) {
    if (lock) {
      uint8_t owner[32];
      const VkrDepotLockStatus status =
          vkr_depot_client_lock(client, path, release, force, owner);
      static const char *const names[] = {"ok", "held by another key",
                                          "not held", "denied"};
      printf("%s %s: %s\n", release ? "unlock" : "lock", path,
             status < ArrayCount(names) ? names[status] : "?");
      result = status == VKR_DEPOT_LOCK_OK ? 0 : 1;
    } else {
      result = vkr_depot_client_locks(client, depot_print_lock, NULL) ? 0 : 1;
    }
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

static void depot_usage(void) {
  fprintf(
      stderr,
      "usage:\n"
      "  vkr_depot keygen\n"
      "  vkr_depot init STORE\n"
      "  vkr_depot serve STORE [--bind ADDR] [--open] [--name NAME]\n"
      "                  [--discovery-port PORT]\n"
      "  vkr_depot discover [--to ADDR] [--timeout MS] [--discovery-port "
      "PORT]\n"
      "  vkr_depot mirror STORE PEER-ADDR PEER-KEY [--local ADDR]\n"
      "                   [--branch NAME] [--watch SECONDS]\n"
      "  vkr_depot access STORE KEY none|read|write|admin\n"
      "  vkr_depot clone ADDR SERVER-KEY DIR [--branch NAME] [--store DIR]\n"
      "                  [--repository ID]\n"
      "                  [--sparse]\n"
      "  vkr_depot status|refs|locks [DIR]\n"
      "  vkr_depot commit [DIR] -m MESSAGE [--push] [--remote NAME]\n"
      "  vkr_depot push|fetch [DIR] [--remote NAME] [--branch NAME]\n"
      "  vkr_depot pull [DIR] [--remote NAME] [--take-theirs|--keep-mine]\n"
      "                 [--bakery PATH]\n"
      "  vkr_depot merge BRANCH|REMOTE/BRANCH [--dir DIR] [--take-theirs|\n"
      "                  --keep-mine] [--bakery PATH]\n"
      "  vkr_depot remote [list|add NAME ADDR KEY [--interval SECONDS]|remove\n"
      "                   NAME] [--dir DIR|--store STORE]\n"
      "  vkr_depot sync STORE [--remote NAME]\n"
      "  vkr_depot branch [NAME [--delete [--force]]] [--dir DIR]\n"
      "  vkr_depot switch NAME [--dir DIR] [--take-theirs|--keep-mine]\n"
      "  vkr_depot cooked [DIR]\n"
      "  vkr_depot stream ADDR SERVER-KEY DIR PATH [--branch NAME]\n"
      "  vkr_depot log [DIR] [--count N]\n"
      "  vkr_depot lock|unlock PATH [--dir DIR] [--force]\n"
      "  vkr_depot service add|remove|list|install|uninstall|status|run "
      "...\n");
}

static int depot_main(int argc, char **argv) {
  if (argc < 2) {
    depot_usage();
    return 1;
  }
  const char *command = argv[1];
  /* The machine's service takes its own arguments. */
  if (strcmp(command, "service") == 0) {
    return vkr_depot_service_command(argc - 2, argv + 2);
  }
  const char *positional[4] = {NULL, NULL, NULL, NULL};
  uint32_t positional_count = 0u;
  const char *bind = NULL;
  const char *branch = "main";
  bool8_t branch_given = false_v;
  const char *store = NULL;
  const char *name = NULL;
  const char *to = NULL;
  const char *local = NULL;
  uint32_t timeout_ms = 1500u;
  uint32_t discovery_port = VKR_DEPOT_DISCOVERY_PORT;
  uint32_t watch_seconds = 0u;
  uint32_t interval_seconds = 0u;
  const char *repository = NULL;
  const char *message = NULL;
  const char *dir = NULL;
  uint32_t count = 20u;
  bool8_t open_access = false_v;
  bool8_t force = false_v;
  bool8_t sparse = false_v;
  bool8_t push = false_v;
  bool8_t remove = false_v;
  const char *remote = NULL;
  const char *bakery = NULL;
  VkrDepotConflictPolicy policy = VKR_DEPOT_CONFLICT_FAIL;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--open") == 0) {
      open_access = true_v;
    } else if (strcmp(arg, "--sparse") == 0) {
      sparse = true_v;
    } else if (strcmp(arg, "--force") == 0) {
      force = true_v;
    } else if (strcmp(arg, "--push") == 0) {
      push = true_v;
    } else if (strcmp(arg, "--delete") == 0) {
      remove = true_v;
    } else if (value && strcmp(arg, "--remote") == 0) {
      remote = value;
      ++i;
    } else if (value && strcmp(arg, "--bakery") == 0) {
      bakery = value;
      ++i;
    } else if (strcmp(arg, "--take-theirs") == 0) {
      policy = VKR_DEPOT_CONFLICT_TAKE_THEIRS;
    } else if (strcmp(arg, "--keep-mine") == 0) {
      policy = VKR_DEPOT_CONFLICT_KEEP_MINE;
    } else if (value && strcmp(arg, "--bind") == 0) {
      bind = value;
      ++i;
    } else if (value && strcmp(arg, "--branch") == 0) {
      branch = value;
      branch_given = true_v;
      ++i;
    } else if (value && strcmp(arg, "--name") == 0) {
      name = value;
      ++i;
    } else if (value && strcmp(arg, "--to") == 0) {
      to = value;
      ++i;
    } else if (value && strcmp(arg, "--local") == 0) {
      local = value;
      ++i;
    } else if (value && strcmp(arg, "--discovery-port") == 0) {
      discovery_port = (uint32_t)strtoul(value, NULL, 10);
      if (discovery_port == 0u || discovery_port > 65535u) {
        depot_usage();
        return 1;
      }
      ++i;
    } else if (value && strcmp(arg, "--timeout") == 0) {
      timeout_ms = (uint32_t)strtoul(value, NULL, 10);
      ++i;
    } else if (value && strcmp(arg, "--watch") == 0) {
      watch_seconds = (uint32_t)strtoul(value, NULL, 10);
      ++i;
    } else if (value && strcmp(arg, "--repository") == 0) {
      repository = value;
      ++i;
    } else if (value && strcmp(arg, "--interval") == 0) {
      interval_seconds = (uint32_t)strtoul(value, NULL, 10);
      ++i;
    } else if (value && strcmp(arg, "--store") == 0) {
      store = value;
      ++i;
    } else if (value && strcmp(arg, "-m") == 0) {
      message = value;
      ++i;
    } else if (value && strcmp(arg, "--dir") == 0) {
      dir = value;
      ++i;
    } else if (value && strcmp(arg, "--count") == 0) {
      count = (uint32_t)strtoul(value, NULL, 10);
      ++i;
    } else if (arg[0] == '-' || positional_count == ArrayCount(positional)) {
      depot_usage();
      return 1;
    } else {
      positional[positional_count++] = arg;
    }
  }
  const char *first = positional[0] ? positional[0] : ".";
  if (strcmp(command, "keygen") == 0) {
    return depot_keygen();
  }
  if (strcmp(command, "init") == 0 && positional[0]) {
    return depot_init(positional[0]);
  }
  if (strcmp(command, "serve") == 0 && positional[0]) {
    return depot_serve(positional[0], bind, open_access, name,
                       (uint16_t)discovery_port);
  }
  if (strcmp(command, "discover") == 0) {
    return depot_discover(to, timeout_ms, (uint16_t)discovery_port);
  }
  if (strcmp(command, "mirror") == 0 && positional_count == 3u) {
    return depot_mirror_command(positional[0], positional[1], positional[2],
                                local ? local : "127.0.0.1",
                                branch_given ? branch : NULL, watch_seconds);
  }
  if (strcmp(command, "access") == 0 && positional_count == 3u) {
    return depot_access(positional[0], positional[1], positional[2]);
  }
  if (strcmp(command, "clone") == 0 && positional_count == 3u) {
    return depot_clone(positional[0], positional[1], positional[2], branch,
                       store, repository, sparse);
  }
  if (strcmp(command, "status") == 0) {
    return depot_status(first);
  }
  if (strcmp(command, "commit") == 0 && message) {
    return depot_commit(first, message, push, remote);
  }
  if (strcmp(command, "push") == 0) {
    return depot_push_command(first, remote, branch_given ? branch : NULL);
  }
  if (strcmp(command, "fetch") == 0) {
    return depot_fetch_command(first, remote, branch_given ? branch : NULL);
  }
  if (strcmp(command, "pull") == 0) {
    return depot_pull(first, remote, policy, bakery);
  }
  if (strcmp(command, "merge") == 0 && positional[0]) {
    return depot_merge_command(dir ? dir : ".", positional[0], policy, bakery);
  }
  if (strcmp(command, "sync") == 0 && positional[0]) {
    return depot_sync_command(positional[0], remote);
  }
  if (strcmp(command, "remote") == 0) {
    return depot_remote_command(dir ? dir : ".", store, positional[0],
                                positional[1], positional[2], positional[3],
                                interval_seconds);
  }
  if (strcmp(command, "branch") == 0) {
    return depot_branch_command(dir ? dir : ".", positional[0], remove, force);
  }
  if (strcmp(command, "switch") == 0 && positional[0]) {
    return depot_switch(dir ? dir : ".", positional[0], policy);
  }
  if (strcmp(command, "cooked") == 0) {
    return depot_cooked(first);
  }
  if (strcmp(command, "stream") == 0 && positional_count == 4u) {
    return depot_stream(positional[0], positional[1], positional[2],
                        positional[3], branch);
  }
  if (strcmp(command, "refs") == 0) {
    return depot_refs(first);
  }
  if (strcmp(command, "log") == 0) {
    return depot_log(first, count);
  }
  if (strcmp(command, "locks") == 0) {
    return depot_locks_command(first, NULL, false_v, false_v, false_v);
  }
  if ((strcmp(command, "lock") == 0 || strcmp(command, "unlock") == 0) &&
      positional[0]) {
    return depot_locks_command(dir ? dir : ".", positional[0], true_v,
                               strcmp(command, "unlock") == 0, force);
  }
  depot_usage();
  return 1;
}

VKR_MAIN(argc, argv) {
  vkr_platform_init();
  if (!vkr_net_crypto_init()) {
    fprintf(stderr, "libsodium or AES-GCM hardware unavailable\n");
    return 1;
  }
  if (!vkr_dmemory_create(MB(16), GB(4), &depot_memory)) {
    fprintf(stderr, "memory reserve failed\n");
    return 1;
  }
  depot_allocator = (VkrAllocator){.ctx = &depot_memory};
  vkr_dmemory_allocator_create(&depot_allocator);
  const int result = depot_main(argc, argv);
  vkr_dmemory_allocator_destroy(&depot_allocator);
  vkr_platform_shutdown();
  return result;
}

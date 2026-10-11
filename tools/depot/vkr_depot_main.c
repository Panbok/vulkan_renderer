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
 *   vkr_depot status [DIR]
 *   vkr_depot commit [DIR] -m MESSAGE
 *   vkr_depot pull [DIR] [--take-theirs|--keep-mine]
 *   vkr_depot refs [DIR]
 *   vkr_depot log [DIR] [--count N]
 *   vkr_depot lock PATH [--dir DIR]
 *   vkr_depot unlock PATH [--dir DIR] [--force]
 *   vkr_depot locks [DIR]
 *
 * A working copy pushes with `commit`: it snapshots the directory, uploads
 * the objects the server lacks and moves the branch from the commit it
 * started from. A branch that moved meanwhile refuses the push; `pull`
 * first. `serve` also answers LAN discovery (vkr_depot_discovery.h), and
 * `mirror` keeps the branches of the local server and a peer server equal,
 * so two machines each hold a full store. */

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_client.h"
#include "vkr_depot_discovery.h"
#include "vkr_depot_server.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_workspace.h"
#include "vkr_depot_zstd.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DepotLink {
  VkrNetHost *host;
  VkrNetSession *session;
  VkrDepotServer *server;
  VkrNetConnectionId connection;
  bool8_t connected;
  bool8_t closed;
  uint16_t close_code;
} DepotLink;

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

// =============================================================================
// Link over a UDP host
// =============================================================================

static bool8_t depot_link_pump(void *context, int32_t timeout_ms) {
  DepotLink *link = context;
  if (!vkr_net_host_pump(link->host, timeout_ms)) {
    return false_v;
  }
  VkrNetCore *core = vkr_net_host_core(link->host);
  const uint64_t now = vkr_net_host_now(link->host);
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    if (vkr_net_session_dispatch(link->session, &event, now)) {
      continue;
    }
    if (link->server) {
      vkr_depot_server_on_event(link->server, &event);
    }
    if (event.type == VKR_NET_EVENT_CONNECTED &&
        event.connection == link->connection) {
      link->connected = true_v;
    } else if (event.type == VKR_NET_EVENT_CLOSED &&
               event.connection == link->connection) {
      link->closed = true_v;
      link->close_code = event.code;
    }
  }
  if (link->server) {
    vkr_depot_server_update(link->server, now);
  } else {
    vkr_net_core_flush(core);
  }
  return true_v;
}

static uint64_t depot_link_now(void *context) {
  return vkr_net_host_now(((DepotLink *)context)->host);
}

static void depot_link_close(DepotLink *link) {
  if (link->host && link->connected && !link->closed) {
    vkr_net_core_close(vkr_net_host_core(link->host), link->connection,
                       VKR_NET_CLOSE_APPLICATION, depot_link_now(link));
    for (uint32_t i = 0u; i < 20u; ++i) {
      (void)depot_link_pump(link, 10);
    }
  }
  vkr_net_session_destroy(link->session);
  vkr_net_host_destroy(link->host);
  MemZero(link, sizeof(*link));
}

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

typedef struct ServeContext {
  VkrDepotServer *server;
} ServeContext;

static void depot_serve_accept(void *context, const VkrNetAddress *address,
                               const uint8_t peer_key[32],
                               const uint8_t *credential,
                               uint32_t credential_size,
                               VkrNetAcceptResult *out_result) {
  ServeContext *serve = context;
  if (!serve->server) {
    out_result->code = VKR_NET_CLOSE_SERVER_FULL;
    return;
  }
  vkr_depot_server_accept(serve->server, address, peer_key, credential,
                          credential_size, out_result);
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(address, text, sizeof(text));
  printf("%s %s key ", out_result->code ? "refused" : "accepted", text);
  depot_print_key(peer_key);
  printf("\n");
  fflush(stdout);
}

static int depot_serve(const char *store_root, const char *bind,
                       bool8_t open_access, const char *name,
                       uint16_t discovery_port) {
  static ServeContext serve;
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 16u << 20, .send_buffer = 16u << 20}};
  if (!vkr_depot_server_key(store_root, &config.core.static_keys) ||
      !vkr_depot_address(bind ? bind : "[::]", &config.bind)) {
    fprintf(stderr, "invalid store or --bind\n");
    return 1;
  }
  config.core.accept_incoming = true_v;
  config.core.accept = depot_serve_accept;
  config.core.accept_context = &serve;
  config.core.max_connections = 256u;
  config.core.sent_packet_capacity = 16384u;
  DepotLink link = {0};
  link.host = vkr_net_host_create(&depot_allocator, &config);
  link.session = link.host ? vkr_net_session_create(
                                 &depot_allocator, vkr_net_host_core(link.host))
                           : NULL;
  const VkrDepotServerConfig server_config = {.store_root = store_root,
                                              .codec = vkr_depot_zstd_codec(),
                                              .open_access = open_access};
  link.server = link.session
                    ? vkr_depot_server_create(&depot_allocator,
                                              vkr_net_host_core(link.host),
                                              link.session, &server_config)
                    : NULL;
  if (!link.server) {
    fprintf(stderr, "could not start the server\n");
    depot_link_close(&link);
    return 1;
  }
  serve.server = link.server;
  VkrNetAddress local;
  (void)vkr_net_host_local_address(link.host, &local);
  char text[VKR_NET_ADDRESS_TEXT];
  vkr_net_address_format(&local, text, sizeof(text));
  printf("serving %s on %s key ", store_root, text);
  depot_print_key(config.core.static_keys.public_key);
  printf("%s\n", open_access ? " (open access)" : "");
  /* Other machines find this server by its name, the store's folder
     unless --name gives one. */
  static VkrDepotResponder responder;
  if (vkr_depot_responder_open(&responder, discovery_port, local.port,
                               config.core.static_keys.public_key,
                               name ? name : vkr_bakery_path_name(store_root),
                               NULL)) {
    printf("discoverable as %s on UDP %u\n", responder.name, discovery_port);
  } else {
    printf("not discoverable: UDP %u is taken\n", discovery_port);
  }
  fflush(stdout);
  for (;;) {
    if (!depot_link_pump(&link, 50)) {
      fprintf(stderr, "the socket failed\n");
      break;
    }
    vkr_depot_responder_update(&responder);
  }
  vkr_depot_responder_close(&responder);
  vkr_depot_server_destroy(link.server);
  depot_link_close(&link);
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
                       const char *store_root) {
  uint8_t server_key[32];
  if (!depot_parse_key(key_hex, server_key) ||
      !vkr_depot_branch_valid(branch)) {
    fprintf(stderr, "clone needs ADDR SERVER-KEY DIR [--branch NAME]\n");
    return 1;
  }
  const VkrDepotCloneConfig config = {.address = address,
                                      .server_key = server_key,
                                      .directory = directory,
                                      .branch = branch,
                                      .store_root = store_root,
                                      .codec = vkr_depot_zstd_codec()};
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
  printf("cloned %s at %.12s: %u files\n", branch, hex, result.files);
  return 0;
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
    printf("%u changes on %s at %.12s\n", changes.count, workspace.branch, hex);
    vkr_depot_change_list_free(&changes);
    result = 0;
  } else {
    fprintf(stderr, "%s\n", error);
  }
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_commit(const char *directory, const char *message) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  /* The snapshot comes first: hashing large files outlasts the idle
     timeout of a connection opened before it. */
  const VkrDepotId base = workspace.base;
  VkrDepotPendingCommit pending;
  char error[256];
  if (!vkr_depot_commit_snapshot(&workspace, &base, message, &pending, error,
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
    if (vkr_depot_commit_push(&workspace, client, branch, &base, &base,
                              &pending, error, sizeof(error))) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&pending.commit, hex);
      printf("pushed %s at %.12s\n", branch, hex);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
    }
  }
  vkr_depot_pending_commit_free(&pending);
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_pull(const char *directory, VkrDepotConflictPolicy policy) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotConnection *connection = depot_connect(&workspace);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  VkrDepotId commit;
  bool8_t exists = false_v;
  if (client &&
      (!vkr_depot_find_ref(client, workspace.branch, &commit, &exists) ||
       !exists)) {
    fprintf(stderr, "branch %s not found\n", workspace.branch);
  } else if (client && vkr_depot_id_equal(&commit, &workspace.base)) {
    printf("up to date\n");
    result = 0;
  } else if (client) {
    uint32_t kept = 0u;
    VkrDepotChangeList conflicts;
    char error[256];
    const bool8_t ok =
        vkr_depot_fetch_checkout(&workspace, client, &commit, policy, &kept,
                                 &conflicts, error, sizeof(error));
    static const char *const sides[] = {"conflict", "took theirs", "kept mine"};
    for (uint32_t i = 0u; i < conflicts.count; ++i) {
      printf("%s %s\n", sides[policy], conflicts.entries[i].path);
    }
    if (ok) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&commit, hex);
      printf("pulled %s at %.12s; %u local changes kept\n", workspace.branch,
             hex, kept);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
      if (conflicts.count) {
        fprintf(stderr, "pull again with --take-theirs or --keep-mine\n");
      }
    }
    vkr_depot_change_list_free(&conflicts);
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
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
      "  vkr_depot status|refs|locks [DIR]\n"
      "  vkr_depot pull [DIR] [--take-theirs|--keep-mine]\n"
      "  vkr_depot commit [DIR] -m MESSAGE\n"
      "  vkr_depot log [DIR] [--count N]\n"
      "  vkr_depot lock|unlock PATH [--dir DIR] [--force]\n");
}

static int depot_main(int argc, char **argv) {
  if (argc < 2) {
    depot_usage();
    return 1;
  }
  const char *command = argv[1];
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
  const char *message = NULL;
  const char *dir = NULL;
  uint32_t count = 20u;
  bool8_t open_access = false_v;
  bool8_t force = false_v;
  VkrDepotConflictPolicy policy = VKR_DEPOT_CONFLICT_FAIL;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--open") == 0) {
      open_access = true_v;
    } else if (strcmp(arg, "--force") == 0) {
      force = true_v;
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
                       store);
  }
  if (strcmp(command, "status") == 0) {
    return depot_status(first);
  }
  if (strcmp(command, "commit") == 0 && message) {
    return depot_commit(first, message);
  }
  if (strcmp(command, "pull") == 0) {
    return depot_pull(first, policy);
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

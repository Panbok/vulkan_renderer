/* vkr_depot: a versioned, content-addressed asset depot over the VKR network
 * protocol (docs/proposals/network-protocol.md, "Depot").
 *
 *   vkr_depot keygen [--identity FILE]
 *   vkr_depot init STORE
 *   vkr_depot serve STORE [--bind ADDR] [--open]
 *   vkr_depot access STORE KEY none|read|write|admin
 *   vkr_depot clone ADDR SERVER-KEY DIR [--branch NAME] [--store DIR]
 *   vkr_depot status [DIR]
 *   vkr_depot commit [DIR] -m MESSAGE
 *   vkr_depot pull [DIR]
 *   vkr_depot refs [DIR]
 *   vkr_depot log [DIR] [--count N]
 *   vkr_depot lock PATH [--dir DIR]
 *   vkr_depot unlock PATH [--dir DIR] [--force]
 *   vkr_depot locks [DIR]
 *
 * A working copy pushes with `commit`: it snapshots the directory, uploads
 * the objects the server lacks and moves the branch from the commit it
 * started from. A branch that moved meanwhile refuses the push; `pull`
 * first. */

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_entry.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_client.h"
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
static VkrDepotConnection *depot_connect(const char *address,
                                         const uint8_t server_key[32],
                                         const VkrDepotStore *store) {
  char error[256];
  VkrDepotConnection *connection = vkr_depot_connect(
      &depot_allocator, address, server_key, store, error, sizeof(error));
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
                       bool8_t open_access) {
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
  fflush(stdout);
  for (;;) {
    if (!depot_link_pump(&link, 50)) {
      fprintf(stderr, "the socket failed\n");
      break;
    }
  }
  vkr_depot_server_destroy(link.server);
  depot_link_close(&link);
  return 1;
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
  VkrDepotWorkspace workspace;
  uint8_t server_key[32];
  if (!depot_parse_key(key_hex, server_key) ||
      !vkr_depot_branch_valid(branch)) {
    fprintf(stderr, "clone needs ADDR SERVER-KEY DIR [--branch NAME]\n");
    return 1;
  }
  if (!vkr_depot_workspace_create(&workspace, directory, store_root,
                                  vkr_depot_zstd_codec())) {
    fprintf(stderr, "could not create the working copy at %s\n", directory);
    return 1;
  }
  snprintf(workspace.remote, sizeof(workspace.remote), "%s", address);
  memcpy(workspace.server_key, server_key, 32u);
  snprintf(workspace.branch, sizeof(workspace.branch), "%s", branch);
  VkrDepotConnection *connection =
      depot_connect(address, server_key, &workspace.store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  if (client) {
    VkrDepotId commit;
    bool8_t exists = false_v;
    uint32_t kept = 0u;
    char error[256];
    if (!vkr_depot_find_ref(client, branch, &commit, &exists)) {
      fprintf(stderr, "refs failed: %s\n", vkr_depot_client_error(client));
    } else if (!exists) {
      printf("branch %s is empty; the first commit creates it\n", branch);
      result = vkr_depot_workspace_save(&workspace) ? 0 : 1;
    } else if (!vkr_depot_fetch_checkout(&workspace, client, &commit, &kept,
                                         error, sizeof(error))) {
      fprintf(stderr, "%s\n", error);
    } else {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&commit, hex);
      printf("cloned %s at %.12s: %u files\n", branch, hex,
             workspace.index.count);
      result = 0;
    }
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

typedef struct ChangePrinter {
  uint32_t count;
} ChangePrinter;

static bool8_t depot_print_change(void *context, const char *path,
                                  VkrDepotChange change) {
  ChangePrinter *printer = context;
  static const char codes[] = {'?', 'A', 'M', 'D'};
  printf("%c %s\n", codes[change], path);
  printer->count += 1u;
  return true_v;
}

static int depot_status(const char *directory) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotId tree;
  VkrDepotFileList files;
  int result = 1;
  if (vkr_depot_workspace_snapshot(&workspace, &tree, &files)) {
    ChangePrinter printer = {0};
    (void)vkr_depot_file_list_diff(&workspace.index, &files, depot_print_change,
                                   &printer);
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&workspace.base, hex);
    printf("%u changes on %s at %.12s\n", printer.count, workspace.branch, hex);
    vkr_depot_file_list_free(&files);
    result = 0;
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
  VkrDepotConnection *connection =
      depot_connect(workspace.remote, workspace.server_key, &workspace.store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  if (client) {
    const VkrDepotId base = workspace.base;
    char branch[VKR_DEPOT_BRANCH_NAME];
    snprintf(branch, sizeof(branch), "%s", workspace.branch);
    VkrDepotId commit;
    char error[256];
    if (vkr_depot_commit_push(&workspace, client, branch, &base, &base, message,
                              &commit, error, sizeof(error))) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&commit, hex);
      printf("pushed %s at %.12s\n", branch, hex);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
    }
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_pull(const char *directory) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotConnection *connection =
      depot_connect(workspace.remote, workspace.server_key, &workspace.store);
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
    char error[256];
    if (vkr_depot_fetch_checkout(&workspace, client, &commit, &kept, error,
                                 sizeof(error))) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&commit, hex);
      printf("pulled %s at %.12s; %u local changes kept\n", workspace.branch,
             hex, kept);
      result = 0;
    } else {
      fprintf(stderr, "%s\n", error);
    }
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
  VkrDepotConnection *connection =
      depot_connect(workspace.remote, workspace.server_key, &workspace.store);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  int result = 1;
  if (client) {
    VkrDepotRef refs[VKR_DEPOT_REFS_MAX];
    uint32_t count = 0u;
    if (vkr_depot_client_refs(client, refs, VKR_DEPOT_REFS_MAX, &count)) {
      for (uint32_t i = 0u; i < count; ++i) {
        char hex[VKR_DEPOT_ID_HEX];
        vkr_depot_id_hex(&refs[i].commit, hex);
        printf("%s %s\n", hex, refs[i].name);
      }
      result = 0;
    }
  }
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&workspace);
  return result;
}

static int depot_log(const char *directory, uint32_t count) {
  VkrDepotWorkspace workspace;
  if (!vkr_depot_workspace_open(&workspace, directory,
                                vkr_depot_zstd_codec())) {
    fprintf(stderr, "no working copy at %s\n", directory);
    return 1;
  }
  VkrDepotId commit = workspace.base;
  for (uint32_t i = 0u; i < count && !vkr_depot_id_zero(&commit); ++i) {
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    VkrDepotCommit parsed;
    if (!vkr_depot_store_get(&workspace.store, &commit, &bytes, &size) ||
        !vkr_depot_commit_parse(bytes, size, &parsed)) {
      free(bytes);
      break;
    }
    char hex[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&commit, hex);
    printf("%.12s %llu ", hex, (unsigned long long)(parsed.time_us / 1000000u));
    for (uint32_t k = 0u; k < 6u; ++k) {
      printf("%02x", parsed.author[k]);
    }
    printf(" %.*s\n", (int)parsed.message_size, parsed.message);
    commit = parsed.parent_count ? parsed.parents[0] : (VkrDepotId){0};
    free(bytes);
  }
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
  VkrDepotConnection *connection =
      depot_connect(workspace.remote, workspace.server_key, &workspace.store);
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
      "  vkr_depot serve STORE [--bind ADDR] [--open]\n"
      "  vkr_depot access STORE KEY none|read|write|admin\n"
      "  vkr_depot clone ADDR SERVER-KEY DIR [--branch NAME] [--store DIR]\n"
      "  vkr_depot status|pull|refs|locks [DIR]\n"
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
  const char *store = NULL;
  const char *message = NULL;
  const char *dir = NULL;
  uint32_t count = 20u;
  bool8_t open_access = false_v;
  bool8_t force = false_v;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--open") == 0) {
      open_access = true_v;
    } else if (strcmp(arg, "--force") == 0) {
      force = true_v;
    } else if (value && strcmp(arg, "--bind") == 0) {
      bind = value;
      ++i;
    } else if (value && strcmp(arg, "--branch") == 0) {
      branch = value;
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
    return depot_serve(positional[0], bind, open_access);
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
    return depot_pull(first);
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

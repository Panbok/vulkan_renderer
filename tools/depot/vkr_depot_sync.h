#pragma once

#include "vkr_depot_client.h"
#include "vkr_depot_workspace.h"

/* A working copy's exchanges with its depot server, for the vkr_depot
 * command and the editor's collaborative session (ADR-105, ADR-106): the
 * user's identity key, a client connection over its own UDP host, and the
 * commit, push, fetch and checkout steps. Every call blocks on the network;
 * one thread uses a connection at a time. */

#define VKR_DEPOT_DEFAULT_PORT 7320u
#define VKR_DEPOT_TIMEOUT_MS 30000u

/* The user's identity key: `$VKR_DEPOT_IDENTITY`, else
   `<user cache>/vkr/depot/identity.key`, made when missing and `create`. */
bool8_t vkr_depot_identity(VkrNetKeyPair *out, bool8_t create);

/* A numeric address or a host name, with an optional port (default
   VKR_DEPOT_DEFAULT_PORT). */
bool8_t vkr_depot_address(const char *text, VkrNetAddress *out);

typedef struct VkrDepotConnection VkrDepotConnection;

/* Connects to the server at `address` whose key is `server_key` with the
   user's identity and opens a client on `store`. NULL with a message. */
VkrDepotConnection *vkr_depot_connect(VkrAllocator *allocator,
                                      const char *address,
                                      const uint8_t server_key[32],
                                      const VkrDepotStore *store, char *error,
                                      uint32_t capacity);
VkrDepotClient *vkr_depot_connection_client(VkrDepotConnection *connection);
void vkr_depot_disconnect(VkrDepotConnection *connection);

/* Where `branch` points on the server; `*out_exists` false for a branch with
   no commit yet. */
bool8_t vkr_depot_find_ref(VkrDepotClient *client, const char *branch,
                           VkrDepotId *out_commit, bool8_t *out_exists);

/* Commits the working directory with `parent` (none when zero) as its
   parent, uploads what the server lacks and moves `branch` from `expected`
   (zero: the branch must not exist) to the commit. On success the commit is
   the working copy's base and `branch` its branch, saved. A branch that
   moved meanwhile fails with a message naming where it is. */
bool8_t vkr_depot_commit_push(VkrDepotWorkspace *workspace,
                              VkrDepotClient *client, const char *branch,
                              const VkrDepotId *parent,
                              const VkrDepotId *expected, const char *message,
                              VkrDepotId *out_commit, char *error,
                              uint32_t capacity);

/* Fetches `commit` and checks it out over the working directory. Files
   changed here since the base stay as they are unless the commit changed
   them too, which fails without changing anything. On success the commit is
   the working copy's base, saved; `out_kept` counts the local changes kept. */
bool8_t vkr_depot_fetch_checkout(VkrDepotWorkspace *workspace,
                                 VkrDepotClient *client,
                                 const VkrDepotId *commit, uint32_t *out_kept,
                                 char *error, uint32_t capacity);

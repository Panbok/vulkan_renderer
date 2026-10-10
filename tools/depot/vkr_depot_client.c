#include "vkr_depot_client.h"

#include "core/vkr_byte_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_REPLY_SLOTS 64u
/* GET and PUT requests in flight at once. */
#define CLIENT_GETS_MAX 16u
#define CLIENT_PUTS_MAX 8u
#define CLIENT_HAVES_MAX 16u
#define CLIENT_PUMP_MS 5

// =============================================================================
// ID sets
// =============================================================================

typedef struct IdSet {
  VkrDepotId *slots;
  uint8_t *used;
  uint32_t capacity;
  uint32_t count;
} IdSet;

static uint64_t idset_hash(const VkrDepotId *id) {
  /* IDs are SHA-256 output: their first bytes are already uniform. */
  return vkr_load_le_u64(id->bytes);
}

static void idset_free(IdSet *set) {
  free(set->slots);
  free(set->used);
  MemZero(set, sizeof(*set));
}

/* 1 when added, 0 when present, -1 on allocation failure. */
static int32_t idset_add(IdSet *set, const VkrDepotId *id) {
  if ((set->count + 1u) * 2u > set->capacity) {
    const uint32_t capacity = set->capacity ? set->capacity * 2u : 1024u;
    VkrDepotId *slots = malloc((size_t)capacity * sizeof(VkrDepotId));
    uint8_t *used = calloc(capacity, 1u);
    if (!slots || !used) {
      free(slots);
      free(used);
      return -1;
    }
    for (uint32_t i = 0u; i < set->capacity; ++i) {
      if (!set->used[i]) {
        continue;
      }
      uint32_t slot = (uint32_t)(idset_hash(&set->slots[i]) & (capacity - 1u));
      while (used[slot]) {
        slot = (slot + 1u) & (capacity - 1u);
      }
      slots[slot] = set->slots[i];
      used[slot] = 1u;
    }
    free(set->slots);
    free(set->used);
    set->slots = slots;
    set->used = used;
    set->capacity = capacity;
  }
  uint32_t slot = (uint32_t)(idset_hash(id) & (set->capacity - 1u));
  while (set->used[slot]) {
    if (vkr_depot_id_equal(&set->slots[slot], id)) {
      return 0;
    }
    slot = (slot + 1u) & (set->capacity - 1u);
  }
  set->slots[slot] = *id;
  set->used[slot] = 1u;
  set->count += 1u;
  return 1;
}

typedef struct WantItem {
  VkrDepotId id;
  uint8_t kind;
} WantItem;

typedef struct WantList {
  WantItem *items;
  uint32_t count;
  uint32_t capacity;
  uint32_t head;
} WantList;

static bool8_t want_push(WantList *list, const VkrDepotId *id, uint8_t kind) {
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 1024u;
    WantItem *items = realloc(list->items, (size_t)capacity * sizeof(WantItem));
    if (!items) {
      return false_v;
    }
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count++] = (WantItem){.id = *id, .kind = kind};
  return true_v;
}

static void want_free(WantList *list) {
  free(list->items);
  MemZero(list, sizeof(*list));
}

// =============================================================================
// Client
// =============================================================================

typedef struct ClientReply {
  bool8_t live;
  bool8_t received;
  uint64_t request;
  uint16_t message_id;
  VkrDepotMessage *message;
} ClientReply;

typedef struct ClientGet {
  bool8_t live;
  uint64_t request;
  uint32_t count;
  uint32_t remaining;
  WantItem items[VKR_DEPOT_BATCH];
  uint8_t done[VKR_DEPOT_BATCH / 8u];
} ClientGet;

struct VkrDepotClient {
  VkrDepotLink link;
  const VkrDepotStore *store;
  uint32_t timeout_ms;
  bool8_t opened;
  bool8_t closed;
  uint16_t close_code;
  uint64_t next_request;
  ClientReply replies[CLIENT_REPLY_SLOTS];
  VkrDepotMessage scratch;
  VkrDepotMessage outgoing;

  /* Fetch state. */
  ClientGet gets[CLIENT_GETS_MAX];
  IdSet seen;
  WantList want;
  bool8_t fetch_failed;
  uint64_t last_progress_us;
  VkrDepotTransferStats stats;
  char error[256];
};

static void client_fail(VkrDepotClient *client, const char *format,
                        const char *detail) {
  snprintf(client->error, sizeof(client->error), format, detail);
}

const char *vkr_depot_client_error(const VkrDepotClient *client) {
  return client->error;
}

static bool8_t client_bit(const uint8_t *bits, uint32_t index) {
  return (bits[index / 8u] >> (index % 8u)) & 1u ? true_v : false_v;
}

static void client_bit_set(uint8_t *bits, uint32_t index) {
  bits[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

/* A received object: check it, queue its children and store it. */
static bool8_t client_process(VkrDepotClient *client, const VkrDepotId *id,
                              uint8_t kind);

static bool8_t client_children(VkrDepotClient *client, uint8_t kind,
                               const uint8_t *bytes, uint64_t size);

typedef struct ChildContext {
  VkrDepotClient *client;
  bool8_t ok;
} ChildContext;

static bool8_t client_chunk_child(void *context,
                                  const VkrDepotChunkRef *chunk) {
  ChildContext *child = context;
  child->ok = client_process(child->client, &chunk->id, VKR_DEPOT_KIND_CHUNK);
  return child->ok;
}

static bool8_t client_entry_child(void *context,
                                  const VkrDepotTreeEntry *entry) {
  ChildContext *child = context;
  child->ok =
      client_process(child->client, &entry->id,
                     entry->kind == VKR_DEPOT_ENTRY_TREE ? VKR_DEPOT_KIND_TREE
                                                         : VKR_DEPOT_KIND_BLOB);
  return child->ok;
}

static bool8_t client_children(VkrDepotClient *client, uint8_t kind,
                               const uint8_t *bytes, uint64_t size) {
  if (vkr_depot_object_kind(bytes, size) != kind) {
    client_fail(client, "object of the wrong kind%s", "");
    return false_v;
  }
  ChildContext child = {.client = client, .ok = true_v};
  if (kind == VKR_DEPOT_KIND_COMMIT) {
    VkrDepotCommit commit;
    return vkr_depot_commit_parse(bytes, size, &commit) &&
           client_process(client, &commit.tree, VKR_DEPOT_KIND_TREE);
  }
  if (kind == VKR_DEPOT_KIND_TREE) {
    return vkr_depot_tree_parse(bytes, size, client_entry_child, &child) &&
           child.ok;
  }
  return vkr_depot_blob_parse(bytes, size, NULL, client_chunk_child, &child) &&
         child.ok;
}

static bool8_t client_process(VkrDepotClient *client, const VkrDepotId *id,
                              uint8_t kind) {
  const int32_t added = idset_add(&client->seen, id);
  if (added < 0) {
    client_fail(client, "out of memory%s", "");
    return false_v;
  }
  if (added == 0) {
    return true_v;
  }
  if (!vkr_depot_store_has(client->store, id)) {
    return want_push(&client->want, id, kind);
  }
  if (kind == VKR_DEPOT_KIND_CHUNK) {
    return true_v;
  }
  /* Present already: its children may still be missing. */
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(client->store, id, &bytes, &size)) {
    client_fail(client, "local store read failed%s", "");
    return false_v;
  }
  const bool8_t ok = client_children(client, kind, bytes, size);
  free(bytes);
  return ok;
}

static void client_on_data(VkrDepotClient *client, const VkrNetEvent *event,
                           uint64_t now) {
  if (!event->has_tag) {
    return;
  }
  const uint64_t request = event->tag >> 8;
  const uint32_t index = (uint32_t)(event->tag & 0xffu);
  ClientGet *get = NULL;
  for (uint32_t i = 0u; i < CLIENT_GETS_MAX; ++i) {
    if (client->gets[i].live && client->gets[i].request == request) {
      get = &client->gets[i];
      break;
    }
  }
  if (!get || index >= get->count || client_bit(get->done, index)) {
    return;
  }
  const WantItem *item = &get->items[index];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  bool8_t ok = vkr_depot_stored_decode(client->store->codec, event->data,
                                       event->size, &bytes, &size);
  if (ok) {
    VkrDepotId actual;
    vkr_depot_id_of(bytes, size, &actual);
    ok = vkr_depot_id_equal(&actual, &item->id);
  }
  if (ok && item->kind != VKR_DEPOT_KIND_CHUNK) {
    ok = client_children(client, item->kind, bytes, size);
  }
  if (ok) {
    ok = vkr_depot_store_put_stored(client->store, &item->id, event->data,
                                    event->size);
  }
  if (!ok) {
    client->fetch_failed = true_v;
    if (client->error[0] == '\0') {
      client_fail(client, "received an object that failed its check%s", "");
    }
  } else {
    client->stats.objects += 1u;
    client->stats.stored_bytes += event->size;
    client->stats.decoded_bytes += size;
  }
  free(bytes);
  client_bit_set(get->done, index);
  get->remaining -= 1u;
  if (get->remaining == 0u) {
    get->live = false_v;
  }
  client->last_progress_us = now;
}

static ClientReply *client_reply_find(VkrDepotClient *client,
                                      uint64_t request) {
  for (uint32_t i = 0u; i < CLIENT_REPLY_SLOTS; ++i) {
    if (client->replies[i].live && client->replies[i].request == request) {
      return &client->replies[i];
    }
  }
  return NULL;
}

static void client_service_event(void *context, VkrNetConnectionId connection,
                                 uint8_t channel, const VkrNetEvent *event,
                                 uint64_t now) {
  (void)connection;
  VkrDepotClient *client = context;
  if (event->type != VKR_NET_EVENT_MESSAGE) {
    return;
  }
  if (channel == VKR_DEPOT_DATA_CHANNEL) {
    client_on_data(client, event, now);
    return;
  }
  const VkrWireMessage *descriptor = NULL;
  if (vkr_wire_decode(vkr_depot_schema(), event->data, event->size,
                      &client->scratch, sizeof(client->scratch),
                      &descriptor) != VKR_WIRE_OK) {
    client->fetch_failed = true_v;
    client_fail(client, "server sent an invalid message%s", "");
    return;
  }
  /* Every reply starts with its request number. */
  const uint64_t request = client->scratch.request.request;
  if (descriptor->id == VKR_DEPOT_MSG_GET_REPLY) {
    const VkrDepotBitsWire *bits = &client->scratch.bits;
    for (uint32_t i = 0u; i < CLIENT_GETS_MAX; ++i) {
      ClientGet *get = &client->gets[i];
      if (!get->live || get->request != request) {
        continue;
      }
      for (uint32_t k = 0u; k < get->count && k / 8u < bits->bits_size; ++k) {
        if (client_bit(bits->bits, k)) {
          client->fetch_failed = true_v;
          char hex[VKR_DEPOT_ID_HEX];
          vkr_depot_id_hex(&get->items[k].id, hex);
          client_fail(client, "the server lacks object %s", hex);
        }
      }
    }
    client->last_progress_us = now;
    return;
  }
  ClientReply *reply = client_reply_find(client, request);
  if (reply && !reply->received) {
    *reply->message = client->scratch;
    reply->message_id = descriptor->id;
    reply->received = true_v;
  }
}

static void client_service_opened(void *context, VkrNetConnectionId connection,
                                  uint16_t version) {
  (void)connection;
  (void)version;
  ((VkrDepotClient *)context)->opened = true_v;
}

static void client_service_closed(void *context, VkrNetConnectionId connection,
                                  uint16_t code) {
  (void)connection;
  VkrDepotClient *client = context;
  client->closed = true_v;
  client->close_code = code;
}

static bool8_t client_alive(VkrDepotClient *client) {
  if (client->closed) {
    snprintf(client->error, sizeof(client->error),
             "the depot service closed (code %u)", client->close_code);
    return false_v;
  }
  return true_v;
}

static bool8_t client_pump(VkrDepotClient *client, int32_t timeout_ms) {
  if (!client->link.pump(client->link.context, timeout_ms)) {
    client_fail(client, "the network failed%s", "");
    return false_v;
  }
  return client_alive(client);
}

VkrDepotClient *vkr_depot_client_create(const VkrDepotLink *link,
                                        const VkrDepotStore *store,
                                        uint32_t timeout_ms) {
  VkrDepotClient *client = calloc(1u, sizeof(VkrDepotClient));
  if (!client) {
    return NULL;
  }
  client->link = *link;
  client->store = store;
  client->timeout_ms = timeout_ms ? timeout_ms : 30000u;
  client->next_request = 1u;
  for (uint32_t i = 0u; i < CLIENT_REPLY_SLOTS; ++i) {
    client->replies[i].message = malloc(sizeof(VkrDepotMessage));
    if (!client->replies[i].message) {
      vkr_depot_client_destroy(client);
      return NULL;
    }
  }
  VkrNetService service = vkr_depot_service_template();
  service.context = client;
  service.opened = client_service_opened;
  service.event = client_service_event;
  service.closed = client_service_closed;
  if (!vkr_net_session_register(link->session, &service) ||
      !vkr_net_session_open(link->session, link->connection,
                            VKR_NET_SERVICE_DEPOT, link->now(link->context))) {
    vkr_depot_client_destroy(client);
    return NULL;
  }
  const uint64_t limit =
      link->now(link->context) + (uint64_t)client->timeout_ms * 1000u;
  while (!client->opened && !client->closed &&
         link->now(link->context) < limit) {
    if (!link->pump(link->context, CLIENT_PUMP_MS)) {
      break;
    }
  }
  if (!client->opened) {
    vkr_depot_client_destroy(client);
    return NULL;
  }
  return client;
}

void vkr_depot_client_destroy(VkrDepotClient *client) {
  if (!client) {
    return;
  }
  for (uint32_t i = 0u; i < CLIENT_REPLY_SLOTS; ++i) {
    free(client->replies[i].message);
  }
  idset_free(&client->seen);
  want_free(&client->want);
  free(client);
}

/* Sends a request and returns its reply slot. */
static ClientReply *client_request(VkrDepotClient *client, VkrDepotMessageId id,
                                   void *message) {
  ClientReply *slot = NULL;
  for (uint32_t i = 0u; i < CLIENT_REPLY_SLOTS; ++i) {
    if (!client->replies[i].live) {
      slot = &client->replies[i];
      break;
    }
  }
  if (!slot) {
    client_fail(client, "too many requests in flight%s", "");
    return NULL;
  }
  const uint64_t request = client->next_request++;
  /* Every request message starts with the request number. */
  memcpy(message, &request, sizeof(request));
  slot->live = true_v;
  slot->received = false_v;
  slot->request = request;
  if (!vkr_depot_send(client->link.session, client->link.connection, id,
                      message, client->link.now(client->link.context))) {
    slot->live = false_v;
    client_fail(client, "could not send a request%s", "");
    return NULL;
  }
  return slot;
}

static bool8_t client_wait(VkrDepotClient *client, ClientReply *slot,
                           uint16_t expected) {
  const uint64_t limit = client->link.now(client->link.context) +
                         (uint64_t)client->timeout_ms * 1000u;
  while (!slot->received) {
    if (!client_pump(client, CLIENT_PUMP_MS)) {
      slot->live = false_v;
      return false_v;
    }
    if (client->link.now(client->link.context) >= limit) {
      slot->live = false_v;
      snprintf(client->error, sizeof(client->error),
               "the server did not answer request %llu (awaiting message %u) "
               "in time",
               (unsigned long long)slot->request, (uint32_t)expected);
      return false_v;
    }
  }
  slot->live = false_v;
  if (slot->message_id != expected) {
    client_fail(client, "the server answered with another message%s", "");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_depot_client_refs(VkrDepotClient *client, VkrDepotRef *out_refs,
                              uint32_t capacity, uint32_t *out_count) {
  VkrDepotRequestWire *request = &client->outgoing.request;
  MemZero(request, sizeof(*request));
  ClientReply *slot =
      client_request(client, VKR_DEPOT_MSG_REFS_REQUEST, request);
  if (!slot || !client_wait(client, slot, VKR_DEPOT_MSG_REFS)) {
    return false_v;
  }
  const VkrDepotRefsWire *refs = &slot->message->refs;
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < refs->ref_count && count < capacity; ++i) {
    snprintf(out_refs[count].name, sizeof(out_refs[count].name), "%s",
             refs->refs[i].name);
    memcpy(out_refs[count].commit.bytes, refs->refs[i].commit,
           VKR_DEPOT_ID_SIZE);
    ++count;
  }
  *out_count = count;
  return true_v;
}

// =============================================================================
// Fetch
// =============================================================================

static bool8_t client_issue_gets(VkrDepotClient *client) {
  while (client->want.head < client->want.count) {
    ClientGet *get = NULL;
    for (uint32_t i = 0u; i < CLIENT_GETS_MAX; ++i) {
      if (!client->gets[i].live) {
        get = &client->gets[i];
        break;
      }
    }
    if (!get) {
      return true_v;
    }
    VkrDepotIdsWire *message = &client->outgoing.ids;
    MemZero(message, sizeof(*message));
    message->zstd = client->store->codec != NULL;
    MemZero(get, sizeof(*get));
    while (client->want.head < client->want.count &&
           get->count < VKR_DEPOT_BATCH) {
      const WantItem *item = &client->want.items[client->want.head++];
      get->items[get->count] = *item;
      memcpy(message->ids[get->count], item->id.bytes, VKR_DEPOT_ID_SIZE);
      get->count += 1u;
    }
    message->id_count = get->count;
    get->request = client->next_request++;
    get->remaining = get->count;
    get->live = true_v;
    message->request = get->request;
    if (!vkr_depot_send(client->link.session, client->link.connection,
                        VKR_DEPOT_MSG_GET, message,
                        client->link.now(client->link.context))) {
      get->live = false_v;
      client_fail(client, "could not send a GET%s", "");
      return false_v;
    }
  }
  return true_v;
}

static bool8_t client_gets_live(const VkrDepotClient *client) {
  for (uint32_t i = 0u; i < CLIENT_GETS_MAX; ++i) {
    if (client->gets[i].live) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_depot_client_fetch(VkrDepotClient *client, const VkrDepotId *commit,
                               VkrDepotTransferStats *out_stats) {
  idset_free(&client->seen);
  want_free(&client->want);
  MemZero(client->gets, sizeof(client->gets));
  MemZero(&client->stats, sizeof(client->stats));
  client->fetch_failed = false_v;
  client->error[0] = '\0';
  if (!client_process(client, commit, VKR_DEPOT_KIND_COMMIT)) {
    return false_v;
  }
  client->last_progress_us = client->link.now(client->link.context);
  while (client->want.head < client->want.count || client_gets_live(client)) {
    if (!client_issue_gets(client) || !client_pump(client, CLIENT_PUMP_MS)) {
      return false_v;
    }
    if (client->fetch_failed) {
      return false_v;
    }
    const uint64_t now = client->link.now(client->link.context);
    if (now - client->last_progress_us > (uint64_t)client->timeout_ms * 1000u) {
      client_fail(client, "the transfer stopped making progress%s", "");
      return false_v;
    }
  }
  if (out_stats) {
    *out_stats = client->stats;
  }
  return true_v;
}

// =============================================================================
// Upload
// =============================================================================

typedef struct UploadSet {
  IdSet seen;
  VkrDepotId *ids;
  uint32_t count;
  uint32_t capacity;
} UploadSet;

static bool8_t upload_add(UploadSet *set, const VkrDepotId *id) {
  const int32_t added = idset_add(&set->seen, id);
  if (added <= 0) {
    return added == 0;
  }
  if (set->count == set->capacity) {
    const uint32_t capacity = set->capacity ? set->capacity * 2u : 1024u;
    VkrDepotId *ids = realloc(set->ids, (size_t)capacity * sizeof(VkrDepotId));
    if (!ids) {
      return false_v;
    }
    set->ids = ids;
    set->capacity = capacity;
  }
  set->ids[set->count++] = *id;
  return true_v;
}

static void upload_free(UploadSet *set) {
  idset_free(&set->seen);
  free(set->ids);
  MemZero(set, sizeof(*set));
}

typedef struct UploadEntries {
  VkrDepotTreeEntry *entries;
  uint32_t count;
  uint32_t capacity;
  uint8_t *bytes;
} UploadEntries;

static bool8_t upload_entry_visit(void *context,
                                  const VkrDepotTreeEntry *entry) {
  UploadEntries *list = context;
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 32u;
    VkrDepotTreeEntry *entries =
        realloc(list->entries, (size_t)capacity * sizeof(VkrDepotTreeEntry));
    if (!entries) {
      return false_v;
    }
    list->entries = entries;
    list->capacity = capacity;
  }
  list->entries[list->count++] = *entry;
  return true_v;
}

static bool8_t upload_entries_load(const VkrDepotStore *store,
                                   const VkrDepotId *tree,
                                   UploadEntries *list) {
  MemZero(list, sizeof(*list));
  uint64_t size = 0u;
  if (!vkr_depot_store_get(store, tree, &list->bytes, &size)) {
    return false_v;
  }
  return vkr_depot_tree_parse(list->bytes, size, upload_entry_visit, list);
}

static void upload_entries_free(UploadEntries *list) {
  free(list->entries);
  free(list->bytes);
  MemZero(list, sizeof(*list));
}

static bool8_t upload_chunk_visit(void *context,
                                  const VkrDepotChunkRef *chunk) {
  return upload_add(context, &chunk->id);
}

static bool8_t upload_blob(const VkrDepotStore *store, UploadSet *set,
                           const VkrDepotId *blob) {
  if (!upload_add(set, blob)) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (!vkr_depot_store_get(store, blob, &bytes, &size)) {
    return false_v;
  }
  const bool8_t ok =
      vkr_depot_blob_parse(bytes, size, NULL, upload_chunk_visit, set);
  free(bytes);
  return ok;
}

/* Adds `tree` and everything below it that `base` (NULL for none) lacks. */
static bool8_t upload_tree(const VkrDepotStore *store, UploadSet *set,
                           const VkrDepotId *tree, const VkrDepotId *base) {
  if (!upload_add(set, tree)) {
    return false_v;
  }
  UploadEntries current;
  UploadEntries previous = {0};
  if (!upload_entries_load(store, tree, &current)) {
    upload_entries_free(&current);
    return false_v;
  }
  const bool8_t has_base = base && upload_entries_load(store, base, &previous);
  bool8_t ok = true_v;
  for (uint32_t i = 0u; i < current.count && ok; ++i) {
    const VkrDepotTreeEntry *entry = &current.entries[i];
    const VkrDepotTreeEntry *before = NULL;
    for (uint32_t k = 0u; has_base && k < previous.count; ++k) {
      if (previous.entries[k].name_size == entry->name_size &&
          memcmp(previous.entries[k].name, entry->name, entry->name_size) ==
              0) {
        before = &previous.entries[k];
        break;
      }
    }
    if (before && before->kind == entry->kind &&
        vkr_depot_id_equal(&before->id, &entry->id)) {
      continue;
    }
    if (entry->kind == VKR_DEPOT_ENTRY_TREE) {
      const bool8_t was_tree = before && before->kind == VKR_DEPOT_ENTRY_TREE;
      ok = upload_tree(store, set, &entry->id, was_tree ? &before->id : NULL);
    } else {
      ok = upload_blob(store, set, &entry->id);
    }
  }
  upload_entries_free(&current);
  if (has_base) {
    upload_entries_free(&previous);
  }
  return ok;
}

typedef struct ClientPut {
  ClientReply *reply;
  uint32_t first;
  uint32_t count;
} ClientPut;

/* Waits for one PUT reply and checks it. */
static bool8_t client_put_finish(VkrDepotClient *client, ClientPut *put,
                                 const VkrDepotId *ids) {
  if (!client_wait(client, put->reply, VKR_DEPOT_MSG_PUT_REPLY)) {
    return false_v;
  }
  const VkrDepotBitsWire *bits = &put->reply->message->bits;
  for (uint32_t k = 0u; k < put->count && k / 8u < bits->bits_size; ++k) {
    if (client_bit(bits->bits, k)) {
      char hex[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&ids[put->first + k], hex);
      client_fail(client, "the server refused object %s", hex);
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_depot_client_upload(VkrDepotClient *client,
                                const VkrDepotId *commit,
                                const VkrDepotId *base,
                                VkrDepotTransferStats *out_stats) {
  client->error[0] = '\0';
  UploadSet set = {0};
  VkrDepotTransferStats stats = {0};
  bool8_t ok = upload_add(&set, commit);

  /* The commit's tree, minus what the base commit already holds. */
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  VkrDepotCommit parsed;
  ok = ok && vkr_depot_store_get(client->store, commit, &bytes, &size) &&
       vkr_depot_commit_parse(bytes, size, &parsed);
  VkrDepotId base_tree = {0};
  bool8_t has_base_tree = false_v;
  if (ok && base && !vkr_depot_id_zero(base)) {
    uint8_t *base_bytes = NULL;
    uint64_t base_size = 0u;
    VkrDepotCommit base_commit;
    if (vkr_depot_store_get(client->store, base, &base_bytes, &base_size) &&
        vkr_depot_commit_parse(base_bytes, base_size, &base_commit)) {
      base_tree = base_commit.tree;
      has_base_tree = true_v;
    }
    free(base_bytes);
  }
  ok = ok && upload_tree(client->store, &set, &parsed.tree,
                         has_base_tree ? &base_tree : NULL);
  free(bytes);
  if (!ok) {
    client_fail(client, "the commit's objects are not all in the store%s", "");
    upload_free(&set);
    return false_v;
  }

  /* Which of them the server lacks: HAVE in pipelined batches. */
  VkrDepotId *missing = malloc((size_t)Max(set.count, 1u) * sizeof(VkrDepotId));
  uint32_t missing_count = 0u;
  ClientReply *haves[CLIENT_HAVES_MAX];
  uint32_t have_first[CLIENT_HAVES_MAX];
  uint32_t have_count[CLIENT_HAVES_MAX];
  uint32_t live = 0u;
  uint32_t cursor = 0u;
  ok = missing != NULL;
  while (ok && (cursor < set.count || live > 0u)) {
    if (cursor < set.count && live < CLIENT_HAVES_MAX) {
      VkrDepotIdsWire *message = &client->outgoing.ids;
      MemZero(message, sizeof(*message));
      const uint32_t count = Min(set.count - cursor, VKR_DEPOT_BATCH);
      for (uint32_t i = 0u; i < count; ++i) {
        memcpy(message->ids[i], set.ids[cursor + i].bytes, VKR_DEPOT_ID_SIZE);
      }
      message->id_count = count;
      haves[live] = client_request(client, VKR_DEPOT_MSG_HAVE, message);
      have_first[live] = cursor;
      have_count[live] = count;
      ok = haves[live] != NULL;
      cursor += count;
      live += 1u;
      continue;
    }
    /* Collect the oldest reply. */
    ok = client_wait(client, haves[0], VKR_DEPOT_MSG_HAVE_REPLY);
    if (ok) {
      const VkrDepotBitsWire *bits = &haves[0]->message->bits;
      for (uint32_t k = 0u; k < have_count[0] && k / 8u < bits->bits_size;
           ++k) {
        if (client_bit(bits->bits, k)) {
          missing[missing_count++] = set.ids[have_first[0] + k];
        }
      }
    }
    for (uint32_t i = 1u; i < live; ++i) {
      haves[i - 1u] = haves[i];
      have_first[i - 1u] = have_first[i];
      have_count[i - 1u] = have_count[i];
    }
    live -= 1u;
  }

  /* Upload the missing ones: PUT names a batch, its bytes follow. */
  ClientPut puts[CLIENT_PUTS_MAX];
  uint32_t puts_live = 0u;
  cursor = 0u;
  while (ok && (cursor < missing_count || puts_live > 0u)) {
    if (cursor < missing_count && puts_live < CLIENT_PUTS_MAX) {
      VkrDepotIdsWire *message = &client->outgoing.ids;
      MemZero(message, sizeof(*message));
      const uint32_t count = Min(missing_count - cursor, VKR_DEPOT_BATCH);
      for (uint32_t i = 0u; i < count; ++i) {
        memcpy(message->ids[i], missing[cursor + i].bytes, VKR_DEPOT_ID_SIZE);
      }
      message->id_count = count;
      ClientReply *reply = client_request(client, VKR_DEPOT_MSG_PUT, message);
      ok = reply != NULL;
      for (uint32_t i = 0u; ok && i < count; ++i) {
        uint8_t *stored = NULL;
        uint64_t stored_size = 0u;
        ok = vkr_depot_store_get_stored(client->store, &missing[cursor + i],
                                        &stored, &stored_size);
        const VkrNetSendOptions options = {
            .flags = VKR_NET_SEND_TAG, .tag = vkr_depot_tag(reply->request, i)};
        while (ok) {
          const VkrNetSendStatus status = vkr_net_session_send(
              client->link.session, client->link.connection,
              VKR_NET_SERVICE_DEPOT, VKR_DEPOT_DATA_CHANNEL, stored,
              (uint32_t)stored_size, &options,
              client->link.now(client->link.context));
          if (status == VKR_NET_SEND_OK) {
            stats.objects += 1u;
            stats.stored_bytes += stored_size;
            break;
          }
          if (status != VKR_NET_SEND_QUEUE_FULL) {
            client_fail(client, "could not send object bytes%s", "");
            ok = false_v;
            break;
          }
          vkr_net_core_flush(client->link.core);
          ok = client_pump(client, CLIENT_PUMP_MS);
        }
        free(stored);
      }
      vkr_net_core_flush(client->link.core);
      puts[puts_live++] =
          (ClientPut){.reply = reply, .first = cursor, .count = count};
      cursor += count;
      continue;
    }
    ok = client_put_finish(client, &puts[0], missing);
    for (uint32_t i = 1u; i < puts_live; ++i) {
      puts[i - 1u] = puts[i];
    }
    puts_live -= 1u;
  }
  free(missing);
  upload_free(&set);
  if (ok && out_stats) {
    *out_stats = stats;
  }
  return ok;
}

VkrDepotRefStatus vkr_depot_client_update_ref(VkrDepotClient *client,
                                              const char *branch,
                                              const VkrDepotId *expected,
                                              const VkrDepotId *commit,
                                              VkrDepotId *out_current) {
  VkrDepotUpdateRefWire *message = &client->outgoing.update_ref;
  MemZero(message, sizeof(*message));
  snprintf(message->name, sizeof(message->name), "%s", branch);
  if (expected) {
    memcpy(message->expected, expected->bytes, VKR_DEPOT_ID_SIZE);
  }
  memcpy(message->commit, commit->bytes, VKR_DEPOT_ID_SIZE);
  ClientReply *slot = client_request(client, VKR_DEPOT_MSG_UPDATE_REF, message);
  if (!slot || !client_wait(client, slot, VKR_DEPOT_MSG_UPDATE_REF_REPLY)) {
    return VKR_DEPOT_REF_DENIED;
  }
  const VkrDepotUpdateRefReplyWire *reply = &slot->message->update_ref_reply;
  if (out_current) {
    memcpy(out_current->bytes, reply->current, VKR_DEPOT_ID_SIZE);
  }
  return (VkrDepotRefStatus)reply->status;
}

VkrDepotLockStatus vkr_depot_client_lock(VkrDepotClient *client,
                                         const char *path, bool8_t release,
                                         bool8_t force, uint8_t out_owner[32]) {
  VkrDepotLockWire *message = &client->outgoing.lock;
  MemZero(message, sizeof(*message));
  snprintf(message->path, sizeof(message->path), "%s", path);
  message->release = release;
  message->force = force;
  ClientReply *slot = client_request(client, VKR_DEPOT_MSG_LOCK, message);
  if (!slot || !client_wait(client, slot, VKR_DEPOT_MSG_LOCK_REPLY)) {
    return VKR_DEPOT_LOCK_DENIED;
  }
  if (out_owner) {
    memcpy(out_owner, slot->message->lock_reply.owner, 32u);
  }
  return (VkrDepotLockStatus)slot->message->lock_reply.status;
}

bool8_t vkr_depot_client_locks(VkrDepotClient *client,
                               VkrDepotLockVisitor visit, void *context) {
  char after[VKR_DEPOT_LOCK_PATH_MAX] = "";
  for (;;) {
    VkrDepotLocksRequestWire *message = &client->outgoing.locks_request;
    MemZero(message, sizeof(*message));
    snprintf(message->after, sizeof(message->after), "%s", after);
    ClientReply *slot =
        client_request(client, VKR_DEPOT_MSG_LOCKS_REQUEST, message);
    if (!slot || !client_wait(client, slot, VKR_DEPOT_MSG_LOCKS)) {
      return false_v;
    }
    const VkrDepotLocksWire *locks = &slot->message->locks;
    for (uint32_t i = 0u; i < locks->entry_count; ++i) {
      const VkrDepotLockEntryWire *entry = &locks->entries[i];
      if (!visit(context, entry->path, entry->owner, entry->time_us)) {
        return true_v;
      }
      snprintf(after, sizeof(after), "%s", entry->path);
    }
    if (!locks->more) {
      return true_v;
    }
  }
}

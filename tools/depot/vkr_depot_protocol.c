#include "vkr_depot_protocol.h"

#include <stddef.h>
#include <string.h>

#define DEPOT_ENCODE_MAX (VKR_DEPOT_BATCH * VKR_DEPOT_ID_SIZE + 64u)

static const VkrWireField depot_request_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotRequestWire, request)},
};

static const VkrWireField depot_ref_fields[] = {
    {.name = "name",
     .kind = VKR_WIRE_UTF8,
     .capacity = VKR_DEPOT_BRANCH_NAME,
     .offset = offsetof(VkrDepotRefWire, name)},
    {.name = "commit",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotRefWire, commit)},
};

static const VkrWireMessage depot_ref_message = {
    .name = "ref",
    .size = sizeof(VkrDepotRefWire),
    .fields = depot_ref_fields,
    .field_count = ArrayCount(depot_ref_fields),
};

static const VkrWireField depot_ref_element = {
    .name = "ref", .kind = VKR_WIRE_MESSAGE, .message = &depot_ref_message};

static const VkrWireField depot_refs_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotRefsWire, request)},
    {.name = "refs",
     .kind = VKR_WIRE_ARRAY,
     .capacity = VKR_DEPOT_REFS_MAX,
     .offset = offsetof(VkrDepotRefsWire, refs),
     .count_offset = offsetof(VkrDepotRefsWire, ref_count),
     .element = &depot_ref_element,
     .stride = sizeof(VkrDepotRefWire)},
};

static const VkrWireField depot_id_element = {.name = "id",
                                              .kind = VKR_WIRE_HASH256};

static const VkrWireField depot_ids_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotIdsWire, request)},
    {.name = "zstd",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(VkrDepotIdsWire, zstd)},
    {.name = "ids",
     .kind = VKR_WIRE_ARRAY,
     .capacity = VKR_DEPOT_BATCH,
     .offset = offsetof(VkrDepotIdsWire, ids),
     .count_offset = offsetof(VkrDepotIdsWire, id_count),
     .element = &depot_id_element,
     .stride = VKR_DEPOT_ID_SIZE},
};

static const VkrWireField depot_bits_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotBitsWire, request)},
    {.name = "bits",
     .kind = VKR_WIRE_BYTES,
     .capacity = VKR_DEPOT_BATCH / 8u,
     .offset = offsetof(VkrDepotBitsWire, bits),
     .count_offset = offsetof(VkrDepotBitsWire, bits_size)},
};

static const VkrWireField depot_update_ref_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotUpdateRefWire, request)},
    {.name = "name",
     .kind = VKR_WIRE_UTF8,
     .capacity = VKR_DEPOT_BRANCH_NAME,
     .offset = offsetof(VkrDepotUpdateRefWire, name)},
    {.name = "expected",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotUpdateRefWire, expected)},
    {.name = "commit",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotUpdateRefWire, commit)},
};

static const VkrWireField depot_update_ref_reply_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotUpdateRefReplyWire, request)},
    {.name = "status",
     .kind = VKR_WIRE_ENUM,
     .enum_count = VKR_DEPOT_REF_STATUS_COUNT,
     .offset = offsetof(VkrDepotUpdateRefReplyWire, status)},
    {.name = "current",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotUpdateRefReplyWire, current)},
};

static const VkrWireField depot_lock_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotLockWire, request)},
    {.name = "path",
     .kind = VKR_WIRE_UTF8,
     .capacity = VKR_DEPOT_LOCK_PATH_MAX,
     .offset = offsetof(VkrDepotLockWire, path)},
    {.name = "release",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(VkrDepotLockWire, release)},
    {.name = "force",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(VkrDepotLockWire, force)},
};

static const VkrWireField depot_lock_reply_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotLockReplyWire, request)},
    {.name = "status",
     .kind = VKR_WIRE_ENUM,
     .enum_count = VKR_DEPOT_LOCK_STATUS_COUNT,
     .offset = offsetof(VkrDepotLockReplyWire, status)},
    {.name = "owner",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotLockReplyWire, owner)},
};

static const VkrWireField depot_locks_request_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotLocksRequestWire, request)},
    {.name = "after",
     .kind = VKR_WIRE_UTF8,
     .capacity = VKR_DEPOT_LOCK_PATH_MAX,
     .offset = offsetof(VkrDepotLocksRequestWire, after)},
};

static const VkrWireField depot_lock_entry_fields[] = {
    {.name = "path",
     .kind = VKR_WIRE_UTF8,
     .capacity = VKR_DEPOT_LOCK_PATH_MAX,
     .offset = offsetof(VkrDepotLockEntryWire, path)},
    {.name = "owner",
     .kind = VKR_WIRE_HASH256,
     .offset = offsetof(VkrDepotLockEntryWire, owner)},
    {.name = "time_us",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotLockEntryWire, time_us)},
};

static const VkrWireMessage depot_lock_entry_message = {
    .name = "lock_entry",
    .size = sizeof(VkrDepotLockEntryWire),
    .fields = depot_lock_entry_fields,
    .field_count = ArrayCount(depot_lock_entry_fields),
};

static const VkrWireField depot_lock_entry_element = {
    .name = "entry",
    .kind = VKR_WIRE_MESSAGE,
    .message = &depot_lock_entry_message};

static const VkrWireField depot_locks_fields[] = {
    {.name = "request",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(VkrDepotLocksWire, request)},
    {.name = "entries",
     .kind = VKR_WIRE_ARRAY,
     .capacity = VKR_DEPOT_LOCKS_PAGE,
     .offset = offsetof(VkrDepotLocksWire, entries),
     .count_offset = offsetof(VkrDepotLocksWire, entry_count),
     .element = &depot_lock_entry_element,
     .stride = sizeof(VkrDepotLockEntryWire)},
    {.name = "more",
     .kind = VKR_WIRE_BOOL,
     .offset = offsetof(VkrDepotLocksWire, more)},
};

#define DEPOT_MESSAGE(identifier, label, type, field_table)                    \
  static const VkrWireMessage depot_##label = {                                \
      .name = #label,                                                          \
      .id = identifier,                                                        \
      .size = sizeof(type),                                                    \
      .fields = field_table,                                                   \
      .field_count = ArrayCount(field_table),                                  \
  }

DEPOT_MESSAGE(VKR_DEPOT_MSG_REFS_REQUEST, refs_request, VkrDepotRequestWire,
              depot_request_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_REFS, refs, VkrDepotRefsWire, depot_refs_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_HAVE, have, VkrDepotIdsWire, depot_ids_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_HAVE_REPLY, have_reply, VkrDepotBitsWire,
              depot_bits_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_GET, get, VkrDepotIdsWire, depot_ids_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_GET_REPLY, get_reply, VkrDepotBitsWire,
              depot_bits_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_PUT, put, VkrDepotIdsWire, depot_ids_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_PUT_REPLY, put_reply, VkrDepotBitsWire,
              depot_bits_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_UPDATE_REF, update_ref, VkrDepotUpdateRefWire,
              depot_update_ref_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_UPDATE_REF_REPLY, update_ref_reply,
              VkrDepotUpdateRefReplyWire, depot_update_ref_reply_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_LOCK, lock, VkrDepotLockWire, depot_lock_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_LOCK_REPLY, lock_reply, VkrDepotLockReplyWire,
              depot_lock_reply_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_LOCKS_REQUEST, locks_request,
              VkrDepotLocksRequestWire, depot_locks_request_fields);
DEPOT_MESSAGE(VKR_DEPOT_MSG_LOCKS, locks, VkrDepotLocksWire,
              depot_locks_fields);

static const VkrWireMessage *const depot_messages[] = {
    &depot_refs_request,  &depot_refs,
    &depot_have,          &depot_have_reply,
    &depot_get,           &depot_get_reply,
    &depot_put,           &depot_put_reply,
    &depot_update_ref,    &depot_update_ref_reply,
    &depot_lock,          &depot_lock_reply,
    &depot_locks_request, &depot_locks,
};

static const VkrWireSchema depot_schema = {
    .name = "vkr.depot",
    .version = VKR_DEPOT_SERVICE_VERSION,
    .messages = depot_messages,
    .message_count = ArrayCount(depot_messages),
};

const VkrWireSchema *vkr_depot_schema(void) { return &depot_schema; }

const VkrWireMessage *vkr_depot_message(VkrDepotMessageId id) {
  for (uint32_t i = 0u; i < ArrayCount(depot_messages); ++i) {
    if (depot_messages[i]->id == id) {
      return depot_messages[i];
    }
  }
  return NULL;
}

VkrNetService vkr_depot_service_template(void) {
  VkrNetService service = {
      .id = VKR_NET_SERVICE_DEPOT,
      .version_count = 1u,
      .channel_count = 2u,
  };
  service.versions[0] = (VkrNetServiceVersion){
      .version = VKR_DEPOT_SERVICE_VERSION,
      .schema_hash = vkr_wire_schema_hash(&depot_schema)};
  /* Requests and replies: reliable, ordered, ahead of bulk bytes. */
  service.channels[VKR_DEPOT_CONTROL_CHANNEL] = (VkrNetChannelConfig){
      .delivery = VKR_NET_RELIABLE_ORDERED,
      .priority = 2u,
      .max_message_size = 64u * 1024u,
      .receive_window = 1024u * 1024u,
      .send_queue = 256u,
  };
  /* Object bytes: any order, the lowest priority, a window that keeps a
     long fat path full. */
  service.channels[VKR_DEPOT_DATA_CHANNEL] = (VkrNetChannelConfig){
      .delivery = VKR_NET_RELIABLE_UNORDERED,
      .priority = 7u,
      .max_message_size = VKR_DEPOT_STORED_MAX,
      .receive_window = 64u * 1024u * 1024u,
      .send_queue = 512u,
  };
  return service;
}

bool8_t vkr_depot_send(VkrNetSession *session, VkrNetConnectionId connection,
                       VkrDepotMessageId id, const void *message,
                       uint64_t now_us) {
  static uint8_t bytes[DEPOT_ENCODE_MAX];
  uint32_t size = 0u;
  const VkrWireMessage *descriptor = vkr_depot_message(id);
  if (!descriptor || vkr_wire_encode(descriptor, message, bytes, sizeof(bytes),
                                     &size) != VKR_WIRE_OK) {
    return false_v;
  }
  return vkr_net_session_send(session, connection, VKR_NET_SERVICE_DEPOT,
                              VKR_DEPOT_CONTROL_CHANNEL, bytes, size, NULL,
                              now_us) == VKR_NET_SEND_OK;
}

bool8_t vkr_depot_branch_valid(const char *name) {
  const size_t size = strlen(name);
  if (size == 0u || size >= VKR_DEPOT_BRANCH_NAME || name[0] == '.') {
    return false_v;
  }
  for (size_t i = 0u; i < size; ++i) {
    const char c = name[i];
    const bool8_t ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                       c == '-';
    if (!ok) {
      return false_v;
    }
  }
  return true_v;
}

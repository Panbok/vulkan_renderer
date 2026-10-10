#include "vkr_net_session.h"

#include "vkr_wire.h"

#include <stddef.h>
#include <string.h>

/* Connection state lives in a two-level table indexed by the connection
   ID's slot (its low 16 bits), allocated a page at a time. */
#define SESSION_PAGE_BITS 8u
#define SESSION_PAGE_SIZE (1u << SESSION_PAGE_BITS)
#define SESSION_PAGE_COUNT (65536u / SESSION_PAGE_SIZE)
#define SESSION_MESSAGE_MAX 1024u

// =============================================================================
// Session schema
// =============================================================================

enum {
  SESSION_OPEN = 1,
  SESSION_ACCEPT = 2,
  SESSION_REFUSE = 3,
  SESSION_CLOSE = 4,
};

typedef struct SessionVersionWire {
  uint64_t version;
  uint64_t schema_hash;
} SessionVersionWire;

typedef struct SessionChannelWire {
  uint64_t channel;
  uint32_t delivery;
  uint64_t priority;
  uint64_t weight;
  uint64_t max_message_size;
  uint64_t receive_window;
  uint64_t send_queue;
  uint64_t deadline_ms;
} SessionChannelWire;

typedef struct SessionOpenWire {
  uint64_t service;
  SessionVersionWire versions[VKR_NET_SERVICE_VERSION_MAX];
  uint32_t version_count;
  SessionChannelWire channels[VKR_NET_SERVICE_CHANNEL_MAX];
  uint32_t channel_count;
} SessionOpenWire;

typedef struct SessionReplyWire {
  uint64_t service;
  /* ACCEPT: the version; REFUSE and CLOSE: the code. */
  uint64_t value;
} SessionReplyWire;

static const VkrWireField session_version_fields[] = {
    {.name = "version",
     .kind = VKR_WIRE_UINT,
     .bits = 16u,
     .offset = offsetof(SessionVersionWire, version)},
    {.name = "schema_hash",
     .kind = VKR_WIRE_HASH64,
     .offset = offsetof(SessionVersionWire, schema_hash)},
};

static const VkrWireMessage session_version_message = {
    .name = "version",
    .size = sizeof(SessionVersionWire),
    .fields = session_version_fields,
    .field_count = ArrayCount(session_version_fields),
};

static const VkrWireField session_channel_fields[] = {
    {.name = "channel",
     .kind = VKR_WIRE_UINT,
     .bits = 6u,
     .offset = offsetof(SessionChannelWire, channel)},
    {.name = "delivery",
     .kind = VKR_WIRE_ENUM,
     .enum_count = VKR_NET_DELIVERY_COUNT,
     .offset = offsetof(SessionChannelWire, delivery)},
    {.name = "priority",
     .kind = VKR_WIRE_UINT,
     .bits = 3u,
     .offset = offsetof(SessionChannelWire, priority)},
    {.name = "weight",
     .kind = VKR_WIRE_UINT,
     .bits = 16u,
     .offset = offsetof(SessionChannelWire, weight)},
    {.name = "max_message_size",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(SessionChannelWire, max_message_size)},
    {.name = "receive_window",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(SessionChannelWire, receive_window)},
    {.name = "send_queue",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(SessionChannelWire, send_queue)},
    {.name = "deadline_ms",
     .kind = VKR_WIRE_VARUINT,
     .offset = offsetof(SessionChannelWire, deadline_ms)},
};

static const VkrWireMessage session_channel_message = {
    .name = "channel",
    .size = sizeof(SessionChannelWire),
    .fields = session_channel_fields,
    .field_count = ArrayCount(session_channel_fields),
};

static const VkrWireField session_version_element = {
    .name = "version",
    .kind = VKR_WIRE_MESSAGE,
    .message = &session_version_message};

static const VkrWireField session_channel_element = {
    .name = "channel",
    .kind = VKR_WIRE_MESSAGE,
    .message = &session_channel_message};

static const VkrWireField session_open_fields[] = {
    {.name = "service",
     .kind = VKR_WIRE_UINT,
     .bits = 16u,
     .offset = offsetof(SessionOpenWire, service)},
    {.name = "versions",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(SessionOpenWire, versions),
     .capacity = VKR_NET_SERVICE_VERSION_MAX,
     .count_offset = offsetof(SessionOpenWire, version_count),
     .element = &session_version_element,
     .stride = sizeof(SessionVersionWire)},
    {.name = "channels",
     .kind = VKR_WIRE_ARRAY,
     .offset = offsetof(SessionOpenWire, channels),
     .capacity = VKR_NET_SERVICE_CHANNEL_MAX,
     .count_offset = offsetof(SessionOpenWire, channel_count),
     .element = &session_channel_element,
     .stride = sizeof(SessionChannelWire)},
};

static const VkrWireField session_reply_fields[] = {
    {.name = "service",
     .kind = VKR_WIRE_UINT,
     .bits = 16u,
     .offset = offsetof(SessionReplyWire, service)},
    {.name = "value",
     .kind = VKR_WIRE_UINT,
     .bits = 16u,
     .offset = offsetof(SessionReplyWire, value)},
};

static const VkrWireMessage session_open_message = {
    .name = "service_open",
    .id = SESSION_OPEN,
    .size = sizeof(SessionOpenWire),
    .fields = session_open_fields,
    .field_count = ArrayCount(session_open_fields),
};

static const VkrWireMessage session_accept_message = {
    .name = "service_accept",
    .id = SESSION_ACCEPT,
    .size = sizeof(SessionReplyWire),
    .fields = session_reply_fields,
    .field_count = ArrayCount(session_reply_fields),
};

static const VkrWireMessage session_refuse_message = {
    .name = "service_refuse",
    .id = SESSION_REFUSE,
    .size = sizeof(SessionReplyWire),
    .fields = session_reply_fields,
    .field_count = ArrayCount(session_reply_fields),
};

static const VkrWireMessage session_close_message = {
    .name = "service_close",
    .id = SESSION_CLOSE,
    .size = sizeof(SessionReplyWire),
    .fields = session_reply_fields,
    .field_count = ArrayCount(session_reply_fields),
};

static const VkrWireMessage *const session_messages[] = {
    &session_open_message,
    &session_accept_message,
    &session_refuse_message,
    &session_close_message,
};

static const VkrWireSchema session_schema = {
    .name = "vkr.session",
    .version = 1u,
    .messages = session_messages,
    .message_count = ArrayCount(session_messages),
};

// =============================================================================
// State
// =============================================================================

typedef enum SessionServiceState {
  SESSION_SERVICE_NONE = 0,
  SESSION_SERVICE_OPENING,
  SESSION_SERVICE_OPEN,
} SessionServiceState;

typedef struct SessionBinding {
  uint16_t service;
  uint16_t version;
  uint8_t state; /**< SessionServiceState. */
  uint8_t registered;
  uint8_t channel_count;
  uint8_t channels[VKR_NET_SERVICE_CHANNEL_MAX];
} SessionBinding;

typedef struct SessionConnection {
  VkrNetConnectionId connection;
  /* Core channels services hold; bit 0 is the session channel. */
  uint64_t used_channels;
  /* Core channel to binding index + 1 and the service's channel index. */
  uint8_t channel_binding[VKR_NET_CHANNEL_MAX];
  uint8_t channel_index[VKR_NET_CHANNEL_MAX];
  SessionBinding bindings[VKR_NET_SESSION_SERVICE_MAX];
} SessionConnection;

struct VkrNetSession {
  VkrAllocator *allocator;
  VkrNetCore *core;
  VkrNetService services[VKR_NET_SESSION_SERVICE_MAX];
  uint32_t service_count;
  SessionConnection *pages[SESSION_PAGE_COUNT];
};

static SessionConnection *session_find(const VkrNetSession *session,
                                       VkrNetConnectionId connection) {
  const uint32_t slot = connection & 0xffffu;
  const SessionConnection *page = session->pages[slot >> SESSION_PAGE_BITS];
  if (!page) {
    return NULL;
  }
  SessionConnection *state =
      (SessionConnection *)&page[slot & (SESSION_PAGE_SIZE - 1u)];
  return state->connection == connection ? state : NULL;
}

static SessionConnection *session_get(VkrNetSession *session,
                                      VkrNetConnectionId connection) {
  SessionConnection *state = session_find(session, connection);
  if (state) {
    return state;
  }
  const uint32_t slot = connection & 0xffffu;
  SessionConnection **page = &session->pages[slot >> SESSION_PAGE_BITS];
  if (!*page) {
    const uint64_t bytes =
        (uint64_t)SESSION_PAGE_SIZE * sizeof(SessionConnection);
    *page = vkr_allocator_alloc(session->allocator, bytes,
                                VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!*page) {
      return NULL;
    }
    MemZero(*page, bytes);
  }
  state = &(*page)[slot & (SESSION_PAGE_SIZE - 1u)];
  /* A reused slot belonged to a closed connection. */
  MemZero(state, sizeof(*state));
  state->connection = connection;
  state->used_channels = 1u;
  return state;
}

static const VkrNetService *session_service(const VkrNetSession *session,
                                            uint16_t id, uint32_t *out_index) {
  for (uint32_t i = 0u; i < session->service_count; ++i) {
    if (session->services[i].id == id) {
      if (out_index) {
        *out_index = i;
      }
      return &session->services[i];
    }
  }
  return NULL;
}

static SessionBinding *session_binding(SessionConnection *state,
                                       uint16_t service) {
  for (uint32_t i = 0u; i < VKR_NET_SESSION_SERVICE_MAX; ++i) {
    if (state->bindings[i].state != SESSION_SERVICE_NONE &&
        state->bindings[i].service == service) {
      return &state->bindings[i];
    }
  }
  return NULL;
}

static SessionBinding *session_binding_alloc(SessionConnection *state) {
  for (uint32_t i = 0u; i < VKR_NET_SESSION_SERVICE_MAX; ++i) {
    if (state->bindings[i].state == SESSION_SERVICE_NONE) {
      MemZero(&state->bindings[i], sizeof(SessionBinding));
      return &state->bindings[i];
    }
  }
  return NULL;
}

/* Closes the binding's core channels and frees it. */
static void session_binding_release(VkrNetSession *session,
                                    SessionConnection *state,
                                    SessionBinding *binding) {
  for (uint32_t i = 0u; i < binding->channel_count; ++i) {
    const uint8_t channel = binding->channels[i];
    vkr_net_core_close_channel(session->core, state->connection, channel);
    state->used_channels &= ~(1ull << channel);
    state->channel_binding[channel] = 0u;
  }
  MemZero(binding, sizeof(*binding));
}

static bool8_t session_send_message(VkrNetSession *session,
                                    VkrNetConnectionId connection,
                                    const VkrWireMessage *message,
                                    const void *value, uint64_t now) {
  uint8_t bytes[SESSION_MESSAGE_MAX];
  uint32_t size = 0u;
  if (vkr_wire_encode(message, value, bytes, sizeof(bytes), &size) !=
      VKR_WIRE_OK) {
    return false_v;
  }
  const VkrNetSendOptions options = {.flags = VKR_NET_SEND_IMMEDIATE};
  return vkr_net_core_send(session->core, connection, 0u, bytes, size, &options,
                           now) == VKR_NET_SEND_OK;
}

static void session_reply(VkrNetSession *session, VkrNetConnectionId connection,
                          const VkrWireMessage *message, uint16_t service,
                          uint16_t value, uint64_t now) {
  const SessionReplyWire reply = {.service = service, .value = value};
  (void)session_send_message(session, connection, message, &reply, now);
}

/* Opens core channels `ids` with the service's configs. */
static bool8_t
session_open_channels(VkrNetSession *session, SessionConnection *state,
                      SessionBinding *binding, uint32_t binding_index,
                      const VkrNetService *service, const uint8_t *ids) {
  for (uint32_t i = 0u; i < service->channel_count; ++i) {
    if (!vkr_net_core_open_channel(session->core, state->connection, ids[i],
                                   &service->channels[i])) {
      session_binding_release(session, state, binding);
      return false_v;
    }
    binding->channels[i] = ids[i];
    binding->channel_count = (uint8_t)(i + 1u);
    state->used_channels |= 1ull << ids[i];
    state->channel_binding[ids[i]] = (uint8_t)(binding_index + 1u);
    state->channel_index[ids[i]] = (uint8_t)i;
  }
  return true_v;
}

// =============================================================================
// API
// =============================================================================

VkrNetSession *vkr_net_session_create(VkrAllocator *allocator,
                                      VkrNetCore *core) {
  if (!allocator || !core || !vkr_wire_schema_validate(&session_schema)) {
    return NULL;
  }
  VkrNetSession *session = vkr_allocator_alloc(allocator, sizeof(VkrNetSession),
                                               VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!session) {
    return NULL;
  }
  MemZero(session, sizeof(*session));
  session->allocator = allocator;
  session->core = core;
  return session;
}

void vkr_net_session_destroy(VkrNetSession *session) {
  if (!session) {
    return;
  }
  for (uint32_t i = 0u; i < SESSION_PAGE_COUNT; ++i) {
    if (session->pages[i]) {
      vkr_allocator_free(session->allocator, session->pages[i],
                         (uint64_t)SESSION_PAGE_SIZE *
                             sizeof(SessionConnection),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    }
  }
  vkr_allocator_free(session->allocator, session, sizeof(VkrNetSession),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

bool8_t vkr_net_session_register(VkrNetSession *session,
                                 const VkrNetService *service) {
  if (!session || !service ||
      session->service_count == VKR_NET_SESSION_SERVICE_MAX ||
      service->version_count == 0u ||
      service->version_count > VKR_NET_SERVICE_VERSION_MAX ||
      service->channel_count > VKR_NET_SERVICE_CHANNEL_MAX ||
      session_service(session, service->id, NULL)) {
    return false_v;
  }
  session->services[session->service_count++] = *service;
  return true_v;
}

bool8_t vkr_net_session_open(VkrNetSession *session,
                             VkrNetConnectionId connection, uint16_t service_id,
                             uint64_t now_us) {
  uint32_t registered = 0u;
  const VkrNetService *service =
      session ? session_service(session, service_id, &registered) : NULL;
  SessionConnection *state = service ? session_get(session, connection) : NULL;
  if (!state || session_binding(state, service_id)) {
    return false_v;
  }
  SessionBinding *binding = session_binding_alloc(state);
  if (!binding) {
    return false_v;
  }
  const uint32_t binding_index = (uint32_t)(binding - state->bindings);

  /* The opener picks free core channels for the service. */
  uint8_t ids[VKR_NET_SERVICE_CHANNEL_MAX];
  uint32_t found = 0u;
  for (uint32_t channel = 1u;
       channel < VKR_NET_CHANNEL_MAX && found < service->channel_count;
       ++channel) {
    if (!(state->used_channels & (1ull << channel))) {
      ids[found++] = (uint8_t)channel;
    }
  }
  if (found < service->channel_count) {
    return false_v;
  }
  binding->service = service_id;
  binding->registered = (uint8_t)registered;
  binding->state = SESSION_SERVICE_OPENING;
  if (!session_open_channels(session, state, binding, binding_index, service,
                             ids)) {
    return false_v;
  }

  SessionOpenWire open = {.service = service_id,
                          .version_count = service->version_count,
                          .channel_count = service->channel_count};
  for (uint32_t i = 0u; i < service->version_count; ++i) {
    open.versions[i] =
        (SessionVersionWire){.version = service->versions[i].version,
                             .schema_hash = service->versions[i].schema_hash};
  }
  for (uint32_t i = 0u; i < service->channel_count; ++i) {
    const VkrNetChannelConfig *config = &service->channels[i];
    open.channels[i] = (SessionChannelWire){
        .channel = ids[i],
        .delivery = config->delivery,
        .priority = config->priority,
        .weight = config->weight,
        .max_message_size = config->max_message_size,
        .receive_window = config->receive_window,
        .send_queue = config->send_queue,
        .deadline_ms = config->deadline_ms,
    };
  }
  if (!session_send_message(session, connection, &session_open_message, &open,
                            now_us)) {
    session_binding_release(session, state, binding);
    return false_v;
  }
  return true_v;
}

void vkr_net_session_close(VkrNetSession *session,
                           VkrNetConnectionId connection, uint16_t service_id,
                           uint16_t code, uint64_t now_us) {
  SessionConnection *state = session ? session_find(session, connection) : NULL;
  SessionBinding *binding = state ? session_binding(state, service_id) : NULL;
  if (!binding) {
    return;
  }
  const VkrNetService *service = &session->services[binding->registered];
  session_reply(session, connection, &session_close_message, service_id, code,
                now_us);
  session_binding_release(session, state, binding);
  if (service->closed) {
    service->closed(service->context, connection, code);
  }
}

bool8_t vkr_net_session_is_open(const VkrNetSession *session,
                                VkrNetConnectionId connection,
                                uint16_t service_id) {
  return vkr_net_session_version(session, connection, service_id) != 0u;
}

uint16_t vkr_net_session_version(const VkrNetSession *session,
                                 VkrNetConnectionId connection,
                                 uint16_t service_id) {
  SessionConnection *state = session ? session_find(session, connection) : NULL;
  const SessionBinding *binding =
      state ? session_binding(state, service_id) : NULL;
  return binding && binding->state == SESSION_SERVICE_OPEN ? binding->version
                                                           : 0u;
}

VkrNetSendStatus vkr_net_session_send(VkrNetSession *session,
                                      VkrNetConnectionId connection,
                                      uint16_t service_id, uint8_t channel,
                                      const void *data, uint32_t size,
                                      const VkrNetSendOptions *options,
                                      uint64_t now_us) {
  SessionConnection *state = session ? session_find(session, connection) : NULL;
  const SessionBinding *binding =
      state ? session_binding(state, service_id) : NULL;
  if (!binding || binding->state != SESSION_SERVICE_OPEN ||
      channel >= binding->channel_count) {
    return VKR_NET_SEND_CLOSED;
  }
  return vkr_net_core_send(session->core, connection,
                           binding->channels[channel], data, size, options,
                           now_us);
}

uint64_t vkr_net_session_queued_bytes(const VkrNetSession *session,
                                      VkrNetConnectionId connection,
                                      uint16_t service_id, uint8_t channel) {
  SessionConnection *state = session ? session_find(session, connection) : NULL;
  const SessionBinding *binding =
      state ? session_binding(state, service_id) : NULL;
  if (!binding || channel >= binding->channel_count) {
    return 0u;
  }
  return vkr_net_core_queued_bytes(session->core, connection,
                                   binding->channels[channel]);
}

// =============================================================================
// Dispatch
// =============================================================================

static bool8_t session_channels_match(const VkrNetService *service,
                                      const SessionOpenWire *open) {
  if (open->channel_count != service->channel_count) {
    return false_v;
  }
  for (uint32_t i = 0u; i < service->channel_count; ++i) {
    const VkrNetChannelConfig *mine = &service->channels[i];
    const SessionChannelWire *theirs = &open->channels[i];
    if (theirs->delivery != mine->delivery ||
        theirs->priority != mine->priority || theirs->weight != mine->weight ||
        theirs->max_message_size != mine->max_message_size ||
        theirs->receive_window != mine->receive_window ||
        theirs->send_queue != mine->send_queue ||
        theirs->deadline_ms != mine->deadline_ms) {
      return false_v;
    }
  }
  return true_v;
}

static void session_on_open(VkrNetSession *session,
                            VkrNetConnectionId connection,
                            const SessionOpenWire *open, uint64_t now) {
  const uint16_t service_id = (uint16_t)open->service;
  uint32_t registered = 0u;
  const VkrNetService *service =
      session_service(session, service_id, &registered);
  if (!service) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_SERVICE_REFUSED, now);
    return;
  }
  /* The highest version both list with the same schema. */
  uint16_t chosen = 0u;
  for (uint32_t i = 0u; i < open->version_count; ++i) {
    for (uint32_t j = 0u; j < service->version_count; ++j) {
      if (open->versions[i].version == service->versions[j].version &&
          open->versions[i].schema_hash == service->versions[j].schema_hash &&
          open->versions[i].version > chosen) {
        chosen = (uint16_t)open->versions[i].version;
      }
    }
  }
  if (chosen == 0u || !session_channels_match(service, open)) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_SCHEMA_MISMATCH, now);
    return;
  }
  SessionConnection *state = session_get(session, connection);
  if (!state) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_INTERNAL, now);
    return;
  }
  uint8_t ids[VKR_NET_SERVICE_CHANNEL_MAX];
  uint64_t requested = 0u;
  for (uint32_t i = 0u; i < open->channel_count; ++i) {
    const uint64_t channel = open->channels[i].channel;
    if (channel == 0u || (state->used_channels & (1ull << channel)) ||
        (requested & (1ull << channel))) {
      session_reply(session, connection, &session_refuse_message, service_id,
                    VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
      return;
    }
    requested |= 1ull << channel;
    ids[i] = (uint8_t)channel;
  }
  if (session_binding(state, service_id)) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_PROTOCOL_VIOLATION, now);
    return;
  }
  const uint16_t code =
      service->accept ? service->accept(service->context, connection, chosen)
                      : 0u;
  if (code != 0u) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  code, now);
    return;
  }
  SessionBinding *binding = session_binding_alloc(state);
  if (!binding) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_INTERNAL, now);
    return;
  }
  binding->service = service_id;
  binding->registered = (uint8_t)registered;
  binding->state = SESSION_SERVICE_OPEN;
  binding->version = chosen;
  const uint32_t binding_index = (uint32_t)(binding - state->bindings);
  if (!session_open_channels(session, state, binding, binding_index, service,
                             ids)) {
    session_reply(session, connection, &session_refuse_message, service_id,
                  VKR_NET_CLOSE_INTERNAL, now);
    return;
  }
  session_reply(session, connection, &session_accept_message, service_id,
                chosen, now);
  if (service->opened) {
    service->opened(service->context, connection, chosen);
  }
}

static void session_on_reply(VkrNetSession *session,
                             VkrNetConnectionId connection, uint32_t id,
                             const SessionReplyWire *reply) {
  SessionConnection *state = session_find(session, connection);
  SessionBinding *binding =
      state ? session_binding(state, (uint16_t)reply->service) : NULL;
  if (!binding) {
    return;
  }
  const VkrNetService *service = &session->services[binding->registered];
  if (id == SESSION_ACCEPT) {
    if (binding->state != SESSION_SERVICE_OPENING) {
      return;
    }
    /* The acceptor must choose one of the versions offered. */
    bool8_t offered = false_v;
    for (uint32_t i = 0u; i < service->version_count; ++i) {
      offered |= service->versions[i].version == reply->value;
    }
    if (!offered) {
      session_binding_release(session, state, binding);
      if (service->closed) {
        service->closed(service->context, connection,
                        VKR_NET_CLOSE_SCHEMA_MISMATCH);
      }
      return;
    }
    binding->state = SESSION_SERVICE_OPEN;
    binding->version = (uint16_t)reply->value;
    if (service->opened) {
      service->opened(service->context, connection, binding->version);
    }
    return;
  }
  /* REFUSE or CLOSE. */
  const uint16_t code = (uint16_t)reply->value;
  session_binding_release(session, state, binding);
  if (service->closed) {
    service->closed(service->context, connection, code);
  }
}

bool8_t vkr_net_session_dispatch(VkrNetSession *session,
                                 const VkrNetEvent *event, uint64_t now_us) {
  if (!session || !event) {
    return false_v;
  }
  if (event->type == VKR_NET_EVENT_CLOSED) {
    SessionConnection *state = session_find(session, event->connection);
    if (state) {
      for (uint32_t i = 0u; i < VKR_NET_SESSION_SERVICE_MAX; ++i) {
        SessionBinding *binding = &state->bindings[i];
        if (binding->state == SESSION_SERVICE_NONE) {
          continue;
        }
        const VkrNetService *service = &session->services[binding->registered];
        MemZero(binding, sizeof(*binding));
        if (service->closed) {
          service->closed(service->context, event->connection, event->code);
        }
      }
      MemZero(state, sizeof(*state));
    }
    return false_v;
  }
  if (event->type == VKR_NET_EVENT_CONNECTED) {
    (void)session_get(session, event->connection);
    return false_v;
  }
  if (event->type != VKR_NET_EVENT_MESSAGE &&
      event->type != VKR_NET_EVENT_ACKED && event->type != VKR_NET_EVENT_LOST) {
    return false_v;
  }

  if (event->channel == 0u) {
    if (event->type != VKR_NET_EVENT_MESSAGE) {
      return true_v;
    }
    union {
      SessionOpenWire open;
      SessionReplyWire reply;
    } message;
    const VkrWireMessage *descriptor = NULL;
    const VkrWireStatus status =
        vkr_wire_decode(&session_schema, event->data, event->size, &message,
                        sizeof(message), &descriptor);
    if (status != VKR_WIRE_OK) {
      vkr_net_core_close(session->core, event->connection,
                         VKR_NET_CLOSE_SCHEMA_VIOLATION, now_us);
      return true_v;
    }
    if (descriptor->id == SESSION_OPEN) {
      session_on_open(session, event->connection, &message.open, now_us);
    } else {
      session_on_reply(session, event->connection, descriptor->id,
                       &message.reply);
    }
    return true_v;
  }

  SessionConnection *state = session_find(session, event->connection);
  if (!state || event->channel >= VKR_NET_CHANNEL_MAX ||
      state->channel_binding[event->channel] == 0u) {
    return false_v;
  }
  const SessionBinding *binding =
      &state->bindings[state->channel_binding[event->channel] - 1u];
  const VkrNetService *service = &session->services[binding->registered];
  if (service->event) {
    service->event(service->context, event->connection,
                   state->channel_index[event->channel], event, now_us);
  }
  return true_v;
}

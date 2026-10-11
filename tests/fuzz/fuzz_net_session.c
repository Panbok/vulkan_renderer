/* Session fuzzer: service negotiation over a real connection (ADR-105,
 * "Security").
 *
 * A server and a client core finish a real in-process handshake; each side
 * runs a VkrNetSession with the same three services. Records then inject
 * fuzzed channel-0 messages and service-channel events into either
 * session, drive real opens, closes and sends through the cores, and
 * advance time. Real replies travel between the cores, so a fuzzed
 * SERVICE_OPEN is answered and its answer dispatched on the other side.
 *
 * Oracles: a service a session reports open has a version it registered,
 * a send on its first channel finds the core channel, callbacks see only
 * registered versions and channels, and every byte returns at the end. */

#include "fuzz_net_pair.h"
#include "vkr_net_session.h"

#define FUZZ_SESSION_RECORDS_MAX 48u
#define FUZZ_SESSION_SERVICES 3u
#define FUZZ_SESSION_CAPTURE_MAX 8u

static const uint16_t fuzz_session_ids[FUZZ_SESSION_SERVICES] = {
    VKR_NET_SERVICE_DEPOT, VKR_NET_SERVICE_STREAM, VKR_NET_SERVICE_COLLAB};

typedef struct FuzzCaptured {
  uint8_t bytes[256];
  uint32_t size;
} FuzzCaptured;

typedef struct FuzzSession {
  FuzzNetPair pair;
  VkrNetSession *server;
  VkrNetSession *client;
  VkrNetService services[FUZZ_SESSION_SERVICES];
  uint16_t refuse_code;
  /* CLOSED reached the session: the core sends no more events for the
     connection, so the fuzzer injects none either. */
  bool8_t server_closed;
  bool8_t client_closed;
  /* Seed mode: channel-0 messages each side received. */
  bool8_t capture;
  FuzzCaptured server_messages[FUZZ_SESSION_CAPTURE_MAX];
  uint32_t server_message_count;
  FuzzCaptured client_messages[FUZZ_SESSION_CAPTURE_MAX];
  uint32_t client_message_count;
} FuzzSession;

static FuzzSession fuzz_session;

// =============================================================================
// Services
// =============================================================================

static const VkrNetService *fuzz_session_service(void *context) {
  return (const VkrNetService *)context;
}

static bool8_t fuzz_session_has_version(const VkrNetService *service,
                                        uint16_t version) {
  for (uint32_t i = 0u; i < service->version_count; ++i) {
    if (service->versions[i].version == version) {
      return true_v;
    }
  }
  return false_v;
}

static uint16_t fuzz_session_accept(void *context,
                                    VkrNetConnectionId connection,
                                    uint16_t version) {
  (void)connection;
  FUZZ_CHECK(fuzz_session_has_version(fuzz_session_service(context), version));
  return fuzz_session.refuse_code;
}

static void fuzz_session_opened(void *context, VkrNetConnectionId connection,
                                uint16_t version) {
  (void)connection;
  FUZZ_CHECK(fuzz_session_has_version(fuzz_session_service(context), version));
}

static void fuzz_session_service_event(void *context,
                                       VkrNetConnectionId connection,
                                       uint8_t channel,
                                       const VkrNetEvent *event,
                                       uint64_t now_us) {
  (void)connection;
  (void)now_us;
  FUZZ_CHECK(channel < fuzz_session_service(context)->channel_count);
  (void)fuzz_net_touch(event);
}

static void fuzz_session_closed(void *context, VkrNetConnectionId connection,
                                uint16_t code) {
  (void)context;
  (void)connection;
  (void)code;
}

static void fuzz_session_define(FuzzSession *fuzz) {
  VkrNetService *depot = &fuzz->services[0];
  *depot = (VkrNetService){
      .id = fuzz_session_ids[0],
      .version_count = 2u,
      .channel_count = 2u,
      .accept = fuzz_session_accept,
      .opened = fuzz_session_opened,
      .event = fuzz_session_service_event,
      .closed = fuzz_session_closed,
  };
  depot->versions[0] = (VkrNetServiceVersion){1u, 0x1111222233334444ull};
  depot->versions[1] = (VkrNetServiceVersion){2u, 0x5555666677778888ull};
  depot->channels[0] =
      (VkrNetChannelConfig){.delivery = VKR_NET_RELIABLE_ORDERED, .weight = 1u};
  depot->channels[1] =
      (VkrNetChannelConfig){.delivery = VKR_NET_RELIABLE_UNORDERED,
                            .priority = 4u,
                            .weight = 2u,
                            .max_message_size = 1u << 20,
                            .receive_window = 1u << 22};

  VkrNetService *stream = &fuzz->services[1];
  *stream = (VkrNetService){
      .id = fuzz_session_ids[1],
      .version_count = 1u,
      .channel_count = 1u,
      .accept = fuzz_session_accept,
      .event = fuzz_session_service_event,
  };
  stream->versions[0] = (VkrNetServiceVersion){3u, 0x0123456789abcdefull};
  stream->channels[0] = (VkrNetChannelConfig){
      .delivery = VKR_NET_UNRELIABLE, .weight = 1u, .max_message_size = 1000u};

  /* No channels and no callbacks. */
  VkrNetService *collab = &fuzz->services[2];
  *collab = (VkrNetService){.id = fuzz_session_ids[2], .version_count = 1u};
  collab->versions[0] = (VkrNetServiceVersion){1u, 42u};

  for (uint32_t i = 0u; i < FUZZ_SESSION_SERVICES; ++i) {
    fuzz->services[i].context = &fuzz->services[i];
  }
}

// =============================================================================
// Events
// =============================================================================

static void fuzz_session_capture(FuzzCaptured *list, uint32_t *count,
                                 const VkrNetEvent *event) {
  if (*count >= FUZZ_SESSION_CAPTURE_MAX || event->size > 256u) {
    return;
  }
  FuzzCaptured *entry = &list[(*count)++];
  MemCopy(entry->bytes, event->data, event->size);
  entry->size = event->size;
}

static void fuzz_session_event(void *context, bool8_t server_side,
                               const VkrNetEvent *event) {
  FuzzSession *fuzz = context;
  if (event->type == VKR_NET_EVENT_CLOSED) {
    if (server_side && event->connection == fuzz->pair.server_connection) {
      fuzz->server_closed = true_v;
    }
    if (!server_side && event->connection == fuzz->pair.client_connection) {
      fuzz->client_closed = true_v;
    }
  }
  if (fuzz->capture && event->type == VKR_NET_EVENT_MESSAGE &&
      event->channel == 0u) {
    if (server_side) {
      fuzz_session_capture(fuzz->server_messages, &fuzz->server_message_count,
                           event);
    } else {
      fuzz_session_capture(fuzz->client_messages, &fuzz->client_message_count,
                           event);
    }
  }
  VkrNetSession *session = server_side ? fuzz->server : fuzz->client;
  if (session) {
    (void)vkr_net_session_dispatch(session, event, fuzz->pair.now);
  }
}

static void fuzz_session_check(FuzzSession *fuzz, bool8_t server_side) {
  VkrNetSession *session = server_side ? fuzz->server : fuzz->client;
  const VkrNetConnectionId connection =
      server_side ? fuzz->pair.server_connection : fuzz->pair.client_connection;
  for (uint32_t i = 0u; i < FUZZ_SESSION_SERVICES; ++i) {
    const VkrNetService *service = &fuzz->services[i];
    const uint16_t version =
        vkr_net_session_version(session, connection, service->id);
    FUZZ_CHECK(vkr_net_session_is_open(session, connection, service->id) ==
               (version != 0u));
    if (version == 0u) {
      continue;
    }
    FUZZ_CHECK(fuzz_session_has_version(service, version));
    if (service->channel_count > 0u) {
      /* The session opened this core channel when the service opened. */
      const uint8_t byte = 0x5au;
      FUZZ_CHECK(vkr_net_session_send(session, connection, service->id, 0u,
                                      &byte, 1u, NULL,
                                      fuzz->pair.now) != VKR_NET_SEND_INVALID);
    }
  }
}

static void fuzz_session_pump(FuzzSession *fuzz, uint64_t advance) {
  fuzz->pair.now += advance;
  fuzz_net_pump(&fuzz->pair, fuzz->pair.now + 100000u);
  fuzz_session_check(fuzz, true_v);
  fuzz_session_check(fuzz, false_v);
}

/* Dispatches a synthesized event whose data is an exact-size copy, so a
   read past it reaches ASan. */
static void fuzz_session_inject(FuzzSession *fuzz, bool8_t server_side,
                                VkrNetConnectionId connection, uint8_t type,
                                uint8_t channel, const uint8_t *body,
                                uint32_t size) {
  uint8_t *data = malloc(size ? size : 1u);
  FUZZ_CHECK(data);
  if (size > 0u) {
    MemCopy(data, body, size);
  }
  const VkrNetEvent event = {
      .type = type,
      .channel = channel,
      .connection = connection,
      .data = size ? data : NULL,
      .size = size,
      .tag = size,
  };
  VkrNetSession *session = server_side ? fuzz->server : fuzz->client;
  const bool8_t closed =
      server_side ? fuzz->server_closed : fuzz->client_closed;
  const VkrNetConnectionId own =
      server_side ? fuzz->pair.server_connection : fuzz->pair.client_connection;
  if (!(closed && connection == own)) {
    (void)vkr_net_session_dispatch(session, &event, fuzz->pair.now);
  }
  free(data);
}

// =============================================================================
// Records
// =============================================================================

static uint16_t fuzz_session_pick(FuzzInput *input) {
  const uint8_t pick = fuzz_u8(input);
  if (pick < 0xf0u) {
    return fuzz_session_ids[pick % FUZZ_SESSION_SERVICES];
  }
  return fuzz_u16(input);
}

static void fuzz_session_record(FuzzSession *fuzz, uint8_t op,
                                const uint8_t *body, uint32_t size) {
  const bool8_t server_side = (op & 0x80u) != 0u;
  VkrNetSession *session = server_side ? fuzz->server : fuzz->client;
  const VkrNetConnectionId connection =
      server_side ? fuzz->pair.server_connection : fuzz->pair.client_connection;
  FuzzInput input = fuzz_input(body, size);
  /* Bit 7 picks the server side, bit 6 another connection ID. */
  switch ((op & 0x3fu) % 7u) {
  case 0u: {
    /* A channel-0 message from the peer. */
    VkrNetConnectionId target = connection;
    if (op & 0x40u) {
      target = fuzz_u32(&input);
    }
    uint32_t message_size = 0u;
    const uint8_t *message =
        fuzz_bytes(&input, fuzz_remaining(&input), &message_size);
    fuzz_session_inject(fuzz, server_side, target, VKR_NET_EVENT_MESSAGE, 0u,
                        message, message_size);
    break;
  }
  case 1u: {
    /* A service-channel event. */
    const uint8_t channel = (uint8_t)(fuzz_u8(&input) % VKR_NET_CHANNEL_MAX);
    static const uint8_t types[4] = {VKR_NET_EVENT_MESSAGE, VKR_NET_EVENT_ACKED,
                                     VKR_NET_EVENT_LOST, VKR_NET_EVENT_NONE};
    const uint8_t type = types[fuzz_u8(&input) & 0x03u];
    uint32_t data_size = 0u;
    const uint8_t *data =
        fuzz_bytes(&input, fuzz_remaining(&input), &data_size);
    fuzz_session_inject(fuzz, server_side, connection, type, channel, data,
                        data_size);
    break;
  }
  case 2u:
    (void)vkr_net_session_open(session, connection, fuzz_session_pick(&input),
                               fuzz->pair.now);
    break;
  case 3u: {
    const uint16_t service = fuzz_session_pick(&input);
    vkr_net_session_close(session, connection, service, fuzz_u16(&input),
                          fuzz->pair.now);
    break;
  }
  case 4u: {
    const uint16_t service = fuzz_session_pick(&input);
    const uint8_t channel = fuzz_u8(&input);
    uint32_t data_size = 0u;
    const uint8_t *data =
        fuzz_bytes(&input, fuzz_remaining(&input), &data_size);
    (void)vkr_net_session_send(session, connection, service, channel, data,
                               data_size, NULL, fuzz->pair.now);
    (void)vkr_net_session_queued_bytes(session, connection, service, channel);
    break;
  }
  case 5u:
    fuzz->refuse_code = fuzz_u16(&input);
    break;
  default: {
    const uint16_t ticks = fuzz_u16(&input);
    if (ticks == 0xffffu) {
      vkr_net_core_close(server_side ? fuzz->pair.server : fuzz->pair.client,
                         connection, fuzz_u16(&input), fuzz->pair.now);
    }
    fuzz_session_pump(fuzz, (uint64_t)ticks * 100u);
    return;
  }
  }
  fuzz_session_pump(fuzz, 0u);
}

// =============================================================================
// Entry
// =============================================================================

static void fuzz_session_setup(FuzzSession *fuzz, bool8_t capture) {
  MemZero(fuzz, sizeof(*fuzz));
  fuzz->capture = capture;
  fuzz_session_define(fuzz);
  FUZZ_CHECK(fuzz_net_pair_create(&fuzz->pair, NULL, fuzz_session_event, fuzz));
  fuzz->server =
      vkr_net_session_create(&fuzz->pair.allocator, fuzz->pair.server);
  fuzz->client =
      vkr_net_session_create(&fuzz->pair.allocator, fuzz->pair.client);
  FUZZ_CHECK(fuzz->server && fuzz->client);
  for (uint32_t i = 0u; i < FUZZ_SESSION_SERVICES; ++i) {
    FUZZ_CHECK(vkr_net_session_register(fuzz->server, &fuzz->services[i]));
    FUZZ_CHECK(vkr_net_session_register(fuzz->client, &fuzz->services[i]));
  }
  static const uint8_t credential[] = "session";
  fuzz_net_pair_connect(&fuzz->pair, credential, sizeof(credential) - 1u);
}

static void fuzz_session_teardown(FuzzSession *fuzz) {
  vkr_net_session_destroy(fuzz->client);
  vkr_net_session_destroy(fuzz->server);
  fuzz_net_pair_destroy(&fuzz->pair);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput input = fuzz_input(data, size);
  FuzzSession *fuzz = &fuzz_session;
  fuzz_session_setup(fuzz, false_v);
  for (uint32_t record = 0u;
       record < FUZZ_SESSION_RECORDS_MAX && fuzz_remaining(&input) >= 3u;
       ++record) {
    const uint8_t op = fuzz_u8(&input);
    const uint16_t length = fuzz_u16(&input);
    uint32_t body_size = 0u;
    const uint8_t *body = fuzz_bytes(&input, length, &body_size);
    fuzz_session_record(fuzz, op, body, body_size);
  }
  fuzz_session_teardown(fuzz);
  return 0;
}

// =============================================================================
// Seeds
// =============================================================================

/* Real channel-0 messages: the client opens every service and the server
   answers; both sides' received messages become injected records. */
static void fuzz_session_seeds(FuzzSeedWriter *writer) {
  FuzzSession *fuzz = &fuzz_session;
  fuzz_session_setup(fuzz, true_v);
  for (uint32_t i = 0u; i < FUZZ_SESSION_SERVICES; ++i) {
    FUZZ_CHECK(vkr_net_session_open(fuzz->client, fuzz->pair.client_connection,
                                    fuzz->services[i].id, fuzz->pair.now));
  }
  fuzz_session_pump(fuzz, 0u);
  vkr_net_session_close(fuzz->client, fuzz->pair.client_connection,
                        fuzz->services[1].id, 0x100u, fuzz->pair.now);
  fuzz->refuse_code = VKR_NET_CLOSE_SERVICE_REFUSED;
  FUZZ_CHECK(vkr_net_session_open(fuzz->client, fuzz->pair.client_connection,
                                  fuzz->services[1].id, fuzz->pair.now));
  fuzz_session_pump(fuzz, 0u);
  FUZZ_CHECK(fuzz->server_message_count >= FUZZ_SESSION_SERVICES);
  FUZZ_CHECK(fuzz->client_message_count >= FUZZ_SESSION_SERVICES);

  /* Channel 1, a MESSAGE event, two bytes of data. */
  static const uint8_t service_event[] = {1u, 0u, 'a', 'b'};
  for (uint32_t i = 0u; i < fuzz->server_message_count; ++i) {
    FuzzSeed seed = {0};
    const FuzzCaptured *message = &fuzz->server_messages[i];
    /* Into the server, then again: a duplicate open. */
    fuzz_seed_record(&seed, 0x80u, message->bytes, message->size);
    fuzz_seed_record(&seed, 0x80u, message->bytes, message->size);
    fuzz_seed_record(&seed, 0x81u, service_event, sizeof(service_event));
    fuzz_seed_emit(writer, &seed);
  }
  for (uint32_t i = 0u; i < fuzz->client_message_count; ++i) {
    /* A reply into a client that opened a service itself first. */
    FuzzSeed seed = {0};
    const uint8_t pick = (uint8_t)(i % FUZZ_SESSION_SERVICES);
    const FuzzCaptured *message = &fuzz->client_messages[i];
    fuzz_seed_record(&seed, 2u, &pick, 1u);
    fuzz_seed_record(&seed, 0u, message->bytes, message->size);
    fuzz_seed_emit(writer, &seed);
  }
  fuzz_session_teardown(fuzz);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  FUZZ_CHECK(vkr_net_crypto_init());
  fuzz_seed_mode(fuzz_session_seeds);
  return 0;
}

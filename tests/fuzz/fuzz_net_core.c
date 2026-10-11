/* Transport fuzzer: datagrams into a VkrNetCore (ADR-105, "Security").
 *
 * The core is compiled into this translation unit so the fuzzer can reach
 * the connection's packet keys: arbitrary datagrams stop at AEAD, so the
 * frame parser behind it is fuzzed by sealing fuzzed plaintext with the
 * real key of an established connection and feeding the result through
 * vkr_net_core_receive. Everything else goes through the public API. The
 * input's first byte picks one of three worlds:
 *
 *   SERVER     datagrams, forged Initials (real Noise message 1 with a
 *              fuzzed credential and token) and time into a server.
 *   ESTABLISHED  a real in-process handshake, then sealed fuzzed frames,
 *              raw datagrams, server sends, channel changes and time.
 *   CLIENT     a connecting client fed datagrams, forged Handshake replies
 *              with a fuzzed accept payload, Retry packets, then sealed
 *              fuzzed frames under the keys the forged reply set up.
 *
 * Each record is an opcode byte, a 16-bit body length and the body. */

#include "vkr_net_core.c"

#include "fuzz_net_pair.h"

#define FUZZ_CORE_RECORDS_MAX 64u
#define FUZZ_CORE_PACKET_MAX 9000u
#define FUZZ_CORE_ADDRESSES 4u
#define FUZZ_CORE_CONNECTIONS 8u
#define FUZZ_CORE_CHANNELS 6u

enum {
  FUZZ_WORLD_SERVER = 0,
  FUZZ_WORLD_ESTABLISHED = 1,
  FUZZ_WORLD_CLIENT = 2,
  FUZZ_WORLD_COUNT = 3,
};

/* Channel 0 is the session channel; 1 to 5 cover every delivery class. */
static const VkrNetChannelConfig fuzz_core_channels[FUZZ_CORE_CHANNELS] = {
    {0},
    {.delivery = VKR_NET_UNRELIABLE, .weight = 1u, .max_message_size = 1024u},
    {.delivery = VKR_NET_SEQUENCED, .weight = 1u, .max_message_size = 1024u},
    {.delivery = VKR_NET_RELIABLE_ORDERED,
     .weight = 2u,
     .max_message_size = 8192u,
     .receive_window = 16384u},
    {.delivery = VKR_NET_RELIABLE_UNORDERED,
     .priority = 3u,
     .weight = 1u,
     .max_message_size = 8192u},
    {.delivery = VKR_NET_DEADLINE,
     .priority = 1u,
     .weight = 1u,
     .max_message_size = 8192u,
     .deadline_ms = 50u},
};

typedef struct FuzzCore {
  FuzzNetPair pair;
  VkrNetAddress addresses[FUZZ_CORE_ADDRESSES];
  uint8_t world;
  /* SERVER: connections the server reported and the last retry cookie. */
  VkrNetConnectionId connections[FUZZ_CORE_CONNECTIONS];
  uint32_t connection_count;
  uint8_t cookie[VKR_NET_COOKIE_SIZE];
  bool8_t has_cookie;
  /* ESTABLISHED: the next packet number the injected sender uses and the
     largest message each server channel accepts now. */
  uint64_t next_pn;
  uint32_t channel_max[VKR_NET_CHANNEL_MAX];
  /* CLIENT: the client's last Initial and the keys a forged reply set. */
  uint8_t initial[FUZZ_NET_DATAGRAM];
  uint32_t initial_size;
  VkrNetPacketKey forged_tx;
  bool8_t has_forged_keys;
  uint32_t forged_dcid;
  uint64_t forged_pn;
} FuzzCore;

static VkrNetKeyPair fuzz_core_extra_client;

// =============================================================================
// Shared steps
// =============================================================================

static void fuzz_core_event(void *context, bool8_t server_side,
                            const VkrNetEvent *event) {
  FuzzCore *fuzz = context;
  if (event->type == VKR_NET_EVENT_MESSAGE) {
    FUZZ_CHECK(event->channel < VKR_NET_CHANNEL_MAX);
    /* Reassembly never delivers more than the channel's largest message. */
    if (server_side && fuzz->channel_max[event->channel] > 0u) {
      FUZZ_CHECK(event->size <= fuzz->channel_max[event->channel]);
    }
  }
  if (server_side && event->type == VKR_NET_EVENT_CONNECTED &&
      fuzz->connection_count < FUZZ_CORE_CONNECTIONS) {
    fuzz->connections[fuzz->connection_count++] = event->connection;
  }
}

static void fuzz_core_capture(FuzzCore *fuzz, const uint8_t *datagram,
                              uint32_t size) {
  if (size < NET_LONG_HEADER_SIZE || !(datagram[0] & NET_FLAG_LONG)) {
    return;
  }
  const uint32_t type = (datagram[0] >> NET_LONG_TYPE_SHIFT) & 0x03u;
  if (type == NET_LONG_RETRY &&
      size == NET_LONG_HEADER_SIZE + VKR_NET_COOKIE_SIZE) {
    MemCopy(fuzz->cookie, datagram + NET_LONG_HEADER_SIZE, VKR_NET_COOKIE_SIZE);
    fuzz->has_cookie = true_v;
  }
  if (type == NET_LONG_INITIAL && size <= sizeof(fuzz->initial)) {
    MemCopy(fuzz->initial, datagram, size);
    fuzz->initial_size = size;
  }
}

/* Takes everything `core` wants to send; nothing reaches the peer. */
static void fuzz_core_drain(FuzzCore *fuzz, VkrNetCore *core) {
  uint8_t datagram[VKR_NET_DATAGRAM_JUMBO];
  for (uint32_t i = 0u; i < 256u; ++i) {
    VkrNetAddress destination = {0};
    const uint32_t size = vkr_net_core_transmit(core, fuzz->pair.now, datagram,
                                                sizeof(datagram), &destination);
    if (size == 0u) {
      return;
    }
    FUZZ_CHECK(size <= sizeof(datagram));
    fuzz_core_capture(fuzz, datagram, size);
  }
}

static void fuzz_core_settle(FuzzCore *fuzz, VkrNetCore *core,
                             bool8_t server_side) {
  fuzz_core_drain(fuzz, core);
  fuzz_net_poll(&fuzz->pair, server_side);
}

static void fuzz_core_receive(FuzzCore *fuzz, VkrNetCore *core,
                              const VkrNetAddress *from, const uint8_t *bytes,
                              uint32_t size) {
  uint8_t packet[FUZZ_CORE_PACKET_MAX];
  const uint32_t length = Min(size, (uint32_t)sizeof(packet));
  if (length > 0u) {
    MemCopy(packet, bytes, length);
  }
  vkr_net_core_receive(core, from, packet, length, fuzz->pair.now);
}

static void fuzz_core_advance(FuzzCore *fuzz, VkrNetCore *core,
                              uint64_t delta) {
  fuzz->pair.now += delta;
  vkr_net_core_update(core, fuzz->pair.now);
}

/* A 1-RTT packet around `payload`, sealed with `key`. */
static uint32_t fuzz_core_seal(const VkrNetPacketKey *key, uint32_t cid,
                               uint64_t pn, uint8_t control,
                               const uint8_t *payload, uint32_t payload_size,
                               uint8_t *out, uint32_t capacity) {
  const uint32_t pn_length = (control & 0x03u) + 1u;
  const uint32_t header_size = 5u + pn_length;
  if ((uint64_t)header_size + payload_size + VKR_NET_TAG_SIZE > capacity) {
    return 0u;
  }
  out[0] = (uint8_t)(NET_FLAG_FIXED | (pn_length - 1u));
  if (control & 0x08u) {
    /* Reserved bits: the receiver must drop the packet. */
    out[0] |= 0x04u;
  }
  vkr_store_le_u32(out + 1, cid);
  for (uint32_t i = 0u; i < pn_length; ++i) {
    out[5u + i] = (uint8_t)(pn >> (8u * i));
  }
  if (payload_size > 0u) {
    MemCopy(out + header_size, payload, payload_size);
  }
  vkr_net_packet_seal(key, pn, out, header_size, out + header_size,
                      payload_size, out + header_size + payload_size);
  return header_size + payload_size + VKR_NET_TAG_SIZE;
}

// =============================================================================
// SERVER: handshakes into a server
// =============================================================================

/* A client Initial carrying a real Noise message 1. Body: client
   connection ID u32, token size u8 (0xff: the last retry cookie), the
   token, then the credential. */
static void fuzz_core_forge_initial(FuzzCore *fuzz, uint8_t op,
                                    const uint8_t *body, uint32_t size) {
  FuzzInput input = fuzz_input(body, size);
  const uint32_t client_cid = fuzz_u32(&input);
  const uint8_t token_control = fuzz_u8(&input);
  uint8_t token[64];
  uint32_t token_size = 0u;
  if (token_control == 0xffu) {
    if (fuzz->has_cookie) {
      MemCopy(token, fuzz->cookie, VKR_NET_COOKIE_SIZE);
      token_size = VKR_NET_COOKIE_SIZE;
    }
  } else {
    const uint8_t *view =
        fuzz_bytes(&input, Min((uint32_t)token_control, 64u), &token_size);
    if (token_size > 0u) {
      MemCopy(token, view, token_size);
    }
  }
  uint32_t credential_size = 0u;
  const uint8_t *credential =
      fuzz_bytes(&input, VKR_NET_CREDENTIAL_MAX, &credential_size);

  const VkrNetKeyPair *keys =
      (op & 0x10u) ? &fuzz_core_extra_client : &fuzz->pair.client_keys;
  VkrNetNoise noise;
  vkr_net_noise_init(&noise, true_v, net_prologue, sizeof(net_prologue) - 1u,
                     keys, fuzz->pair.server_keys.public_key);
  uint8_t message[VKR_NET_NOISE_MESSAGE1_OVERHEAD + VKR_NET_CREDENTIAL_MAX];
  const uint32_t message_size = vkr_net_noise_write_message1(
      &noise, NULL, credential, credential_size, message, sizeof(message));
  vkr_net_noise_wipe(&noise);
  FUZZ_CHECK(message_size > 0u);

  uint8_t packet[FUZZ_CORE_PACKET_MAX];
  MemZero(packet, sizeof(packet));
  uint8_t *cursor = packet;
  const uint8_t *end = packet + sizeof(packet);
  cursor +=
      net_write_long_header(cursor, NET_LONG_INITIAL, 0x01020304u, client_cid);
  cursor += vkr_net_varint_write(cursor, end, token_size);
  MemCopy(cursor, token, token_size);
  cursor += token_size;
  cursor += vkr_net_varint_write(cursor, end, message_size);
  MemCopy(cursor, message, message_size);
  cursor += message_size;
  const uint32_t used = (uint32_t)(cursor - packet);
  const uint32_t packet_size = Max(used, VKR_NET_DATAGRAM_INITIAL);
  const VkrNetAddress *from = &fuzz->addresses[(op >> 2) & 0x03u];
  vkr_net_core_receive(fuzz->pair.server, from, packet, packet_size,
                       fuzz->pair.now);
}

static void fuzz_core_server_record(FuzzCore *fuzz, uint8_t op,
                                    const uint8_t *body, uint32_t size) {
  VkrNetCore *server = fuzz->pair.server;
  switch (op % 4u) {
  case 0u:
    fuzz_core_receive(fuzz, server, &fuzz->addresses[(op >> 2) & 0x03u], body,
                      size);
    break;
  case 1u:
    fuzz_core_forge_initial(fuzz, op, body, size);
    break;
  case 2u: {
    FuzzInput input = fuzz_input(body, size);
    fuzz_core_advance(fuzz, server, fuzz_u32(&input) & 0x03ffffffu);
    break;
  }
  default: {
    FuzzInput input = fuzz_input(body, size);
    const uint8_t pick = fuzz_u8(&input);
    const uint16_t code = fuzz_u16(&input);
    if (fuzz->connection_count > 0u) {
      vkr_net_core_close(server,
                         fuzz->connections[pick % fuzz->connection_count], code,
                         fuzz->pair.now);
    }
    break;
  }
  }
  fuzz_core_settle(fuzz, server, true_v);
}

// =============================================================================
// ESTABLISHED: frames into an established server connection
// =============================================================================

static void fuzz_core_open_channels(FuzzCore *fuzz) {
  fuzz->channel_max[0] = 64u * 1024u;
  for (uint8_t channel = 1u; channel < FUZZ_CORE_CHANNELS; ++channel) {
    fuzz->channel_max[channel] = fuzz_core_channels[channel].max_message_size;
    FUZZ_CHECK(vkr_net_core_open_channel(fuzz->pair.server,
                                         fuzz->pair.server_connection, channel,
                                         &fuzz_core_channels[channel]));
    FUZZ_CHECK(vkr_net_core_open_channel(fuzz->pair.client,
                                         fuzz->pair.client_connection, channel,
                                         &fuzz_core_channels[channel]));
  }
}

/* Body: packet-number control, header control, then the frames. The
   client's key seals them; the client core itself stays silent, so the
   fuzzer owns the packet number space from here. */
static void fuzz_core_inject(FuzzCore *fuzz, const uint8_t *body,
                             uint32_t size) {
  NetConnection *client =
      net_connection(fuzz->pair.client, fuzz->pair.client_connection);
  FUZZ_CHECK(client && client->has_keys);
  FuzzInput input = fuzz_input(body, size);
  const uint8_t pn_control = fuzz_u8(&input);
  const uint8_t header_control = fuzz_u8(&input);
  uint32_t frames_size = 0u;
  const uint8_t *frames =
      fuzz_bytes(&input, fuzz_remaining(&input), &frames_size);

  uint64_t pn = fuzz->next_pn;
  if (pn_control & 0x80u) {
    /* An old number: a replay or a reordered packet. */
    const uint64_t back = (uint64_t)(pn_control & 0x7fu) + 1u;
    pn = pn > back ? pn - back : 0u;
  } else if (pn_control & 0x40u) {
    pn += (uint64_t)(pn_control & 0x3fu) << 10;
  } else {
    pn += pn_control & 0x3fu;
  }
  fuzz->next_pn = Max(fuzz->next_pn, pn + 1u);

  uint8_t packet[FUZZ_CORE_PACKET_MAX];
  const uint32_t packet_size =
      fuzz_core_seal(&client->tx, client->remote_cid, pn, header_control,
                     frames, frames_size, packet, sizeof(packet));
  if (packet_size == 0u) {
    return;
  }
  const VkrNetAddress *from = &fuzz->addresses[(header_control >> 4) & 0x03u];
  vkr_net_core_receive(fuzz->pair.server, from, packet, packet_size,
                       fuzz->pair.now);
}

static void fuzz_core_server_send(FuzzCore *fuzz, const uint8_t *body,
                                  uint32_t size) {
  FuzzInput input = fuzz_input(body, size);
  const uint8_t channel = (uint8_t)(fuzz_u8(&input) % FUZZ_CORE_CHANNELS);
  const uint8_t flags = fuzz_u8(&input);
  const uint32_t length = fuzz_u16(&input) & 0x3fffu;
  static uint8_t data[0x4000u];
  for (uint32_t i = 0u; i < length; ++i) {
    data[i] = (uint8_t)(i * 13u + channel);
  }
  const VkrNetSendOptions options = {
      .flags = flags & (VKR_NET_SEND_IMMEDIATE | VKR_NET_SEND_NOTIFY |
                        VKR_NET_SEND_TAG),
      .tag = flags,
      .deadline_us = (flags & 0x80u) ? fuzz->pair.now + 1000u : 0u,
  };
  (void)vkr_net_core_send(fuzz->pair.server, fuzz->pair.server_connection,
                          channel, data, length, &options, fuzz->pair.now);
  vkr_net_core_flush(fuzz->pair.server);
}

static void fuzz_core_channel_change(FuzzCore *fuzz, const uint8_t *body,
                                     uint32_t size) {
  FuzzInput input = fuzz_input(body, size);
  const uint8_t channel = (uint8_t)(fuzz_u8(&input) % VKR_NET_CHANNEL_MAX);
  const uint8_t action = fuzz_u8(&input);
  if (action & 0x01u) {
    vkr_net_core_close_channel(fuzz->pair.server, fuzz->pair.server_connection,
                               channel);
    return;
  }
  const uint32_t config =
      1u + (uint32_t)(action >> 1) % (FUZZ_CORE_CHANNELS - 1u);
  if (channel > 0u &&
      vkr_net_core_open_channel(fuzz->pair.server, fuzz->pair.server_connection,
                                channel, &fuzz_core_channels[config])) {
    fuzz->channel_max[channel] = fuzz_core_channels[config].max_message_size;
  }
}

static void fuzz_core_established_record(FuzzCore *fuzz, uint8_t op,
                                         const uint8_t *body, uint32_t size) {
  VkrNetCore *server = fuzz->pair.server;
  switch (op % 6u) {
  case 0u:
    fuzz_core_inject(fuzz, body, size);
    break;
  case 1u:
    fuzz_core_receive(fuzz, server, &fuzz->addresses[(op >> 3) & 0x03u], body,
                      size);
    break;
  case 2u:
    fuzz_core_server_send(fuzz, body, size);
    break;
  case 3u: {
    FuzzInput input = fuzz_input(body, size);
    fuzz_core_advance(fuzz, server, (uint64_t)fuzz_u16(&input) * 32u);
    break;
  }
  case 4u:
    fuzz_core_channel_change(fuzz, body, size);
    break;
  default: {
    FuzzInput input = fuzz_input(body, size);
    vkr_net_core_close(server, fuzz->pair.server_connection, fuzz_u16(&input),
                       fuzz->pair.now);
    break;
  }
  }
  fuzz_core_settle(fuzz, server, true_v);
}

// =============================================================================
// CLIENT: replies into a connecting client
// =============================================================================

/* A server Handshake answering the client's last Initial with a real Noise
   message 2. Body: server connection ID u32, then the accept payload
   (status, code u16, application payload). */
static void fuzz_core_forge_handshake(FuzzCore *fuzz, const uint8_t *body,
                                      uint32_t size) {
  if (fuzz->initial_size < VKR_NET_DATAGRAM_INITIAL) {
    return;
  }
  const uint8_t *cursor = fuzz->initial + NET_LONG_HEADER_SIZE;
  const uint8_t *end = fuzz->initial + fuzz->initial_size;
  const uint32_t client_cid = vkr_load_le_u32(fuzz->initial + 9);
  uint64_t token_size = 0u;
  uint32_t used = vkr_net_varint_read(cursor, end, &token_size);
  FUZZ_CHECK(used > 0u && token_size <= (uint64_t)(end - cursor - used));
  cursor += used + token_size;
  uint64_t message_size = 0u;
  used = vkr_net_varint_read(cursor, end, &message_size);
  FUZZ_CHECK(used > 0u && message_size <= (uint64_t)(end - cursor - used));
  cursor += used;

  VkrNetNoise noise;
  vkr_net_noise_init(&noise, false_v, net_prologue, sizeof(net_prologue) - 1u,
                     &fuzz->pair.server_keys, NULL);
  uint8_t credential[VKR_NET_CREDENTIAL_MAX];
  uint32_t credential_size = 0u;
  FUZZ_CHECK(vkr_net_noise_read_message1(&noise, cursor, (uint32_t)message_size,
                                         credential, sizeof(credential),
                                         &credential_size));
  FuzzInput input = fuzz_input(body, size);
  const uint32_t server_cid = fuzz_u32(&input);
  uint32_t payload_size = 0u;
  const uint8_t *payload = fuzz_bytes(&input, 1024u, &payload_size);
  uint8_t message[VKR_NET_NOISE_MESSAGE2_OVERHEAD + 1024u];
  const uint32_t written = vkr_net_noise_write_message2(
      &noise, NULL, payload, payload_size, message, sizeof(message));
  FUZZ_CHECK(written > 0u);

  uint8_t initiator_key[VKR_NET_KEY_SIZE];
  uint8_t responder_key[VKR_NET_KEY_SIZE];
  vkr_net_noise_split(&noise, initiator_key, responder_key);
  vkr_net_noise_wipe(&noise);
  vkr_net_packet_key_init(&fuzz->forged_tx, responder_key);
  fuzz->has_forged_keys = true_v;
  fuzz->forged_dcid = client_cid;
  fuzz->forged_pn = 0u;

  uint8_t packet[FUZZ_CORE_PACKET_MAX];
  uint8_t *out = packet;
  const uint8_t *out_end = packet + sizeof(packet);
  out += net_write_long_header(out, NET_LONG_HANDSHAKE, client_cid, server_cid);
  out += vkr_net_varint_write(out, out_end, written);
  MemCopy(out, message, written);
  out += written;
  vkr_net_core_receive(fuzz->pair.client, &fuzz->pair.server_address, packet,
                       (uint32_t)(out - packet), fuzz->pair.now);
}

static void fuzz_core_forge_retry(FuzzCore *fuzz, const uint8_t *body,
                                  uint32_t size) {
  if (fuzz->initial_size < NET_LONG_HEADER_SIZE) {
    return;
  }
  uint8_t packet[NET_LONG_HEADER_SIZE + VKR_NET_COOKIE_SIZE];
  MemZero(packet, sizeof(packet));
  (void)net_write_long_header(packet, NET_LONG_RETRY,
                              vkr_load_le_u32(fuzz->initial + 9), 0u);
  MemCopy(packet + NET_LONG_HEADER_SIZE, body,
          Min(size, (uint32_t)VKR_NET_COOKIE_SIZE));
  vkr_net_core_receive(fuzz->pair.client, &fuzz->pair.server_address, packet,
                       sizeof(packet), fuzz->pair.now);
}

/* Frames sealed with the forged server's keys. Body: header control, then
   the frames. */
static void fuzz_core_inject_client(FuzzCore *fuzz, const uint8_t *body,
                                    uint32_t size) {
  if (!fuzz->has_forged_keys) {
    return;
  }
  FuzzInput input = fuzz_input(body, size);
  const uint8_t header_control = fuzz_u8(&input);
  uint32_t frames_size = 0u;
  const uint8_t *frames =
      fuzz_bytes(&input, fuzz_remaining(&input), &frames_size);
  uint8_t packet[FUZZ_CORE_PACKET_MAX];
  const uint32_t packet_size = fuzz_core_seal(
      &fuzz->forged_tx, fuzz->forged_dcid, fuzz->forged_pn++, header_control,
      frames, frames_size, packet, sizeof(packet));
  if (packet_size > 0u) {
    vkr_net_core_receive(fuzz->pair.client, &fuzz->pair.server_address, packet,
                         packet_size, fuzz->pair.now);
  }
}

static void fuzz_core_client_record(FuzzCore *fuzz, uint8_t op,
                                    const uint8_t *body, uint32_t size) {
  VkrNetCore *client = fuzz->pair.client;
  switch (op % 5u) {
  case 0u: {
    const VkrNetAddress *from =
        (op & 0x08u) ? &fuzz->addresses[1] : &fuzz->pair.server_address;
    fuzz_core_receive(fuzz, client, from, body, size);
    break;
  }
  case 1u:
    fuzz_core_forge_handshake(fuzz, body, size);
    break;
  case 2u:
    fuzz_core_forge_retry(fuzz, body, size);
    break;
  case 3u: {
    FuzzInput input = fuzz_input(body, size);
    fuzz_core_advance(fuzz, client, fuzz_u32(&input) & 0x01ffffffu);
    break;
  }
  default:
    fuzz_core_inject_client(fuzz, body, size);
    break;
  }
  fuzz_core_settle(fuzz, client, false_v);
}

// =============================================================================
// Entry
// =============================================================================

static void fuzz_core_setup(FuzzCore *fuzz, uint8_t world, uint8_t flags) {
  MemZero(fuzz, sizeof(*fuzz));
  fuzz->world = world;
  VkrNetCoreConfig server = {0};
  if (world == FUZZ_WORLD_SERVER) {
    server.require_retry = (flags & 0x01u) != 0u;
    server.max_connections = (flags & 0x02u) ? 2u : 0u;
    server.retry_threshold = (flags & 0x04u) ? 1u : 0u;
  } else {
    server.background = (flags & 0x01u) != 0u;
    server.max_datagram = (flags & 0x02u) ? VKR_NET_DATAGRAM_JUMBO : 0u;
  }
  FUZZ_CHECK(fuzz_net_pair_create(&fuzz->pair, &server, fuzz_core_event, fuzz));
  fuzz->addresses[0] = fuzz->pair.client_address;
  FUZZ_CHECK(vkr_net_address_parse("10.0.0.3:5000", 0u, &fuzz->addresses[1]));
  FUZZ_CHECK(vkr_net_address_parse("10.0.0.2:5001", 0u, &fuzz->addresses[2]));
  FUZZ_CHECK(vkr_net_address_parse("[fd00::2]:5000", 0u, &fuzz->addresses[3]));

  if (world == FUZZ_WORLD_ESTABLISHED) {
    static const uint8_t credential[] = "fuzz";
    fuzz_net_pair_connect(&fuzz->pair, credential, sizeof(credential) - 1u);
    fuzz_core_open_channels(fuzz);
    NetConnection *server_conn =
        net_connection(fuzz->pair.server, fuzz->pair.server_connection);
    NetConnection *client_conn =
        net_connection(fuzz->pair.client, fuzz->pair.client_connection);
    FUZZ_CHECK(server_conn && server_conn->state == NET_STATE_ESTABLISHED);
    FUZZ_CHECK(client_conn && client_conn->state == NET_STATE_ESTABLISHED);
    fuzz->next_pn = client_conn->next_pn;
  } else if (world == FUZZ_WORLD_CLIENT) {
    VkrNetConnectionId pending = VKR_NET_CONNECTION_NONE;
    FUZZ_CHECK(vkr_net_core_connect(
        fuzz->pair.client, &fuzz->pair.server_address,
        fuzz->pair.server_keys.public_key, (const uint8_t *)"fuzz", 4u,
        fuzz->pair.now, &pending));
    fuzz_core_drain(fuzz, fuzz->pair.client);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput input = fuzz_input(data, size);
  if (fuzz_remaining(&input) < 2u) {
    return 0;
  }
  const uint8_t world = (uint8_t)(fuzz_u8(&input) % FUZZ_WORLD_COUNT);
  const uint8_t flags = fuzz_u8(&input);
  static FuzzCore fuzz;
  fuzz_core_setup(&fuzz, world, flags);

  for (uint32_t record = 0u;
       record < FUZZ_CORE_RECORDS_MAX && fuzz_remaining(&input) >= 3u;
       ++record) {
    const uint8_t op = fuzz_u8(&input);
    const uint16_t length = fuzz_u16(&input);
    uint32_t body_size = 0u;
    const uint8_t *body = fuzz_bytes(&input, length, &body_size);
    if (world == FUZZ_WORLD_SERVER) {
      fuzz_core_server_record(&fuzz, op, body, body_size);
    } else if (world == FUZZ_WORLD_ESTABLISHED) {
      fuzz_core_established_record(&fuzz, op, body, body_size);
    } else {
      fuzz_core_client_record(&fuzz, op, body, body_size);
    }
  }
  fuzz_net_pair_destroy(&fuzz.pair);
  return 0;
}

// =============================================================================
// Seeds
// =============================================================================

static uint32_t fuzz_varint(uint8_t *out, uint64_t value) {
  return vkr_net_varint_write(out, out + 8u, value);
}

/* An injected-frames record body: packet-number and header controls, then
   the frames. */
typedef struct FuzzFrames {
  uint8_t bytes[2048];
  uint32_t size;
} FuzzFrames;

static void fuzz_frames_begin(FuzzFrames *frames, uint8_t pn_control,
                              uint8_t header_control) {
  frames->size = 0u;
  frames->bytes[frames->size++] = pn_control;
  frames->bytes[frames->size++] = header_control;
}

static void fuzz_frames_u8(FuzzFrames *frames, uint8_t value) {
  frames->bytes[frames->size++] = value;
}

static void fuzz_frames_varint(FuzzFrames *frames, uint64_t value) {
  frames->size += fuzz_varint(frames->bytes + frames->size, value);
}

static void fuzz_frames_data(FuzzFrames *frames, uint32_t size, uint8_t fill) {
  MemSet(frames->bytes + frames->size, fill, size);
  frames->size += size;
}

/* MESSAGE: flags, channel, then the optional fields in wire order. */
static void fuzz_frames_message(FuzzFrames *frames, uint8_t channel,
                                bool8_t sequenced, uint64_t seq,
                                bool8_t fragmented, uint64_t fragment,
                                uint64_t fragment_size, uint64_t total,
                                uint32_t length) {
  uint8_t flags = MESSAGE_LEN;
  if (sequenced) {
    flags |= MESSAGE_SEQ;
  }
  if (fragmented) {
    flags |= MESSAGE_FRAG;
  }
  fuzz_frames_u8(frames, (uint8_t)(FRAME_MESSAGE | flags));
  fuzz_frames_u8(frames, channel);
  if (sequenced) {
    fuzz_frames_varint(frames, seq);
  }
  if (fragmented) {
    fuzz_frames_varint(frames, fragment);
    fuzz_frames_varint(frames, fragment_size);
    fuzz_frames_varint(frames, total);
  }
  fuzz_frames_varint(frames, length);
  fuzz_frames_data(frames, length, (uint8_t)(channel + seq));
}

static void fuzz_seed_frames(FuzzSeed *seed, const FuzzFrames *frames) {
  fuzz_seed_record(seed, 0u, frames->bytes, frames->size);
}

static void fuzz_core_seed_server(FuzzSeedWriter *writer) {
  /* A real client Initial. */
  FuzzNetPair pair;
  FUZZ_CHECK(fuzz_net_pair_create(&pair, NULL, NULL, NULL));
  VkrNetConnectionId pending = VKR_NET_CONNECTION_NONE;
  FUZZ_CHECK(vkr_net_core_connect(
      pair.client, &pair.server_address, pair.server_keys.public_key,
      (const uint8_t *)"seed", 4u, pair.now, &pending));
  uint8_t initial[FUZZ_NET_DATAGRAM];
  VkrNetAddress destination;
  const uint32_t initial_size = vkr_net_core_transmit(
      pair.client, pair.now, initial, sizeof(initial), &destination);
  FUZZ_CHECK(initial_size >= VKR_NET_DATAGRAM_INITIAL);
  fuzz_net_pair_destroy(&pair);

  FuzzSeed seed = {0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_SERVER);
  fuzz_seed_u8(&seed, 0u);
  fuzz_seed_record(&seed, 0u, initial, initial_size);
  fuzz_seed_record(&seed, 0u, initial, initial_size);
  const uint8_t later[4] = {0x40, 0x42, 0x0f, 0x00};
  fuzz_seed_record(&seed, 2u, later, sizeof(later));
  fuzz_seed_emit(writer, &seed);

  /* Forged Initials: accepted, repeated, refused, from other addresses. */
  static const uint8_t accept[] = {1, 0, 0, 0, 0, 'h', 'e', 'l', 'l', 'o'};
  static const uint8_t refuse[] = {2, 0, 0, 0, 0, 0xff, 5};
  static const uint8_t retry[] = {3, 0, 0, 0, 0xff, 'c'};
  seed = (FuzzSeed){0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_SERVER);
  fuzz_seed_u8(&seed, 0u);
  fuzz_seed_record(&seed, 1u, accept, sizeof(accept));
  fuzz_seed_record(&seed, 1u, accept, sizeof(accept));
  fuzz_seed_record(&seed, 1u | (1u << 2), refuse, sizeof(refuse));
  fuzz_seed_record(&seed, 3u, "\0\x07\x00", 3u);
  fuzz_seed_record(&seed, 2u, later, sizeof(later));
  fuzz_seed_emit(writer, &seed);

  /* Retry required: the second Initial carries the server's cookie. */
  seed = (FuzzSeed){0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_SERVER);
  fuzz_seed_u8(&seed, 0x01u);
  fuzz_seed_record(&seed, 1u, retry, sizeof(retry));
  fuzz_seed_record(&seed, 1u, retry, sizeof(retry));
  fuzz_seed_emit(writer, &seed);

  /* A full server with a retry threshold of one. */
  seed = (FuzzSeed){0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_SERVER);
  fuzz_seed_u8(&seed, 0x06u);
  for (uint8_t i = 0u; i < 4u; ++i) {
    uint8_t body[] = {(uint8_t)(10u + i), 0, 0, 0, 0, 'x'};
    fuzz_seed_record(&seed, (uint8_t)(1u | (i << 2) | (i & 1u) << 4), body,
                     sizeof(body));
  }
  fuzz_seed_emit(writer, &seed);
}

static void fuzz_core_seed_established(FuzzSeedWriter *writer) {
  FuzzFrames frames;
  FuzzSeed seed;

  /* One seed per frame type, each after a server send so acks have a
     target. */
  static const uint8_t send_reliable[] = {3, VKR_NET_SEND_TAG, 0x00, 0x02};
  static const uint8_t send_fragmented[] = {4, VKR_NET_SEND_NOTIFY, 0x00, 0x30};
  static const uint8_t send_deadline[] = {5, 0x80, 0x40, 0x00};
  static const uint8_t wait[] = {0x00, 0x10};

  for (uint32_t kind = 0u; kind < 12u; ++kind) {
    seed = (FuzzSeed){0};
    fuzz_seed_u8(&seed, FUZZ_WORLD_ESTABLISHED);
    fuzz_seed_u8(&seed, 0u);
    fuzz_seed_record(&seed, 2u, send_reliable, sizeof(send_reliable));
    fuzz_seed_record(&seed, 2u, send_fragmented, sizeof(send_fragmented));
    fuzz_frames_begin(&frames, 0u, 0u);
    switch (kind) {
    case 0u:
      fuzz_frames_u8(&frames, FRAME_PING);
      fuzz_frames_u8(&frames, FRAME_PADDING);
      fuzz_frames_u8(&frames, FRAME_PADDING);
      break;
    case 1u:
      fuzz_frames_message(&frames, 1u, false_v, 0u, false_v, 0u, 0u, 0u, 40u);
      fuzz_frames_message(&frames, 2u, true_v, 0u, false_v, 0u, 0u, 0u, 20u);
      fuzz_frames_message(&frames, 2u, true_v, 3u, false_v, 0u, 0u, 0u, 20u);
      break;
    case 2u:
      fuzz_frames_message(&frames, 3u, true_v, 1u, false_v, 0u, 0u, 0u, 64u);
      fuzz_frames_message(&frames, 3u, true_v, 0u, false_v, 0u, 0u, 0u, 64u);
      fuzz_frames_message(&frames, 0u, true_v, 0u, false_v, 0u, 0u, 0u, 8u);
      break;
    case 3u:
      fuzz_frames_message(&frames, 4u, true_v, 0u, true_v, 0u, 400u, 1000u,
                          400u);
      fuzz_frames_message(&frames, 4u, true_v, 0u, true_v, 2u, 400u, 1000u,
                          200u);
      fuzz_seed_frames(&seed, &frames);
      fuzz_frames_begin(&frames, 0u, 1u);
      fuzz_frames_message(&frames, 4u, true_v, 0u, true_v, 1u, 400u, 1000u,
                          400u);
      break;
    case 4u:
      fuzz_frames_message(&frames, 5u, true_v, 0u, false_v, 0u, 0u, 0u, 30u);
      fuzz_frames_u8(&frames, FRAME_CANCEL);
      fuzz_frames_u8(&frames, 5u);
      fuzz_frames_varint(&frames, 1u);
      fuzz_frames_varint(&frames, 3000u);
      break;
    case 5u:
      /* ACK: largest, delay, range count, first range, gap, length. */
      fuzz_frames_u8(&frames, FRAME_ACK);
      fuzz_frames_varint(&frames, 3u);
      fuzz_frames_varint(&frames, 10u);
      fuzz_frames_varint(&frames, 1u);
      fuzz_frames_varint(&frames, 0u);
      fuzz_frames_varint(&frames, 0u);
      fuzz_frames_varint(&frames, 1u);
      break;
    case 6u:
      fuzz_frames_u8(&frames, FRAME_WINDOW);
      fuzz_frames_u8(&frames, 3u);
      fuzz_frames_varint(&frames, 1u << 20);
      fuzz_frames_u8(&frames, FRAME_BLOCKED);
      fuzz_frames_u8(&frames, 4u);
      fuzz_frames_varint(&frames, 8192u);
      break;
    case 7u:
      fuzz_frames_u8(&frames, FRAME_PATH_CHALLENGE);
      fuzz_frames_data(&frames, 8u, 0x5au);
      fuzz_frames_u8(&frames, FRAME_PATH_RESPONSE);
      fuzz_frames_data(&frames, 8u, 0xa5u);
      break;
    case 8u:
      /* From another address: path validation starts. */
      fuzz_frames_begin(&frames, 0u, 0x10u);
      fuzz_frames_u8(&frames, FRAME_PING);
      break;
    case 9u:
      fuzz_frames_u8(&frames, FRAME_EXTENSION | 0x05u);
      fuzz_frames_varint(&frames, 3u);
      fuzz_frames_data(&frames, 3u, 0x11u);
      fuzz_frames_u8(&frames, FRAME_CLOSE);
      fuzz_frames_varint(&frames, 0x100u);
      fuzz_frames_varint(&frames, 0u);
      break;
    case 10u:
      /* A replayed packet number, then a jump. */
      fuzz_frames_u8(&frames, FRAME_PING);
      fuzz_seed_frames(&seed, &frames);
      fuzz_frames_begin(&frames, 0x81u, 0u);
      fuzz_frames_u8(&frames, FRAME_PING);
      fuzz_seed_frames(&seed, &frames);
      fuzz_frames_begin(&frames, 0x41u, 0x03u);
      fuzz_frames_u8(&frames, FRAME_PING);
      break;
    default:
      fuzz_seed_record(&seed, 2u, send_deadline, sizeof(send_deadline));
      fuzz_seed_record(&seed, 3u, wait, sizeof(wait));
      fuzz_frames_u8(&frames, FRAME_PING);
      break;
    }
    fuzz_seed_frames(&seed, &frames);
    fuzz_seed_record(&seed, 3u, wait, sizeof(wait));
    fuzz_seed_emit(writer, &seed);
  }
}

static void fuzz_core_seed_client(FuzzSeedWriter *writer) {
  static const uint8_t accepted[] = {0x22, 0, 0, 0, 0, 0, 0, 'o', 'k'};
  static const uint8_t refused[] = {0x22, 0, 0, 0, 1, 5, 0};
  static const uint8_t cookie[VKR_NET_COOKIE_SIZE] = {1, 2, 3};
  FuzzFrames frames;

  FuzzSeed seed = {0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_CLIENT);
  fuzz_seed_u8(&seed, 0u);
  fuzz_seed_record(&seed, 1u, accepted, sizeof(accepted));
  fuzz_frames_begin(&frames, 0u, 0u);
  fuzz_frames_u8(&frames, FRAME_PING);
  fuzz_frames_message(&frames, 0u, true_v, 0u, false_v, 0u, 0u, 0u, 12u);
  fuzz_seed_record(&seed, 4u, frames.bytes + 1u, frames.size - 1u);
  fuzz_seed_emit(writer, &seed);

  seed = (FuzzSeed){0};
  fuzz_seed_u8(&seed, FUZZ_WORLD_CLIENT);
  fuzz_seed_u8(&seed, 0u);
  fuzz_seed_record(&seed, 2u, cookie, sizeof(cookie));
  fuzz_seed_record(&seed, 3u, "\x00\x00\x04\x00", 4u);
  fuzz_seed_record(&seed, 1u, refused, sizeof(refused));
  fuzz_seed_emit(writer, &seed);
}

static void fuzz_core_seeds(FuzzSeedWriter *writer) {
  fuzz_core_seed_server(writer);
  fuzz_core_seed_established(writer);
  fuzz_core_seed_client(writer);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  FUZZ_CHECK(vkr_net_crypto_init());
  fuzz_net_key(3u, &fuzz_core_extra_client);
  fuzz_seed_mode(fuzz_core_seeds);
  return 0;
}
